#!/usr/bin/env python3
"""Run a project's deterministic gates (typecheck, lint, test, build) and prove they can fail.

Reads <project>/.agent/gates.json. The project is --project PATH, else the first folder
above the current directory that holds .agent/gates.json.

  gates.py list                       show the configured gates
  gates.py run [--only NAME ...] [--enforced-only] [--on-stop] [--json]
                                      run gates, write .agent/gates/last.json
  gates.py prove [--gate NAME] [--canary-path DIR]
                                      plant a failing canary, check the gate catches it,
                                      write .agent/gates/proof.json

Needs only the standard library and works copied alone to <project>/.agent/bin/gates.py.
Importable: find_project, load_config, select_gates, run_gates.

Exit codes: 0 every selected enforced gate passed (prove: at least one gate proven, none missed);
1 a check failed (prove: a canary was not caught); 2 usage, config or infrastructure error.
"""
import argparse
import datetime
import json
import os
import re
import signal
import subprocess
import sys
import time
from pathlib import Path

DEFAULT_TIMEOUT = 600
TAIL_LINES = 40
SHOWN_TAIL_LINES = 15
KINDS = ("typecheck", "lint", "test", "build", "other")
CANARY_MARKERS = ("gate canary", "gate_canary", "gateCanary", "__gate_canary__")
PM_SCRIPT = re.compile(r"(?:npm|pnpm|yarn|bun)\s+(?:run\s+)?([\w:.-]+)")


class ConfigError(Exception):
    """Bad usage, bad config, or something the runner cannot work around."""


def now_iso():
    return datetime.datetime.now().astimezone().isoformat(timespec="seconds")


def write_json(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")


def find_project(explicit=None, start=None):
    if explicit:
        return Path(explicit).expanduser().resolve()
    here = Path(start or os.getcwd()).resolve()
    for folder in (here, *here.parents):
        if (folder / ".agent" / "gates.json").is_file():
            return folder
    script = Path(__file__).resolve()
    if script.parent.name == "bin" and script.parent.parent.name == ".agent":
        folder = script.parent.parent.parent
        if (folder / ".agent" / "gates.json").is_file():
            return folder
    return None


def load_config(project):
    path = Path(project) / ".agent" / "gates.json"
    if not path.is_file():
        raise ConfigError(f"{path} not found (run harden-build's install.py, or write it by hand)")
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except ValueError as err:
        raise ConfigError(f"{path} is not valid JSON ({err})")
    if not isinstance(raw, dict) or not isinstance(raw.get("gates"), list):
        raise ConfigError(f"{path} needs a top-level object with a 'gates' list")
    gates, seen = [], set()
    for i, g in enumerate(raw["gates"], 1):
        if not isinstance(g, dict) or not g.get("name") or not g.get("cmd"):
            raise ConfigError(f"gate #{i} in {path} needs a 'name' and a 'cmd'")
        name = str(g["name"])
        if name in seen:
            raise ConfigError(f"gate name '{name}' appears twice in {path}")
        seen.add(name)
        kind = g.get("kind", "other")
        if kind not in KINDS:
            raise ConfigError(f"gate '{name}' has kind '{kind}'; use one of {', '.join(KINDS)}")
        timeout = g.get("timeout") or DEFAULT_TIMEOUT
        if isinstance(timeout, bool) or not isinstance(timeout, (int, float)) or timeout <= 0:
            raise ConfigError(f"gate '{name}' has a bad timeout {timeout!r}")
        gate = {"name": name, "kind": kind, "cmd": str(g["cmd"]), "timeout": timeout,
                "enforce": bool(g.get("enforce", True)), "on_stop": bool(g.get("on_stop", True))}
        if g.get("note"):
            gate["note"] = str(g["note"])
        gates.append(gate)
    attempts = raw.get("stop_max_attempts", 2)
    if isinstance(attempts, bool) or not isinstance(attempts, int) or attempts < 1:
        raise ConfigError(f"stop_max_attempts in {path} must be a whole number of 1 or more")
    return {"version": raw.get("version", 1), "stop_max_attempts": attempts, "gates": gates}


def select_gates(cfg, only=None, enforced_only=False, on_stop=False):
    gates = cfg["gates"]
    if only:
        known = {g["name"] for g in gates}
        unknown = [n for n in only if n not in known]
        if unknown:
            raise ConfigError(f"unknown gate name: {', '.join(unknown)} (known: {', '.join(sorted(known)) or 'none'})")
        gates = [g for g in gates if g["name"] in only]
    if enforced_only:
        gates = [g for g in gates if g["enforce"]]
    if on_stop:
        gates = [g for g in gates if g["on_stop"]]
    return gates


def kill_group(proc):
    try:
        os.killpg(proc.pid, signal.SIGKILL)
    except (ProcessLookupError, PermissionError):
        proc.kill()


def as_text(value):
    if isinstance(value, bytes):
        return value.decode("utf-8", "replace")
    return value or ""


def run_command(cmd, cwd, timeout):
    """Run a shell command in its own process group. Returns (exit or None, output, timed_out, seconds)."""
    env = dict(os.environ, CI="1", FORCE_COLOR="0", NO_COLOR="1")
    started = time.monotonic()
    proc = subprocess.Popen(cmd, shell=True, cwd=str(cwd), env=env, stdin=subprocess.DEVNULL,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                            errors="replace", start_new_session=True)
    try:
        output, _ = proc.communicate(timeout=timeout)
        return proc.returncode, output or "", False, time.monotonic() - started
    except subprocess.TimeoutExpired as exc:
        kill_group(proc)
        try:
            output, _ = proc.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            output = as_text(exc.output)
        return None, output or "", True, time.monotonic() - started
    except BaseException:
        kill_group(proc)
        raise


def tail_of(output):
    return "\n".join(output.splitlines()[-TAIL_LINES:])


def run_gates(project, cfg, selected=None, progress=None, write=True):
    """Run the selected gates (gate dicts or names; None means all) and return the report dict.

    The report is also written to <project>/.agent/gates/last.json unless write is False.
    progress, if given, is called as progress(result) after each gate.
    """
    project = Path(project)
    by_name = {g["name"]: g for g in cfg["gates"]}
    if selected is None:
        gates = list(cfg["gates"])
    else:
        gates = [by_name[g] if isinstance(g, str) else g for g in selected]
    results = []
    for g in gates:
        code, output, timed_out, seconds = run_command(g["cmd"], project, g["timeout"])
        status = "timeout" if timed_out else ("pass" if code == 0 else "fail")
        result = {"name": g["name"], "kind": g["kind"], "enforce": g["enforce"], "status": status,
                  "exit": code, "seconds": round(seconds, 1),
                  "tail": "" if status == "pass" else tail_of(output)}
        results.append(result)
        if progress:
            progress(result)
    report = {"ts": now_iso(), "project": str(project),
              "all_pass": all(r["status"] == "pass" for r in results),
              "enforced_pass": all(r["status"] == "pass" for r in results if r["enforce"]),
              "results": results}
    if write:
        write_json(project / ".agent" / "gates" / "last.json", report)
    return report


def result_line(r):
    if r["status"] == "pass":
        return f"PASS  {r['name']}  ({r['seconds']}s)"
    flag = "" if r["enforce"] else "  (report-only)"
    if r["status"] == "timeout":
        return f"TIMEOUT  {r['name']}  after {r['seconds']}s{flag}"
    return f"FAIL  {r['name']}  exit={r['exit']}  ({r['seconds']}s){flag}"


def print_result(r):
    print(result_line(r), flush=True)
    if r["status"] != "pass" and r["tail"]:
        for line in r["tail"].splitlines()[-SHOWN_TAIL_LINES:]:
            print("    " + line)


def cmd_list(project, cfg, args):
    if not cfg["gates"]:
        print("no gates configured")
    for g in cfg["gates"]:
        print(f"{g['name']}  kind={g['kind']}  enforce={str(g['enforce']).lower()}  "
              f"on_stop={str(g['on_stop']).lower()}  timeout={g['timeout']}s  cmd: {g['cmd']}"
              + (f"  note: {g['note']}" if g.get("note") else ""))
    return 0


def cmd_run(project, cfg, args):
    gates = select_gates(cfg, args.only, args.enforced_only, args.on_stop)
    report = run_gates(project, cfg, gates, progress=None if args.json else print_result)
    if args.json:
        print(json.dumps(report, indent=2))
    else:
        counts = {s: sum(1 for r in report["results"] if r["status"] == s) for s in ("pass", "fail", "timeout")}
        verdict = "enforced gates GREEN" if report["enforced_pass"] else "enforced gates RED"
        print(f"{len(gates)} gate{'' if len(gates) == 1 else 's'}: {counts['pass']} passed, {counts['fail']} failed, "
              f"{counts['timeout']} timed out. {verdict}. Details: .agent/gates/last.json" if gates
              else "no gates selected")
    return 0 if report["enforced_pass"] else 1


def package_scripts(project):
    try:
        scripts = json.loads((project / "package.json").read_text(encoding="utf-8")).get("scripts")
    except (OSError, ValueError, AttributeError):
        return {}
    return scripts if isinstance(scripts, dict) else {}


def effective_text(project, cmd):
    """The gate command plus the bodies of any package.json scripts it calls (to a small depth)."""
    scripts = package_scripts(project)
    text, frontier = cmd, [cmd]
    for _ in range(3):
        bodies = []
        for piece in frontier:
            for name in PM_SCRIPT.findall(piece):
                name = {"t": "test"}.get(name, name)
                if isinstance(scripts.get(name), str):
                    bodies.append(scripts[name])
        if not bodies:
            break
        text += " " + " ".join(bodies)
        frontier = bodies
    return text.lower()


JS_TEST_FILE = re.compile(r"\.(?:test|spec)\.(js|jsx|mjs|cjs|ts|tsx|mts|cts)$")


def js_test_extension(folder, max_depth=3):
    """The extension most existing tests under folder use, so the canary lands inside the
    runner's include pattern (a vitest config with tests/**/*.test.ts never sees a .js file)."""
    folder = Path(folder)
    counts = {}
    for root, dirs, files in os.walk(folder):
        depth = len(Path(root).relative_to(folder).parts)
        dirs[:] = [] if depth >= max_depth else [d for d in dirs if d != "node_modules" and not d.startswith(".")]
        for name in files:
            found = JS_TEST_FILE.search(name)
            if found:
                counts[found.group(1)] = counts.get(found.group(1), 0) + 1
    return max(sorted(counts), key=counts.get) if counts else "js"


def pick_canary(project, gate, override=None):
    """Choose a canary for a gate. Returns (path, body, note); path is None when none applies."""
    project = Path(project)
    text = effective_text(project, gate["cmd"])
    def word(name):
        return re.search(rf"(?<![a-z0-9]){name}(?![a-z0-9])", text)

    kinds = {"pytest": word("pytest"), "vitest": word("vitest"), "jest": word("jest"),
             "go": re.search(r"\bgo\s+test\b", text), "mypy": word("mypy"),
             "tsc": word("tsc") or re.search(r"type-?check", text)}
    order = ["pytest", "vitest", "jest", "go", "mypy", "tsc"]
    if gate["kind"] == "typecheck":
        order = ["tsc", "mypy", "pytest", "vitest", "jest", "go"]
    runner = next((k for k in order if kinds[k]), None)
    if runner is None:
        return None, None, "no canary recipe for this command (pytest, vitest, jest, go test, tsc and mypy are supported)"

    def where(*names):
        if override:
            folder = Path(override).expanduser()
            folder = folder if folder.is_absolute() else project / folder
            if not folder.is_dir():
                raise ConfigError(f"--canary-path {folder} is not a folder")
            return folder
        for name in names:
            if (project / name).is_dir():
                return project / name
        return project

    if runner == "pytest":
        return (where("tests") / "test_gate_canary.py",
                'def test_gate_canary():\n    assert False, "gate canary"\n', "")
    if runner in ("vitest", "jest"):
        body = 'test("gate canary", () => { expect(1).toBe(2); });\n'
        if runner == "vitest":
            body = 'import { test, expect } from "vitest";\n' + body
        folder = where("tests", "test", "__tests__", "src")
        return folder / f"__gate_canary__.test.{js_test_extension(folder)}", body, ""
    if runner == "go":
        folder = where()
        package = None
        for src in sorted(folder.glob("*.go"), key=lambda p: (p.name.endswith("_test.go"), p.name)):
            found = re.search(r"^\s*package\s+(\w+)", src.read_text(encoding="utf-8", errors="replace"), re.M)
            if found:
                package = found.group(1)
                break
        if package is None:
            return None, None, f"no .go file in {folder} to take a package name from"
        return (folder / "gate_canary_test.go",
                f'package {package}\n\nimport "testing"\n\nfunc TestGateCanary(t *testing.T) {{ t.Fatal("gate canary") }}\n', "")
    if runner == "mypy":
        return where() / "gate_canary.py", 'gate_canary: int = "gate canary"\n', ""
    if not (project / "tsconfig.json").is_file():
        return None, None, "typecheck command found but there is no tsconfig.json"
    return (where("src") / "__gate_canary__.ts",
            'const gateCanary: number = "gate canary";\nexport {};\n', "")


def remove_canary(path):
    try:
        path.unlink()
    except FileNotFoundError:
        pass
    cache = path.parent / "__pycache__"
    if cache.is_dir():
        for stale in cache.glob(f"*{path.stem}*"):
            try:
                stale.unlink()
            except OSError:
                pass


def prove_one(project, gate, path, body):
    shown = path.relative_to(project) if path.is_relative_to(project) else path
    try:
        path.write_text(body, encoding="utf-8")
        code, output, timed_out, _ = run_command(gate["cmd"], project, gate["timeout"])
    finally:
        remove_canary(path)
    result = {"gate": gate["name"], "status": "not_caught", "canary_path": str(shown), "exit": code, "note": ""}
    if timed_out:
        result["note"] = "gate timed out with the canary planted, so nothing is proven; raise the gate's timeout"
    elif code != 0 and any(m in output for m in CANARY_MARKERS):
        result["status"] = "caught"
        result["note"] = "the planted failing canary turned the gate red"
    else:
        reason = ("the gate passed with a failing canary planted" if code == 0
                  else "the gate failed but its output never mentioned the canary (it may already be red for another reason; run it alone first)")
        result["note"] = (f"{reason}: either the canary at {shown} sits outside the gate's include patterns "
                          f"(retry with --canary-path DIR pointing at a folder the gate covers) "
                          f"or the gate is not testing what it claims")
    return result


def cmd_prove(project, cfg, args):
    if args.gate:
        gates = select_gates(cfg, [args.gate])
    else:
        gates = [g for g in cfg["gates"] if g["kind"] in ("test", "typecheck")]
    plan = [(g, *pick_canary(project, g, args.canary_path)) for g in gates]
    leftovers = sorted({str(p) for _, p, _, _ in plan if p is not None and p.exists()})
    if leftovers:
        raise ConfigError("a canary file already exists (left by a crashed earlier run?); delete it and rerun: "
                          + ", ".join(leftovers))
    signal.signal(signal.SIGTERM, lambda signum, frame: sys.exit(143))
    results = []
    for gate, path, body, note in plan:
        if path is None:
            result = {"gate": gate["name"], "status": "skipped", "canary_path": "", "exit": None, "note": note}
        else:
            result = prove_one(project, gate, path, body)
        results.append(result)
        label = {"caught": "CAUGHT", "not_caught": "MISSED", "skipped": "SKIP"}[result["status"]]
        print(f"{label}  {result['gate']}  {result['canary_path']}  {result['note']}".rstrip())
    write_json(project / ".agent" / "gates" / "proof.json", {"ts": now_iso(), "results": results})
    caught = sum(1 for r in results if r["status"] == "caught")
    missed = sum(1 for r in results if r["status"] == "not_caught")
    print(f"{caught} caught, {missed} missed, {len(results) - caught - missed} skipped. Details: .agent/gates/proof.json")
    if missed:
        return 1
    return 0 if caught else 2


def build_parser():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--project", help="project folder (default: nearest parent holding .agent/gates.json)")
    sub = ap.add_subparsers(dest="command", required=True)

    def add(name, func, help_text):
        p = sub.add_parser(name, help=help_text)
        p.add_argument("--project", default=argparse.SUPPRESS, help=argparse.SUPPRESS)
        p.set_defaults(func=func)
        return p

    add("list", cmd_list, "show the configured gates")
    run = add("run", cmd_run, "run gates and write .agent/gates/last.json")
    run.add_argument("--only", nargs="+", action="extend", metavar="NAME", help="run only these gates")
    run.add_argument("--enforced-only", action="store_true", help="skip report-only gates")
    run.add_argument("--on-stop", action="store_true", help="only gates marked on_stop")
    run.add_argument("--json", action="store_true", help="print last.json instead of one line per gate")
    prove = add("prove", cmd_prove, "plant a failing canary and check the gates catch it")
    prove.add_argument("--gate", metavar="NAME", help="prove one gate (default: every test and typecheck gate)")
    prove.add_argument("--canary-path", metavar="DIR", help="folder to plant the canary in")
    return ap


def main(argv=None):
    args = build_parser().parse_args(argv)
    try:
        project = find_project(args.project)
        if project is None:
            raise ConfigError("no .agent/gates.json in this folder or any parent (use --project PATH)")
        if not project.is_dir():
            raise ConfigError(f"{project} is not a folder")
        return args.func(project, load_config(project), args)
    except ConfigError as err:
        print(f"error: {err}", file=sys.stderr)
        return 2
    except OSError as err:
        print(f"error: {err}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())

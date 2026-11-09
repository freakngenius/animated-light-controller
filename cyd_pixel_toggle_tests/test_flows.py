#!/usr/bin/env python3
"""test_flows.py - drives the REAL app code (cyd_sim) with scripted touches and checks what comes out.

Each scenario is a small touch script plus the `state` lines it must print, in order. A state line
looks like:
  screen=2 transition=0 dark=1 hue=209 brightness=100 bulb_power=unset bulb_hue=209 bulb_brightness=100 bulb_packets=8
Expectations name only the fields that matter; numbers can be exact ("screen": "2") or a range
("hue": (80, 90)). `bulb_packets` can be compared with ">0" style strings via the helpers below.

Usage: python3 test_flows.py [OUTDIR]     exit code 0 = every scenario passed.
Set CYD_SRC to point at another copy of the sketch folder (used to prove the tests can fail).
"""
import os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SIM = os.path.join(HERE, "cyd_sim.sh")
OUT = sys.argv[1] if len(sys.argv) > 1 else tempfile.mkdtemp(prefix="cyd_flows_")

failures = []
checks = 0


def parse(line):
    d = {}
    for part in line.split():
        if "=" in part:
            k, v = part.split("=", 1)
            d[k] = v
    return d


def run(script):
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
        f.write(script)
        path = f.name
    try:
        p = subprocess.run(["sh", SIM, path, OUT], capture_output=True, text=True)
    finally:
        os.unlink(path)
    if p.returncode != 0:
        return None, p.stderr.strip() or p.stdout.strip() or ("exit %d" % p.returncode)
    states = [parse(l) for l in p.stdout.splitlines() if l.startswith("screen=")]
    return states, ""


def ok(value, want):
    if callable(want):
        return want(value)
    if isinstance(want, tuple):
        try:
            return want[0] <= float(value) <= want[1]
        except ValueError:
            return False
    return str(value) == str(want)


def scenario(name, script, expected):
    """expected: list of dicts, one per `state` line in the script."""
    global checks
    states, err = run(script)
    if states is None:
        failures.append("%s: the simulator did not run: %s" % (name, err))
        checks += 1
        return
    if len(states) != len(expected):
        failures.append("%s: script printed %d state lines, test expects %d" % (name, len(states), len(expected)))
        checks += 1
        return
    for i, (got, want) in enumerate(zip(states, expected)):
        for k, w in want.items():
            checks += 1
            if k not in got or not ok(got[k], w):
                failures.append("%s: state #%d: %s is %s, expected %s   [%s]" % (
                    name, i + 1, k, got.get(k), w if not callable(w) else "(rule)", " ".join("%s=%s" % kv for kv in got.items())))


def positive(v):
    return float(v) > 0


# ------------------------------------------------------------------ scenarios
TOGGLE_X, TOGGLE_Y = 160, 120

scenario("tap flips the toggle and tells the bulb", """
state
tap %d %d
settle
state
tap %d %d
settle
state
""" % (TOGGLE_X, TOGGLE_Y, TOGGLE_X, TOGGLE_Y), [
    {"screen": "1", "dark": "1", "bulb_power": "unset", "bulb_packets": "0"},
    {"screen": "1", "dark": "0", "bulb_power": "on", "bulb_packets": "1"},
    {"screen": "1", "dark": "1", "bulb_power": "off", "bulb_packets": "2"},
])

scenario("a tap outside the toggle does nothing", """
tap 160 30
tap 20 220
settle
state
""", [{"screen": "1", "dark": "1", "bulb_power": "unset", "bulb_packets": "0"}])

scenario("swipe up goes to screen two, swipe down on the left goes back", """
swipe 160 200 160 90 200
settle
state
swipe 100 60 100 190 200
settle
state
""", [
    {"screen": "2", "transition": "0"},
    {"screen": "1", "transition": "0"},
])

# ten round trips in a row, with slightly different swipes each time
trip = ""
for i in range(10):
    x = 40 + 25 * i
    trip += "swipe %d 205 %d 85 %d\nsettle\nstate\nswipe %d 50 %d 195 %d\nsettle\nstate\n" % (x, x + (i % 3) * 6, 160 + 10 * i, x % 150 + 20, x % 150 + 20 + (i % 4) * 5, 170 + 8 * i)
scenario("ten round trips with no missed or doubled swipe", trip,
         [{"screen": "2"}, {"screen": "1"}] * 10)

scenario("a swipe up that is too short does nothing", """
swipe 160 150 160 130 150
settle
state
""", [{"screen": "1"}])

scenario("a swipe up from the very bottom edge and a slow one both work", """
swipe 160 238 160 120 400
settle
state
swipe 20 60 20 190 600
settle
state
""", [{"screen": "2"}, {"screen": "1"}])

scenario("a sideways swipe on screen one does nothing", """
swipe 40 120 280 130 200
settle
state
""", [{"screen": "1", "bulb_packets": "0"}])

scenario("swipe down that starts on the brightness slider never leaves screen two", """
swipe 160 200 160 90 200
settle
swipe 268 60 268 200 150
settle
state
swipe 214 60 214 200 150
settle
state
swipe 300 20 300 230 150
settle
state
swipe 250 100 262 220 150
settle
state
""", [{"screen": "2"}] * 4)

scenario("a swipe down at the edge of the left zone does go back", """
swipe 160 200 160 90 200
settle
swipe 213 60 213 200 150
settle
state
""", [{"screen": "1"}])

scenario("a sideways drag on the hue slider never leaves screen two", """
swipe 160 200 160 90 200
settle
drag 120 120 20 120 150
up
settle
state
drag 20 120 190 124 150
up
settle
state
drag 20 100 190 130 150
up
settle
state
drag 190 150 20 112 150
up
settle
state
""", [{"screen": "2"}] * 4)

scenario("hue slider limits and the bulb follows", """
swipe 160 200 160 90 200
settle
drag 120 120 0 120 150
up
settle
state
drag 0 120 319 120 150
up
settle
state
""", [
    {"screen": "2", "hue": (0, 3), "bulb_hue": (0, 3), "bulb_power": "unset"},
    {"screen": "2", "hue": (357, 360), "bulb_hue": (357, 360), "bulb_power": "unset"},
])

scenario("brightness slider limits and the bulb follows", """
swipe 160 200 160 90 200
settle
drag 268 100 268 239 150
up
settle
state
drag 268 239 268 0 200
up
settle
state
""", [
    {"screen": "2", "brightness": (1, 3), "bulb_brightness": (1, 3), "bulb_power": "unset"},
    {"screen": "2", "brightness": (99, 100), "bulb_brightness": (99, 100), "bulb_power": "unset"},
])

scenario("a tap on the rainbow bar and on the meter jumps the knob there", """
swipe 160 200 160 90 200
settle
tap 40 120
settle
state
tap 268 120
settle
state
""", [
    {"screen": "2", "hue": (30, 37)},                 # (40 - 25) / 164 * 360 = 33
    {"screen": "2", "brightness": (47, 53)},          # (202 - 120) / 168 = 0.49 -> about 50%
])

scenario("a drag that starts at the top band moves no slider", """
swipe 160 200 160 90 200
settle
state
drag 60 30 200 32 150
up
settle
state
""", [{"screen": "2", "hue": (208, 211), "brightness": (99, 100), "bulb_packets": "0"},
      {"screen": "2", "hue": (208, 211), "brightness": (99, 100), "bulb_packets": "0"}])

scenario("a drag that starts mostly downwards on the hue slider swipes back", """
swipe 160 200 160 90 200
settle
swipe 120 70 130 190 150
settle
state
""", [{"screen": "1"}])

scenario("touches during the screen change are ignored and nothing sticks", """
swipe 160 200 160 90 100
wait 100
tap 160 120
wait 50
swipe 100 60 100 190 60
settle
state
swipe 160 200 160 90 200
settle
state
swipe 100 60 100 190 200
settle
state
""", [
    {"screen": "2", "dark": "1", "bulb_power": "unset", "bulb_packets": "0"},
    {"screen": "2"},
    {"screen": "1"},
])

scenario("a finger that is already down when the screen changes cannot act until it lifts", """
drag 160 200 160 80 120
wait 60
drag 160 80 20 120 100
wait 400
state
up
settle
state
swipe 100 60 100 190 150
settle
state
""", [
    {"screen": "2", "transition": "0", "bulb_packets": "0", "hue": (208, 211)},
    {"screen": "2", "bulb_packets": "0", "hue": (208, 211)},
    {"screen": "1", "bulb_packets": "0"},
])

scenario("packets stay modest while a finger drags", """
swipe 160 200 160 90 200
settle
drag 20 120 190 120 2000
up
settle
state
""", [{"screen": "2", "bulb_packets": lambda v: 0 < int(v) <= 260}])   # the simulator counts every change; lifx.cpp throttles to 10/s (tested in lifx_sim)

scenario("the toggle still works after visiting screen two", """
swipe 160 200 160 90 200
settle
drag 20 120 100 120 100
up
settle
swipe 100 60 100 190 150
settle
tap %d %d
settle
state
""" % (TOGGLE_X, TOGGLE_Y), [{"screen": "1", "dark": "0", "bulb_power": "on"}])

# ------------------------------------------------------------------ report
print("== test_flows: %d checks, %d failed" % (checks, len(failures)))
for f in failures:
    print("  FAIL " + f)
print("CHECKS FAILED" if failures else "all checks passed")
sys.exit(1 if failures else 0)

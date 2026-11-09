#!/bin/sh
# run_all.sh - run every host test in this folder. Exit code 0 only when all of them pass.
#
#   ./run_all.sh            run everything
#
# Each test script prints its own result. A short summary line per script comes at the end,
# so one red test cannot hide behind the green ones above it.
set -u
cd "$(dirname "$0")" || exit 2

OUT="${TMPDIR:-/tmp}/cyd_gate_preview"
FAILED=""
RAN=0

run() {   # run <label> <command...>
  label="$1"; shift
  RAN=$((RAN+1))
  echo "== $label"
  if "$@"; then
    echo "-- $label: pass"
  else
    echo "-- $label: FAIL (exit $?)"
    FAILED="$FAILED $label"
  fi
}

run physics sh ./build_physics_test.sh
run gesture sh ./build_gesture_test.sh
run lifx_proto sh ./build_lifx_test.sh
run lifx_sim sh ./build_lifx_sim.sh
run preview sh ./build_preview.sh "$OUT"
run screens sh ./test_screens.sh "$OUT/screens"
run flows python3 ./test_flows.py "$OUT/flows"

echo
if [ -n "$FAILED" ]; then
  echo "== $RAN host test scripts ran, FAILED:$FAILED"
  exit 1
fi
echo "== $RAN host test scripts ran, all passed"

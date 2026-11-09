#!/bin/sh
# build_gesture_test.sh - compile and run the gesture recogniser tests on your laptop.
#
#   ./build_gesture_test.sh            normal run (exit code 0 = all passed)
#   ./build_gesture_test.sh broken     builds the test with ONE expectation deliberately wrong,
#                                      to prove the test can fail (this run MUST exit non-zero)
set -u
cd "$(dirname "$0")" || exit 2

CXX="${CXX:-g++}"
OUT="${TMPDIR:-/tmp}/test_gesture"
FLAGS="-std=gnu++11 -Wall -Wextra -I ../cyd_pixel_toggle"
SRC="test_gesture.cpp ../cyd_pixel_toggle/gesture.cpp"

EXTRA=""
if [ "${1:-}" = "broken" ]; then
  EXTRA="-DBREAK_ON_PURPOSE"
  echo "== building with one expectation deliberately wrong (expect FAIL below) =="
fi

$CXX $FLAGS $EXTRA -o "$OUT" $SRC || exit 2
"$OUT"
RESULT=$?

if [ "${1:-}" != "broken" ] && [ "$RESULT" -eq 0 ]; then
  $CXX $FLAGS -g -fsanitize=address,undefined -fno-sanitize-recover=all -o "$OUT.asan" $SRC || exit 2
  "$OUT.asan" > /dev/null
  RESULT=$?
  if [ "$RESULT" -eq 0 ]; then echo "sanitizer build: clean"; else echo "sanitizer build: FAILED"; fi
fi
exit $RESULT

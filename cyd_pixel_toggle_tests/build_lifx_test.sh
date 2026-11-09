#!/bin/sh
# build_lifx_test.sh - compile and run the LIFX packet tests on your laptop.
#
#   ./build_lifx_test.sh            normal run (exit code 0 = all passed)
#   ./build_lifx_test.sh broken     builds the test with ONE expectation deliberately wrong,
#                                   to prove the test can fail (this run MUST exit non-zero)
#
# Needs only a C++ compiler (g++ or clang++). No Arduino, no board.
set -u
cd "$(dirname "$0")" || exit 2

CXX="${CXX:-g++}"
OUT="${TMPDIR:-/tmp}/test_lifx_proto"
FLAGS="-std=gnu++11 -Wall -Wextra"
SRC="test_lifx_proto.cpp ../cyd_pixel_toggle/lifx_proto.cpp"

EXTRA=""
if [ "${1:-}" = "broken" ]; then
  EXTRA="-DBREAK_ON_PURPOSE"
  echo "== building with one expectation deliberately wrong (expect FAIL below) =="
fi

# 1) the normal build the project asked for
$CXX $FLAGS $EXTRA -o "$OUT" $SRC || exit 2
"$OUT"
RESULT=$?

# 2) the same tests again with address + undefined-behaviour checking, which turns any
#    read past the end of a buffer into a loud error (skipped in "broken" mode)
if [ "${1:-}" != "broken" ] && [ "$RESULT" -eq 0 ]; then
  $CXX $FLAGS -g -fsanitize=address,undefined -fno-sanitize-recover=all \
       -o "$OUT.asan" $SRC || exit 2
  "$OUT.asan" > /dev/null
  RESULT=$?
  if [ "$RESULT" -eq 0 ]; then
    echo "sanitizer build: no buffer over-reads, no undefined behaviour"
  else
    echo "sanitizer build: FAILED"
  fi
fi

exit $RESULT

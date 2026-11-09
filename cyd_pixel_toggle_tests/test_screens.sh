#!/bin/sh
# Builds and runs test_screens.cpp (screen two + the wipe). Exit code 0 = all pass.
# Usage: ./test_screens.sh [scratch-folder]
set -e
cd "$(dirname "$0")"
OUT="${1:-${TMPDIR:-/tmp}/cyd_gate_screens}"
mkdir -p "$OUT"
g++ -std=gnu++11 -Wall -Wextra -O2 -fno-rtti \
    -I mock -I ../cyd_pixel_toggle \
    test_screens.cpp ../cyd_pixel_toggle/draw.cpp ../cyd_pixel_toggle/physics.cpp \
    -o "$OUT/test_screens"
"$OUT/test_screens"

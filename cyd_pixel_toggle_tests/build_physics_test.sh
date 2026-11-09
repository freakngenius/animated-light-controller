#!/bin/bash
# Builds and runs the physics unit test on your Mac (no board needed).
# Uses the same C++ standard as the ESP32 Arduino core (gnu++11).
set -e
cd "$(dirname "$0")"
OUT="${TMPDIR:-/tmp}/cyd_test_physics"
g++ -std=gnu++11 -fno-rtti -Wall -Wextra -Wno-unused-parameter -O1 -fsanitize=address,undefined \
    test_physics.cpp ../cyd_pixel_toggle/physics.cpp -o "$OUT"
"$OUT"

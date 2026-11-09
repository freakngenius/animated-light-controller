#!/bin/sh
# Builds the laptop preview of the CYD drawing code and runs it.
# Usage: ./build_preview.sh [output-folder-for-PNGs]
set -e
cd "$(dirname "$0")"
OUT="${1:-${TMPDIR:-/tmp}/cyd_preview}"
mkdir -p "$OUT"
g++ -std=gnu++11 -Wall -Wextra -O2 -fno-rtti \
    -I mock -I ../cyd_pixel_toggle \
    preview.cpp ../cyd_pixel_toggle/draw.cpp ../cyd_pixel_toggle/physics.cpp \
    -o "$OUT/preview"
"$OUT/preview" "$OUT"

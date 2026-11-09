#!/bin/sh
# Builds the laptop driver cyd_sim and runs a touch script through it.
# Usage: ./cyd_sim.sh SCRIPT [OUTDIR]      (see the top of cyd_sim.cpp for the script commands)
# Exit code: 0 = ran fine; 2 = bad script or build problem.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SCRIPT="$1"
OUT="${2:-.}"
[ -n "$SCRIPT" ] || { echo "usage: cyd_sim.sh SCRIPT [OUTDIR]" >&2; exit 2; }
case "$SCRIPT" in /*) ;; *) SCRIPT="$PWD/$SCRIPT" ;; esac
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
cd "$HERE"
SRC="${CYD_SRC:-../cyd_pixel_toggle}"
KEY="$(printf %s "$(cd "$SRC" && pwd)" | cksum | cut -d" " -f1)"
BIN="${CYD_SIM_BIN:-${TMPDIR:-/tmp}/cyd_sim_bin_$(id -u)_$KEY}"   # one binary per sketch folder
if [ ! -x "$BIN" ] || [ -n "$(find cyd_sim.cpp png_writer.h mock $SRC \( -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) -newer "$BIN" 2>/dev/null | head -1)" ]; then
  g++ -std=gnu++11 -Wall -Wextra -O2 -fno-rtti -I mock -I "$SRC" \
      cyd_sim.cpp "$SRC/draw.cpp" "$SRC/physics.cpp" "$SRC/gesture.cpp" "$SRC/screens.cpp" "$SRC/app.cpp" \
      -o "$BIN"
fi
exec "$BIN" "$SCRIPT" "$OUT"

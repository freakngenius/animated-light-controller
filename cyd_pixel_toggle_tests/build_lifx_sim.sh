#!/bin/sh
# build_lifx_sim.sh - run the REAL lifx.cpp on your laptop against a pretend bulb (no board needed).
#
#   ./build_lifx_sim.sh            run every scenario; exit code 0 = all passed
#   SIM_VERBOSE=1 ./build_lifx_sim.sh   also print the board's log lines as they happen
#
# It copies the project files to a temp folder so three different config.h variants can be tried
# (default / fixed bulb IP / specific bulb id), then runs each scenario three times with the fake
# clock starting at different places, one of which makes millis() wrap around mid-run.
set -u
cd "$(dirname "$0")" || exit 2

CXX="${CXX:-g++}"
SRC=../cyd_pixel_toggle
WORK="${TMPDIR:-/tmp}/lifx_sim_build"
FLAGS="-std=gnu++11 -Wall -Wextra -g -pthread"
rm -rf "$WORK"; mkdir -p "$WORK"

build() {   # build <name> <sed-expression applied to config.h>
  d="$WORK/$1"; mkdir -p "$d"
  for f in lifx.cpp lifx_proto.cpp lifx_proto.h lifx.h status.h; do cp "$SRC/$f" "$d/"; done
  # a fresh clone has no secrets.h (it is git-ignored): the placeholder file is enough for the simulator
  if [ -f "$SRC/secrets.h" ]; then cp "$SRC/secrets.h" "$d/secrets.h"; else cp "$SRC/secrets.h.example" "$d/secrets.h"; fi
  sed -e "$2" "$SRC/config.h" > "$d/config.h"
  $CXX $FLAGS -Ilifx_sim/mock -I"$d" -o "$d/sim" lifx_sim/sim_lifx.cpp "$d/lifx.cpp" "$d/lifx_proto.cpp" || exit 2
}
build default  's/^$//'
build fallback 's|#define LIFX_FALLBACK_IP ""|#define LIFX_FALLBACK_IP "192.168.1.42"|'
build target   's|#define LIFX_TARGET_MAC ""|#define LIFX_TARGET_MAC "d073d5aabb02"|'

FAILS=0; RUNS=0
run() {   # run <build> <scenario>
  for start in 0 4294962295 4294950000; do
    RUNS=$((RUNS+1))
    "$WORK/$1/sim" "$2" "$start" || FAILS=$((FAILS+1))
  done
}
for s in $("$WORK/default/sim" list); do
  case "$s" in
    fallback|fallback_blind|target|target_missing) ;;
    *) run default "$s" ;;
  esac
done
run fallback fallback
run fallback fallback_blind
run target target
run target target_missing

echo "== $RUNS runs, $FAILS failed =="
[ "$FAILS" -eq 0 ]

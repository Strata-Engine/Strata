#!/bin/sh
# CPU cost of one Strata.Mesh.build for each mode of lib/mesh_translucent.march:
# a REPS=200 run minus a REPS=0 run, over 200. CPU time is user + system from
# /usr/bin/time -l; instructions are its counter. BIN names another build of
# the same program (one made in a checkout from before a change) to compare.
#   ./run.sh            every mode
#   ./run.sh 2 4        only modes 2 and 4
cd "$(dirname "$0")" || exit 1
. ../../scripts/env.sh
if [ -z "$BIN" ]; then
  forge build --release > /dev/null 2>&1 || { echo "build failed"; exit 1; }
  BIN=.march/build/release/mesh_translucent
fi
REPS=${REPS:-200}
measure() { # mode reps -> "cpu_seconds instructions"
  MODE=$1 REPS=$2 MARCH_PIN_MAIN=1 /usr/bin/time -l "$BIN" 2>&1 >/dev/null | awk '
    / real / { cpu = $3 + $5 }
    /instructions retired/ { ins = $1 }
    END { printf "%s %s\n", cpu, ins }'
}
[ $# -eq 0 ] && set -- 1 2 3 4 5 6
for m in "$@"; do
  MODE=$m REPS=0 "$BIN"
  set_a=$(measure "$m" "$REPS"); set_b=$(measure "$m" 0)
  echo "$set_a $set_b" | awk -v n="$REPS" '{ printf "  %.1f us and %.2f M instructions a build\n", ($1 - $3) / n * 1e6, ($2 - $4) / n / 1e6 }'
done

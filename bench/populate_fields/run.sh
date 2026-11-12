#!/bin/sh
# CPU cost of one Strata.Populate.fields_with call over 32 x 32 columns
# (4 fields, 3 layers), and of the curve code inside it. For each mode, a
# REPS=n run minus a REPS=0 run (the set-up alone), divided by n. CPU time is
# user + system from /usr/bin/time -l; instructions are its counter.
# Wall-clock time is not used.
#
#   ./run.sh                 build (march-main and forge's default) and measure
#   NOBUILD=1 ./run.sh       measure the binaries already in out/
#   REPS=4000 ./run.sh       more calls (default 2000)
#
# forge resolves the toolchain ~/.march/current names (March 0.5.0 on this
# machine) ahead of scripts/env.sh's march-main; MARCH_HOME pointing at an
# empty directory makes it use the `march` on PATH, march-main.
cd "$(dirname "$0")" || exit 1
. ../../scripts/env.sh
REPS=${REPS:-2000}
if [ -z "$NOBUILD" ]; then
  mkdir -p out/empty_march_home
  forge clean > /dev/null 2>&1
  forge build --release > out/build_default.log 2>&1 || { tail -20 out/build_default.log; exit 1; }
  cp .march/build/release/populate_fields out/populate_fields_default
  forge clean > /dev/null 2>&1
  MARCH_HOME="$PWD/out/empty_march_home" forge build --release > out/build_main.log 2>&1 || { tail -20 out/build_main.log; exit 1; }
  cp .march/build/release/populate_fields out/populate_fields_main
fi
measure() { # binary mode reps -> "cpu_seconds instructions"
  MODE=$2 REPS=$3 /usr/bin/time -l "$1" 2>&1 >/dev/null | awk '
    / real / { cpu = $3 + $5 }
    /instructions retired/ { ins = $1 }
    END { printf "%s %s\n", cpu, ins }'
}
echo "load average: $(uptime | sed 's/.*load averages*: //')"
printf "%-8s %-44s %10s %12s\n" compiler what "cpu us" "instr (k)"
for c in main default; do
  bin=out/populate_fields_$c
  [ -x "$bin" ] || { echo "no $bin"; continue; }
  for m in 1 2 3; do
    case $m in 1) what="fields_with, 32 x 32, 4 fields, 3 layers";; 2) what="5 curve passes over 1024 columns";; 3) what="the same passes, no branch (v * 0.5)";; esac
    a=$(measure "$bin" $m 0); b=$(measure "$bin" $m "$REPS")
    echo "$a $b" | awk -v c="$c" -v w="$what" -v r="$REPS" '{ printf "%-8s %-44s %10.1f %12.1f\n", c, w, ($3 - $1) * 1e6 / r, ($4 - $2) / r / 1e3 }'
  done
done

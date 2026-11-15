#!/bin/sh
# CPU cost of one Strata.Populate.fields_with call over 32 x 32 columns
# (4 fields, 3 layers), of the curve code inside it, and of one
# Strata.Populate.place_with call (no layers; one cover layer at 0.3 per
# square metre on a flat floor; that layer on a chunk no floor reaches; two
# cover layers with weights over three surfaces a column, mode 7; one scatter
# layer of a 9 x 12 x 9 template, 27 stamps meeting the chunk at 1 m, mode
# 8, and the same fitted to 0.25 m, mode 9; mode 10 is mode 8 with every stamp
# below the chunk, so the cells are walked and nothing is blitted; mode 11 is a
# cover layer and a layer of posts for a chunk only the posts reach). Modes 8
# to 11 print how many stamps meet the chunk, the voxels blit visits and how
# many are not holes: run out/populate_fields_main with MODE=8 REPS=0 to see
# it. Mode 12 is Strata.Template.blit alone, the template whole (972 voxels
# visited a call). Modes 13 to 15 are sites: Strata.Populate.adapt_with over
# 32 x 32 columns with two sites in reach, place_with with one site (its
# template whole in the chunk), and adapted_with over mode 13's area. Mode
# 16 is fields_with over a chunk and its pad (44 x 44) with one cover field
# of radius 6 m over a scatter layer of one tree a 4 m cell; mode 17 the
# same population without the cover field. Modes 18 and 19 are place_with at
# level 1 and level 2 with mode 8's scatter layer; mode 20 is mode 18 with a
# density by noise; modes 21 to 23 a 6 m scatter cover read at levels 0, 1
# and 2; mode 24 a cover only a level-0 layer reads, at level 2. For each mode, a
# REPS=n run minus a REPS=0 run (the set-up alone), divided by n. CPU time is
# user + system from /usr/bin/time -l; instructions are its counter.
# Wall-clock time is not used.
#
#   ./run.sh                 build with scripts/env.sh's March and measure
#   NOBUILD=1 ./run.sh       measure the binary already in out/
#   REPS=4000 ./run.sh       more calls (default 2000)
#   MODES="4 5" ./run.sh     only these modes (default 1 to 24)
#
# scripts/env.sh exports MARCH_HOME so forge uses its March (march-main)
# rather than the toolchain ~/.march/current names (March 0.5.0 on this
# machine). The bench no longer builds under 0.5.0 (Authority.must).
cd "$(dirname "$0")" || exit 1
. ../../scripts/env.sh
REPS=${REPS:-2000}
if [ -z "$NOBUILD" ]; then
  mkdir -p out
  forge clean > /dev/null 2>&1
  forge build --release > out/build_main.log 2>&1 || { tail -20 out/build_main.log; exit 1; }
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
echo "march: $(march --version)"
for c in main; do
  bin=out/populate_fields_$c
  [ -x "$bin" ] || { echo "no $bin"; continue; }
  for m in ${MODES:-1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24}; do
    case $m in 1) what="fields_with, 32 x 32, 4 fields, 3 layers";; 2) what="5 curve passes over 1024 columns";; 3) what="the same passes, no branch (v * 0.5)";;
      7) what="place_with, 2 cover layers, weights, surfaces";; 4) what="place_with, no layers";; 5) what="place_with, one cover layer 0.3/m2, flat floor";; 6) what="place_with, that layer, chunk no floor reaches";;
      8) what="place_with, scatter 9x12x9, 27 stamps, 1 m";; 9) what="place_with, the same fitted to 0.25 m";; 10) what="place_with, mode 8, no stamp meets the chunk";;
      11) what="place_with, cover + posts, chunk only posts reach";; 12) what="Strata.Template.blit, 9x12x9 whole";;
      13) what="adapt_with, 32 x 32, two sites in reach";; 14) what="place_with, one site, 9x12x9 whole in chunk";; 15) what="adapted_with, 32 x 32, two sites in reach";;
      16) what="fields_with, 44 x 44, a 6 m cover field";; 17) what="fields_with, 44 x 44, no cover field";;
      18) what="place_with, mode 8 at level 1";; 19) what="place_with, mode 8 at level 2";; 20) what="place_with, level 1, density by noise";;
      21) what="fields + place, 6 m scatter cover, level 0";; 22) what="table_for + place, the same, level 1";; 23) what="table_for + place, the same, level 2";; 24) what="level 2, a cover only a level-0 layer reads";; esac
    a=$(measure "$bin" $m 0); b=$(measure "$bin" $m "$REPS")
    echo "$a $b" | awk -v c="$c" -v w="$what" -v r="$REPS" '{ printf "%-8s %-44s %10.1f %12.1f\n", c, w, ($3 - $1) * 1e6 / r, ($4 - $2) / r / 1e3 }'
  done
done

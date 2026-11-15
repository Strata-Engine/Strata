#!/bin/sh
# Per-stage CPU cost of an island surface chunk: for each stage, a REPS=10 run
# minus a REPS=0 run (the preparation alone), over 2000 chunk-stages. CPU time
# is user + system from /usr/bin/time -l; instructions and cycles are its
# counters. Wall-clock time is not used. Run ./build.sh first, and the viewer
# once so that viewer/.march/atlas holds the island atlas.
#   ./run.sh            every stage
#   ./run.sh 5 11       only stages 5 and 11
cd "$(dirname "$0")/../.." || exit 1
BIN=bench/chunk_stages/out/chunk_stages_main
[ -x "$BIN" ] || { echo "run bench/chunk_stages/build.sh first"; exit 1; }
export STRATA_ATLAS_DIR=viewer/.march/atlas MARCH_PIN_MAIN=1
REPS=${REPS:-10}
name() {
  case $1 in
    1) echo "island_full (whole chunk)";; 2) echo "atlas window (3 layers + river list)";;
    3) echo "columns (cols3_go)";; 4) echo "talus field (C) + talus_go";;
    5) echo "thermal relax";; 6) echo "area_n (2..5 + span/inner/beard)";;
    7) echo "island_shared (column half + population)";; 8) echo "lattice + entrance fields (C)";;
    9) echo "cave factors";; 10) echo "4 lattice fills (C)";;
    11) echo "per-voxel density (dens_go)";; 12) echo "any_cut + drop_islands";;
    13) echo "above_go";; 14) echo "surface scan + rule";;
    15) echo "scan + finish_go (subtract 14)";; 16) echo "Chunk.from_flat";;
    17) echo "pop_ground (fields, instances, tops)";; 18) echo "place_with with floors";; 19) echo "island_shared, no population";;
    25) echo "place_with, no floors (stamps)";;
    20) echo "air early-out";; 21) echo "stone early-out";;
    22) echo "area_n at pad 0 (48 x 48)";; 23) echo "island_full, no trees or ruins";;
    24) echo "any_cut alone";;
    26) echo "columns, no river list";; 27) echo "columns, no lake-rim search";;
    28) echo "columns, neither";;
    33) echo "a column's full chunks, one by one";; 34) echo "a column's full chunks, one call";;
    35) echo "a column's full chunks, two a call";;
  esac
}
measure() { # stage reps -> "cpu_seconds instructions cycles"
  STAGE=$1 REPS=$2 /usr/bin/time -l "$BIN" 2>&1 >/dev/null | awk '
    / real / { cpu = $3 + $5 }
    /instructions retired/ { ins = $1 }
    /cycles elapsed/ { cyc = $1 }
    END { printf "%s %s %s\n", cpu, ins, cyc }'
}
STAGES=${*:-"1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 22 23 24 25 26 27 28 33 34 35 20 21"}
printf "%-5s %-38s %10s %12s %12s\n" stage what "cpu us" "instr (k)" "cycles (k)"
for s in $STAGES; do
  r=$REPS; case $s in 20|21|24) r=$((REPS * 100));; esac  # cheap stages need more runs
  a=$(measure "$s" 0); b=$(measure "$s" "$r")
  echo "$a $b" | awk -v s="$s" -v n="$(name "$s")" -v r="$r" '{
    d = 200 * r
    printf "%-5s %-38s %10.1f %12.1f %12.1f\n", s, n, ($4 - $1) * 1e6 / d, ($5 - $2) / d / 1e3, ($6 - $3) / d / 1e3 }'
done

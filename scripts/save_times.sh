#!/bin/sh
# Build savecheck and run it: Strata.Save timed against the design's five
# budget rows, headless. A row is judged only under a load average below 8
# (the integer part is passed); the script fails only if savecheck itself
# does (a store that will not open, a write that fails, a loaded world that
# is not the saved one).
cd "$(dirname "$0")/.." || exit 1
. scripts/env.sh
OUT="${TMPDIR:-/tmp}/strata-savetimes.$$"; mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT
(cd savecheck && forge build --release > "$OUT/build.log" 2>&1) || { echo "FAIL: savecheck build"; tail -20 "$OUT/build.log"; exit 1; }
SAVECHECK_LOAD=$(uptime | sed 's/.*load average[s]*: *//' | cut -d' ' -f1 | cut -d. -f1 | tr -d ,)
export SAVECHECK_LOAD
SAVECHECK_DIR="$OUT/store" savecheck/.march/build/release/savecheck || { echo "FAIL: savecheck"; exit 1; }

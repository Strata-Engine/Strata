#!/bin/sh
# A long scripted flight with streaming and edits: 3000 frames round a
# circuit, a sphere carved every 100. Asserts nothing was refused, nothing is
# left waiting, the slot table is within bounds and no frame took over
# 33.3 ms. Prints the viewer's summary line.
#   scripts/soak.sh            the default quad buffer
#   scripts/soak.sh 200000     a quad buffer of that many quads, smaller than the
#                              scene needs: uploads are refused and wait, and the
#                              run must still finish cleanly
cd "$(dirname "$0")/.." || exit 1
. scripts/env.sh
(cd viewer && forge build --release > /dev/null 2>&1) || { echo "FAIL: viewer build"; exit 1; }
QUADS="${1:-0}"
line=$(env MARCH_PIN_MAIN=1 STRATA_SOAK=3000 STRATA_QUADS="$QUADS" STRATA_SHOT=/dev/null ./viewer/.march/build/release/strata_viewer 2>&1 | grep '^soak:')
[ -n "$line" ] || { echo "FAIL: the viewer printed no soak line"; exit 1; }
echo "$line"
get() { echo "$line" | sed -n "s/.*$1=\\([0-9.]*\\).*/\\1/p"; }
fail=0
# with the default buffer everything must fit; with a given one the run only has to survive
if [ "$QUADS" = "0" ]; then
  [ "$(get waiting)" = "0" ] || { echo "FAIL: uploads still waiting"; fail=1; }
  [ "$(get refused)" = "0" ] || { echo "FAIL: uploads were refused with the default buffer"; fail=1; }
fi
[ "$(get slots)" -le 16384 ] || { echo "FAIL: slot table overflowed"; fail=1; }
awk "BEGIN { exit !($(get worst_ms) <= 33.3) }" || { echo "FAIL: a frame took over 33.3 ms"; fail=1; }
[ $fail -eq 0 ] && echo "soak: PASS" || exit 1

#!/bin/sh
# A long scripted flight with streaming and edits: 3000 frames round a
# circuit, a sphere carved every 100. Asserts nothing was refused, nothing is
# left waiting, no buffer space was lost, the quad buffer did not grow
# without bound, and no frame took over 20 ms (the target is 16.7; the
# margin is for a busy machine). Prints the viewer's summary line.
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
[ "$(get leaked)" = "0" ] || { echo "FAIL: an allocator lost buffer space"; fail=1; }
# the circuit keeps a steady view, so what the GPU holds must stay near what it started with
[ "$(get live_quads)" -le 1500000 ] || { echo "FAIL: the quad buffer grew without bound"; fail=1; }
awk "BEGIN { exit !($(get worst_ms) <= 20.0) }" || { echo "FAIL: a frame took over 20 ms"; fail=1; }
[ $fail -eq 0 ] && echo "soak: PASS" || exit 1

#!/bin/sh
# A long scripted flight with streaming, edits, moving water and the display on: 3000 frames
# round a lake that is breached at frame 300, a sphere carved every 100. Asserts nothing was refused, nothing is
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
# Offscreen unless told otherwise: with the display asleep or the window hidden,
# MoltenVK holds a presented frame for a whole second, which is no fault of the engine.
out=$(env MARCH_PIN_MAIN=1 STRATA_HIDDEN="${STRATA_HIDDEN:-1}" STRATA_OFFSCREEN="${STRATA_OFFSCREEN:-1}" STRATA_UI=1 STRATA_SOAK=3000 STRATA_BREACH=300 STRATA_QUADS="$QUADS" STRATA_SHOT=/dev/null ./viewer/.march/build/release/strata_viewer 2>&1)
line=$(echo "$out" | grep '^soak:')
lake_start=$(echo "$out" | sed -n 's/^soak-lake: lake_start=\([0-9]*\).*/\1/p')
[ -n "$line" ] || { echo "FAIL: the viewer printed no soak line"; exit 1; }
echo "$line"
# a field of the summary line by its whole name: the space keeps worst_ms from matching fluid_worst_ms
get() { echo "$line" | sed -n "s/.* $1=\\([0-9.]*\\).*/\\1/p"; }
fail=0
# with the default buffer everything must fit; with a given one the run only has to survive
if [ "$QUADS" = "0" ]; then
  [ "$(get waiting)" = "0" ] || { echo "FAIL: uploads still waiting"; fail=1; }
  [ "$(get refused)" = "0" ] || { echo "FAIL: uploads were refused with the default buffer"; fail=1; }
fi
# the display is on, with a readout that changes every frame
[ "$(get ui_dropped)" = "0" ] || { echo "FAIL: the 2D layer dropped quads"; fail=1; }
[ "$(get text_waiting)" = "0" ] || { echo "FAIL: glyphs were still waiting at the end"; fail=1; }
[ "$(get atlas_bytes)" -le 16777216 ] || { echo "FAIL: the glyph atlas is over 16 MB"; fail=1; }
[ "$(get leaked)" = "0" ] || { echo "FAIL: an allocator lost buffer space"; fail=1; }
# the circuit keeps a steady view, so what the GPU holds must stay near what it started with
[ "$(get live_quads)" -le 1500000 ] || { echo "FAIL: the quad buffer grew without bound"; fail=1; }
awk "BEGIN { exit !($(get worst_ms) <= 20.0) }" || { echo "FAIL: a frame took over 20 ms"; fail=1; }
# the lake breached at frame 300 must be draining, and water is never made
echo "lake at the breach: ${lake_start:-?} voxels"
[ -n "$lake_start" ] && [ "$lake_start" -gt 0 ] || { echo "FAIL: no lake to breach"; fail=1; }
[ -n "$lake_start" ] && [ "$(get lake_end)" -lt "$lake_start" ] || { echo "FAIL: the breached lake did not drain"; fail=1; }
[ "$(get fluid_waiting)" = "0" ] || { echo "FAIL: fluid cells were left waiting at the end: the cap was exceeded"; fail=1; }
# offscreen a frame is about 3 ms, so six frames can be less than the worker needs and the main thread waits for it
awk "BEGIN { exit !($(get fluid_worst_ms) <= 6.0) }" || { echo "FAIL: a fluid step held the main thread for over 6 ms"; fail=1; }
[ $fail -eq 0 ] && echo "soak: PASS" || exit 1

#!/bin/sh
# A long scripted flight over the island at 0.25 m a voxel, with streaming,
# blasts and moving water: 6010 frames (1000 fluid steps, one every sixth
# frame) round the test lake, 40 m out and 12 m over its water, a crater
# blasted where the camera looks every 100 frames. Asserts nothing was
# refused, nothing is left waiting, no buffer space was lost, the quad buffer
# did not grow without bound, no frame took over 20 ms (the target is 16.7;
# the margin is for a busy machine), no fluid step held the main thread over
# 6 ms, and no atlas lake loaded at the start and at the end lost more than
# 1% of its water (Strata.Fluid.amount over its box). Prints the viewer's
# summary line and each lake's water.
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
out=$(env MARCH_PIN_MAIN=1 STRATA_OFFSCREEN="${STRATA_OFFSCREEN:-1}" STRATA_SCALE="${STRATA_SCALE:-0.25}" STRATA_SOAK="${STRATA_SOAK:-6010}" STRATA_QUADS="$QUADS" STRATA_SHOT=/dev/null ./viewer/.march/build/release/strata_viewer 2>&1)
line=$(echo "$out" | grep '^soak:')
[ -n "$line" ] || { echo "FAIL: the viewer printed no soak line"; echo "$out" | tail -5; exit 1; }
echo "$line"
# a field of the summary line by its whole name: the space keeps worst_ms from matching fluid_worst_ms
get() { echo "$line" | sed -n "s/.* $1=\\([0-9.]*\\).*/\\1/p"; }
fail=0
# with the default buffer everything must fit; with a given one the run only has to survive
if [ "$QUADS" = "0" ]; then
  [ "$(get waiting)" = "0" ] || { echo "FAIL: uploads still waiting"; fail=1; }
  [ "$(get refused)" = "0" ] || { echo "FAIL: uploads were refused with the default buffer"; fail=1; }
fi
[ "$(get leaked)" = "0" ] || { echo "FAIL: an allocator lost buffer space"; fail=1; }
# the circuit keeps a steady view, so what the GPU holds must stay near what it started with
[ "$(get live_quads)" -le 3000000 ] || { echo "FAIL: the quad buffer grew without bound"; fail=1; }
awk "BEGIN { exit !($(get worst_ms) <= 20.0) }" || { echo "FAIL: a frame took over 20 ms"; fail=1; }
[ "$(get fluid_waiting)" = "0" ] || { echo "FAIL: fluid cells were left waiting at the end: the cap was exceeded"; fail=1; }
# offscreen a frame is about 3 ms, so six frames can be less than the worker needs and the main thread waits for it
awk "BEGIN { exit !($(get fluid_worst_ms) <= 6.0) }" || { echo "FAIL: a fluid step held the main thread for over 6 ms"; fail=1; }
# the lakes at rest: each loaded at both ends keeps 99% of its water
lakes=$(echo "$out" | grep '^soak-lake:')
echo "$lakes"
compared=0
for l in $(echo "$lakes" | tr ' ' ','); do
  s=$(echo "$l" | sed -n 's/.*start=\(-*[0-9]*\).*/\1/p'); e=$(echo "$l" | sed -n 's/.*end=\(-*[0-9]*\).*/\1/p')
  if [ "$s" -gt 0 ] && [ "$e" -ge 0 ]; then
    compared=$((compared + 1))
    [ $((e * 100)) -ge $((s * 99)) ] || { echo "FAIL: a lake lost more than 1% of its water: $l"; fail=1; }
  fi
done
echo "lakes loaded at both ends and compared: $compared"
[ $compared -gt 0 ] || { echo "FAIL: no lake was loaded at both ends"; fail=1; }
[ $fail -eq 0 ] && echo "soak: PASS" || exit 1

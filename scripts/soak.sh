#!/bin/sh
# A long scripted flight over the island at 0.25 m a voxel, with streaming,
# blasts, moving water and the display on: 6010 frames (1000 fluid steps, one
# every sixth frame) round the test lake, 40 m out and 12 m over its water, a
# crater blasted where the camera looks every 100 frames. Asserts nothing was
# refused, nothing is left waiting, no buffer space was lost, the quad buffer
# did not grow without bound, no frame took over 20 ms (the target is 16.7;
# the margin is for a busy machine), no fluid step held the main thread over
# 6 ms, and no atlas lake loaded at the start and at the end lost more than
# 1% of its water (Strata.Fluid.amount over its box). Prints the viewer's
# summary line and each lake's water.
#
# Sound: the flight runs with sound on miniaudio's null device (paced like a
# real one, silent), and asserts the mixer never underran and no command was
# refused. Two shorter runs follow: one with sound off, whose worst frame the
# sound run must match within 0.5 ms (judged only on a quiet machine), and
# one that holds the main thread 100 ms every 600 frames, which the mixer
# must not notice. Nothing is played aloud. STRATA_SOAK_AUDIO=0 skips sound.
#
# The flight runs with the viewer's far field (four levels unless
# STRATA_LEVELS says otherwise) and records it: the chunks loaded at the
# start, level by level, the ticks and time they took to settle, and at the
# end the chunks loaded, slots in use and meshes still retained.
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
AUDIO=null; [ "${STRATA_SOAK_AUDIO:-1}" = "0" ] && AUDIO=0
soak() { # frames, audio mode, stall interval
  env MARCH_PIN_MAIN=1 STRATA_HIDDEN="${STRATA_HIDDEN:-1}" STRATA_OFFSCREEN="${STRATA_OFFSCREEN:-1}" STRATA_UI=1 STRATA_SCALE="${STRATA_SCALE:-0.25}" STRATA_SOAK="$1" STRATA_QUADS="$QUADS" STRATA_SHOT=/dev/null STRATA_AUDIO="$2" STRATA_STALL="$3" ./viewer/.march/build/release/strata_viewer 2>&1
}
out=$(soak "${STRATA_SOAK:-6010}" $AUDIO 0)
line=$(echo "$out" | grep '^soak:')
[ -n "$line" ] || { echo "FAIL: the viewer printed no soak line"; echo "$out" | tail -5; exit 1; }
echo "$line"
# a field of the summary line by its whole name: the space keeps worst_ms from matching fluid_worst_ms
get() { echo "$line" | sed -n "s/.* $1=\\([0-9.]*\\).*/\\1/p"; }
echo "$out" | grep '^strata_viewer: far field' | sed 's/^strata_viewer: /at the start, /'
echo "far field at the end: levels $(get levels), $(get loaded) chunks loaded, $(get slots) slots in use, $(get retained) retained, worst frame $(get worst_ms) ms (load average $(uptime | sed 's/.*load averages*: *\([0-9.]*\).*/\1/'))"
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
# the circuit keeps a steady view, so what the GPU holds must stay near what it started with: 3 million
# quads for the near field alone, Part F's 64 MB (8 388 608 quads) with the far field
qmax=3000000; [ "$(get levels)" != "0" ] && qmax=8388608
[ "$(get live_quads)" -le $qmax ] || { echo "FAIL: the quad buffer grew without bound"; fail=1; }
awk "BEGIN { exit !($(get worst_ms) <= 20.0) }" || { echo "FAIL: a frame took over 20 ms"; fail=1; }
# no figures: the animation is never updated, and the renderer's figure path (its draw list, culling
# and shadow map) costs under 0.1 ms, CPU and shadow pass together, in the worst of each (a regression guard;
# the light pass's share with no figures is not timed here)
[ "$(get fig_updates)" = "0" ] || { echo "FAIL: figures were updated in a run with none"; fail=1; }
echo "figures with none: worst preparation $(get fig_prep_us) us, worst shadow pass $(get fig_shadow_ms) ms"
awk "BEGIN { exit !($(get fig_prep_us) / 1000.0 + $(get fig_shadow_ms) < 0.1) }" || { echo "FAIL: with no figures the figure path took 0.1 ms or more in a frame"; fail=1; }
[ "$(get fluid_waiting)" = "0" ] || { echo "FAIL: fluid cells were left waiting at the end: the cap was exceeded"; fail=1; }
# offscreen a frame is about 3 ms, so six frames can be less than the worker needs and the main thread waits for it
awk "BEGIN { exit !($(get fluid_worst_ms) <= 6.0) }" || { echo "FAIL: a fluid step held the main thread for over 6 ms"; fail=1; }
# sound: the mixer kept up and took every command
if [ "$AUDIO" = "null" ]; then
  [ "$(get audio_underruns)" = "0" ] || { echo "FAIL: the mixer underran"; fail=1; }
  [ "$(get audio_ring_refused)" = "0" ] || { echo "FAIL: sound commands were refused"; fail=1; }
  awk "BEGIN { exit !($(get audio_main_ms) <= 0.2) }" || echo "MISS: sound took $(get audio_main_ms) ms of the main thread in its worst frame (target 0.2)"
  awk "BEGIN { exit !($(get acoustics_ms) <= 2.0) }" || echo "MISS: an acoustics update took $(get acoustics_ms) ms (target 2)"
  # the same flight, shorter, with sound off and then with stalls
  quiet=$(soak 1200 0 0 | grep '^soak:'); loud=$(soak 1200 null 0 | grep '^soak:')
  qw=$(echo "$quiet" | sed -n 's/.* worst_ms=\([0-9.]*\).*/\1/p'); lw=$(echo "$loud" | sed -n 's/.* worst_ms=\([0-9.]*\).*/\1/p')
  load=$(uptime | sed 's/.*load averages*: *\([0-9.]*\).*/\1/')
  echo "worst frame, 1200 frames: sound off $qw ms, sound on $lw ms (load average $load)"
  if awk "BEGIN { exit !($load < 8) }"; then
    awk "BEGIN { exit !($lw - $qw <= 0.5) }" || { echo "FAIL: sound added over 0.5 ms to the worst frame"; fail=1; }
  else echo "not judged: the machine is busy (load average $load); the 0.5 ms comparison needs it under 8"; fi
  stalled=$(soak 1200 null 600 | grep '^soak:')
  su=$(echo "$stalled" | sed -n 's/.* audio_underruns=\([0-9]*\).*/\1/p')
  echo "with a 100 ms stall every 600 frames: underruns $su"
  [ "$su" = "0" ] || { echo "FAIL: a stalled frame made the mixer underrun"; fail=1; }
fi
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

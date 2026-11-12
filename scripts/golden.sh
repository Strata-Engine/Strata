#!/bin/sh
# Render the viewer's fixed scenes headless and compare each with its stored
# image in test/golden/. `scripts/golden.sh --update` rewrites the stored
# images instead; look at them before committing.
#
# Scenes draw offscreen in a window that is never shown, so a run does not
# take over the screen; the image is the one a shown window would get.
#
# A scene is a line "name:environment" below. Every scene renders 60 frames
# at 1280x800 pixels with radius 6 and writes the frame three before the end.
# Every scene but fine-overview (the 10 cm world) is on the island, its atlas cached in .march/atlas, generated
# from viewer/assets/island.gen.
cd "$(dirname "$0")/.." || exit 1
. scripts/env.sh
UPDATE=0; [ "$1" = "--update" ] && UPDATE=1
OUT="${TMPDIR:-/tmp}/strata-golden.$$"; mkdir -p "$OUT" test/golden native/tests/bin
trap 'rm -rf "$OUT"' EXIT

cc -O1 -Wall -Wextra -o native/tests/bin/ppm_diff native/tests/ppm_diff.c || exit 1
cc -O1 -Wall -Wextra -o native/tests/bin/ppm_mean native/tests/ppm_mean.c || exit 1
DIFF=native/tests/bin/ppm_diff

# the comparison tool itself: identical passes, 1% changed fails, sizes must match
{ printf 'P6\n10 10\n255\n'; head -c 300 /dev/zero; } > "$OUT/a.ppm"
{ printf 'P6\n10 10\n255\n'; printf '\377\377\377'; head -c 297 /dev/zero; } > "$OUT/b.ppm"
{ printf 'P6\n5 5\n255\n'; head -c 75 /dev/zero; } > "$OUT/c.ppm"
$DIFF "$OUT/a.ppm" "$OUT/a.ppm" > /dev/null; [ $? -eq 0 ] || { echo "FAIL: ppm_diff rejects identical images"; exit 1; }
$DIFF "$OUT/a.ppm" "$OUT/b.ppm" > /dev/null; [ $? -eq 1 ] || { echo "FAIL: ppm_diff accepts a 1% difference"; exit 1; }
$DIFF "$OUT/a.ppm" "$OUT/c.ppm" > /dev/null 2>&1; [ $? -eq 2 ] || { echo "FAIL: ppm_diff accepts mismatched sizes"; exit 1; }
# a stricter level and share: one pixel in a hundred off by 5 passes by default and fails at level 4, 0.05%
{ printf 'P6\n10 10\n255\n'; printf '\005\000\000'; head -c 297 /dev/zero; } > "$OUT/d.ppm"
$DIFF "$OUT/a.ppm" "$OUT/d.ppm" > /dev/null; [ $? -eq 0 ] || { echo "FAIL: ppm_diff rejects a difference of 5 by default"; exit 1; }
$DIFF "$OUT/a.ppm" "$OUT/d.ppm" 4 0.05 > /dev/null; [ $? -eq 1 ] || { echo "FAIL: ppm_diff accepts a difference of 5 at level 4"; exit 1; }
$DIFF "$OUT/a.ppm" "$OUT/a.ppm" 4 0.05 > /dev/null; [ $? -eq 0 ] || { echo "FAIL: ppm_diff rejects identical images at level 4"; exit 1; }

(cd viewer && forge build --release > "$OUT/build.log" 2>&1) || { echo "FAIL: viewer build"; tail -20 "$OUT/build.log"; exit 1; }

SCENES='
overview:STRATA_SCENE=1
shore:STRATA_SCENE=2
forest:STRATA_SCENE=3
overview-rebuilt:STRATA_SCENE=1 STRATA_REBUILD_AT=30 GOLDEN_AS=overview
dug-out:STRATA_SCENE=4
bad-ids:STRATA_SCENE=5
glass-front:STRATA_SCENE=6
glass-back:STRATA_SCENE=7
underwater:STRATA_SCENE=8
meadow:STRATA_SCENE=9
post:STRATA_SCENE=10
walk:STRATA_SCENE=11 STRATA_FRAMES=200
overview-offscreen:STRATA_SCENE=1 STRATA_OFFSCREEN=1 GOLDEN_AS=overview
volume:STRATA_SCENE=1 STRATA_LIGHT=9
volume-walk:STRATA_SCENE=11 STRATA_FRAMES=200 STRATA_LIGHT=9
sun-high:STRATA_SCENE=1 STRATA_LIGHT=1 STRATA_SUN=55
sun-low:STRATA_SCENE=2 STRATA_LIGHT=1 STRATA_SUN=8
shadow-35:STRATA_SCENE=1 STRATA_LIGHT=2 STRATA_SUN=35
shadow-15:STRATA_SCENE=1 STRATA_LIGHT=2 STRATA_SUN=15
shadow-forest:STRATA_SCENE=3 STRATA_LIGHT=2
shadow-slab:STRATA_SCENE=12 STRATA_LIGHT=2
shadow-placed:STRATA_SCENE=12 STRATA_LIGHT=2 STRATA_SLAB_AT=30 STRATA_FRAMES=37
shadow-removed:STRATA_SCENE=12 STRATA_LIGHT=2 STRATA_SLAB_OFF=60 STRATA_FRAMES=67
shadow-small-box:STRATA_SCENE=1 STRATA_LIGHT=2 STRATA_SUN=35 STRATA_LIGHT_BOX=2
reflect-lake:STRATA_SCENE=2 STRATA_LIGHT=3 STRATA_SUN=12
reflect-glint:STRATA_SCENE=13 STRATA_LIGHT=3 STRATA_SUN=12
reflect-down:STRATA_SCENE=14 STRATA_LIGHT=3 STRATA_SUN=35
reflect-high:STRATA_SCENE=15 STRATA_LIGHT=3 STRATA_SUN=30
full-forest:STRATA_SCENE=3 STRATA_LIGHT=4
full-cave:STRATA_SCENE=16 STRATA_LIGHT=4
full-lantern:STRATA_SCENE=17 STRATA_LIGHT=4 STRATA_SUN=-5
full-overview:STRATA_SCENE=1 STRATA_LIGHT=4 STRATA_SUN=35
lake-breached:STRATA_SCENE=18 STRATA_LIGHT=4 STRATA_BREACH=30 STRATA_FRAMES=63 DRAWS=6
lake-draining:STRATA_SCENE=18 STRATA_LIGHT=4 STRATA_BREACH=30 STRATA_FRAMES=363 DRAWS=6
lake-later:STRATA_SCENE=18 STRATA_LIGHT=4 STRATA_BREACH=30 STRATA_FRAMES=1203
valley-later:STRATA_SCENE=19 STRATA_LIGHT=4 STRATA_BREACH=30 STRATA_FRAMES=1203
water-levels:STRATA_SCENE=20 STRATA_LIGHT=4 STRATA_FLUID=0
splash-early:STRATA_SCENE=21 STRATA_LIGHT=4 STRATA_SPLASH=30 STRATA_FRAMES=57 STRATA_FLUID_HZ=10 DRAWS=6
splash-late:STRATA_SCENE=21 STRATA_LIGHT=4 STRATA_SPLASH=30 STRATA_FRAMES=69 STRATA_FLUID_HZ=10 DRAWS=6
island-mouth:STRATA_SCENE=22 STRATA_LIGHT=4
island-lake:STRATA_SCENE=23 STRATA_LIGHT=4
crate-drop-air:STRATA_SCENE=25 STRATA_CRATE=10 STRATA_FRAMES=43
crate-drop-rest:STRATA_SCENE=25 STRATA_CRATE=10 STRATA_FRAMES=123
tilted-slab:STRATA_SCENE=26 STRATA_LIGHT=1 STRATA_SUN=12 STRATA_FRAMES=123
fine-overview:STRATA_SCENE=27 STRATA_SCALE=0.1 STRATA_FRAMES=63
tilted-slab-lit:STRATA_SCENE=26 STRATA_LIGHT=4 STRATA_SUN=12 STRATA_FRAMES=123
body-by-lake:STRATA_SCENE=28 STRATA_LIGHT=3 STRATA_SUN=12 STRATA_FRAMES=90
water-reflects:STRATA_SCENE=126 STRATA_LIGHT=4 STRATA_FLUID=0 STRATA_SUN=30
splash-shaded:STRATA_SCENE=127 STRATA_LIGHT=4 STRATA_SPLASH=30 STRATA_FRAMES=57 STRATA_FLUID_HZ=10 DRAWS=6
lake-low-sun:STRATA_SCENE=18 STRATA_LIGHT=4 STRATA_BREACH=30 STRATA_FRAMES=1203 STRATA_SUN=25
under-sea:STRATA_SCENE=8 STRATA_LIGHT=4
under-sea-up:STRATA_SCENE=124 STRATA_LIGHT=4
under-lake:STRATA_SCENE=125 STRATA_LIGHT=4 STRATA_FLUID=0
ui-scale-1:STRATA_SCENE=1 STRATA_LIGHT=4 STRATA_UI=1 DRAWS=6 STRICT=1
ui-scale-15:STRATA_SCENE=1 STRATA_LIGHT=4 STRATA_UI=1 STRATA_UI_SCALE=150 DRAWS=6 STRICT=1
ui-scale-2:STRATA_SCENE=1 STRATA_LIGHT=4 STRATA_UI=1 STRATA_UI_SCALE=200 DRAWS=6 STRICT=1
ui-flood:STRATA_SCENE=1 STRATA_UI=1 STRATA_UI_SHOW=1 DRAWS=6 SAYS=ui:_dropped_3616_quads
ui-frames:STRATA_SCENE=1 STRATA_UI=1 STRATA_UI_SHOW=2 DRAWS=6 STRICT=1
text-sheet:STRATA_SCENE=1 STRATA_UI=1 STRATA_UI_SHOW=3 DRAWS=6 STRICT=1
text-styles:STRATA_SCENE=1 STRATA_LIGHT=4 STRATA_UI=1 STRATA_UI_SHOW=4 DRAWS=6 STRICT=1
text-scripts:STRATA_SCENE=1 STRATA_UI=1 STRATA_UI_SHOW=7 DRAWS=6 STRICT=1
humanoid-poses:STRATA_SCENE=31 STRATA_LIGHT=4 STRATA_SUN=12 STRATA_FIG_SHADOWS=0 DRAWS=20
figure-walk-lit-2s:STRATA_SCENE=32 STRATA_LIGHT=4 STRATA_FRAMES=123 STRATA_FIG_SHADOWS=0 DRAWS=20
figure-walk-lit-3s:STRATA_SCENE=32 STRATA_LIGHT=4 STRATA_FRAMES=183 STRATA_FIG_SHADOWS=0 DRAWS=20
figure-walk-lit-4s:STRATA_SCENE=32 STRATA_LIGHT=4 STRATA_FRAMES=243 STRATA_FIG_SHADOWS=0 DRAWS=20
figure-shadow:STRATA_SCENE=33 STRATA_LIGHT=4 STRATA_SUN=18 DRAWS=35
figure-shadow-walk-2s:STRATA_SCENE=33 STRATA_LIGHT=4 STRATA_SUN=18 STRATA_FRAMES=123 DRAWS=35
figure-shadow-walk-4s:STRATA_SCENE=33 STRATA_LIGHT=4 STRATA_SUN=18 STRATA_FRAMES=243 DRAWS=35
traced-shadow:STRATA_SCENE=34 STRATA_LIGHT=4 STRATA_SUN=18 DRAWS=35
traced-shadow-walk-2s:STRATA_SCENE=34 STRATA_LIGHT=4 STRATA_SUN=18 STRATA_FRAMES=123 DRAWS=35
traced-shadow-walk-4s:STRATA_SCENE=34 STRATA_LIGHT=4 STRATA_SUN=18 STRATA_FRAMES=243 DRAWS=35
figure-reflection:STRATA_SCENE=35 STRATA_LIGHT=3 STRATA_SUN=12 STRATA_FRAMES=90 DRAWS=20
glowing-figure:STRATA_SCENE=36 STRATA_LIGHT=4 STRATA_SUN=2 STRATA_FRAMES=123 DRAWS=21
glowing-figure-4s:STRATA_SCENE=36 STRATA_LIGHT=4 STRATA_SUN=2 STRATA_FRAMES=243 DRAWS=21
island-cliff:STRATA_SCENE=24 STRATA_LIGHT=4
island-cave:STRATA_SCENE=29 STRATA_LIGHT=4
forest-border:STRATA_SCENE=128 STRATA_LIGHT=4
ruin-slope:STRATA_SCENE=129 STRATA_LIGHT=4
crater-ruin:STRATA_SCENE=134 STRATA_LIGHT=4 STRATA_FRAMES=120
overview-regenerated:STRATA_SCENE=1 STRATA_REGEN=1 GOLDEN_AS=overview
'
# indirect draw calls a frame must issue, whatever the scene holds; a scene
# with a kind of geometry nowhere in it says DRAWS=n (a figure scene adds one
# draw per figure piece shown: the humanoid's 15 parts are one piece each, and
# as many again for the plain figures' shadow map in light modes 2 to 8; scenes
# 31 and 32 were stored before figures cast shadows and keep them off with
# STRATA_FIG_SHADOWS=0, scene 33 shows them; a traced figure is left out of
# the map: scene 34 maps its one plain runner, 35 and 36 map none, 36 adds the
# torch's one piece; 36 walks its torch-bearer in, shot at 2 s and 4 s). STRICT=1 holds a scene
# to a difference of 4 in 0.05% of pixels: small text is few pixels. Line 9's note stands:
# a new scene's first picture is left as test/golden/name.actual.ppm to be
# looked at and stored by hand (gzip -9 -c it to test/golden/name.ppm.gz)
EXPECT_DRAWS=5
FAILED=0
for line in $(echo "$SCENES" | tr ' ' ','); do
  name="${line%%:*}"; envs="$(echo "${line#*:}" | tr ',' ' ')"
  stored="$name"
  draws=$EXPECT_DRAWS
  says=""; strict=""
  for kv in $envs; do case "$kv" in GOLDEN_AS=*) stored="${kv#GOLDEN_AS=}";; DRAWS=*) draws="${kv#DRAWS=}";; SAYS=*) says="$(echo "${kv#SAYS=}" | tr '_' ' ')";; STRICT=1) strict="4 0.05";; esac; done
  shot="$OUT/$name.ppm"
  if ! env MARCH_PIN_MAIN=1 STRATA_RADIUS=6 STRATA_HIDDEN=1 STRATA_OFFSCREEN=1 STRATA_PIXELS=1 STRATA_SIZE=1280x800 STRATA_FRAMES=60 STRATA_LIGHT=0 STRATA_SHOT="$shot" $envs \
       ./viewer/.march/build/release/strata_viewer > "$OUT/$name.log" 2>&1 || [ ! -s "$shot" ]; then
    echo "FAIL: $name did not render"; tail -5 "$OUT/$name.log"; FAILED=1; continue
  fi
  if ! grep -q "draw calls $draws\$" "$OUT/$name.log"; then
    echo "FAIL: $name did not issue $draws draw calls: $(grep 'done after' "$OUT/$name.log")"; FAILED=1; continue
  fi
  # a scene that must print a line (SAYS=the_line, underscores for spaces)
  if [ -n "$says" ] && ! grep -q "^$says\$" "$OUT/$name.log"; then echo "FAIL: $name did not say '$says'"; FAILED=1; continue; fi
  # a scene that asks for a swapchain rebuild must have had one
  case "$envs" in *STRATA_REBUILD_AT*)
    if grep -q "rebuilds 0;" "$OUT/$name.log"; then echo "FAIL: $name asked for a swapchain rebuild and none happened"; FAILED=1; continue; fi;;
  esac
  if [ $UPDATE -eq 1 ]; then
    [ "$stored" = "$name" ] && gzip -9 -c "$shot" > "test/golden/$name.ppm.gz" && echo "updated $name"
    continue
  fi
  if [ ! -f "test/golden/$stored.ppm.gz" ]; then echo "FAIL: $name has no stored image (look at test/golden/$name.actual.ppm, then store it)"; cp "$shot" "test/golden/$name.actual.ppm"; FAILED=1; continue; fi
  gunzip -c "test/golden/$stored.ppm.gz" > "$OUT/want.ppm"
  if res=$($DIFF "$OUT/want.ppm" "$shot" $strict 2>&1); then echo "ok   $name: $res"
  else echo "FAIL $name: $res"; cp "$shot" "test/golden/$name.actual.ppm"; FAILED=1; fi
done
# Glow through a figure's grid: at 2 s in glowing-figure, sky rays from the
# tunnel's right wall reach the torch's flame across the empty voxels round its
# handle (a 4 x 15 x 4 grid, the handle 2 x 10 x 2), so the wall beside it is
# lit. Measured 26.6 with the hit distance right; 17.6 when a figure hit's
# distance was scaled wrong (Task 8 review C1), which the image comparison's
# default tolerance lets through. The floor is between.
if [ $UPDATE -eq 0 ] && [ -s "$OUT/glowing-figure.ppm" ]; then
  if res=$(native/tests/bin/ppm_mean "$OUT/glowing-figure.ppm" 775 380 810 470 22 2>&1); then echo "ok   torch glow on the wall: $res"
  else echo "FAIL torch glow on the wall (glowing-figure): $res"; FAILED=1; fi
fi
# Stillness: with nothing moving, the lit picture must have stopped changing.
# Frames 118 and 119 of the overview under shadows (the forest camera stands in
# dappled leaves on this land and never settles to 0.05%), from two runs of the
# same deterministic sequence.
if [ $UPDATE -eq 0 ]; then
  for mode in 2 4; do
    for n in 121 122; do
      env MARCH_PIN_MAIN=1 STRATA_RADIUS=6 STRATA_HIDDEN=1 STRATA_OFFSCREEN=1 STRATA_PIXELS=1 STRATA_SIZE=1280x800 STRATA_SCENE=1 STRATA_LIGHT=$mode STRATA_FRAMES=$n STRATA_SHOT="$OUT/still$n.ppm" ./viewer/.march/build/release/strata_viewer > /dev/null 2>&1
    done
    if res=$($DIFF "$OUT/still121.ppm" "$OUT/still122.ppm" 4 0.05 2>&1); then echo "ok   stillness, light mode $mode: $res"
    else echo "FAIL stillness, light mode $mode (frames 118 and 119): $res"; FAILED=1; fi
  done
fi
# A nearly full quad buffer: a chunk emptied ahead (scene 4, at frame 10)
# makes its neighbours' new meshes larger, and they are refused while the old
# ones are still held, and must land on a later frame once the emptied chunk's
# mesh is freed. The capacity is the scene's own need plus 40 000 quads (on
# the island at seed 7: 35 000 to 45 000 give refusals that all land; a
# blast's remesh alone is one or two chunks that only grow, so it either fits
# or never does), so the check follows the terrain.
if [ $UPDATE -eq 0 ]; then
  RUN="env MARCH_PIN_MAIN=1 STRATA_LIGHT=0 STRATA_RADIUS=6 STRATA_HIDDEN=1 STRATA_OFFSCREEN=1 STRATA_PIXELS=1 STRATA_SIZE=1280x800 STRATA_SCENE=4 STRATA_FRAMES=60 STRATA_SHOT=/dev/null ./viewer/.march/build/release/strata_viewer"
  need=$($RUN 2>&1 | sed -n 's/.*; \([0-9]*\) quads,.*/\1/p' | head -1)
  line=$(STRATA_QUADS=$((need + 40000)) $RUN 2>&1 | grep 'done after')
  refused=$(echo "$line" | sed -n 's/.*uploads refused \([0-9]*\),.*/\1/p')
  if echo "$line" | grep -q 'still waiting 0;' && [ "${refused:-0}" -gt 0 ]; then echo "ok   retry: $refused uploads refused, none left waiting"
  else echo "FAIL retry (capacity $((need + 40000))): $line"; FAILED=1; fi
fi
[ $FAILED -eq 0 ] && echo "golden: PASS" || { echo "golden: FAIL (actual images left in test/golden/*.actual.ppm)"; exit 1; }

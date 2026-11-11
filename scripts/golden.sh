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
cd "$(dirname "$0")/.." || exit 1
. scripts/env.sh
UPDATE=0; [ "$1" = "--update" ] && UPDATE=1
OUT="${TMPDIR:-/tmp}/strata-golden.$$"; mkdir -p "$OUT" test/golden native/tests/bin
trap 'rm -rf "$OUT"' EXIT

cc -O1 -Wall -Wextra -o native/tests/bin/ppm_diff native/tests/ppm_diff.c || exit 1
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
lake-later:STRATA_SCENE=18 STRATA_LIGHT=4 STRATA_BREACH=30 STRATA_FRAMES=1203 DRAWS=6
valley-later:STRATA_SCENE=19 STRATA_LIGHT=4 STRATA_BREACH=30 STRATA_FRAMES=1203 DRAWS=6
water-levels:STRATA_SCENE=20 STRATA_LIGHT=4 STRATA_FLUID=0
splash-early:STRATA_SCENE=21 STRATA_LIGHT=4 STRATA_SPLASH=30 STRATA_FRAMES=57 DRAWS=6
splash-late:STRATA_SCENE=21 STRATA_LIGHT=4 STRATA_SPLASH=30 STRATA_FRAMES=69 DRAWS=6
'
# indirect draw calls a frame must issue, whatever the scene holds; a scene
# with a kind of geometry nowhere in it says DRAWS=n
EXPECT_DRAWS=5
FAILED=0
for line in $(echo "$SCENES" | tr ' ' ','); do
  name="${line%%:*}"; envs="$(echo "${line#*:}" | tr ',' ' ')"
  stored="$name"
  draws=$EXPECT_DRAWS
  for kv in $envs; do case "$kv" in GOLDEN_AS=*) stored="${kv#GOLDEN_AS=}";; DRAWS=*) draws="${kv#DRAWS=}";; esac; done
  shot="$OUT/$name.ppm"
  if ! env MARCH_PIN_MAIN=1 STRATA_RADIUS=6 STRATA_HIDDEN=1 STRATA_OFFSCREEN=1 STRATA_PIXELS=1 STRATA_SIZE=1280x800 STRATA_FRAMES=60 STRATA_LIGHT=0 STRATA_SHOT="$shot" $envs \
       ./viewer/.march/build/release/strata_viewer > "$OUT/$name.log" 2>&1 || [ ! -s "$shot" ]; then
    echo "FAIL: $name did not render"; tail -5 "$OUT/$name.log"; FAILED=1; continue
  fi
  if ! grep -q "draw calls $draws\$" "$OUT/$name.log"; then
    echo "FAIL: $name did not issue $draws draw calls: $(grep 'done after' "$OUT/$name.log")"; FAILED=1; continue
  fi
  # a scene that asks for a swapchain rebuild must have had one
  case "$envs" in *STRATA_REBUILD_AT*)
    if grep -q "rebuilds 0;" "$OUT/$name.log"; then echo "FAIL: $name asked for a swapchain rebuild and none happened"; FAILED=1; continue; fi;;
  esac
  if [ $UPDATE -eq 1 ]; then
    [ "$stored" = "$name" ] && gzip -9 -c "$shot" > "test/golden/$name.ppm.gz" && echo "updated $name"
    continue
  fi
  if [ ! -f "test/golden/$stored.ppm.gz" ]; then echo "FAIL: $name has no stored image (run scripts/golden.sh --update)"; FAILED=1; continue; fi
  gunzip -c "test/golden/$stored.ppm.gz" > "$OUT/want.ppm"
  if res=$($DIFF "$OUT/want.ppm" "$shot" 2>&1); then echo "ok   $name: $res"
  else echo "FAIL $name: $res"; cp "$shot" "test/golden/$name.actual.ppm"; FAILED=1; fi
done
# Stillness: with nothing moving, the lit picture must have stopped changing.
# Frames 118 and 119 of the forest under shadows, from two runs of the same
# deterministic sequence.
if [ $UPDATE -eq 0 ]; then
  for mode in 2 4; do
    for n in 121 122; do
      env MARCH_PIN_MAIN=1 STRATA_RADIUS=6 STRATA_HIDDEN=1 STRATA_OFFSCREEN=1 STRATA_PIXELS=1 STRATA_SIZE=1280x800 STRATA_SCENE=3 STRATA_LIGHT=$mode STRATA_FRAMES=$n STRATA_SHOT="$OUT/still$n.ppm" ./viewer/.march/build/release/strata_viewer > /dev/null 2>&1
    done
    if res=$($DIFF "$OUT/still121.ppm" "$OUT/still122.ppm" 4 0.05 2>&1); then echo "ok   stillness, light mode $mode: $res"
    else echo "FAIL stillness, light mode $mode (frames 118 and 119): $res"; FAILED=1; fi
  done
fi
# A nearly full quad buffer: a carve's new meshes are refused while the old
# ones are still held, and must land on a later frame. The capacity is the
# scene's own need plus a little, so the check follows the terrain.
if [ $UPDATE -eq 0 ]; then
  RUN="env MARCH_PIN_MAIN=1 STRATA_LIGHT=0 STRATA_RADIUS=6 STRATA_HIDDEN=1 STRATA_OFFSCREEN=1 STRATA_PIXELS=1 STRATA_SIZE=1280x800 STRATA_SCENE=1 STRATA_DIG=1 STRATA_FRAMES=60 STRATA_SHOT=/dev/null ./viewer/.march/build/release/strata_viewer"
  need=$($RUN 2>&1 | sed -n 's/.*; \([0-9]*\) quads,.*/\1/p' | head -1)
  line=$(STRATA_QUADS=$((need + 2100)) $RUN 2>&1 | grep 'done after')
  refused=$(echo "$line" | sed -n 's/.*uploads refused \([0-9]*\),.*/\1/p')
  if echo "$line" | grep -q 'still waiting 0;' && [ "${refused:-0}" -gt 0 ]; then echo "ok   retry: $refused uploads refused, none left waiting"
  else echo "FAIL retry (capacity $((need + 2100))): $line"; FAILED=1; fi
fi
[ $FAILED -eq 0 ] && echo "golden: PASS" || { echo "golden: FAIL (actual images left in test/golden/*.actual.ppm)"; exit 1; }

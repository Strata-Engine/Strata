#!/bin/sh
# Render the viewer's fixed scenes headless and compare each with its stored
# image in test/golden/. `scripts/golden.sh --update` rewrites the stored
# images instead; look at them before committing.
#
# A scene is a line "name:environment" below. Every scene renders 60 frames
# at 640x400 points with radius 6 and writes the frame three before the end.
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

(cd viewer && forge build --release > "$OUT/build.log" 2>&1) || { echo "FAIL: viewer build"; tail -20 "$OUT/build.log"; exit 1; }

SCENES='
overview:STRATA_SCENE=1
shore:STRATA_SCENE=2
forest:STRATA_SCENE=3
overview-rebuilt:STRATA_SCENE=1 STRATA_REBUILD_AT=30 GOLDEN_AS=overview
'
FAILED=0
for line in $(echo "$SCENES" | tr ' ' ','); do
  name="${line%%:*}"; envs="$(echo "${line#*:}" | tr ',' ' ')"
  stored="$name"
  for kv in $envs; do case "$kv" in GOLDEN_AS=*) stored="${kv#GOLDEN_AS=}";; esac; done
  shot="$OUT/$name.ppm"
  if ! env MARCH_PIN_MAIN=1 STRATA_RADIUS=6 STRATA_SIZE=640x400 STRATA_FRAMES=60 STRATA_SHOT="$shot" $envs \
       ./viewer/.march/build/release/strata_viewer > "$OUT/$name.log" 2>&1 || [ ! -s "$shot" ]; then
    echo "FAIL: $name did not render"; tail -5 "$OUT/$name.log"; FAILED=1; continue
  fi
  if [ $UPDATE -eq 1 ]; then
    [ "$stored" = "$name" ] && gzip -9 -c "$shot" > "test/golden/$name.ppm.gz" && echo "updated $name"
    continue
  fi
  if [ ! -f "test/golden/$stored.ppm.gz" ]; then echo "FAIL: $name has no stored image (run scripts/golden.sh --update)"; FAILED=1; continue; fi
  gunzip -c "test/golden/$stored.ppm.gz" > "$OUT/want.ppm"
  if res=$($DIFF "$OUT/want.ppm" "$shot" 2>&1); then echo "ok   $name: $res"
  else echo "FAIL $name: $res"; cp "$shot" "test/golden/$name.actual.ppm"; FAILED=1; fi
done
[ $FAILED -eq 0 ] && echo "golden: PASS" || { echo "golden: FAIL (actual images left in test/golden/*.actual.ppm)"; exit 1; }

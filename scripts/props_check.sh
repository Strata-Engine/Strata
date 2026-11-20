#!/bin/sh
# Props through a save (needs a GPU): the viewer with STRATA_PROPS=1 and a
# store cuts scene 130's tree and quits; a second run of the same store,
# which cuts nothing, must find the tree cut: the shapes' memory is a section
# of the region's file (docs/2026-11-19-shapes-design.md, section 9).
cd "$(dirname "$0")/.." || exit 1
. scripts/env.sh
OUT="${TMPDIR:-/tmp}/strata-props.$$"; mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT
(cd viewer && forge build --release > "$OUT/build.log" 2>&1) || { echo "FAIL: viewer build"; tail -20 "$OUT/build.log"; exit 1; }
run() { # log, extra environment...
  log="$1"; shift
  (cd viewer && env MARCH_PIN_MAIN=1 STRATA_HIDDEN=1 STRATA_OFFSCREEN=1 STRATA_PIXELS=1 STRATA_AUDIO=0 STRATA_SIZE=640x400 STRATA_RADIUS=4 STRATA_LEVELS=0 \
     STRATA_PROPS=1 STRATA_SCENE=130 STRATA_FRAMES=100 STRATA_SHOT="$OUT/shot.ppm" STRATA_STORE="$OUT/store" "$@" .march/build/release/strata_viewer > "$log" 2>&1) || { echo "FAIL: viewer run"; tail -20 "$log"; exit 1; }
}
run "$OUT/one.log" STRATA_CUT_AT=60 STRATA_CUT_DROP=110 STRATA_CUT_SIDE=50
run "$OUT/two.log"
grep '^shapes:' "$OUT/one.log" "$OUT/two.log" | sed "s|$OUT/||g"
grep -q '^shapes: .* 1 remembered' "$OUT/one.log" || { echo "FAIL: the first run cut no prop"; exit 1; }
grep -q '^shapes: .* 1 remembered' "$OUT/two.log" || { echo "FAIL: the second run did not find the cut prop in the store"; exit 1; }
s1=$(sed -n 's/^shapes: \([0-9]*\) standing.*/\1/p' "$OUT/one.log")
s2=$(sed -n 's/^shapes: \([0-9]*\) standing.*/\1/p' "$OUT/two.log")
[ -n "$s1" ] && [ "$s1" = "$s2" ] || { echo "FAIL: $s1 shapes stood at the first run's end and $s2 at the second's"; exit 1; }
grep -q 'writes failed 0, files set aside 0' "$OUT/two.log" || { echo "FAIL: a write failed or a file was set aside"; exit 1; }
echo "props_check: PASS"

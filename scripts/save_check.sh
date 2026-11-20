#!/bin/sh
# The viewer's store: a hidden scripted run carves a crater (STRATA_DIG=1)
# with STRATA_STORE set and quits; a second run with the same folder and no
# carve must load the same world (the world hash printed at quit) and put
# the walker back where it stood. Needs a GPU.
cd "$(dirname "$0")/.." || exit 1
. scripts/env.sh
OUT="${TMPDIR:-/tmp}/strata-save.$$"; mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT
(cd viewer && forge build --release > "$OUT/build.log" 2>&1) || { echo "FAIL: viewer build"; tail -20 "$OUT/build.log"; exit 1; }
run() { # log, extra environment...
  log="$1"; shift
  (cd viewer && env MARCH_PIN_MAIN=1 STRATA_HIDDEN=1 STRATA_OFFSCREEN=1 STRATA_PIXELS=1 STRATA_AUDIO=0 STRATA_SIZE=640x400 STRATA_RADIUS=4 STRATA_LEVELS=0 \
     STRATA_FRAMES=200 STRATA_SHOT="$OUT/shot.ppm" STRATA_STORE="$OUT/store" "$@" .march/build/release/strata_viewer > "$log" 2>&1) || { echo "FAIL: viewer run"; tail -20 "$log"; exit 1; }
}
# the walker looks down (STRATA_PITCH, hundredths of a radian), so the blast at frame 10 lands on the ground in front of it
run "$OUT/one.log" STRATA_DIG=1 STRATA_PITCH=-100
run "$OUT/two.log"
h1=$(sed -n 's/^save: world hash \([0-9-]*\);.*/\1/p' "$OUT/one.log")
h2=$(sed -n 's/^save: world hash \([0-9-]*\);.*/\1/p' "$OUT/two.log")
grep '^save:' "$OUT/one.log" "$OUT/two.log" | sed "s|$OUT/||g"
grep -q '^save: world hash .*; 0 edited chunks' "$OUT/one.log" && { echo "FAIL: the first run carved nothing (0 edited chunks at quit)"; exit 1; }
[ -n "$h1" ] || { echo "FAIL: the first run printed no world hash"; exit 1; }
[ "$h1" = "$h2" ] || { echo "FAIL: the loaded world's hash $h2 is not the saved world's $h1"; exit 1; }
grep -q '^save: store .* open; 0 edited chunks' "$OUT/two.log" && { echo "FAIL: the second run found no edited chunk"; exit 1; }
grep -q '^save: the walker is back' "$OUT/two.log" || { echo "FAIL: the walker's place was not restored"; exit 1; }
grep -q 'writes failed 0, files set aside 0' "$OUT/two.log" || { echo "FAIL: a write failed or a file was set aside"; exit 1; }
[ -f "$OUT/store/store.lock" ] && { echo "FAIL: the lock was left"; exit 1; }
echo "save_check: PASS"

#!/bin/sh
# Build animcheck and run every scene: figures built through Strata.Rig and
# Strata.Clip and posed by the C kernel through Strata.Anim, headless.
# Exits non-zero on the first scene that fails.
cd "$(dirname "$0")/.." || exit 1
. scripts/env.sh
native/stamp_anim.sh --check || exit 1
OUT="${TMPDIR:-/tmp}/strata-anim.$$"; mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT
(cd animcheck && forge build --release > "$OUT/build.log" 2>&1) || { echo "FAIL: animcheck build"; tail -20 "$OUT/build.log"; exit 1; }
AC=animcheck/.march/build/release/animcheck
for s in chain determinism marks mask empty table stairs hits; do
  if ! ANIMCHECK_SCENE=$s $AC > "$OUT/$s.log" 2>&1; then echo "FAIL: animcheck scene $s"; cat "$OUT/$s.log"; exit 1; fi
  tail -1 "$OUT/$s.log"
done
echo "animcheck: every scene ok"

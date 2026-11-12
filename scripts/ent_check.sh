#!/bin/sh
# Build entcheck and run every scene: entities added, read and written by the
# C kernel through Strata.Entities, headless.
# Exits non-zero on the first scene that fails.
cd "$(dirname "$0")/.." || exit 1
. scripts/env.sh
native/stamp_ent.sh --check || exit 1
OUT="${TMPDIR:-/tmp}/strata-ent.$$"; mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT
(cd entcheck && forge build --release > "$OUT/build.log" 2>&1) || { echo "FAIL: entcheck build"; tail -20 "$OUT/build.log"; exit 1; }
EC=entcheck/.march/build/release/entcheck
for s in basics columns determinism closed ray bullets storm sweep walk; do
  if ! ENTCHECK_SCENE=$s $EC > "$OUT/$s.log" 2>&1; then echo "FAIL: entcheck scene $s"; cat "$OUT/$s.log"; exit 1; fi
  tail -1 "$OUT/$s.log"
done
echo "entcheck: every scene ok"

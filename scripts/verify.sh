#!/bin/sh
# The gate: check, release build, strict lint, tests -- in the library, then
# in each consumer app that exists. Prints PASS, or the first failing command.
cd "$(dirname "$0")/.." || exit 1
. scripts/env.sh
run() { # dir, command...
  d="$1"; shift
  if ! (cd "$d" && "$@") > .verify.log 2>&1; then
    echo "FAIL: ($d) $*"; tail -40 .verify.log; exit 1
  fi
}
for d in . demo viewer soundcheck animcheck; do
  [ -f "$d/forge.toml" ] || continue
  run "$d" forge check
  run "$d" forge build --release
  run "$d" forge lint --strict
  if [ -d "$d/test" ]; then run "$d" forge test; fi
done
# C tests, the audio goldens, then the golden images; all need a compiler, the last a GPU
mkdir -p native/tests/bin
for t in native/tests/*_test.c; do
  [ -f "$t" ] || continue
  b="native/tests/bin/$(basename "$t" .c)"
  if ! cc -O1 -Wall -Wextra -Inative -o "$b" "$t" > .verify.log 2>&1 || ! "$b" >> .verify.log 2>&1; then
    echo "FAIL: $t"; tail -20 .verify.log; exit 1
  fi
done
# the animation scenes: figures posed by the C kernel through Strata.Anim, headless
if ! scripts/anim_check.sh > .verify.log 2>&1; then echo "FAIL: scripts/anim_check.sh"; tail -20 .verify.log; exit 1; fi
# the audio goldens: rendered in game time, no device opened, nothing played aloud
if ! scripts/audio_golden.sh > .verify.log 2>&1; then echo "FAIL: scripts/audio_golden.sh"; tail -20 .verify.log; exit 1; fi
if [ "${STRATA_NO_GPU:-0}" != "1" ]; then
  if ! scripts/golden.sh > .verify.log 2>&1; then echo "FAIL: scripts/golden.sh"; tail -20 .verify.log; exit 1; fi
fi
tail -3 .verify.log
echo PASS

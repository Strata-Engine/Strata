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
for d in . demo viewer soundcheck animcheck entcheck savecheck plugins/strata_tactics plugins/strata_tactics/example plugins/strata_kit plugins/strata_kit/example; do
  [ -f "$d/forge.toml" ] || continue
  run "$d" forge check
  run "$d" forge build --release
  run "$d" forge lint --strict
  if [ -d "$d/test" ]; then run "$d" forge test; fi
done
# C tests, the audio goldens, then the golden images; all need a compiler, the last a GPU
mkdir -p native/tests/bin
# (*_test.cpp: tests of native/strata_vox.cpp, C++ because ogt_vox.h is;
# -fno-exceptions keeps them free of the C++ runtime, and -ffp-contract=off keeps
# floating point unfused, as in the forge builds, so the noise test runs the kernel forge builds)
for t in native/tests/*_test.c native/tests/*_test.cpp; do
  [ -f "$t" ] || continue
  b="native/tests/bin/$(basename "${t%.*}")"
  # the text test links the text module and what it stands on
  extra=""; [ "$(basename "$t")" = "text_test.c" ] && extra="native/strata_text.c -Inative/sheenbidi/Headers -I/opt/homebrew/include/freetype2 -I/opt/homebrew/include/harfbuzz -L/opt/homebrew/lib -lfreetype -lharfbuzz"
  if ! cc -O1 -Wall -Wextra -ffp-contract=off -fno-exceptions -Inative -o "$b" "$t" $extra > .verify.log 2>&1 || ! "$b" >> .verify.log 2>&1; then
    echo "FAIL: $t"; tail -20 .verify.log; exit 1
  fi
done
# the animation scenes: figures posed by the C kernel through Strata.Anim, headless
if ! scripts/anim_check.sh > .verify.log 2>&1; then echo "FAIL: scripts/anim_check.sh"; tail -20 .verify.log; exit 1; fi
# the entity scenes: entities added, read and written by the C kernel through Strata.Entities, headless
if ! scripts/ent_check.sh > .verify.log 2>&1; then echo "FAIL: scripts/ent_check.sh"; tail -20 .verify.log; exit 1; fi
# the store timed against its budget, headless: fails only if a store will not open, a write fails or a loaded world is not the saved one
if ! scripts/save_times.sh > .verify.log 2>&1; then echo "FAIL: scripts/save_times.sh"; tail -20 .verify.log; exit 1; fi
# the viewer's store: carve with STRATA_STORE set, quit, and a second run must load the same world (needs a GPU)
if [ "${STRATA_NO_GPU:-0}" != "1" ]; then
  if ! scripts/save_check.sh > .verify.log 2>&1; then echo "FAIL: scripts/save_check.sh"; tail -20 .verify.log; exit 1; fi
fi
# props through a save: the viewer cuts a standing tree with STRATA_PROPS=1 and a store, quits, and a second run must find it cut (needs a GPU)
if [ "${STRATA_NO_GPU:-0}" != "1" ]; then
  if ! scripts/props_check.sh > .verify.log 2>&1; then echo "FAIL: scripts/props_check.sh"; tail -20 .verify.log; exit 1; fi
fi
# the match: hidden scripted runs of the viewer with STRATA_COMBAT=1 (needs a GPU)
if [ "${STRATA_NO_GPU:-0}" != "1" ]; then
  if ! scripts/combat_check.sh > .verify.log 2>&1; then echo "FAIL: scripts/combat_check.sh"; tail -20 .verify.log; exit 1; fi
fi
# the devtools relay: an MCP client drives a hidden viewer through every tool (needs a GPU)
if [ "${STRATA_NO_GPU:-0}" != "1" ]; then
  if ! scripts/devtools_check.sh > .verify.log 2>&1; then echo "FAIL: scripts/devtools_check.sh"; tail -20 .verify.log; exit 1; fi
fi
# the audio goldens: rendered in game time, no device opened, nothing played aloud
if ! scripts/audio_golden.sh > .verify.log 2>&1; then echo "FAIL: scripts/audio_golden.sh"; tail -20 .verify.log; exit 1; fi
if [ "${STRATA_NO_GPU:-0}" != "1" ]; then
  if ! scripts/golden.sh > .verify.log 2>&1; then echo "FAIL: scripts/golden.sh"; tail -20 .verify.log; exit 1; fi
fi
tail -3 .verify.log
echo PASS

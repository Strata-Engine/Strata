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
for d in . demo viewer; do
  [ -f "$d/forge.toml" ] || continue
  run "$d" forge check
  run "$d" forge build --release
  run "$d" forge lint --strict
  if [ -d "$d/test" ]; then run "$d" forge test; fi
done
# C tests, then the golden images; both need a compiler, the second a GPU
mkdir -p native/tests/bin
# (*_test.cpp: tests of native/strata_vox.cpp, C++ because ogt_vox.h is;
# -fno-exceptions keeps them free of the C++ runtime, as in the forge builds)
for t in native/tests/*_test.c native/tests/*_test.cpp; do
  [ -f "$t" ] || continue
  b="native/tests/bin/$(basename "${t%.*}")"
  if ! cc -O1 -Wall -Wextra -fno-exceptions -Inative -o "$b" "$t" > .verify.log 2>&1 || ! "$b" >> .verify.log 2>&1; then
    echo "FAIL: $t"; tail -20 .verify.log; exit 1
  fi
done
if [ "${STRATA_NO_GPU:-0}" != "1" ]; then
  if ! scripts/golden.sh > .verify.log 2>&1; then echo "FAIL: scripts/golden.sh"; tail -20 .verify.log; exit 1; fi
fi
tail -3 .verify.log
echo PASS

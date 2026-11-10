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
tail -3 .verify.log
echo PASS

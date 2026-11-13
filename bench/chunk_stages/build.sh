#!/bin/sh
# Build the stage harness. The viewer's generator is private to its module,
# so this copies viewer/lib into lib/ with every `pfn` made `fn` and the
# viewer's `main` renamed; nothing in viewer/ is touched. Run from anywhere.
# The compiler is the one scripts/env.sh names (STRATA_MARCH_HOME).
cd "$(dirname "$0")" || exit 1
. ../../scripts/env.sh
for f in ../../viewer/lib/*.march; do
  sed -e 's/^\( *\)pfn /\1fn /' -e 's/^  fn main(/  fn viewer_main(/' "$f" > "lib/$(basename "$f")"
done
mkdir -p out
forge clean > /dev/null 2>&1
forge build --release > out/build.log 2>&1 || { grep -a -B2 -A12 "ERROR\|^error" out/build.log | head -60; exit 1; }
cp .march/build/release/chunk_stages out/chunk_stages_main
echo "built out/chunk_stages_main ($(march --version) at $(git -C "$STRATA_MARCH_HOME" rev-parse --short HEAD))"

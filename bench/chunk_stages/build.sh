#!/bin/sh
# Build the stage harness, twice. The viewer's generator is private to its
# module, so this copies viewer/lib into lib/ with every `pfn` made `fn` and
# the viewer's `main` renamed; nothing in viewer/ is touched. Run from anywhere.
#
#   out/chunk_stages_default   built as `forge build --release` builds Strata
#                              today. forge puts the toolchain that
#                              ~/.march/current names first on PATH (March
#                              0.5.0 on this machine), ahead of scripts/env.sh's
#                              march-main, so this is NOT march-main's compiler.
#   out/chunk_stages_main      built with MARCH_HOME pointing at an empty
#                              directory, so forge resolves no toolchain and
#                              runs the `march` on PATH: march-main (0.7.0).
cd "$(dirname "$0")" || exit 1
. ../../scripts/env.sh
for f in strata_viewer params trees textures walker; do
  sed -e 's/^\( *\)pfn /\1fn /' -e 's/^  fn main(/  fn viewer_main(/' "../../viewer/lib/$f.march" > "lib/$f.march"
done
mkdir -p out/empty_march_home
forge clean > /dev/null 2>&1
forge build --release > out/build_default.log 2>&1 || { tail -20 out/build_default.log; exit 1; }
cp .march/build/release/chunk_stages out/chunk_stages_default
forge clean > /dev/null 2>&1
MARCH_HOME="$PWD/out/empty_march_home" forge build --release > out/build_main.log 2>&1 || { tail -20 out/build_main.log; exit 1; }
cp .march/build/release/chunk_stages out/chunk_stages_main
echo "built out/chunk_stages_default ($(~/.march/current/bin/march --version 2>/dev/null || echo 'no ~/.march/current: same as main')) and out/chunk_stages_main ($(march --version))"

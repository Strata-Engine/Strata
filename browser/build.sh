#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/env.sh
out="$PWD/browser/.build"
wasm_clang="${STRATA_WASM_CLANG:-/opt/homebrew/opt/llvm/bin/clang}"
bridge="$PWD/browser/native/bridge.c"
noise="$PWD/native/strata_noise.c"

# Limit island compilation to the shared modules this fixture uses.
export MARCH_LIB_PATH="$out/lib"
mkdir -p "$MARCH_LIB_PATH/strata/ffi"
for module in blocks chunk coord ffi/noise mesh occupancy quad size store tree; do
    ln -sfn "$PWD/lib/strata/$module.march" "$MARCH_LIB_PATH/strata/$module.march"
done
cp browser/core/strata_browser.march "$out/core.march"

march --compile -o "$out/native" --ffi-c "$bridge" --ffi-c "$noise" \
    --ffi-link -ffp-contract=off --ffi-link -fwrapv "$out/core.march"
march --emit-llvm --target wasm32-unknown-unknown "$out/core.march"
node browser/native/prepare-ir.mjs "$out/core.ll" browser/native/fixture-abi.ll.in "$out/link.ll"
cat "$STRATA_MARCH_HOME/runtime/march_runtime_wasm.c" browser/native/scoped-heap.c > "$out/runtime.c"
"$wasm_clang" --target=wasm32-unknown-unknown -ffreestanding -nostdlib \
    -O2 -ffp-contract=off -fwrapv -Wl,--no-entry -Wl,--export-memory -Wl,--fatal-warnings \
    -Ibrowser/native/freestanding "$out/link.ll" "$bridge" browser/native/arrays.c browser/native/flight.c "$noise" \
    "$out/runtime.c" -o "$out/core.wasm"

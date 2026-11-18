#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
source scripts/env.sh
ur_root="${STRATA_UR_HOME:-$HOME/code/ur}"
clang="${STRATA_WASM_CLANG:-/opt/homebrew/opt/llvm/bin/clang}"
out="$PWD/browser/.build/ur"
if [[ "${1:-}" == --relink ]]; then
    test -s "$out/link.ll"
else
    export MARCH_LIB_PATH="$out/lib"
    mkdir -p "$MARCH_LIB_PATH/ur" "$MARCH_LIB_PATH/strata/ffi"
    for module in sim demography architecture landscape citizens utilities fleet food freight_network utilities_network; do
        ln -sfn "$ur_root/lib/ur/$module.march" "$MARCH_LIB_PATH/ur/$module.march"
    done
    for module in seed noise quad ffi/noise; do
        ln -sfn "$PWD/lib/strata/$module.march" "$MARCH_LIB_PATH/strata/$module.march"
    done
    node browser/ur/extract-scene.mjs "$ur_root/lib/ur/scene.march" "$MARCH_LIB_PATH/ur/browser_scene.march"
    ln -sfn "$PWD/browser/ur/fixture.march" "$MARCH_LIB_PATH/ur/browser_fixture.march"
    cp browser/ur/ur_browser.march "$out/core.march"
    march --emit-llvm --target wasm32-unknown-unknown "$out/core.march"
    cat browser/native/fixture-abi.ll.in browser/ur/abi.ll.in > "$out/abi.ll"
    node browser/native/prepare-ir.mjs "$out/core.ll" "$out/abi.ll" "$out/link.ll" \
        ur_publish ur_crossing_built ur_save_motion ur_scene_mesh ur_scene_texture ur_scene_begin ur_scene_part \
        ur_scene_emission ur_scene_voxel ur_scene_pose ur_scene_fog ur_scene_sun ur_scene_lamp \
        ur_scene_static_lamp ur_scene_footprint ur_scene_blocked ur_scene_changed_lot \
        ur_scene_volume_begin ur_scene_volume_box ur_scene_deck sn_sample2
fi
"$clang" --target=wasm32-unknown-unknown -ffreestanding -nostdlib \
    -DMARCH_WASM_ARRAY_RC -DMARCH_WASM_EXTERNAL_ALLOC -O2 -ffp-contract=off -fwrapv -Wl,--no-entry -Wl,--export-memory \
    -Wl,--fatal-warnings -Wl,--max-memory=268435456 -Ibrowser/native/freestanding \
    "$out/link.ll" browser/ur/bridge.c browser/ur/heap.c browser/native/arrays.c native/strata_noise.c \
    "$STRATA_MARCH_HOME/runtime/march_runtime_wasm.c" -o "$out/core.wasm"

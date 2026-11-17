# Strata WASM parity fixture

From the repository root:

```sh
browser/build.sh
node browser/test/core.mjs browser/.build/core.wasm browser/.build/native
```

The build uses the March checkout selected by `scripts/env.sh`. The fixture
was verified with March 0.7.0 at revision
`f04f0dc0c6735362a05ce21331d333baa5182b9c`, Apple Clang 17.0.0 for the
native build, Homebrew LLVM 22.1.8 and wasm-ld 22.1.8 for wasm32, and Node
26.7.0. Set `STRATA_MARCH_HOME` for a different checkout location and
`STRATA_WASM_CLANG` for a different LLVM install. The WASM clang needs
`wasm-ld` on its search path.

The same March entry uses Strata's existing FastNoiseLite binding to sample
terrain at negative and positive coordinates, builds a 32³ chunk, and calls
the shared greedy mesher. The fixture seed has both words set. FastNoiseLite
uses its low 32 bits by design. Both builds disable floating-point contraction
and enable wrapping signed arithmetic for the C noise implementation.

The test checks the module has no imports, compares three noise results
exactly, and compares all 30,816 packed mesh bytes in order. It reads the same
mesh again after growing WASM memory to verify that callers can acquire a
fresh view. Expected output:

```text
Native/WASM parity: 3 noise samples, 3852 ordered quads, memory growth
```

JavaScript calls `init(seedLo, seedHi)` once, then reads `mesh_ptr()` and
`mesh_bytes()` from the instance's exported memory. `noise_sample(index)`
exposes the three fixture samples. The bridge copies the borrowed March
array into a stable buffer before returning to the host.

The browser compiler currently omits several native array declarations,
emits an island entry call with the wrong signature for a capability-bearing
`main`, and zero-extends tagged negative values on wasm32. The build uses
explicit LLVM linking, a fixture-specific IR adapter, and freestanding
implementations of the array operations and runtime symbols reached by the
fixture. The sign fix covers values in the signed 31-bit immediate range.
The seed stays on the typed i64 path. General March programs can require
larger tagged values and additional runtime support.

The browser keeps the editable cells, camera matrix and published mesh in
static buffers. Each remesh uses the March allocator as scratch space and
rewinds it after copying out the mesh. The generated code has literal
caching disabled so the runtime does not retain a pointer into that
scratch space. This scope applies to the fixture's mesher call graph.

The test runs 10,000 frame steps with 100 alternating edits, compares the
restored mesh with native, and checks both consumed scratch bytes and
linear memory after warm-up. It then clears the fixture one voxel at a
time and checks that the last edit publishes zero mesh bytes and a new
revision.

To see the same fixture drawn by WebGPU, run:

```sh
python3 -m http.server 8765 --bind 127.0.0.1 --directory browser
```

Open `http://127.0.0.1:8765/preview.html` in a WebGPU-capable browser.
The preview opens in an isometric view. Select **Fly through terrain** to
capture the mouse. Fly with WASD and Space or Left Shift, look with the
mouse, and remove or place a block with the left or right button. Escape
releases the mouse. The browser steps the core at 60 Hz and uploads mesh
data when its revision changes.

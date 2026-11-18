# Strata Browser Target Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Run a small, editable Strata voxel scene in a browser with WebGPU rendering, first-person controls, and event-driven audio.

**Architecture:** JavaScript owns the browser lifecycle and WebGPU device. March owns simulation and mesh generation inside a WASM module. C/C++ helpers link into that module when their dependencies and ABI are proven.

**Tech Stack:** March `wasm32-unknown-unknown`, LLVM/Clang and wasm-ld, JavaScript modules, WGSL, Web Audio.

**Spec:** This plan follows the browser investigation and uses the existing [voxel core](../../2026-11-10-strata-design.md), [renderer](../../2026-11-10-renderer-design.md), and [audio](../../2026-11-11-audio-design.md) designs as references for shared engine behavior.

## Global constraints

- Begin with a single-threaded browser build and a deterministic scene spanning one 32³ chunk. Scope parity work separately.
- Reuse the existing 8-byte quad encoding, mesh generation and raycast. Keep browser build inputs separate from the desktop viewer's Vulkan/GLFW link configuration.
- Use desktop Chromium for the first acceptance run, served over localhost or HTTPS. Require a usable WebGPU adapter at runtime and report initialization failures in the page.
- Pin the March revision and LLVM version when the first link passes. Start with the checkout selected by `scripts/env.sh`.
- Preserve native integer and floating point behavior across the helper boundary. Keep 64-bit values inside WASM or marshal them explicitly with BigInt or split words.

## Review focus

- Linear memory growth invalidates cached JavaScript views. Reacquire the view after every WASM call that can allocate.
- An unchanged frame should neither remesh the chunk nor upload it again. Editing the final solid voxel must remove the old mesh from the draw list.
- A hidden tab or lost Pointer Lock must clear input and discard elapsed simulation time.
- Unavailable or lost WebGPU devices must produce a visible status and stop rendering with invalid resources.
- An audio context that is suspended or unavailable must leave the scene playable.

### Task 1 — Prove a real March and C build

**Files:** Create `browser/build.sh`, `browser/core/strata_browser.march`, `browser/native/bridge.c` and `browser/test/core.mjs`. Reuse `native/strata_noise.c`, `lib/strata/ffi/noise.march` and `lib/strata/mesh.march`. Inspect March's WASM link branch in `bin/main.ml` and its runtime in `runtime/march_runtime_wasm.c`.

**Interface:** Export WASM memory and a scalar ABI. Use `init(seedLo: u32, seedHi: u32) -> i32` (zero means success), `step(dtMs: f64, held: u32, pressed: u32, dx: f64, dy: f64) -> u32` (event bits), `mesh_ptr() -> u32`, `mesh_bytes() -> u32`, `mesh_revision() -> u32`, and `camera_ptr() -> u32` (16 column-major f32 values in WebGPU clip space, valid after successful init and each step). Pointers are WASM byte offsets. The host borrows data until the next mutating export.

- [ ] Add a native fixture that produces the ordered encoded quad bytes for a fixed seed and checks the same FastNoiseLite sample inputs as WASM. Include negative coordinates and a seed with the high word set.
- [ ] Build the browser module from generated March output, the March WASM runtime and the C helper with one consistent target. Check the compiler's export, import and FFI conventions with the fixture. If compiler support is needed for linking helper objects or exporting the ABI, make that a scoped March change before proceeding. Evaluate an Emscripten build only if required helper dependencies make the freestanding path impractical.
- [ ] Start with a fixed camera and make `init` produce the chunk and mesh. In `node browser/test/core.mjs`, instantiate the module, assert no unexpected platform imports and compare the packed mesh bytes and helper outputs with the native fixture. Exercise the host's read path after memory growth.
- [ ] Record the exact compiler versions, build command and any March prerequisites. Commit the verified build and fixture.

### Task 2 — Prove a bounded frame step

**Files:** Modify `browser/core/strata_browser.march`, `browser/native/bridge.c` and `browser/test/core.mjs`. Consult `viewer/lib/strata_viewer.march`, `lib/strata/query.march` and `lib/strata/mesh.march`.

- [ ] Define input bits in ABI order: forward, left, back, right, rise, descend, remove, place. The movement keys use the held mask, the edit buttons use the pressed mask, and mouse deltas are relative pixels consumed once per input sample. Event bit 0 signals a successful edit. Implement fly camera movement and raycast-based editing with those inputs.
- [ ] Increment the mesh revision only after a successful edit changes the published mesh. Assert idle frames keep the revision stable and removing the final solid voxel publishes a new revision with zero mesh bytes.
- [ ] Instrument allocation or linear-memory high water in the test build. Run 10,000 steps with movement and recurring alternating edits after warm-up. Require the high water to plateau under the repeated workload. Inspect consumed heap bytes as well as memory pages so reserved capacity cannot hide a leak.
- [ ] If the bump runtime keeps growing, use fixed storage for persistent state and a resettable scratch arena only after proving that no references escape a step, or implement reclaiming allocation in March. Treat this as a gate for continuous play. Run the fixture again and record the result before committing.

### Task 3 — Draw and control the scene

**Files:** Create `browser/index.html`, `browser/app.mjs`, `browser/input.mjs`, `browser/render.mjs` and `browser/shaders/opaque.wgsl`. Use `native/shaders/quad.vert` and `native/strata_vk.c` as references for packed-quad decoding.

**Interfaces:** `createRenderer(canvas) -> Promise<{draw(frame), dispose()}>`, with `frame = {camera, meshRevision, quads}`. `createInput(canvas) -> {sample(), clear(), dispose()}`, with `sample() -> {held, pressed, dx, dy}`.

- [ ] Draw an opaque depth-tested scene. Decode each 8-byte quad as two little-endian 32-bit words and generate its vertices in WGSL. Verify an asymmetric fixture with different block colors and face directions. Upload only on revision changes and draw zero instances on an empty mesh. Resize the depth target with the canvas backing size.
- [ ] Bind WASD, Space and Left Shift to the movement bits. Map left and right mouse buttons to removal and placement and use Pointer Lock for mouse look. Capture keyboard state only while playing. Retain input edges and mouse deltas until a simulation step consumes them.
- [ ] Schedule one render per `requestAnimationFrame` callback, with simulation steps of `1000 / 60` ms and at most four ticks per callback. Drop excess accumulated time. Pass edges and mouse deltas only on the first tick that consumes that sample. Reset timing and input on tab hide, blur and Pointer Lock loss.
- [ ] In Chromium, verify camera movement, edits and resize. Hide and restore the tab while moving and confirm no stuck key or catch-up burst. Test unavailable adapter and forced device loss and verify the page shows a failure state. Record the smoke run and commit.

### Task 4 — Add browser audio and document the run

**Files:** Create `browser/audio.mjs` and `browser/README.md`. Modify `browser/app.mjs`. Use `native/strata_audio.c` and `native/strata_mix.h` as references for later mixer parity.

**Interface:** `createAudio() -> {resume(), playEvents(bits), dispose()}`. Call `resume()` directly from the start gesture and `playEvents(bits)` once per simulation step that emits events.

- [ ] Play a short Web Audio sound for event bit 0. Keep a suspended or unavailable audio context non-blocking and allow a later user gesture to retry resume.
- [ ] Document the pinned toolchain, build command, and local serving command (`python3 -m http.server 8000 --bind 127.0.0.1 --directory browser`). Verify the scene from a clean build at localhost, including one sound per successful edit and play with audio disabled.
- [ ] Run `source scripts/env.sh && forge test` for the shared native code, plus the WASM fixture and browser smoke checks. Record any pre-existing failures separately and commit.

### Follow-on work after the slice passes

1. Add a small multi-chunk scene using bounded generation and meshing jobs. Profile frame cost first. Consider workers only when the measurements justify them, and finish asset preloading and storage before porting the full island.
2. Port render passes individually, starting with material textures and lighting, then water and UI. Evaluate culling and indirect submission against WebGPU adapter limits and feature flags. Use fixed camera scenes for comparison with Vulkan.
3. Audit animation and entity helpers for clock and allocation dependencies. Prove the VOX loader's allocation and jump behavior in the selected toolchain. Build FreeType and HarfBuzz for the same target and feed font bytes from the browser host. Use existing native fixtures as parity references.
4. For audio parity, isolate the mixer from pthread loading, device callbacks and WAV capture. Run it in an AudioWorklet with a bounded command queue and predecoded samples. Measure underruns and test suspension and resume. If using shared memory, test deployment with cross-origin isolation.

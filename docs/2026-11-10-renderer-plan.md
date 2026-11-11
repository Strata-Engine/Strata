# Strata Renderer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn Strata's early rendering slice into the renderer the engine keeps: GPU-driven culling and indirect draws, game-supplied textures, cross and model blocks on screen, slot bookkeeping in the library, a walkable viewer, and measured frame costs. This is Parts A to E of the design; the far field (Part F) is a second plan.

**Architecture:** The C shim (`native/strata_vk.c`) keeps a GPU chunk table; a compute shader culls it and writes indirect draw commands; one multi-draw call per range type per pass draws them. A pure March module assigns renderer slots and a thin one drives the bindings. Correctness is held by golden images rendered headless and compared within a tolerance, plus a C test for the allocator and March tests for everything pure.

**Tech Stack:** March `origin/main` (via `scripts/env.sh`), C11 with Vulkan 1.2 on MoltenVK, GLFW, GLSL compiled by `glslangValidator` into `native/strata_shaders.h`. Load the `march-lang`, `march-test` and `build-test` skills before writing March. Read `GAPS.md` first: it lists reserved words and other pitfalls already hit.

**Spec:** `docs/2026-11-10-renderer-design.md`. Parent: `docs/2026-11-10-strata-design.md`.

## Global Constraints

- Work in `~/code/strata` on a new branch `renderer` cut from `voxel-core`. cube_forge and `~/code/march` are not touched.
- Baseline is Vulkan 1.2 plus `multiDrawIndirect` and `shaderDrawParameters`. `drawIndirectCount` must not be required. Nothing may depend on KosmicKrisp.
- No Vulkan type crosses into March: native arrays, Ints and Floats only. Every extern is wrapped in an ordinary `fn`. A call made only for its effect must have its result used (GAPS S9); call Unit functions by full module path (GAPS S10).
- `scripts/verify.sh` prints PASS at the end of every task. From Task 1 on it also runs the golden images and the C tests.
- No frame may wait on the GPU because a chunk was uploaded or freed.
- No per-frame CPU work proportional to the number of loaded chunks, in the shim or in March.
- Lighting stays sun, corner shading and fog. No deferred pass, shadows or emitted light.
- The repository ships no image files other than screenshots under `docs/` and golden images under `test/golden/`.
- Do not change March or forge; record new gaps in `GAPS.md`.
- Commit after every task.

## Review Focus

Conditions the spec implies but does not spell out. Each has a check in the task named.

1. **The window is resized or minimised while two frames are in flight.** The swapchain rebuilds with no hang, no stale framebuffer and the same picture afterwards. Task 2.
2. **The quad buffer fills.** An upload is refused, reported, and succeeds on retry after frees; the old mesh stays on screen meanwhile; nothing is corrupted. Tasks 2 and 4.
3. **A chunk's new mesh is empty where its old one was not** (the last block was dug out). The old quads must disappear. Tasks 3 and 4.
4. **A quad or instance names a texture layer or block id that does not exist.** It draws with layer 0 or not at all; it never reads out of bounds. Tasks 5 and 7.
5. **More chunks than slots.** The planner refuses the excess, keeps them queued, and places them when slots free up. Task 4.

---

## File Structure

```
native/strata_vk.c            the shim (grows; split only if a task says so)
native/strata_alloc.h         range allocator with deferred frees, header-only
native/shaders/cull.comp      chunk culling
native/shaders/quad.vert|frag packed quads
native/shaders/cross.vert     cross instances (shares quad.frag)
native/shaders/loose.vert     loose quads for models (shares quad.frag)
native/tests/alloc_test.c     allocator tests
native/tests/ppm_diff.c       image comparison
scripts/golden.sh             render scenes, compare or --update
scripts/soak.sh               long scripted flight with assertions
test/golden/*.ppm.gz          stored images
lib/strata/slots.march        Strata.Slots: pure slot planning
lib/strata/render.march       Strata.Render: drives the bindings
lib/strata/model.march        Strata.Model: model geometry builder
lib/strata/ffi/vk.march       bindings (grows)
viewer/lib/strata_viewer.march  the viewer
viewer/lib/textures.march     StrataViewer.Textures: procedural tiles
viewer/lib/walker.march       StrataViewer.Walker: walking physics, pure
viewer/test/walker_test.march
```

---

### Task 1: Golden images and frame timing

Nothing about the picture changes. This task builds the safety net the rest relies on.

**Files:** Create `native/tests/ppm_diff.c`, `scripts/golden.sh`, `test/golden/`; modify `native/strata_vk.c`, `lib/strata/ffi/vk.march`, `viewer/lib/strata_viewer.march`, `scripts/verify.sh`, `README.md`

**Interfaces — Produces:**
```
C      void    sv_set_vsync(int64_t on)        -- 0 selects the immediate present mode when available
C      double  sv_stat(int64_t which)          -- of the last frame: 0 ms inside sv_frame before present,
                                               --   1 ms in acquire + present, 2 draw calls issued, 3 GPU ms or -1.0
March  Strata.Ffi.Vk.set_vsync(yes : Bool) : Unit
March  Strata.Ffi.Vk.stat(which : Int) : Float
env    STRATA_SCENE=n     viewer: fixed camera pose n (1..), no input, deterministic
env    STRATA_VSYNC=0     viewer: immediate present
env    STRATA_SIZE=WxH    viewer: window size (default 1280x800)
tool   native/tests/ppm_diff A.ppm B.ppm   -- exit 0 when at most 0.5% of pixels differ by more than 8 in any channel;
                                           --   prints the fraction and the largest difference; exit 2 on size mismatch
script scripts/golden.sh [--update]        -- renders every scene at 640x400 for 60 frames, compares with
                                           --   test/golden/<scene>.ppm.gz; --update rewrites them
```

- [ ] **Step 1:** Write `ppm_diff.c` and a shell check in `golden.sh` that it exits 0 on identical files, 1 on a file with 1% of pixels inverted, 2 on mismatched sizes. Run; expect the check to fail (no tool yet), then implement and pass.
- [ ] **Step 2:** Add `sv_set_vsync` (rebuild the swapchain with `VK_PRESENT_MODE_IMMEDIATE_KHR` if the surface lists it, else keep FIFO) and `sv_stat`, with bindings. `stat(3)` comes from a pair of timestamp queries around the frame's commands when the queue family's `timestampValidBits` is non-zero, and is -1.0 otherwise.
- [ ] **Step 3:** Viewer: `STRATA_SCENE` poses 1 "overview" (the current opening view), 2 "shore" (low over water looking along the shoreline), 3 "forest floor" (among trees, looking level). In a scene the world settles fully before frame 0. Print one line every 240 frames: fps, `stat(0)`, `stat(1)`, `stat(2)`.
- [ ] **Step 4:** `golden.sh` builds the viewer in release, renders each scene, converts nothing (PPM in, PPM compared), stores gzip. Run `scripts/golden.sh --update`, look at each image (`sips -s format png`), then run `scripts/golden.sh` twice. Expected: PASS both times, so rendering is deterministic.
- [ ] **Step 5:** `verify.sh` compiles and runs everything under `native/tests/` and runs `golden.sh`, unless `STRATA_NO_GPU=1`.
- [ ] **Step 6:** Record in `RESULTS.md` the baseline: scene 1 at 2560x1600 with `STRATA_VSYNC=0`, fps and the three stats. Gate green. Commit.

### Task 2: Two frames in flight, deferred frees, no stalls

**Files:** Create `native/strata_alloc.h`, `native/tests/alloc_test.c`; modify `native/strata_vk.c`, `lib/strata/ffi/vk.march`, `viewer/lib/strata_viewer.march`

**Interfaces — Produces:**
```
C  typedef struct { uint32_t first, count; } SaRange;
C  void sa_init(SaAlloc *a, uint32_t total);
C  int  sa_alloc(SaAlloc *a, uint32_t n, uint32_t *first);        -- first fit; 0 when it does not fit
C  void sa_release(SaAlloc *a, SaRange r, uint64_t frame);       -- held, not yet reusable
C  void sa_collect(SaAlloc *a, uint64_t frame);                  -- returns to the free list everything released at frame - 2 or earlier
C  uint32_t sa_live(const SaAlloc *a);                           -- elements allocated and not released
C  int64_t sv_open(int64_t w, int64_t h, int64_t capture_mouse, int64_t quad_capacity);   -- capacity in quads; <= 0 means 8388608
March Strata.Ffi.Vk.open(w : Int, h : Int, capture_mouse : Bool, quad_capacity : Int) : Bool
env   STRATA_QUADS=n          viewer: quad capacity
env   STRATA_REBUILD_AT=f     viewer: force a swapchain rebuild at frame f (scripted runs)
```

- [ ] **Step 1: failing C tests** in `alloc_test.c`: allocate to full, the next alloc returns 0; a released range is not reusable at the same frame or the next, and is at frame + 2; adjacent released ranges merge after collect (allocate three, release all, collect, one alloc of the total succeeds); releasing a zero-count range is a no-op; `sa_live` tracks allocs minus releases; 10 000 random alloc/release/collect steps keep the invariant "live + free + held == total" and never hand out overlapping ranges.
- [ ] **Step 2:** Run; expect a compile failure. Implement `strata_alloc.h`. Pass.
- [ ] **Step 3:** In the shim: replace the inline allocator with `strata_alloc.h`; two frames in flight, each with its own command buffer, fence and acquire semaphore; `sa_collect` at frame start; remove every `vkWaitForFences` from `sv_chunk_upload` and `sv_chunk_free`. Reset a frame's fence only after its submit succeeded. Size the swapchain arrays from the actual image count. If a swapchain rebuild fails, leave the renderer in a state where `sv_frame` returns 0 and retries, with nothing half-built.
- [ ] **Step 4:** Golden images still pass unchanged. Add a golden run with `STRATA_REBUILD_AT=30` against the same stored images (Review Focus 1).
- [ ] **Step 5:** With `STRATA_QUADS=20000`, scene 1 must run to completion without a crash and print the count of refused uploads, which is greater than 0 (Review Focus 2; the retry arrives in Task 4).
- [ ] **Step 6:** Gate green. Commit.

### Task 3: The chunk table, compute culling, indirect draws

**Files:** Create `native/shaders/cull.comp`; modify `native/shaders/quad.vert`, `native/build_shaders.sh`, `native/strata_vk.c`, `lib/strata/ffi/vk.march`, `RESULTS.md`

**Interfaces — Produces:** the public upload and free calls are unchanged. New:
```
C     void sv_pad_commands(int64_t n)     -- debugging: append n zero-count commands to each indirect call
March Strata.Ffi.Vk.pad_commands(n : Int) : Unit
env   STRATA_PAD_CMDS=n                   viewer
```
`sv_stat(2)` now reports indirect calls issued; the expected value this task is 3 (opaque, cutout, translucent).

- [ ] **Step 1: prove the path in isolation.** In the shim, behind an environment switch, draw scene 1's opaque layer from a command buffer written by a compute shader with one `vkCmdDrawIndirect`. Expected: the opaque parts of the scene-1 golden image match. If compute or multi-draw fails on this driver, stop and report; the design's first risk has landed.
- [ ] **Step 2: measure.** With `STRATA_VSYNC=0`, record scene 1's `stat(0)` and fps at `STRATA_PAD_CMDS` 0, 10 000 and 50 000. Write the three rows to `RESULTS.md`.
- [ ] **Step 3: choose and record.** If 50 000 padded commands add less than 0.5 ms to the frame, build the fixed list. Otherwise build the compacted list with a late count, exactly as the design's Part A describes it (submit last frame's count plus a quarter plus 64; the shader zero-fills to that length). Write the choice and the numbers into `RESULTS.md` and the top of `cull.comp`.
- [ ] **Step 4: build it.** The chunk table (origin, level, drawable flag, a first and count per range; ranges for the three quad layers now, room for two more) lives in a GPU buffer and in a C mirror; only changed entries are uploaded. `cull.comp` tests each chunk's box (side 32 << level voxels, scaled) against the six planes of the view-projection and writes the commands. `quad.vert` reads origin and level through `gl_DrawID` and the push constant is removed. The CPU frustum loop is deleted.
- [ ] **Step 5:** Golden images pass unchanged. Add scene 4 "dug out": scene 1's camera, after `Edit.fill_box` has set to air every voxel of the level-0 chunk that holds the ground point 40 m ahead of the camera, and the stream has settled. Look at the image before storing it: the chunk is a clean square hole and nothing of its old surface remains (Review Focus 3).
- [ ] **Step 6:** Assert in the viewer's scripted output that `stat(2)` is 3 at every printed interval. Gate green. Commit.

### Task 4: Slot planning and the render module

**Files:** Create `lib/strata/slots.march`, `lib/strata/render.march`, `test/slots_test.march`; modify `viewer/lib/strata_viewer.march`

**Interfaces — Produces:**
```
-- Strata.Slots (pure; no capability)
type Slots
fn new(capacity : Int) : Slots
type Action                                          -- read with the functions below
fn is_upload(a : Action) : Bool                      -- false means free
fn slot(a : Action) : Int
fn key(a : Action) : Strata.Coord.Key                -- of an upload
fn mesh(a : Action) : Strata.Mesh.Meshed             -- of an upload
fn plan(s : Slots, meshes : List((Key, Meshed)), gone : List(Key)) : (Slots, List(Action))
      -- frees first, then uploads; a mesh for a key with no slot takes a freed slot before a new one;
      -- when no slot is left the mesh is kept waiting inside Slots and planned again next time
fn refused(s : Slots, a : Action) : Slots            -- the renderer could not take this upload: keep it waiting
fn slot_of(s : Slots, k : Key) : Int                 -- -1 when the key holds no slot
fn used(s : Slots) : Int
fn waiting(s : Slots) : Int

-- Strata.Render (needs Window)
type Render
fn new(capacity : Int) : Render
fn sync(r : Render, s : Strata.Stream.Stream) : (Render, Strata.Stream.Stream)
      -- takes the stream's meshes and unloads, plans, performs the actions, feeds refusals back
fn used(r : Render) : Int
fn waiting(r : Render) : Int
```
An upload action is performed for all three layers even when a layer is empty, so an emptied layer is cleared.

- [ ] **Step 1: failing tests** in `slots_test.march`: a first mesh takes slot 0, a second slot 1; a mesh for a key that has a slot reuses it; freeing a key's slot makes it the next one handed out; a `gone` key with no slot yields no action; frees come before uploads in the action list; with capacity 2, a third mesh yields no upload action and `waiting == 1`, and the next `plan` with one key gone uploads it into the freed slot (Review Focus 5); `refused` puts an upload back in waiting and it is planned again (Review Focus 2); a newer mesh for a waiting key replaces the waiting one; a key that is waiting and then goes is dropped from waiting. Property: over random sequences of meshes and unloads, no two keys ever share a slot and `used + free == handed out`.
- [ ] **Step 2:** Run; expect failure. Implement `Strata.Slots`. Pass.
- [ ] **Step 3:** Implement `Strata.Render` over it and replace the viewer's hand-written slot code with `Render.sync`.
- [ ] **Step 4:** Golden images pass. Rerun Task 2's `STRATA_QUADS=20000` case with a scripted flight away and back: the run ends with `waiting == 0` and zero refused uploads outstanding.
- [ ] **Step 5:** Gate green. Commit.

### Task 5: Textures and the block table

**Files:** Create `viewer/lib/textures.march`; modify `native/shaders/quad.vert`, `native/shaders/quad.frag`, `native/strata_vk.c`, `lib/strata/ffi/vk.march`, `lib/strata/blocks.march`, `lib/strata/render.march`, `test/blocks_test.march`, `viewer/lib/strata_viewer.march`

**Interfaces — Produces:**
```
C     int64_t sv_textures_create(int64_t tile_px, int64_t layers);       -- tile_px a power of two 4..256; layers 1..2048
C     int64_t sv_texture_set(int64_t layer, void *rgba_u8, int64_t nbytes);  -- nbytes == tile_px * tile_px * 4
C     int64_t sv_textures_commit(void);                                   -- builds the mip chain
C     int64_t sv_blocks_set(void *words_i32, int64_t nblocks);            -- 8 words a block
March Strata.Ffi.Vk.textures_create(tile_px : Int, layers : Int) : Bool
March Strata.Ffi.Vk.texture_set(layer : Int, rgba : NativeU8Arr) : Bool
March Strata.Ffi.Vk.textures_commit() : Bool
March Strata.Blocks.gpu_table(r : Registry) : NativeI32Arr
        -- 8 words a block id: the six face textures, then flags (bit 0 collides, bits 1-2 opacity,
        --   bits 3-4 layer, bits 5-6 style), then emission
March Strata.Render.upload_blocks(r : Registry) : Bool
March StrataViewer.Textures.tile(id : Int) : NativeU8Arr    -- 16 x 16 RGBA, deterministic, for the viewer's texture ids
```
`sv_set_palette` and the palette are removed.

- [ ] **Step 1: failing tests** in `blocks_test.march`: `gpu_table` has `8 * count` entries; a block registered with `with_faces(d, 3, 4, 5)` reads 3, 4, 5, 5, 5, 5 in words 0..5; air's row is all zeros except the style bits meaning no mesh; the emission word equals `emission(r, id)`.
- [ ] **Step 2:** Run; expect failure. Implement `gpu_table`. Pass.
- [ ] **Step 3:** Shim: an sRGB `VK_FORMAT_R8G8B8A8_SRGB` array texture with a full mip chain; a sampler with nearest magnification, trilinear minification and anisotropy; the block table in a storage buffer. `quad.frag` samples layer `tex` at the quad's unit-face coordinate (repeat addressing) and, on the cutout layer, discards below alpha 0.5. A `tex` at or beyond the layer count samples layer 0 (clamp in the shader).
- [ ] **Step 4:** Viewer: `Textures.tile` paints stone, soil, grass top, grass side, sand, water, snow, bark and leaves with visible pixel structure; leaves have transparent pixels.
- [ ] **Step 5:** The picture changes, so run `scripts/golden.sh --update` and look at all four images: textures tile once per block across merged quads, grass sides differ from tops, leaves show holes, distant terrain does not shimmer. Add scene 5 "bad ids": a row of blocks whose texture ids are past the layer count and one unregistered block id; expected: they draw with layer 0 or not at all, and the run does not crash (Review Focus 4).
- [ ] **Step 6:** Gate green. Commit.

### Task 6: Translucent order

**Files:** Modify `native/strata_vk.c`, `native/shaders/cull.comp`

- [ ] **Step 1:** Add scene 6 "through water": a camera just above a lake looking through water at terrain and at more water behind a spit of land. Store its image now and look at it: note the artefact where a far water surface draws over a near one.
- [ ] **Step 2:** The translucent range stops being culled by the compute shader. The shim keeps a list of slots that hold translucent quads, frustum-tests and sorts that list far to near by chunk centre each frame, and writes their commands itself. Blending on, depth test on, depth writes off.
- [ ] **Step 3:** Rerender scene 6; expected: the artefact from Step 1 is gone. Update that image. `stat(2)` is still 3.
- [ ] **Step 4:** Record in `RESULTS.md` the number of translucent slots in scene 1 and the microseconds the sort costs, to show it is not per-chunk work in the general sense. Gate green. Commit.

### Task 7: Cross blocks

**Files:** Create `native/shaders/cross.vert`; modify `native/strata_vk.c`, `lib/strata/ffi/vk.march`, `lib/strata/slots.march`, `lib/strata/render.march`, `viewer/lib/strata_viewer.march`

**Interfaces — Produces:**
```
C     int64_t sv_chunk_instances(int64_t slot, void *words_i32, int64_t n);   -- Strata.Quad instances, two words each; n == 0 clears
March Strata.Ffi.Vk.chunk_instances(slot : Int, words : NativeI32Arr, n : Int) : Bool
```
An upload action now also sends the mesh's instances.

- [ ] **Step 1:** Viewer: register a cross block "tall grass" with its own texture (transparent background) and have terrain place it on some grass columns. Add scene 7 "meadow", a low view across it. Render: expected at this point, no plants (they are not drawn yet). Store nothing.
- [ ] **Step 2:** Shim: cross instances (model index -1) go to their own range in the quad buffer's instance region and the chunk table. `cross.vert` expands one instance into two crossed quads spanning the cell's diagonals, textured with the block's side texture from the block table, in the cutout pass with back-face culling off. An instance whose block id is at or past the block table's length draws nothing (Review Focus 4).
- [ ] **Step 3:** Render scene 7; expected: plants standing on the grass, visible from both sides, with transparent backgrounds. Store its image; update the others that now contain plants. `stat(2)` rises to 4.
- [ ] **Step 4:** Extend scene 5 with a cross instance carrying an unregistered id; the run does not crash and draws nothing for it.
- [ ] **Step 5:** Gate green. Commit.

### Task 8: Model blocks

**Files:** Create `lib/strata/model.march`, `test/model_test.march`, `native/shaders/loose.vert`; modify `lib/strata/blocks.march`, `native/strata_vk.c`, `lib/strata/ffi/vk.march`, `lib/strata/render.march`, `viewer/lib/strata_viewer.march`

**Interfaces — Produces:**
```
-- Strata.Model: a model's drawing geometry is quads of 21 floats each:
--   four corners of (x, y, z, u, v), positions in cell units, then the texture id
type Geometry
fn empty() : Geometry
fn quad(g : Geometry, corners : NativeF32Arr, tex : Int) : Geometry     -- corners: 20 floats; any other length leaves g unchanged
fn box(g : Geometry, x0 : Float, y0 : Float, z0 : Float, x1 : Float, y1 : Float, z1 : Float, tex : Int) : Geometry   -- six quads, outward facing
fn count(g : Geometry) : Int
fn floats(g : Geometry) : NativeF32Arr                                  -- 21 * count floats, the form Blocks.add_model takes

March Strata.Blocks.model_count(r : Registry) : Int
C     int64_t sv_model_set(int64_t index, void *floats_f32, int64_t nquads);
March Strata.Ffi.Vk.model_set(index : Int, floats : NativeF32Arr, nquads : Int) : Bool
March Strata.Render.upload_models(r : Registry) : Bool
```

- [ ] **Step 1: failing tests** in `model_test.march`: `box` adds 6 quads and `floats` has 126 entries; each quad's four corners lie on one face of the box; the texture id is the 21st float of each quad; `quad` with 19 floats changes nothing; `count(empty()) == 0`. In `blocks_test.march`: `model_count` is 0 for an empty registry and rises by one per `add_model`.
- [ ] **Step 2:** Run; expect failure. Implement. Pass.
- [ ] **Step 3:** Shim: `sv_model_set` stores a model's quads. When a chunk's instances are uploaded, each instance with a model index 0 or above is expanded into loose quads (at most 16 bytes a vertex: position in the chunk, texture coordinate, texture id) in a second buffer with its own allocator and its own range in the chunk table. `loose.vert` draws them in the opaque pass. An instance naming a model that was never set draws nothing.
- [ ] **Step 4:** Viewer: one model block, a half-height "lantern post" (a thin box with a wider cap), placed sparsely; give it collision boxes matching its geometry.
- [ ] **Step 5:** Add scene 8 "post" close to one; look at the image: the model is the right shape, textured, lit like its neighbours. Store it. `stat(2)` rises to 5.
- [ ] **Step 6:** Gate green. Commit.

### Task 9: A viewer you can walk in, with timings

**Files:** Create `viewer/lib/walker.march`, `viewer/test/walker_test.march`; modify `viewer/lib/strata_viewer.march`, `README.md`

**Interfaces — Produces:**
```
-- StrataViewer.Walker (pure)
type Body                                             -- eye position (metres), vertical speed, on the ground or not
fn body(x : Float, y : Float, z : Float) : Body
fn eye_x(b : Body) : Float   fn eye_y   fn eye_z
fn grounded(b : Body) : Bool
fn step(w : Strata.World.World, b : Body, move_x : Float, move_z : Float, jump : Bool, dt : Float) : Body
      -- move_x, move_z: wanted horizontal velocity in m/s
```
Values: body half extents 0.3, 0.9, 0.3 m; eyes 0.7 m above the body's centre; gravity 28 m/s^2; jump speed 8.5 m/s; walking speed 5 m/s (applied by the caller); terminal fall speed 60 m/s; `dt` clamped to 0.05 s inside `step`.

- [ ] **Step 1: failing tests** in `walker_test.march` on a flat stone world loaded by hand: a body dropped from 3 m comes to rest with its feet on the surface and `grounded`; standing still for 100 steps does not move it; walking into a wall stops at the wall and keeps the free axis; a jump leaves the ground, rises less than 1.5 m, and lands; a jump request in mid-air does nothing; a step of 1 m (one block up) blocks walking while a jump clears it; with `dt` of 1.0 the body does not fall through the floor.
- [ ] **Step 2:** Run; expect failure. Implement over `Strata.Query.sweep`. Pass.
- [ ] **Step 3:** Viewer: start walking, standing on the surface; `F` toggles flying; space jumps (or rises when flying). Clicks carve and build as before. The interval line adds March time per frame (tick plus sync, measured with `Vk.time`) beside the shim's stats.
- [ ] **Step 4:** Add a scripted walk (`STRATA_SCENE=9`): spawn, walk forward 200 frames into terrain, and store the final frame as a golden image, proving the body follows the ground deterministically.
- [ ] **Step 5:** README: controls, the environment switches, how to run the golden images. Gate green. Commit.

### Task 10: Soak run and results

**Files:** Create `scripts/soak.sh`; modify `viewer/lib/strata_viewer.march`, `RESULTS.md`, `README.md`

**Interfaces — Produces:**
```
env    STRATA_SOAK=frames    viewer: fly a fixed looping path for that many frames, carving a sphere every 100
                             frames, then print one line:
                             soak: frames=N worst_ms=X mean_ms=Y refused=R waiting=W slots=S live_quads=Q loaded=C
script scripts/soak.sh       runs 3000 frames with vsync on and asserts refused == 0, waiting == 0,
                             slots <= 16384, worst_ms <= 33.3; prints the line
```

- [ ] **Step 1:** Implement the soak mode and script. Run. Expected: the assertions hold. If `worst_ms` exceeds 16.7 (the design's target is no frame over 16.7 ms while flying), find which part of the frame it is from the three timings and fix it if the cause is in this plan's code; otherwise record it.
- [ ] **Step 2:** Run the soak again with `STRATA_QUADS=200000`. Expected: it completes with `waiting == 0` at the end and no crash; `refused` may be non-zero mid-run but the final line's `refused` counts only uploads still outstanding, which is 0.
- [ ] **Step 3:** `RESULTS.md`, a section for this plan: for scene 1 at 2560 x 1600, vsync off, fps and March, shim and present times; draw calls per frame (5); quads and chunks drawn; GPU memory for quads; the soak line; the Task 3 measurement and choice; what GPU timing the driver gave or that it gave none. State which of the design's targets are met and which wait for the far-field plan (the 5 km figures).
- [ ] **Step 4:** Update `docs/` screenshots from scenes 1, 7 and 8. Gate green. Commit.

---

## Out of scope for this plan

A staging-buffer upload path for devices without unified memory (the design's Part A mentions it): host-visible memory exists on every device, so the mapped buffer is correct everywhere and merely slower on a discrete GPU; with no such device to test on, the path is not built, and the README's known limits say so. The far field (levels, rings, coarsening), deferred shading and any lighting beyond what exists, particles, occlusion culling, image file loading, and a GPU-written draw count.

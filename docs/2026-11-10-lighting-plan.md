# Strata Lighting Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give Strata's renderer ordinary ray-traced lighting: a sun and sky, sun shadows (dappled through leaves), sun highlights and reflections on smooth surfaces, sky light, and glowing blocks, all traced on the GPU against the voxel world.

**Architecture:** The renderer becomes deferred: the existing draws fill screen-sized buffers, a full-screen light pass traces rays and writes the lit image, and translucent surfaces draw over it. Rays read a 3D texture holding one byte per voxel, a "light class" the registry assigns each block; each chunk's bytes are produced with its mesh and uploaded with it, so there is no other lighting state. Noisy terms are averaged over frames by re-projection.

**Tech Stack:** As the renderer plan: March `origin/main`, C11 with Vulkan 1.2 on MoltenVK, GLSL via `glslangValidator`. Golden images are the correctness check.

**Spec:** `docs/2026-11-10-lighting-design.md`. Parents: `docs/2026-11-10-renderer-design.md`, `docs/2026-11-10-strata-design.md`.

## Global Constraints

- Work in `~/code/strata` on a new branch `lighting` cut from `renderer`.
- Vulkan 1.2 plus the features the renderer already requires. No hardware ray tracing.
- No Vulkan type crosses into March. `scripts/verify.sh` prints PASS at the end of every task.
- Each frame in flight owns a copy of anything the CPU changes; the light volume is written only by copy commands recorded in a frame, never under a frame in flight.
- No lighting state on the CPU that must be recomputed when a block changes, beyond re-uploading the chunk's class bytes with its mesh.
- No light shafts, refraction, coloured shadows, or more than one bounce.
- Every golden image is looked at before it is stored.
- Commit after every task.

## Answers to the design review

Settled by the designer; the owner asked that scope not grow.

- **An edit and untouched surfaces.** A frame in which a chunk's class bytes were replaced caps every pixel's history at 2 samples, so the whole picture's lighting is re-estimated at once: briefly noisier, never stale.
- **Water** is lit forward, in the translucent pass, by the same shader functions. Its reflection and shadow are not averaged over frames: one shadow ray at the sun's centre, one reflection ray.
- **Model and cross blocks.** A cross is empty to a ray. A model block is "thin": it never blocks a ray, but a sky ray passing through its voxel collects its emission. So a lantern lights its surroundings and casts no shadow.
- **A lone glowing voxel is noisy.** Few sky rays find it. Averaging over frames is the only cure used; no list of emitters is kept. The lantern scene is exempt from the stillness check and its noise is reported.
- **The light volume covers the whole stream box** (radius + 1 on every side), so nothing is drawn beyond it and there is no seam. At the viewer's radius 10 x 3 that is about 156 MB, over the design's 128 MB target; reported, not hidden.
- **`Blocks.def()` becomes fully rough** (roughness 255). It was 0, which would make every existing block a mirror. `with_emission` is the light a block gives; `with_material`'s emissive value stays unused.

## Review Focus

1. **A game with more than 255 distinct light classes.** The excess shares one class, lighting stays sane, and the overflow is reported. Task 2.
2. **Lighting switched off.** The picture matches the renderer's current golden images. Tasks 1 and 7.
3. **An edit under a settled picture.** A block placed in sunlight casts its shadow within 2 frames; removing it clears the shadow as fast. Task 4.
4. **The camera at the edge of the light volume, and rays that leave it.** They return sky; they do not read another chunk's voxels through the wrap. Tasks 2 and 4.
5. **Nothing moving.** The picture stops changing. Task 4.

---

## File Structure

```
lib/strata/blocks.march        + light classes, kept as blocks are registered
lib/strata/mesh.march          + each quad carries its block's light class; a mesh carries its chunk's class bytes
lib/strata/render.march        + sun, quality settings, light table upload
lib/strata/ffi/vk.march        + bindings
native/strata_vk.c             + geometry buffers, light volume, light pass, composite
native/shaders/common.glsl     the uniform block and helpers shared by all shaders
native/shaders/trace.glsl      ray stepping through the light volume
native/shaders/sky.glsl        sky and sun colour
native/shaders/gbuffer.frag    replaces quad.frag for solid geometry
native/shaders/light.frag      the light pass
native/shaders/composite.frag  lit image to the screen
native/shaders/water.frag      translucent surfaces, lit and reflecting
native/shaders/fullscreen.vert
test/light_test.march          light classes and chunk class bytes
viewer/lib/*.march             ice, smoothness, emission, sun keys, scenes
```

`native/build_shaders.sh` concatenates the shared `.glsl` files in front of each shader that needs them.

---

### Task 1: Geometry buffers and a light pass that reproduces today's shading

**Files:** `native/strata_vk.c`, `native/shaders/*`, `native/build_shaders.sh`, `lib/strata/ffi/vk.march`, `viewer/lib/strata_viewer.march`, `scripts/golden.sh`

**Interfaces — Produces:**
```
C     void sv_set_lighting(int64_t mode)        -- 0: today's shading through the new passes; later tasks add modes
March Strata.Ffi.Vk.set_lighting(mode : Int) : Unit
env   STRATA_LIGHT=n                            viewer
```
Three passes a frame: geometry (the four solid ranges write colour, normal, material and depth), light (a full-screen pass writes the lit image), final (the lit image goes to the screen, then translucent quads draw over it, tested against the geometry pass's depth).

- [ ] **Step 1:** Split the shaders: `common.glsl` (uniform block, clip conversion), `gbuffer.frag`, `fullscreen.vert`, `light.frag`, `composite.frag`, `water.frag`. In mode 0 the light pass computes exactly what `quad.frag` computes today, from the buffers.
- [ ] **Step 2:** Shim: the buffers, three render passes, pipelines for each, per-swapchain rebuild of all of it, offscreen mode and screenshot still working.
- [ ] **Step 3:** Run `scripts/golden.sh` WITHOUT `--update`. Expected: every scene passes against the stored images. A scene that differs is a bug in this task.
- [ ] **Step 4:** Record GPU and shim CPU time for scene 1 offscreen beside the renderer's figures. Gate green. Commit.

### Task 2: Light classes and the light volume

**Files:** `lib/strata/blocks.march`, `lib/strata/mesh.march`, `lib/strata/render.march`, `lib/strata/ffi/vk.march`, `native/strata_vk.c`, `native/shaders/trace.glsl`, `test/light_test.march`, `test/blocks_test.march`, `test/mesh_test.march`

**Interfaces — Produces:**
```
-- Strata.Blocks
fn light_class(r : Registry, id : Int) : Int            -- 0 for air and for ids nobody registered; 1..255 otherwise
fn light_class_count(r : Registry) : Int                -- classes in use, the empty class 0 included
fn light_overflow(r : Registry) : Int                   -- blocks that had to share the last class; 0 when all fit
fn light_table(r : Registry) : NativeI32Arr             -- 8 words a class: kind (0 empty, 1 solid, 2 cutout, 3 see-through,
                                                        --   4 thin), top texture, side texture, emission, roughness, 3 spare
-- Strata.Mesh
fn light_bytes(m : Meshed) : NativeU8Arr                -- 32768 class bytes by flat index, or 1 byte when the chunk is one class throughout
-- Strata.Quad: word 1's flags byte now carries the block's light class
-- Strata.Blocks.gpu_table: the flags word carries the light class in bits 8-15

C     int64_t sv_light_table_set(void *words_i32, int64_t nclasses);
C     int64_t sv_chunk_light(void *bytes_u8, int64_t n);          -- between sv_chunk_begin and sv_chunk_commit; n is 32768 or 1
C     void    sv_set_light_box(int64_t radius_h, int64_t radius_v);   -- chunks; sizes the volume, once, before the first chunk
March Strata.Render.upload_light(r : Registry) : Bool
```
Two blocks share a class when they agree on kind, top and side texture, emission and roughness. Kind: solid for solid-opaque; cutout for a see-through block on the cutout layer; see-through for other see-through cube blocks; thin (kind 4) for a model block; cross and no-mesh blocks are empty (class 0). The 256th distinct class and every one after it share class 255, and `light_overflow` counts them.

The volume is a 3D texture of one byte per voxel covering the stream's whole box, (2 * radius_h + 3) by (2 * radius_v + 3) by (2 * radius_h + 3) chunks, addressed by chunk coordinate modulo its size, with a second texture one eighth the size on each axis marking 8-voxel cells that hold anything. A chunk's bytes are copied in by commands recorded at the start of the next frame; freeing a slot clears its cells the same way. The shim also keeps, per volume cell, which chunk it holds, so a ray never reads a cell that belongs to a different chunk than the one it is in.

- [ ] **Step 1: failing tests.** `light_test.march`: an empty registry has 1 class; stone and a second identical definition share a class; changing roughness, emission, a texture, or the layer makes a new one; a cross block is class 0; an unregistered id is class 0; `light_table` row for a leaf-like block has kind 2 and its textures; registering 300 blocks that differ only in roughness gives `light_class_count == 256` and `light_overflow == 45` (Review Focus 1). `mesh_test.march`: every quad's flags byte equals its block's class; `light_bytes` of a mixed chunk equals `light_class` of each voxel; of a uniform air chunk is one byte, 0; of uniform stone is one byte, stone's class. `blocks_test.march`: `gpu_table` bits 8-15 of the flags word are the class.
- [ ] **Step 2:** Run; expect failure. Implement the March side. Pass.
- [ ] **Step 3:** Shim: the volume, the coarse copy, the cell-owner table, staged copies at frame start (bounded per frame; the rest wait), clearing on free. `trace.glsl`: step a ray through the coarse cells, descending to voxels where a cell holds something; return whether it hit, the voxel, the face entered, the distance and the class.
- [ ] **Step 4:** A debugging light mode (`sv_set_lighting(9)`): each pixel's colour is the class of the voxel under it, found by tracing a short ray into the surface from just outside it. Add a golden scene of it over the overview. Look at it: it must look like the scene in flat class colours, with no holes and no wrapped copies of distant terrain (Review Focus 4).
- [ ] **Step 5:** Record volume memory and the worst frame's upload count in a soak. Gate green. Commit.

### Task 3: Sky and sun

**Files:** `native/shaders/sky.glsl`, `native/shaders/light.frag`, `native/shaders/composite.frag`, `native/strata_vk.c`, `lib/strata/ffi/vk.march`, `lib/strata/render.march`, `viewer/lib/strata_viewer.march`

**Interfaces — Produces:**
```
March Strata.Render.set_sun(dx : Float, dy : Float, dz : Float, r : Float, g : Float, b : Float) : Unit   -- direction to the sun; colour
env   STRATA_SUN=degrees     viewer: sun height above the horizon in scripted runs (default 55)
```
`set_sky` is removed: the sky's colour comes from the sun's height and the view direction, with a sun disc. Fog and the clear colour take the sky's colour toward the horizon in the view direction. Light mode 1 is "sky and sun, no rays": surfaces lit by sun and a flat sky term.

- [ ] **Step 1:** Implement. The viewer gets keys (`[` and `]`) to lower and raise the sun.
- [ ] **Step 2:** Mode 0 goldens still pass (mode 0 keeps the old flat sky colour). Add scenes in mode 1 at sun heights 55 and 8 degrees; look at both: blue sky and a pale horizon at 55, a warm low sun at 8, fog matching the horizon in each.
- [ ] **Step 3:** Gate green. Commit.

### Task 4: Sun shadows

**Files:** `native/shaders/light.frag`, `native/shaders/trace.glsl`, `native/strata_vk.c`, `scripts/golden.sh`, `viewer/lib/strata_viewer.march`

Light mode 2 adds sun shadows. One ray per pixel toward a random point on the sun's disc. A solid voxel blocks it; a cutout voxel blocks it where its texture is opaque at the point the ray enters the voxel; see-through voxels do not block. Sun visibility is averaged over frames: each pixel finds where its surface point was last frame, and blends with what was stored there, weighting history by a per-pixel sample count capped at 64. History is dropped where the stored distance disagrees with the re-projected one. A frame in which a chunk's class bytes were REPLACED (an edit) caps every pixel's count at 2.

- [ ] **Step 1:** Implement, with the history buffers and the previous frame's view-projection.
- [ ] **Step 2: scenes.** (a) the overview at sun 35: hills and trees cast shadows in the right direction. (b) the same at sun 15: longer shadows, same direction rule. (c) the forest floor: dappled light under leaves, a solid shadow behind a trunk. (d) a stone slab hung over grass: a hard-edged shadow with a soft rim. Look at each before storing.
- [ ] **Step 3: an edit (Review Focus 3).** A scripted run places a block above sunlit ground at frame 30 and takes its shot at frame 34; the stored image must show the shadow at close to full darkness. A second run removes it at frame 60 and shoots at frame 64: no shadow.
- [ ] **Step 4: stillness (Review Focus 5).** `scripts/golden.sh` renders scene (c) for 120 frames and compares frames 118 and 119 with a tolerance of 0.05% of pixels differing by more than 4.
- [ ] **Step 5: the volume's edge (Review Focus 4).** A scene with the light box smaller than the stream box: no shadows cast by terrain that is not there.
- [ ] **Step 6:** Record GPU time for mode 2 against the 2 ms target. Gate green. Commit.

### Task 5: Sun highlight and reflections

**Files:** `native/shaders/light.frag`, `native/shaders/water.frag`, `native/strata_vk.c`, `viewer/lib/strata_viewer.march`, `viewer/lib/textures.march`

Light mode 3 adds: a sun highlight on every surface, sharper and stronger the smoother its class (none at roughness 255), scaled by sun visibility; and one reflection ray for a pixel whose roughness is under 64. The reflection takes its colour from the lit image where the hit point is on screen and the stored depth agrees, otherwise from the hit class's colour under the sun and a flat sky term; a miss takes the sky. A class's colour is the mean of its top texture, computed when textures are committed. Translucent surfaces reflect by view angle, a little head-on and strongly at a glancing angle, and water's normal is rippled by a small moving pattern driven by a time the game sets.

**Interfaces — Produces:**
```
March Strata.Render.set_time(seconds : Float) : Unit      -- drives water ripples; scripted runs pass frame / 60
```

- [ ] **Step 1:** Viewer: an ice block (roughness 20, a pale blue texture) replacing water's top layer where the surface is above a height; snow roughness 140; water roughness 10.
- [ ] **Step 2:** Implement.
- [ ] **Step 3: scenes.** (a) a lake at sun 12, looking across it toward a hillside: the hillside and sky reflect in the water, the sun's glint lies on it. (b) high ground: snow carries a soft highlight, ice a sharp one and a reflection. (c) the lake with the reflected hillside mostly off screen (camera tilted down): the reflection is still there, in flat colours. Look at each before storing.
- [ ] **Step 4:** Record GPU time for mode 3. Gate green. Commit.

### Task 6: Sky light, emission and one bounce

**Files:** `native/shaders/light.frag`, `native/strata_vk.c`, `lib/strata/render.march`, `viewer/lib/strata_viewer.march`

Light mode 4, the default from here on, replaces the flat sky term: a few rays per pixel over the hemisphere about the normal, at most 48 voxels long. A ray that escapes returns the sky's colour in its direction. A ray that hits returns the hit class's emission plus its colour under the sun (by the hit face's angle to it) and a flat sky term. The result is averaged over frames with sun visibility. A surface's own emission is added to what it shows.

**Interfaces — Produces:**
```
March Strata.Render.set_quality(sky_rays : Int, reflections : Bool) : Unit     -- sky rays per pixel per frame, 0..8; default 2
```

- [ ] **Step 1:** Viewer: the lantern post's cap emits warm light.
- [ ] **Step 2:** Implement.
- [ ] **Step 3: scenes.** (a) the forest floor: darker under the canopy than in the open, with no corner-shading pattern. (b) a cave mouth dug into a hillside, looking in: light falls off with depth. (c) a lantern post at sun -5 (below the horizon): the ground near it is lit warm, and a block beside it casts no hard shadow but is darker on its far side. Look at each before storing.
- [ ] **Step 4:** Re-run Task 4's stillness check in mode 4. Record GPU time against the 6 ms target; if it is over, run the light pass at half resolution, re-store the images, and say so in `RESULTS.md`.
- [ ] **Step 5:** Gate green. Commit.

### Task 7: The viewer, settings and measurements

**Files:** `viewer/lib/strata_viewer.march`, `scripts/golden.sh`, `scripts/soak.sh`, `RESULTS.md`, `README.md`, `docs/`

- [ ] **Step 1:** Viewer: `L` cycles light modes; the sun keys; the default is mode 4. `STRATA_LIGHT=0` goldens are the renderer's images and still pass (Review Focus 2).
- [ ] **Step 2:** `scripts/soak.sh` with lighting on: same assertions.
- [ ] **Step 3:** `RESULTS.md`: for scene 1 offscreen, GPU and CPU time in modes 0 to 4; volume memory; the edit-response and stillness results; each target met or missed, with the cause where missed.
- [ ] **Step 4:** README: what lighting does, its settings, its limits. Screenshots of the valley lake, the snowfield, and the forest floor into `docs/`. Gate green. Commit.

---

## Out of scope for this plan

Light shafts, refraction, coloured shadows, more than one bounce, lighting the far field, a day and night cycle, hardware ray tracing.

# Strata lighting: design

Status: design, awaiting approval. Nothing here is built.
Date: 2026-11-10
Follows: `2026-11-10-strata-design.md` (project 3 of 4) and the renderer
built from `2026-11-10-renderer-design.md`. The far field (Part F of the
renderer design) is deferred; lighting comes first.

## Intent

The renderer draws a textured world lit by a flat sun term and corner
shading. This project gives it ordinary ray-traced lighting: the sun casts
shadows, shiny surfaces reflect, open sky lights what it can see, and
glowing blocks light what is near them. Rays are traced against the voxel
world itself, on the GPU, so lighting follows an edit on the next frame.

## Decisions the owner made

1. Ray-traced lighting, the normal kind: shadows, reflections, ambient
   light. Keep it simple; do not grow the scope.
2. The picture to aim at: sunlight reflecting off water in a valley and
   off snow and ice on a mountain, and light coming through nearby trees.
3. Lighting before the far field.
4. From the first design: frame rate stays the priority; rays are traced
   against the voxel grid in compute shaders (hardware ray tracing is not
   dependable on this driver).

Everything else below is the designer's call.

## What changes from the first design

The first design ruled out mirror reflections for the first version. The
owner's picture is mostly reflection, so reflections are in. Full path
tracing is still out.

## Goals

- A sun with a direction that can change, and a sky to match.
- Sun shadows on everything, including through leaves: a shadow ray that
  crosses a leaf block is blocked or passed by the leaf texture's holes.
- Reflections on surfaces the registry marks smooth (water, ice), and a
  sun highlight on anything not fully rough (snow).
- Sky light: a point sees as much sky as rays from it can reach.
- Glowing blocks light their surroundings, in their own colour.
- The corner shading baked into meshes is no longer what lights the
  world.
- An edit changes the lighting within a frame or two.
- A stated GPU budget, measured at every step.

## Non-goals

- Light shafts or any lighting of the air itself.
- Refraction, caustics, or coloured shadows through glass.
- Many-bounce global illumination. One bounce of sky and emitted light.
- Lighting the far field. It does not exist yet; the design keeps room
  for it.
- A day and night cycle as a feature. The sun is a direction the game
  sets; the viewer gets a key to move it.
- Hardware ray tracing.

## Anti-goals

- Lighting data the CPU has to recompute when a block changes (cube_forge's
  light sweep was that, and was its costliest edit).
- A second copy of world state that can drift from the chunks.
- Noise that crawls when the camera is still.
- Per-frame CPU work proportional to loaded chunks.

## Constraints

- MoltenVK on an Apple M3 Max; Vulkan 1.2 plus the features already
  required. Compute shaders, 3D textures and storage images are
  available. Nothing may require more.
- No Vulkan type crosses into March. `scripts/verify.sh` stays green.
- Each frame in flight keeps its own copy of anything the CPU changes,
  as the renderer does now.
- The golden-image harness is the correctness check; images are looked at
  before they are stored.

## The design

### 1. What a ray can see: the light volume

A ray needs to know, for any voxel, whether it blocks light and what it
looks like when hit. One 3D texture answers both: one byte per voxel
holding a **light class**, 0 for empty.

- A light class is what a block is to a ray: whether it blocks fully, is
  a cutout (and which texture decides), or is see-through; its average
  colour; what it emits; how smooth it is. The registry is reduced to at
  most 255 classes by grouping blocks that agree on those. A game with
  more distinct classes than that shares the last one; this is reported,
  not silent.
- A block's average colour is the mean of its texture, computed when the
  textures are committed.
- The volume covers a box around the camera, the **lighting radius**, a
  setting no larger than the stream's radius. At radius 8 x 3 it is about
  66 MB. Addressing wraps, so moving the camera rewrites only the chunks
  that enter.
- Each chunk's 32 768 class bytes are produced beside its mesh, by the
  same worker, and uploaded with it. An edit that remeshes a chunk
  re-uploads its bytes. There is no other lighting state.
- A coarser "anything here?" copy of the volume lets a ray cross empty
  space in large steps.
- Beyond the lighting radius a ray sees nothing and returns sky.

### 2. The frame

- **Geometry pass.** The existing five draws write to screen-sized
  buffers instead of the screen: surface colour, normal, depth, and
  material (smoothness, emission, and whether the pixel is see-through).
- **Light pass.** Compute shaders trace rays from each pixel's surface
  and write the lit colour.
- **Translucent pass.** Water and glass draw last over the lit image, as
  now, lit and shadowed by the same code, with their reflection.
- **Present.**

Corner shading stays in the mesh format (the mesher is not changed) but
the light pass does not use it.

### 3. The rays

In the order they are built. Each is one step with its own golden images
and its own measured cost.

1. **Sun shadows.** One ray per pixel toward the sun. A fully solid voxel
   blocks it. A cutout voxel is tested against its texture where the ray
   crosses it, so leaves cast dappled shade. The shadow edge is softened
   by aiming at a random point on the sun's disc and smoothing over
   frames.
2. **Sun highlight and reflections.** Every surface gets a sun highlight
   scaled by its smoothness; this needs no ray. A pixel smoother than a
   threshold also traces one reflection ray. What the ray hits is
   coloured from the screen when the hit point is on screen, and from the
   hit voxel's class colour, lit by the sun, when it is not; a miss
   returns sky. Water's surface normal ripples slightly so its reflection
   shimmers.
3. **Sky light and one bounce.** A few rays per pixel over the hemisphere.
   A ray that escapes returns sky colour; one that hits returns the hit
   voxel's emission plus its colour under sun and sky, roughly estimated.
   This one mechanism gives ambient shading, light from glowing blocks,
   and a little bounce.

### 4. Keeping the picture still

Shadow softness and sky light are estimated from few rays, so they are
noisy. Each pixel blends this frame's result with what the same surface
point had last frame, found by re-projecting its world position. History
is thrown away where the surface changed (an edit, or something newly
uncovered). When the camera and world are still the image must settle and
stop moving.

### 5. Sky and sun

A sky colour that depends on view direction and sun height, with a sun
disc. The game sets the sun's direction and colour. The clear colour and
the fog take their colour from the same sky, so the horizon matches.

### 6. The API a game sees

- The registry already carries roughness and emission per block; they
  start to mean something. No new per-block fields are required.
- `Strata.Render` gains the sun (direction, colour), the lighting radius,
  and quality settings: sky rays per pixel, reflections on or off, and a
  switch that turns ray tracing off entirely and falls back to today's
  shading (for comparison, and for weak devices).
- Nothing in a game's frame loop changes.

### 7. The viewer

- Ice: a smooth block on water above a height, so mountains have it.
- Snow and water given smoothness values; lantern posts given emission.
- Keys to move the sun and to switch lighting off and on.
- Scenes for golden images: a valley lake under a low sun, a snowfield
  with ice, the forest floor under trees, a lantern post at dusk, a cave
  mouth.

## Budget and targets

Designer's targets, stated so a miss is reported. 2560 x 1600, M3 Max,
offscreen timing.

| | target |
|---|---|
| GPU time, sun shadows only | 2 ms or less |
| GPU time, everything on, default quality | 6 ms or less |
| frame rate, everything on | 120 fps or better (8.3 ms) |
| lighting follows an edit | within 2 frames |
| light volume memory at the default radius | 128 MB or less |
| picture when nothing moves | settles within 1 second, then no visible change |

If the 6 ms target cannot be met at full resolution, the light pass runs
at half resolution and is scaled up, and that is reported.

## Testing

- **Pure logic in March tests:** reducing a registry to light classes
  (grouping, the 255 limit, the reported overflow), a chunk's class
  bytes against its blocks, the volume's wrap addressing.
- **Golden images** for each ray kind, including: a shadow that moves
  when the sun does; dappled shade under a tree against a solid shadow
  under stone; a lake reflecting a hillside; a block placed and removed
  changing a shadow and a reflection; lighting switched off matching the
  renderer's current images.
- **Stillness:** render the same pose for 120 frames and require the last
  two frames to match within a tight tolerance.
- **Soak:** the existing run with lighting on, with the same assertions.

## Order of work

One plan. Each step leaves the viewer working and its cost measured.

1. Geometry buffers and a light pass that reproduces today's shading.
2. The light volume: classes, chunk bytes, upload, wrap addressing.
3. Sky and sun.
4. Sun shadows, with cutout leaves and frame-to-frame smoothing.
5. Sun highlight and reflections.
6. Sky light, emission and one bounce.
7. Viewer scenes, settings, measurements against the targets.

## Left to the builder

- Buffer formats, the volume's texture format and its coarse copy.
- The ray-stepping algorithm and step limits.
- How class bytes travel with a mesh from worker to renderer.
- The smoothing blend, its rejection test, and the random sequence.
- Sky colour model; the reflection threshold; default ray counts.
- Whether the light pass is one compute shader or several.

## Risks

- **Cost.** Per-pixel rays at 4 million pixels may not fit the budget.
  Each ray kind is measured as it lands; half-resolution lighting is the
  named fallback.
- **Noise.** Few rays and frame-to-frame smoothing can smear or crawl.
  The stillness test and the edit-response target bound it from both
  sides.
- **Memory.** One byte per voxel is large. The lighting radius is a
  setting, and the volume can be smaller than the stream's box.
- **Leaf shadows** depend on sampling a texture along a ray; if that is
  too slow, leaves fall back to casting a fixed partial shadow.
- **Two frames in flight** and a volume that is written while rays read
  it: uploads must land between frames, as chunk uploads do.

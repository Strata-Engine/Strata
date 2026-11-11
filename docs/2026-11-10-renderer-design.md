# Strata renderer and far field: design

Status: design, awaiting approval. Nothing here is built.
Date: 2026-11-10
Follows: `2026-11-10-strata-design.md` (the whole shape) and the voxel core
it produced. This is project 2 of 4.

## Intent

The voxel core exists and an early rendering slice draws it. The slice
proves the data layout but not the renderer: it culls on the CPU, issues a
draw per chunk, shades with flat colours, and sees about 300 m. This
project makes it the renderer the engine keeps: the GPU decides what to
draw, a game supplies textures, everything the mesher emits appears on
screen, and the world is visible to about 5 km.

## Decisions the owner made

1. Vulkan (from the first design).
2. High frame rate under load is the point (from the first design).
3. The view should reach "pretty far"; about 5 km at one-metre voxels is
   the agreed ballpark.

Everything else below is the designer's call unless marked otherwise.

## What exists to build on

- `native/strata_vk.c`: window, swapchain, one host-mapped 64 MB quad
  buffer with a free-list allocator, an instanced 4-vertex strip pipeline
  that decodes `Strata.Quad`'s two words, CPU frustum culling, a PPM
  screenshot.
- `lib/strata/ffi/vk.march`: the bindings.
- `viewer/`: a streamed fly-through with click-to-carve.
- The core: `Strata.Mesh` already emits quads per layer and instances for
  cross and model blocks; `Strata.Coord.Key` already carries a level field
  that is always 0; `Strata.Stream` hands out finished meshes and unloads.

## Constraints

- **The driver.** Development is on MoltenVK (Vulkan 1.4 API, Apple M3
  Max). It has compute shaders, multi-draw indirect, per-draw ids
  (`shaderDrawParameters`), an immediate present mode and 2048-layer
  texture arrays. It does NOT have `drawIndirectCount`. Baseline is
  Vulkan 1.2 plus those features; nothing may require more.
- **March and forge.** As in `GAPS.md`: a consumer lists the C shim in its
  own `[ffi]` block; GLFW needs `MARCH_PIN_MAIN=1`; no Vulkan type crosses
  into March; values cross as native arrays, Ints and Floats.
- **The gate.** `scripts/verify.sh` stays green after every task.
- **cube_forge is not touched.**

## Goals

- Draw calls per frame do not grow with the number of chunks.
- A game supplies its own block textures; the engine ships none.
- Cube, cross and model blocks all draw; cutout blocks have holes;
  translucent blocks blend acceptably.
- At one-metre voxels the world is visible to about 5 km, and an edit made
  nearby is still there when seen from far away.
- The viewer is something you can walk around in, and it reports honest
  frame costs.
- The geometry pass is left ready for project 3 to turn into a deferred
  pass without reworking the data path.

## Non-goals

- Deferred shading, shadows, traced or emitted light. Lighting stays sun,
  corner shading and fog. That is project 3.
- Particles and simulation hooks (project 4).
- Occlusion culling. The article this follows skipped it too.
- The article's ray-cast sprite far field. Rings of coarser chunks do the
  job with the renderer already built (see "The far field").
- Loading image files. A game hands the engine pixels.
- A GPU-written draw count. The fixed-length command list is the only
  path for now.
- Testing on Windows or Linux.

## Anti-goals

- A second mesh format or a second mesher for distant terrain.
- Per-frame CPU work proportional to the number of loaded chunks.
- Renderer state the March side has to mirror by hand. Slot bookkeeping
  moves into the library.
- A frame that waits on the GPU because a chunk was uploaded or freed.

## Part A: the GPU decides what to draw

**The chunk table.** One GPU buffer, an entry per chunk slot: the chunk's
origin in voxels, its level, and for each of its geometry ranges (three
quad layers, cross instances, loose quads) a first index and a count. The
C side keeps the same table and uploads the entries that changed.

**Culling.** Once per frame a compute shader reads the table and the
camera and writes indirect draw commands, one per visible chunk range.

**How many commands the CPU submits** is the open engineering question,
because the driver lacks `drawIndirectCount` and MoltenVK may replay a
multi-draw as one Metal draw per command, zero-count or not. The first
task therefore measures the cost of a 50 000-command multi-draw, not
only its correctness, and picks between two designs:

- *Fixed list.* One command per slot per range, zero count when culled,
  list length the highest slot in use. Simplest. Used if the measured
  cost of zero-count commands is negligible.
- *Compacted list with a late count.* The compute shader packs visible
  commands to the front of the list with an atomic counter, zero-fills
  the remainder up to the length it was told, and leaves its count in a
  buffer the CPU can read. The CPU submits "last frame's count plus a
  quarter, plus 64", and tells the shader that length. A view that
  suddenly needs more than that draws the excess a frame late. Used if
  zero-count commands are not free.

Either way the CPU does no per-chunk work. If neither holds 240 fps, what
gives way is, in order: the constant draw-call count (a means, not an
end), then the 240 fps target, which is then reported as missed with the
measured cause. KosmicKrisp, the conformant driver the first design
named, is a valid thing to try if it can be installed; nothing may depend
on it.

**Drawing.** One multi-draw-indirect call per range type per pass. The
vertex shader finds its chunk through the per-draw id and reads the origin
and level from the table; the per-chunk push constant goes away.

**Translucency.** Opaque and cutout ranges draw first, in any order.
Translucent ranges draw last with blending on and depth writes off. Their
commands are ordered far to near by chunk; the C side sorts that short
list each frame, since few chunks hold translucent quads. Quads inside one
chunk are not sorted. This is "acceptably", not "correctly", and is the
designer's call.

**Uploads and frees never stall a frame.** Today an upload or free waits
on the frame fence. Instead, a freed range is held for two frames before
it returns to the allocator, and the renderer keeps two frames in flight
with their own command buffers and fences. The buffer stays host-mapped
where the device has unified memory; on a device without it, uploads go
to a staging buffer and are copied in one pass at frame start. The quad
buffer's size becomes a parameter of `open`.

**Review findings folded in.** The frame fence is reset only after a
successful submit; swapchain image counts above the fixed array are
handled; a failed swapchain rebuild leaves no half-built state; a refused
upload is reported to March and retried.

## Part B: textures

- One texture array, sRGB, mipmapped, one layer per texture id. The id is
  the 16 bits each quad already carries, so the 8-bit palette lookup and
  the palette itself are removed.
- A game creates the array with a tile size and a layer count, then sets
  each layer from a `NativeU8Arr` of RGBA pixels. The engine generates the
  mip chain.
- Sampling is nearest when magnified, so blocks keep their pixel look, and
  trilinear with anisotropy when minified.
- A quad's texture coordinate is its position in unit faces, so a merged
  quad tiles its texture once per block for free.
- The cutout layer discards pixels below an alpha threshold.
- **The block table.** Per block id, the data shaders need (side texture
  for crosses now; emission and material for project 3) is uploaded once
  from the registry.
- The viewer builds a small set of 16 x 16 textures in March code, so the
  repository still ships no image files.

## Part C: drawing instances

**Cross blocks.** A chunk's cross instances are uploaded as they are,
eight bytes each. The vertex shader expands one instance into two
crossed quads, textured from the block table. Crosses are drawn in the
cutout pass.

**Model blocks.** A model's drawing geometry gets a defined format: a
list of quads, each four corners of position (in cell units) and texture
coordinate, plus a texture id. A game registers it with the model, as
`Strata.Blocks.add_model` already accepts. At upload the C side expands
each model instance into "loose quads" at no more than 16 bytes a vertex
in a second buffer, with its own range in the chunk table. Models are
rarer than crosses, so the larger format is affordable there and not for
crosses.

Instances are drawn at level 0 only.

## Part D: the library owns the bookkeeping

A new `Strata.Render` module sits above the bindings and holds what the
viewer does by hand today: which slot belongs to which chunk key, handing
a stream's finished meshes to the GPU, freeing unloaded chunks, retrying
refused uploads. A game's frame is: tick the stream, sync the renderer
with it, set the camera, draw.

## Part E: the viewer

- Walkable: gravity, jumping and collision through `Strata.Query.sweep`,
  with flying on a toggle.
- Textured, with cross plants and at least one model block in the world.
- Reports per-frame cost: CPU time in March, CPU time in the shim, and the
  whole frame with vsync off (the immediate present mode). GPU time by
  timestamp query where the driver supports it; if it does not, say so
  rather than print a wrong number.
- Scripted runs for screenshots and timings, as now.

## Part F: the far field

**Rings of coarser chunks.** Level 0 is today's chunks. A level-L chunk is
still 32 voxels on a side, but each of its voxels is 2^L metres at metre
scale, so it covers (32 x 2^L) voxels of world per side. Its key's
coordinates count level-L chunks.

**The target.** At one-metre voxels: level 0 to about 320 m (radius 10
chunks), then levels 1 to 4, each reaching twice as far as the one before:
640 m, 1.3 km, 2.6 km, 5.1 km. Level 0 keeps a vertical radius of 3
chunks (3087 chunks). Levels 1 to 4 use a vertical radius of 1, since a
coarse chunk is at least 64 m tall (1323 chunks each). The reference
scene is therefore about 8400 chunks, under three times level 0, and the
slot table stays at 16 384. Radii per level are settings; 8400 is the
benchmark scene, not a hard ceiling.

**What it asks of a game.** A generator declares the highest level it
supports and must produce a chunk for any key up to that level directly,
without generating the fine chunks beneath it; a height-map generator
samples at a wider stride. A generator that declares level 0 gets the
near field only. To keep cracks between levels small, a coarse voxel
should be solid if any of the fine voxels it stands for is solid; the
design recommends this to generators and uses it wherever the engine
itself coarsens data.

**Which chunk draws.** A chunk is hidden when all eight of its children
(the level-below chunks that tile it) are loaded and meshed, and drawn
otherwise. A fine chunk is drawn only when its seven siblings are meshed
too. So a region is always drawn by exactly one level: no holes while
fine chunks are still arriving, no double drawing. The fine box is
snapped to whole coarse chunks so this rule has clean edges.

**Each level loads its whole box**, not only the ring outside the finer
level. The chunks under the fine region are generated (they are mostly
uniform and cheap) so that meshing a coarse chunk at the ring's inner
edge has all its neighbours. They are meshed only when they need to be
drawn.

**No holes on the trailing edge.** When fine chunks leave a region the
stream unloads them at once, but their parent's mesh takes a frame or
more to arrive. `Strata.Render` therefore keeps a departed chunk's GPU
mesh, and keeps drawing it, until its parent's mesh is uploaded; only
then does it free the slots. Renderer slots can outlive the stream's
chunks, and the which-chunk-draws rule is evaluated on what the renderer
holds, not on what the stream has loaded.

**Meshing and drawing a coarse chunk** uses the existing mesher on
level-L neighbours and the existing quad format. The renderer scales a
chunk by its level, read from the chunk table. Textures repeat per fine
voxel, so a coarse face is not one stretched tile.

**Edits.** Editing, ray casting and collision stay at level 0. An edited
level-0 chunk already survives in the world's cache. When a coarse chunk
is produced, any edited fine chunks inside it are coarsened and laid over
the generated data, so a crater is still a crater from 2 km. When an edit
changes a fine chunk, its coarse ancestors are marked stale and rebuilt
before they are next drawn. An edit aimed beyond the level-0 box is
queued as today and appears when the area is next near.

**Seams and popping** are accepted, measured and reported, not
eliminated: a seam between levels may show a step of one coarse voxel,
softened by fog, and a ring boundary passing over terrain changes its
shape once. The far-field result includes screenshots aimed at a ring
boundary.

## Testing

- **Pure logic in March tests:** ring boxes and snapping, the
  which-chunk-draws rule, coarsening, edit overlay, `Strata.Render` slot
  bookkeeping (against a fake binding layer or by keeping the logic free
  of binding calls).
- **The C allocator and table** get a small C test program run by the
  gate.
- **Golden images.** A script renders fixed scenes (seed, camera, frame
  count) headless and compares against stored images within a tolerance.
  It covers: textures and cutout, crosses and models, translucent water,
  a ring boundary, an edit seen from the far field.
- **A soak run.** Fly a fixed path for a few thousand frames with
  streaming and edits; assert no refused uploads, no allocator growth
  without bound, and no frame over a stated limit.

## Targets

These are the designer's, stated so a miss is reported rather than
unnoticed. Viewer on the M3 Max at 2560 x 1600, release build.

| | target |
|---|---|
| draw calls per frame | constant: one per range type per pass |
| standing still, 5 km view, vsync off | 240 fps or better |
| flying at 40 m/s, vsync on | no frame over 16.7 ms |
| CPU time per frame in the shim | under 0.5 ms with the 8400-chunk scene |
| GPU memory for quads at the 5 km view | under 64 MB |

## Order of work

Two plans, each ending in something to run.

1. **Renderer.** Parts A to E: GPU-driven drawing, textures, instances,
   `Strata.Render`, the walkable viewer, measurements, golden images.
2. **Far field.** Part F: levels in the core (generator contract, ring
   loading, the drawing rule, coarsening and edit overlay), level support
   in the renderer, the 5 km viewer.

## Left to the builder

- Layouts of the chunk table, block table, loose-quad vertex and the
  uniform block.
- Whether culling runs once per frame for all ranges or once per range.
- The allocator's deferred-free mechanics.
- Mip generation method; anisotropy level; alpha threshold.
- How `Strata.Render` is kept testable without a window.
- How stale coarse chunks are tracked and when they are rebuilt, provided
  an edit is never missing from a level that is being drawn for longer
  than about a second.
- Ring radii per level beyond the stated target, and how they are
  configured.

## Risks

- **Compute and multi-draw indirect on MoltenVK are unproven here, and
  multi-draw may cost CPU per command.** First task: a compute shader
  writes commands that one indirect call draws, verified by a golden
  image AND timed at 50 000 commands, before anything else depends on
  it. Part A names the two designs that measurement chooses between.
- **Cracks between levels may be more visible than hoped.** They are
  measured early with a ring-boundary screenshot; the fallback is a
  one-chunk overlap between levels with fog blending.
- **Coarse generation may be slow for games with expensive terrain.**
  The viewer's generator is the reference; the far-field result records
  generation time per level.
- **March-side per-frame cost.** `Strata.Stream` and `Strata.Render` must
  not walk every loaded chunk each frame; with rings the loaded count
  nearly triples. The soak run measures it.

# Strata: a voxel engine library for March

Status: design, awaiting approval. Nothing is built.
Date: 2026-11-10

## Intent

cube_forge proved that March can run a voxel game. Its engine and its game
are one program, its renderer is OpenGL 3.3, and its frame rate suffers when
simulation and particles get busy. Strata is a fresh, standalone library
that takes the engine ideas worth keeping from cube_forge and rebuilds them
for a higher ceiling:

- high frame rates in busy scenes: heavy simulation, many particles;
- good lighting, ray traced where possible;
- generic enough to build Minecraft, or a high-detail destructible
  environment, on the same library.

The rendering template is Project Ascendant
(https://vkguide.dev/docs/ascendant/ascendant_geometry/): packed geometry in
one large GPU buffer, compute-shader culling, indirect draws, deferred
lighting.

## Decisions the owner made

These came from the owner directly and are not open to the builder.

1. Strata stands alone, in `~/code/strata`, with a small demo world.
   cube_forge is not migrated and is not modified.
2. The graphics backend is Vulkan.
3. The performance goal is high FPS under complicated simulation and
   particle effects.
4. Strata compiles against March `origin/main`.
5. The block seam is fairly generic: Minecraft or a high-detail
   destructible environment must both be buildable.
6. Good lighting, with ray tracing if possible.
7. The work is four projects in this order, each with its own design and
   approval: voxel core, Vulkan renderer, traced lighting, GPU particles
   and simulation hooks.

## The whole shape

Four projects. This document designs the first in detail and records the
rest so later designs stay consistent.

### Project 1: voxel core (designed below)

Pure March plus a small C memory helper. No graphics. Block registry, cubic
chunks, storage, streaming, terrain interface, edits, meshing into packed
quads, raycast and collision.

### Project 2: Vulkan renderer

- Vulkan 1.3 baseline. On macOS this runs through the LunarG Vulkan SDK
  (KosmicKrisp is Vulkan 1.4 conformant on macOS 26; MoltenVK is the
  fallback). Windows and Linux are supported by design but not tested in
  this effort.
- A C shim owns Vulkan and GLFW, as cube_forge's shim owns OpenGL. March
  calls it through `extern` blocks split by capability.
- First milestone is a triangle drawn from March, to prove the toolchain
  before anything depends on it.
- One large GPU buffer holds every chunk's packed quads, sub-allocated.
  A compute shader culls chunks against the view and writes indirect draw
  commands. One shared quad index pattern; no per-chunk index buffers.
- Uploads are staged on the CPU and copied at frame start. A chunk is never
  updated in place: a new allocation replaces the old one.
- Ends with a demo world you can walk through.

### Project 3: traced lighting

- Deferred rendering: geometry writes position, normal and material to
  screen buffers; lighting runs once per pixel.
- Rays are traced in compute shaders against the voxel occupancy grid held
  on the GPU. This works on any Vulkan 1.3 device and updates instantly
  when a block changes. Hardware ray query is an optional path on drivers
  that offer it; on macOS today it is not dependable.
- Rays provide sun and moon shadows, coloured light from emitting blocks,
  and ambient and bounce light. Ray count per pixel is a setting; noise is
  accumulated over frames. Frame rate stays the priority.
- No mirror reflections and no full path tracing in the first version.
- A triangle-based hardware path may be prototyped against the grid path;
  the faster one stays.

### Project 4: GPU particles and simulation hooks

- Particles simulated on the GPU, lit by the same deferred path, able to
  collide with the occupancy grid.
- The edit event stream and budgeted ticks a game's simulation needs.
- Save format for edited chunks (see non-goals of project 1).

## Project 1 in detail: the voxel core

### Goals

- A game registers its block types; the core knows no specific block.
- Worlds are unbounded on all three axes.
- Editing is cheap: one block or ten thousand at once.
- The mesh output is the packed format project 2 uploads without
  conversion.
- Everything is testable headless and deterministic for a given seed.

### Non-goals

- No window, no GPU, no shaders.
- No baked light field. cube_forge's CPU light sweep is not ported;
  lighting is project 3's job. The mesher still emits per-corner ambient
  occlusion because it is static and nearly free.
- No game simulation. Water, weather, growth and their rules belong to
  games.
- No save format yet. It lands in project 4. The chunk type must be
  serialisable by design, nothing more.
- No smooth (surface nets) terrain. Cubes, crosses and small models only.

### Anti-goals

- Any Strata module naming a game concept (grass, water, mycelium).
- Any core module that needs a window or GPU capability to compile.
  cube_forge's terrain code depended on GLFW to reach a `memcpy`; that must
  not recur.
- A panic reachable from library code. Every lookup is total with a
  documented default, as cube_forge's `Tree` is.
- A core so abstract it is slower than cube_forge at the same job.

### Constraints

- March `origin/main`. Record the commit Strata was last verified against
  in the README. If the installed compiler is older, build main from
  `~/code/march` in a separate worktree; do not disturb that checkout's
  current branch.
- When March or forge blocks progress, work around it inside Strata and
  record the gap in a `GAPS.md`, cube_forge style. Do not change March.
- forge may not build a dependency's C sources (this was true when
  cube_forge's extraction plan was written; verify on main first). If still
  true, consumers paste a documented `[ffi]` block, and the demo does so.
- Release builds treat undischarged refinement obligations as errors, and
  strict lint forbids panics in libraries. The verification gate is
  `forge check`, `forge build --release`, `forge lint --strict`,
  `forge test`, all clean.
- cube_forge code is copied and adapted, never moved.

### Design decisions made by the designer

These are my calls. The owner has not ruled on them and may overturn any.

**Chunks are 32 x 32 x 32, fixed at compile time.** Cubic chunks give an
unbounded vertical axis and tight culling. The side is a compile-time
constant so the flat-index refinement proofs survive, which cube_forge's
plan argued for and I agree with. 32 fits a local coordinate in five bits,
which the packed quad relies on.

**Voxel scale is a runtime world parameter.** A world states metres per
voxel. Minecraft uses 1.0; a destructible environment might use 0.1. This
is what "high detail" means here: more, smaller voxels of the same chunk
type, not a different chunk type. All queries take and return world units.

**Block ids are 16 bits, stored through per-chunk palettes.** A chunk is
one of three forms:

- uniform: a single block id and no array (all air, all stone). With cubic
  chunks most of the world is this form.
- paletted: a `NativeU8Arr` of 32768 indices plus a palette of up to 256
  block ids.
- direct: 32-bit entries, for the rare chunk with more than 256 distinct
  ids.

The form is hidden behind `get` and `set`. This holds Minecraft's block
count at cube_forge's one byte per voxel. The direct form uses 32-bit
entries only because March has no 16-bit native array.

**Every block state is its own id. There is no second per-voxel channel.**
Orientation, fluid level and damage stage are each distinct registered
ids, as modern Minecraft does and as cube_forge does for water levels. The
registry offers a helper to register a family of states and map between a
base id and a state number. Sixteen bits and per-chunk palettes exist to
make this affordable. A game that needs richer per-voxel data keeps it in
its own structure, keyed by position.

**The block registry is flat arrays built once.** A game builds a registry
at startup and passes it into the world. Per block id it records:

- solidity for collision;
- opacity class: empty, opaque, or see-through;
- emitted light colour and strength (used by project 3);
- render layer: opaque, cutout, translucent;
- mesh style: cube, cross, model (with a model index), or none;
- texture layer per face;
- surface material values the lighting needs (roughness, emissive).

A model is game-supplied data in a table beside the registry: a list of
quads for drawing, which the core passes through untouched for project 2,
and a list of boxes for collision and raycast, which `Strata.Query` reads.
A cross block has no collision and is hit by raycast as a full cell.

Lookups outside the registered range return a documented default (air).
There is no per-block callback into game code on any hot path.

**Terrain is a pure function the game supplies.** Given chunk coordinates
and a seed it returns a chunk. It must be pure so workers can run it ahead
of need and in parallel, and so an unedited chunk can be dropped and
regenerated identically. Whether this is a function value or an interface
is the builder's choice with the code open.

**The loaded world is a box of chunks around one or more focus points.**
Horizontal and vertical radii are runtime settings. Chunks are keyed by
three coordinates in a persistent map; cube_forge's `Tree` is the starting
point. Worker actors generate and mesh off the main thread, with a
per-frame budget for applying results. Edited chunks that leave the box are
kept in a cache; unedited ones are dropped. cube_forge's generation-hash
trick (drop a chunk again once it returns to its generated state) is kept.

**Edits return events; they do not call back.** `set_block` and bulk edits
(box, sphere, arbitrary list) return the new world plus the list of dirty
chunks and a compact change record. A game's simulation reads those
records. This is the hook surface project 4 formalises, and it keeps the
engine free of game callbacks.

An edit never blocks on generation. The part of an edit that lands in a
chunk that is not loaded, or is still with a worker, is queued against
that chunk's coordinates and applied when the chunk arrives; the edit's
result reports those chunks as deferred. A chunk with queued edits counts
as edited for caching.

The edited-chunk cache is unbounded in project 1, because nothing can be
written to disk until project 4. The demo reports its size.

Remeshing after an edit runs on workers, so it costs latency before the
change is visible, not frame time. A whole 32-cubed chunk is remeshed per
edit in the first version. Sub-chunk dirty regions are added only if the
measured single-edit latency exceeds one 60 Hz frame.

**Level of detail is out of scope, with room left for it.** Chunk keys and
mesh outputs carry a level field that is always zero in project 1.

**The mesher is greedy and emits packed quads.** One quad is eight bytes,
two 32-bit words:

- word one: local x, y, z (5 bits each), width and height minus one (5 bits
  each), face direction (3 bits), four corner ambient occlusion values
  (2 bits each, taken from the top of the word downward as space allows;
  the builder may move fields between words);
- word two: texture layer (16 bits), flags and spare.

cube_forge spends 240 bytes on the same quad (six vertices of ten floats).
Output is one `NativeI32Arr` per render layer per chunk. Cross and model
blocks are emitted as a separate instance list, one small record per block,
in the spirit of the article's clutter units. Meshing a chunk reads its six
face neighbours, and edge and corner neighbours for ambient occlusion.
A uniform chunk surrounded by the same uniform chunks meshes to nothing
without touching an array.

**Each chunk also yields an occupancy bitmask.** 32768 bits, 4 KB, derived
from the opacity class. The mesher wants it anyway and project 3 uploads it
for ray tracing. Project 1 only produces and tests it.

**Raycast and collision are engine code.** A voxel-stepping raycast and a
swept box collision, both scale-aware, both ported from cube_forge and
generalised to read the registry. Player movement rules are not ported.

**A small C helper carries the memory primitives.** Block copies, box
fills and the quad packers, with no graphics include. If March main is fast
enough without one of them, drop it.

### Modules

Names are indicative; the builder may regroup.

| Module | Holds |
|---|---|
| `Strata.Blocks` | the registry and its total lookups |
| `Strata.Chunk` | the three chunk forms, index proofs, get and set |
| `Strata.Coord` | world, chunk and local coordinates; voxel scale |
| `Strata.Store` | the persistent chunk map |
| `Strata.Terrain` | the generator contract |
| `Strata.Stream` | focus points, load and unload, workers, budgets |
| `Strata.Edit` | single and bulk edits, dirty sets, change records |
| `Strata.Mesh` | greedy mesher, packed quads, instance list |
| `Strata.Occupancy` | per-chunk bitmasks |
| `Strata.Query` | raycast and swept collision |
| `Strata.Ffi.Mem` | externs for the C memory helper |
| `Strata.Math`, `Strata.Noise` | vectors, matrices, noise, copied from cube_forge |

### The demo for project 1

A headless program in the Strata tree, a separate forge app that depends on
Strata by path and names no cube_forge module. It:

- registers a handful of block types;
- supplies a simple noise terrain function;
- streams a world around a moving focus, applies a scripted set of edits
  including one large sphere, and meshes everything;
- prints timings and sizes, and writes a top-down image of the surface so
  a person can see the world is right.

### What done looks like

- The verification gate passes on March main.
- Tests cover: chunk form transitions and round trips; registry totality;
  the greedy mesher against a naive per-face mesher (same covered area per
  face direction and layer, on random chunks); occupancy against `get`;
  raycast against brute force; streaming determinism (same seed and path
  give the same world hash); edit then regenerate-hash behaviour.
  Property tests where the invariant is general.
- The demo runs and reports, at minimum: chunk generation time, mesh time
  per chunk, single-edit remesh time, bulk-edit remesh time, bytes of mesh
  per chunk, bytes of voxel storage per chunk.
- Mesh bytes per quad are 8 against cube_forge's 240.
- Mesh time is compared with cube_forge on an equal voxel volume (two
  16 x 256 x 16 chunks equal four Strata chunks). The baseline is the most
  recent entry in cube_forge's `RESULTS.md` that includes ambient
  occlusion in the mesher, not the earlier, faster figures. This is a
  reporting requirement, not a hard bar: if Strata is slower, that is
  reported with the cause, not hidden.
- The demo proves two configurations: scale 1.0 with a wide box, and
  scale 0.1 with a small box (a room-sized destructible scene). OPEN: the
  owner has not said how far a high-detail world must be visible.

### Left to the builder

- Function value versus interface for terrain.
- Exact bit layout of the quad words and the instance record.
- The persistent map's structure for three-coordinate keys.
- Worker count, mailbox protocol and budget mechanics.
- Whether meshing uses bitmask tricks or cube_forge's slice approach.
- File layout, test organisation, which C helpers survive.

## Risks

- **March main may have moved under cube_forge's idioms.** Mitigation:
  build and run a few copied cube_forge modules against main first.
- **Vulkan from March is unproven.** Deferred to project 2, which opens
  with the triangle milestone.
- **Palette chunks may cost more per access than a flat byte array.**
  Mitigation: the benchmark compares against cube_forge; a chunk can be
  unpacked to a flat scratch array for meshing.
- **Scope creep in the core.** Save format, light fields and smooth terrain
  are explicitly out.

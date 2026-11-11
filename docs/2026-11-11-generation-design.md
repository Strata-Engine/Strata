# Strata generation: design

Status: design, awaiting approval. Nothing here is built.
Date: 2026-11-11
Follows: `2026-11-10-strata-design.md` (the voxel core's terrain contract)
and the fluids built from `2026-11-10-fluid-design.md`.
Research: `reports/Procedural generation for Strata.md` and the notes under
`research_notes/Procedural generation for Strata/`. Every number below that
is not Strata's own measurement comes from there.

## Intent

A Strata world today is a sum of three value noises with lakes dropped in by
a rule and trees kept away from chunk edges. This project makes a world
worth walking through: a seed makes one island with coasts, hills, cliffs,
rivers that run downhill to the sea, lakes that sit in real basins, caves,
forests, and ruins stamped from voxel models. The player digs and blasts
into all of it, and the water already in the engine answers. It is the
first game on Strata, and it is the foundation the later games (a bounded
water-economy map, a city with interiors, authored destructible scenes) are
built on, so what it adds to the library is shaped for them without
building them.

The visual target is Lay of the Land: small cubes, sculpted detail, lit by
rays. Teardown is the reference for how authored voxel models and
destruction fit a world.

## Decisions the owner made

1. The first game is a procedurally generated world with simple destructible
   gameplay. Keep it focused.
2. Lay of the Land is the visual sweet spot. Teardown is a strong reference
   point.
3. The generation must be able to grow to games in the vein of Timberborn,
   Shadows of Doubt and Teardown. This design builds none of them.
4. From the first design: the library names no game concept; a chunk is a
   pure function of its key and the seed; frame rate stays the priority.

Everything else below is the designer's call.

## Goals

- One seed makes one island, the same island every time, on every machine.
- The island reads as shaped by water: valleys lead to rivers, rivers lead
  to the sea, lakes fill basins and stay full until a player cuts a rim.
- Terrain is three-dimensional where it matters: overhangs, cliffs with
  scree, caves that open on the surface and run under it.
- Forests cross chunk borders. Ruins are voxel models stamped onto the
  ground, with the ground pulled up to meet them.
- Digging and blasting work on everything, including ruins, and water
  released by an edit flows as it does today.
- Generation parameters are written in metres. The world can be previewed
  at 1 m per voxel and shipped at 10 cm with the same definitions.
- A chunk still costs about a millisecond to generate; the one global pass
  per seed takes well under a minute.

## Non-goals

- Cities, streets, buildings with interiors, furnishing. (A later design;
  the atlas below leaves room for a city plan.)
- Bounded editor maps with water sources and sinks, Timberborn's model.
  (The atlas's river and lake data is the half of it that carries over.)
- Rigid debris: pieces of a blown-up ruin that fall and tumble. Blasted
  voxels become air and particles. (Stamped voxels carry a structure id
  so a later project can split and drop what no longer stands.)
- Saving worlds. The edited-chunk cache stays in memory as it is.
- Random-walk cave carvers of Minecraft's kind, wave function collapse,
  grammars for buildings, roads between points of interest. All are
  recorded in the research as later additions that fit the atlas.
- Smooth terrain. Cubes, as before.
- Lighting the far field with rays. Beyond the lighting radius a ray
  returns sky, as the lighting design says; far chunks are lit by the sun
  and the sky without shadows.

## Anti-goals

- A generator that reads parameters as voxel counts. Changing the voxel
  scale must not change the island.
- A chunk whose content depends on which chunks were generated before it.
- A library module that names a biome, a tree or a ruin. Regions, features
  and templates are the game's; the library gives them a shape.
- A dependency the consumer cannot build: forge does not compile a
  dependency's C sources (GAPS S11), so new C lands beside `strata_vk.c`
  and the consumer lists it.
- A world that only looks right at 1 m per voxel.

## Constraints

- March `origin/main`, as before. The gaps in `GAPS.md` still apply; two
  matter here: an extern result bound to `_` is dropped (S9) and float
  literals have no exponent form (S13).
- C through the existing FFI path only: a header or source under
  `native/`, listed by the consumer's `[ffi]` block.
- The chunk generator stays `Strata.Terrain.Gen`: a pure function of key
  and seed. Anything global is immutable data the function closes over.
- Measured today on the release viewer: terrain 0.3 ms and mesh plus upload
  2.3 ms a chunk. A chunk at the end of this project may cost 1 ms.

## The design

### 0. Water the island is born with

The fluid module knows two waters: a finite one that moves and is never
refilled, and the sink the sea is made of. The island uses both and adds
nothing. Lakes are finite water at the atlas's level, so a breached lake
drains as the fluid design promises. Rivers and the sea are sink water:
a river never runs dry when a blast opens its bank, and never floods the
crater either; what the blast releases is the finite water of any lake
upstream. A river that refills from a source is the Timberborn-style
project's addition to `Strata.Fluid`, and is a non-goal here. This is my
call: without sources the alternative is a river that empties to the sea
after its first breach and stays a dry bed.

### 1. Scale

`Strata.World` already carries metres per voxel. The rule this project adds
is that nothing in generation ever sees a voxel count. Every frequency,
amplitude, radius, cell size and threshold is a float in metres, and the one
conversion happens where a chunk's voxel coordinates become metres:
`x_m = (32 * kx + i) * scale`. A template (section 6) records its own
voxel size, and a template whose size does not match the world's is
refused, not resampled.

The first milestone is measured at 0.5 m and 0.25 m per voxel; 0.1 m is
tried once the lighting and fluid costs at those scales are known. This is
my call: the references are at 10 cm, Strata's renderer and fluid have only
ever run at 1 m, and the generator must not care either way. At 0.25 m a
chunk is an 8 m cube, the near field's radius of 10 chunks is 80 m, and a
2 km island is 250 by 250 columns of chunks. Seeing the island from a
hill is the far field's job (section 9), which the owner chose to build
in this project rather than defer.

### 2. The noise substrate

`Strata.Noise` keeps its names and gains a C implementation: FastNoiseLite's
single header (MIT) compiled with `FNL_IMPL` into `native/strata_noise.c`.
It gives OpenSimplex2 with the rotation that sends one axis up the cube
diagonal (no axis-aligned cliffs in 3D), OpenSimplex2S for ridged output,
cellular noise for region ids, fractal fBm and ridged combinators and a
domain warp, at about 14 ns a 2D sample and 27 ns a 3D sample.

The FFI is crossed per array, never per sample. The externs fill a
`NativeFloatArr`:

- a 2D field: origin, step, width, depth, noise settings → `w * d` values;
- a 3D field: origin, steps, dimensions, settings → `w * h * d` values;
- a warped 2D field, for selectors that must not look polygonal.

A 32 by 32 heightmap at three octaves is about 45 µs; a 9 by 5 by 9 lattice
(section 4) is a few microseconds. The existing value noise stays for a
release, behind a flag, so the goldens can be re-stored deliberately.

Seeds: one 64-bit world seed, and every derived seed is a mix of it with a
named salt through a PCG-style hash. The same hash, of world voxel
coordinates and a salt, is the per-cell random stream for features. Nothing
adds small integers to the seed as the viewer does today; the research
records that small salts correlate placements.

Determinism: the C is built with `-ffp-contract=off` and without fast
math, so a lattice value is the same on Apple silicon and x86. A test
hashes a fixed set of chunks from a fixed seed and compares with stored
hashes. Only Apple silicon is available, so the hashes are stored from
it and the cross-architecture claim is recorded as untested until an x86
run confirms them.

### 3. The atlas: one global pass per seed

Every shipped world with rivers that run downhill, lakes that hold, or
roads between places precomputes something per seed. Strata gets exactly
one such pass. `Strata.Atlas` builds, once at world creation, a coarse grid
over the island that chunk generation reads and never writes:

- a sample every 4 m (my call; 512 by 512 samples cover a 2 km island);
- per sample: height, region id, drainage area, receiver direction,
  distance to the nearest river, river width, lake level (or none),
  coast distance;
- a list of river polylines with widths, a list of lakes with their spill
  height, and a list of structure sites (section 6).

It is built in this order:

1. Island mask and regions. A warped low-frequency noise and a radial
   falloff make the landmass (`e = lerp(e, 1 - d, mix)`); cellular noise
   in cell-value mode, warped, splits it into regions. Regions are ids;
   the game maps ids to its own names.
2. Uplift and erosion. Stream-power erosion in the form Veloren took from
   Cordonnier et al.: each sample is a node, flow goes to the lowest
   neighbour, lakes become single nodes that spill at their lowest pass,
   and the implicit solver runs about 50 iterations with m = 0.4, n = 1,
   an uplift field from the island mask and regions, and a talus angle
   from a rock-strength noise between 15 and 60 degrees. This is what
   turns noise into valleys.
3. Priority-flood. Barnes's algorithm gives every sample a receiver and
   every basin a spill height, so the river network is a tree to the sea
   and each lake has a level: spill height less a small margin, so the
   fluid simulation finds it already at rest.
4. Rivers. Flow accumulation gives drainage area; a sample with enough
   area is river, and width follows drainage as Veloren does (width grows
   with discharge to the power 3/8). Polylines are traced down the tree.
5. Sites. Structure sites are chosen on the atlas with constraints a
   chunk cannot check: flat enough ground, a minimum spacing, not in a
   river, a count per region.

The atlas is immutable once built, and the generator closes over it, so
`Gen` stays a pure function of key and seed. Cost from the published
figures: about 8 µs a node an iteration, so a 512 by 512 grid is around
2 s an iteration single-threaded and 50 iterations are under two minutes.
The budget below asks for under a minute, so the solver runs on the
scheduler's workers by rows. A 256 by 256 grid at 8 m, for previews, is a
quarter of that.

A minute is not invisible, and the goldens, the soak and the tests are
each their own run. So the atlas is cached on disk as a development
artefact, not a saved world: one file under the consumer's build
directory, named by the seed and a hash of the generator definition,
rewritten when either changes. The solver starts in March; the fallback
in the risks below is C for its loop only. This is my call.

The atlas is also where every later game plugs in: a Timberborn-style map
is an atlas with water sources and sinks and no erosion; a city is a plan
layer on the same grid; roads are paths on it. None of that is built here.

### 4. The chunk: terrain from the atlas, detail from noise

The height of a column is the atlas height, sampled bilinearly, plus
detail noise whose amplitude is damped to zero within a river's half-width
and over a lake, so detail never moves water. A few passes of thermal
erosion over the chunk's 34 by 34 column heights (the chunk plus a one
column halo), with the talus angle from the same rock-strength noise, turn
ridged spikes into cliffs with scree at their feet.

Below the surface the chunk is a density field, solid where positive:

    density = (H - y) * squash + A * noise3(x, y, z)

with `H` the column height above, `squash` and `A` in metres, and the
squash-to-amplitude ratio the one knob for how cavernous the terrain is.
Keeping `H` in the density is what stops overhangs detaching from the
hill they belong to. The 3D noise is sampled on a lattice of 4 by 8 by 4
voxels (9 by 5 by 9 samples a chunk, 405 instead of 32,768) and filled
by trilinear interpolation written as flat arrays with precomputed
weights; the research found a naive interpolation loop erases the gain.

Caves are two terms subtracted from the density, both chunk-local:

- tunnels: `max(|a|, |b|) - t` for two 3D ridge noises, the intersection
  of two sheets, which is a winding tube one to a few metres wide;
- caverns: a low-frequency 3D noise thresholded, for rooms.

Both are gated to lie below the surface band except where an entrance
noise lets a tunnel cut the surface, and both have per-region
parameters, so a region's caves differ in character the way Lay of the
Land's do.

Materials come from a per-column decision tree over conditions that are
all functions of the column and of depth from the surface: region, depth,
steepness, altitude, under water, a noise threshold. The scan runs
top-down and resets its depth counter on air or water, so every surface
under an overhang gets its own top and filler layers and a one-voxel
ceiling stays rock. The scan needs the density a few voxels above the
chunk, which the lattice gives for free.

Early outs, because the Y axis streams: the fractal amplitude bounds the
density, so a chunk wholly above `max(H) + A / squash` is air and one
wholly below `min(H) - A / squash - cave depth` is solid, with no noise
evaluated. Uniform chunks stay `Chunk.uniform`.

### 5. Regions in place of biomes

A region id from the atlas chooses parameters, never height: cave
character, material tree, which features and templates, and their
densities. Because height is continuous across a region border and only
materials and features change, there is no seam to blend. The library
offers the region id and a distance to the nearest other region; the game
decides what a region is.

### 6. Features and templates

A feature is anything placed on the terrain: a tree, a boulder, a ruin.
Features are decided by hashed cells. The world is divided into cells of a
size in metres per feature kind; the hash of a cell's coordinates with the
kind's salt decides whether it holds a feature, which template, which
rotation and what offset within the cell (jitter at most 0.6 of the cell,
so spacings do not pile up). A chunk enumerates every cell overlapping its
bounds expanded by the kind's maximum radius, and writes only the voxels
inside itself. Neighbours agree because they ask the same hash. Padded
enumeration costs about 2.25 times the candidates at an 8-voxel radius
and more at 10 cm, which is why a feature's voxels come from a template
and not a per-instance procedure.

A template is a box of voxels with, per voxel, a block id and a
probability out of 255, plus per-slice probabilities and allowed rotations
(the four about Y), the record Luanti's schematics use. Templates come
from two sources:

- `.vox` files, through `ogt_vox.h` (single header, MIT) in
  `native/strata_vox.c`: the whole scene graph is walked with composed
  transforms, because a single model stops at 256 voxels a side, and a
  palette index maps to a block id through a table the game supplies, as
  Teardown does;
- the game's own code, run once at start-up, for trees and the like.

The owner decided the builder makes the ruins: a few placeholder models
(walls, a tower, an arch), released CC0, written as `.vox` so the import
path is proven, good enough to show the pipeline and no more.

A template is blitted with a mask, so its air does not cut the terrain,
and with its structure id (section 7) written to the voxels it places.

Structure sites from the atlas are features whose cell is the site. The
terrain under a site is adapted during section 4, not after: the column
heights inside the site's footprint are blended toward the site's floor
within a margin around it, the "beard" that Minecraft applies and that Lay
of the Land had to patch in. The atlas supplies sites; the chunk asks
whether one covers it before it computes heights.

Trees cross chunk borders for the first time. The viewer's current rule,
trees two voxels inside the chunk, goes.

### 7. Destruction

Carving and building exist: `Strata.Edit` spheres and boxes, dirty sets,
fluid wake-ups. This project adds what makes digging into a generated
world feel right and nothing more:

- a blast: a carve sphere with a radius and a shape noise on its surface,
  so a crater is not a ball, and a burst of the spray ring's particles in
  the colour of what was removed;
- a structure layer per chunk, so a later project can tell a ruin's
  voxels from the hill's and split what no longer stands. It is not
  stored: a second pure function of key and seed, `Strata.Terrain.stamps`,
  replays only the template pass for a chunk and returns which placed
  voxels belong to which site. Templates are cheap to replay, the chunk
  cache and the stream do not change, and a chunk nobody blasted never
  pays for it. An edit that removes a stamped voxel removes it from the
  replay's answer by comparison with the chunk;
- a regenerate: the viewer can drop every unedited chunk and remake it
  from a changed definition, for tuning, with the fluid state reset first
  (both Timberborn map tools warn that regenerating under live water
  breaks the simulation).

### 8. The generator as data, and the viewer

The chain above is a graph of named nodes with parameters in metres, loaded
from a file the game ships, so that a top-down image of any node (the
atlas height, drainage, a region map, a noise selector) can be written by
the demo and looked at, and so the viewer can reload the file and
regenerate without a rebuild. Every engine in the research that got good
terrain had this. The file format is the builder's choice; it must be
readable by March without new C.

The viewer gains an island seed, an atlas visualiser (one PPM a layer,
beside the goldens), a blast on left click and scenes for the goldens: a
river mouth, a lake above a river, a cliff with scree, a cave mouth, a
forest crossing a chunk border, a stamped ruin on a slope, a crater.

### 9. The far field

The owner chose to build the far field in this project. Its design is
Part F of `2026-11-10-renderer-design.md`, unchanged: rings of coarser
chunks, a level-L chunk still 32 voxels on a side with each voxel 2^L
times the world's scale, exactly one level drawing any region, departed
meshes kept until their parent's arrives, edits coarsened into their
ancestors, seams accepted and measured. This design adds only what Part F
asks of the generator, and the generator in metres makes it nearly free:

- `Strata.Terrain.Gen` takes a level as well as a key and seed, and a
  generator declares the highest level it supports. A level-L chunk is
  the same chain run at `scale * 2^L`: the atlas sampled at a coarser
  step, the detail noise at the same metre frequencies, and the water at
  the same levels.
- Levels above 0 skip what cannot be seen from afar: caves, features
  under a size in metres, the surface scan's filler layers. Level 1 keeps
  trees as a single canopy voxel colour; level 2 and above drop them.
  Sites keep their beards at every level so a ruin's hill stays.
- A coarse voxel is solid if the density at its centre is positive after
  the amplitude is reduced by the level, which approximates Part F's
  "solid if any fine voxel is solid" and keeps cracks to a voxel. The
  early-outs apply as before.
- The island's edge: beyond the atlas the generator returns sea at every
  level, so the far field ends at open water and the renderer's rings
  need no special case.

At 0.25 m the rings reach 80 m, 160 m, 320 m, 640 m and 1.3 km; a fifth
level reaches the far shore of a 2 km island. The level count and radii
are settings, as Part F says, and the reference scene stays Part F's
8400 chunks.

## Budget and targets

- Atlas build, 512 by 512 samples: under 60 s on the development machine
  with the scheduler's workers, under 15 s for the 256 by 256 preview.
- Chunk generation, release, 0.25 m per voxel, surface chunk with caves and
  features: 1 ms median, 3 ms worst. Air and solid chunks: under 20 µs.
- Streaming at 0.25 m with the current view radius holds the current
  frame budget; the soak passes.
- A fixed seed gives the same chunk hashes on every run on Apple
  silicon; x86 is unverified until a machine is available.
- The far field: Part F's reference scene of about 8400 chunks holds the
  frame budget; a level-1 chunk generates in under 1 ms; the island's far
  shore is visible from its highest hill at 0.25 m.
- The island's lakes are at rest: the soak runs the fluid simulation over
  a loaded island for a thousand steps and no lake loses more than one
  per cent of its water.

## Testing

- Noise: the C field externs against FastNoiseLite's own single-sample
  calls at random points; the hash mixer's distribution over a million
  cells; a golden hash of a fixed seed's chunks.
- Atlas: every land sample reaches the sea through receivers; every lake's
  level is below its spill height; rivers run monotonically downhill along
  their polylines; the same seed gives the same atlas hash.
- Chunk: the density early-outs agree with full evaluation on random
  chunks; a chunk generated alone equals the same chunk generated after
  its neighbours; features straddling a border are identical from both
  sides; the surface scan puts a top layer under every overhang.
- Templates: a `.vox` with a scene graph blits to the same voxels as its
  flattened form; rotations are exact.
- Destruction: a blast dirties only the chunks it touches; structure ids
  follow the stamped voxels and are cleared by carving.
- Far field: a level-L chunk's solid set contains the coarsening of its
  eight children's on random keys within a voxel; a crater carved at
  level 0 is visible in its level-2 ancestor; the which-chunk-draws rule
  never draws a region twice or not at all over a scripted walk.
- Goldens for the scenes in section 8 and a ring boundary, looked at
  before they are stored.

## Order of work

1. Noise substrate: the C header, the field externs, the seed mixer,
   determinism flags and the golden hash. Convert the viewer's and demo's
   terrain to metres with no change to what they draw, then re-store the
   goldens once on the new noise.
2. The atlas: mask, regions, erosion, priority-flood, rivers, lakes, the
   visualiser. The viewer's terrain reads atlas height and draws rivers
   and lakes as water at the atlas's levels: lakes as the finite lake
   water that drains when breached, rivers and the sea as sink water.
3. Density, caves, thermal erosion, the surface scan and early-outs.
4. Hashed-cell features, templates, `.vox` import, sites with beards.
5. Blasts, the structure layer, regenerate, the scenes and goldens, the
   soak over an island, measurements and the ledger.
6. The far field: levels in the generator contract, Part F's renderer
   work (chunk scaling, which-chunk-draws, retained meshes, coarsened
   edits), the ring-boundary golden, the 8400-chunk scene measured.

Each step leaves the viewer running and the goldens passing. Step 6 is a
renderer project inside a generation one and is last so that the island
is right before it is seen from afar.

## Left to the builder

- The node-graph file format and how much of the chain is data versus
  code in the first release.
- Exact atlas sample spacing and grid size, and whether the solver's rows
  are split across workers or the whole pass runs on one.
- The erosion constants beyond those named (uplift field shape, time step,
  iteration count) and the river threshold on drainage area.
- The lattice dimensions if 4 by 8 by 4 proves wrong at 0.25 m.
- Template storage in memory and whether tree templates are generated or
  imported for the demo.
- How the structure-id store and the fluid-level store share code.

## Risks

- The erosion solver in March may be slow enough to miss the minute. The
  fallback is a smaller grid and fewer iterations, then C for the solver
  loop only, behind the same interface.
- Lattice interpolation of 3D density written naively erases its gain; the
  research has one measurement showing this. The fill loop is written as
  flat arrays and measured before anything is built on it.
- The fluid simulation may still move water in a lake the atlas thinks is
  at rest, from detail noise or a cave meeting the bed. The soak catches
  it; the fix is the damping radius and gating caves away from lake beds.
- At 0.25 m and below the current lighting and fluid costs are unknown.
  The scale is a constant; the milestone measures before deciding.
- The far field is most of a renderer project and is scoped only by
  Part F. If it threatens the rest, the near field ships first and the
  far field becomes its own plan, which Part F already is.
- `ogt_vox.h` and FastNoiseLite are C99 single headers with no build of
  their own, but the consumer must list the new sources; the demo's and
  viewer's `forge.toml` are part of step 1 and step 4.

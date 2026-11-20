# Strata shapes: design (population stage 4, props)

Status: design, being built on branch `claude/hungry-pasteur-784291`.
Date: 2026-11-19
Follows: `2026-11-12-population-design.md` (sections 6 and 9, and stage 4
of its order of work) and `2026-11-11-physics-design.md`.
Research: a review of the IOLITE engine (the MIT-licensed legacy branch at
github.com/MissingDeadlines/iolite).

## Intent

On the 1 m island a tree is a stamp of 1 m voxels. The population design
planned props for this: larger plants and decorations as their own fine
voxel grids, static until disturbed, then carved voxel by voxel or handed
to physics. It planned them through the figure path, with falling left to
a later stage, because when it was written a body had no voxel size of its
own and could not be carved.

Both have since been built. `Strata.Physics` holds bodies at a half, a
quarter or an eighth of the world's voxel (`add_fine`), a body can be
pinned in place until nothing holds it, and `cut` carves a live grid and
splits what is left. The renderer draws a body at its own voxel size and
the traced lighting steps through its grid.

IOLITE's strongest idea is that everything except terrain is one kind of
voxel shape, with its own grid and voxel size, that is static, kinematic
or dynamic, and that the same shapes are what its animation moves. This
design adopts that idea: one shape type, shared by props, by loose physics
bodies and (later) by character parts. Stage 4 is then mostly a matter of
letting a body's grid stand still cheaply, in thousands, and of the
population emitting shapes instead of stamping voxels.

## Decisions taken from the brief

1. One shape type. A prop, a loose body and (later) a character part are
   the same thing in different modes.
2. A static prop knocked loose is the same shape switched to dynamic.
   Falling does not copy it.
3. `Strata.Support` and `Strata.Fracture` stay. IOLITE's rule (an island
   stays only if it touches the shape's bottom row) is cruder and is not
   copied.
4. No PhysX, no second physics.
5. Slow code is fixed in March with a reproducer; C is at most a marked
   stopgap. (The renderer is C and stays C: that is where the GPU is.)

Everything else below is the designer's call.

## Goals

- A population layer says `props` and its instances stand as fine grids
  on a coarser world, with no other change to the declaration.
- An unedited prop holds no grid of its own: it is a few words naming a
  shared shape and a pose.
- A forest of props is drawn from shared meshes, shadowed and shaded by
  the same rays as the world, and found by rays cast by a game.
- A blast carves a prop per fine voxel; what the carving leaves hanging
  falls as a body made of the same grid.
- A carved prop is remembered with its chunk and is in the save.

## Non-goals

- Character parts as shapes. The type is made so they can join (section
  1); `Strata.Rig` and the figure path are not changed here.
- Wind or animation of props.
- Exact fine-voxel collision for walkers and entities. They meet a prop
  through boxes (section 8), which this pass specifies and builds last.
- Growth on surfaces (stage 5) and falling building parts (stage 8).
  Section 12 says how each follows.

## Constraints

- March main; the gaps in `GAPS.md` apply.
- Another session is repairing three physics goldens and a slow physics
  test. This work adds to `lib/strata/physics.march` only at its edges
  (new public calls beside `add_fine`) and changes none of its stepping.
- The existing goldens show stamped trees. The viewer therefore keeps
  stamps by default and places props when asked (`STRATA_PROPS=1`); the
  owner flips the default when the goldens are re-stored.

## The design

### 1. A shape

A shape is:

- **a grid**: a box of voxels, at most 128 on a side, each a block id of
  the game's registry. This is `Strata.Body.Grid`, unchanged;
- **a voxel size** in metres. A population layer sets it (`detail`); the
  default is 0.25 m. Physics accepts the world's voxel divided by 1, 2, 4
  or 8, so a layer's detail must be one of those for its props to fall;
- **a pose**: where the grid's low corner is, in metres, and a rotation
  about that corner. A placed prop's rotation is a quarter turn about Y;
  a body's is any;
- **a mode**: static, kinematic or dynamic;
- **a palette**: the distinct block ids the grid holds, at most 255, air
  being entry 0.

`Strata.Shape` holds the type and what can be done to one shape alone:
its palette, its bounds, a ray through it, a sphere or box carved out of
it, its split into parts, and its bytes for a save.

**Why block ids and not a palette byte in memory.** IOLITE keeps one
palette byte a voxel. Strata's physics, fracture, support, mesher and
light classes all read block ids, and a body cut from the world is a
piece of the world's ids. Keeping ids in memory means a grid moves
between the world, a prop and a body with no translation, and physics
does not change. The palette is where a byte a voxel pays: on disk (a
shape is saved as its palette and one byte a voxel, run-length encoded)
and for character parts, whose voxels are already palette bytes
(`Strata.Rig`): a rig part becomes a shape by giving its colours as the
palette of a shape whose mode is kinematic. That step is not taken here.

**Shared grids.** A grid is an immutable value. Every unedited instance of
a tree variant holds the same grid; carving makes a new one for that
instance alone.

### 2. Modes, and how a body's grid is a shape

- **static**: the shape stands where it is and nothing steps it. It is
  held in `Strata.Shapes` (section 3) and physics does not know it.
- **dynamic**: physics moves it. It is a `Strata.Physics` body: the
  body's `grid`, `vs` and pose are the shape, and the rest of the body
  record is its motion.
- **kinematic**: something else moves it and it pushes what it meets.
  Physics' `pinned` and `driven` kinds are this mode today; an animated
  part is this mode later.

Today's `Strata.Body` grids therefore do not become something else: a
body always was a grid, a voxel size and a pose. What changes is that the
triple has a name and can exist outside physics. Two calls join them:

- `Strata.Physics.add_shape(p, shape, kind)`: a body made of the shape's
  own grid value at its pose and voxel size. Nothing is resampled or
  copied; the body and the prop it came from share the grid until one of
  them is carved.
- `Strata.Physics.shape_of(p, id)`: a body's grid, voxel size and pose as
  a shape, for a body that has come to rest and is to stand as a static
  shape again (section 7).

Switching a static prop to dynamic is: take it out of the shapes, hand
the same value to `add_shape`. The prop's place in its chunk is marked
gone (section 6), so it does not come back when the chunk reloads.

Why static shapes are not pinned bodies: physics keeps its bodies in a
list and walks it several times a step, and caps them at 4,096. A forest
is more than that and must cost nothing while nothing moves.

### 3. The shapes of a world

`Strata.Shapes` is what a game keeps beside its stream and physics. It
holds:

- **prototypes**: the grids of the population's prop shape sets, one per
  variant, each with its voxel size. Registered once at start-up;
- **the standing shapes** of each loaded chunk: for each, an id, the
  chunk, the instance's hash, and either a prototype and a pose (an
  unedited prop) or a grid of its own;
- **the memory**: per chunk, what has happened to its props (section 6).

A shape's id is given when its chunk loads and is good until the chunk
unloads. Its hash, with its chunk, names it for good.

### 4. Placed by the population

A layer becomes a prop layer by naming a second shape set, the same
variants at the detail scale: `Strata.Population.props(p, layer,
fine_shapes)`, and `props = NAME` on a layer in a population file. The
detail scale is the voxel size of that set's templates. `freeze` checks
that the set exists, holds templates of one voxel size, and has as many
variants as the layer's own set. `fit_all` leaves a prop set at its own
size and checks that the world's voxel is 1, 2, 4 or 8 of it.

The generators take the voxel size they are asked for, so the game builds
the set twice, as the population design says: once at the world's scale
(the coarse stamps of the far field) and once at the detail scale.

At a world whose voxel is the detail scale or finer, a prop layer is an
ordinary stamp layer: the declarations are the same in both worlds.
Otherwise:

- at level 0, `Strata.Populate.place_with` does not stamp the layer;
- `Strata.Populate.props_with(plan, seed, key, ground, table)` lists the
  layer's instances whose root voxel is in the chunk: layer, variant,
  place, top, turn and hash. It is `instances_with` narrowed to the
  chunk, so a prop stands exactly where its stamp would;
- above level 0 the layer is stamped coarse as before, so the forest is
  still there on the horizon.

A prop's pose follows from its instance: the fine grid is turned by the
instance's quarter turn, centred on the middle of the instance's column,
its low corner snapped to a whole fine voxel, its bottom row on the
voxel above the column's top.

A game's generator carries the listing as a third function beside its
chunk and its stamps: `Strata.Terrain.with_props(gen, Props(fn (key,
seed) -> ...))`, pure like the other two. `Strata.Shapes.sync` asks it
for each level-0 chunk the stream has loaded, on a worker task, and drops
a chunk's shapes when the chunk unloads. The stream itself is not
changed: it reports what loaded and unloaded (`take_changed`), and the
props of a chunk arrive a frame or two after its voxels.

Site layers (the ruins) can be prop layers by the same declaration. This
pass builds scatter; a site layer's `props` is refused by `freeze` until
it is built.

### 5. Drawn, shadowed, lit

Three mechanisms exist and were weighed.

- **The far field's per-chunk scale.** A slot is drawn with voxels of
  `2^level` world voxels. It cannot go below the world's voxel, has no
  rotation and sits on the chunk grid. Not suitable.
- **The figure path.** Per-rig voxel size, shared part shapes, sun
  shadows by shadow map. But its voxels are palette colours, not the
  game's textured blocks; at most 64 figures are in the traced rays, so
  a forest would cast shadow-map shadows only and no sky shade; and a
  falling tree would change from a figure to a body mid-fall and look
  different. Not chosen.
- **The body path.** A body part is a chunk-format mesh in a renderer
  slot, drawn at a pose with its own voxel size, in the game's textures;
  its grid of light classes is in the table the rays step through, so it
  is shadowed, shades the ground from the sky and is reflected exactly
  as the world is. This is what a prop becomes when it falls.

Props use the body path, with the two things it lacks for a forest:

- **Shared meshes.** A prototype's grid is meshed once, in 32-cubed
  parts, into slots of its own that are never drawn. An instance's slot
  takes the prototype's mesh ranges and its own pose
  (`sv_body_instance`, beside the existing `sv_chunk_instance`). A
  thousand oaks of one variant are one mesh.
- **Shared ray shapes.** `sv_body_share` gives a body row the class
  bytes of another, so the rays' 64 MB of shapes holds each prototype
  once.

A carved prop gets a mesh and class bytes of its own, as a body does.

Shadows and shade need nothing more: the light pass traces the sun and
the sky through the body table. The population design's undrawn leaf
occupancy in the light volume is not needed and is not built. A prop is
therefore lit as bodies are: in light modes that trace. In mode 1 (sun
and sky without rays) a prop casts no shadow, as a body casts none.

Past level 0 a prop is not drawn; the far field's chunks hold the coarse
stamp.

**Limits this sets.** The renderer's body table holds 16,384 rows and
each instance part takes a slot. The viewer's slots are raised to fit
(section 10 gives the count).

### 6. Carved, and remembered

An edit reaches a prop by its region, not by its change records, because
a prop stands in the world's air and an edit of the world records no
change there. So:

- `Strata.Shapes.carve_sphere(shapes, x, y, z, r)` (metres) takes every
  voxel of every standing shape whose centre is within the sphere, as
  `Strata.Physics.cut` does for bodies. A game calls both beside
  `Strata.Edit.blast`. `carve_box` is the same for a box.
- `Strata.Shapes.unrooted(shapes, world, outcome)`: after an edit of the
  world, the standing shapes whose bounds meet the edit and that the
  ground no longer holds.

**Held.** A shape is held when one of its bottom voxels (a solid voxel
with nothing solid under it in its own grid) lies on a solid world voxel:
physics' rule for pinned bodies (`held`), moved to `Strata.Shape` so both
use one. After a carve the grid is split into face-connected parts
(`Strata.Body.split`); the parts that are held stay standing, as one
shape with one grid; every other part is loose.

**Seams.** No per-face "disconnected" bit is added. Connection by faces
and the held rule cut a trunk where it is cut; the bit would only serve
authored break lines, which nothing here needs yet.

**Loose.** A loose part is handed to physics with `add_shape` as a
dynamic body at the pose it had, in the same step. From then on it is a
body: `Strata.Fracture` breaks it on impact and `cut` carves it. Parts
smaller than 8 voxels are fragments, as physics has it.

**Remembered.** A chunk's memory is a short list keyed by the instance's
hash: `gone` (the whole prop left), or `carved` with the grid that
stands. It is consulted when the chunk's props arrive, so an edited prop
survives its chunk unloading and returning, and an unedited prop costs
nothing to keep.

### 7. Come to rest

A fine body never becomes world blocks; it sleeps where it landed. For
it to be in a save, and to free physics' caps, a fine body that has slept
is taken out of physics (`shape_of`, `remove`) and stands as a static
shape at the pose it fell to, recorded in the memory of the chunk its
centre is in as `settled`: a grid and a full pose. `Strata.Shapes.settle`
does this for the bodies a game hands it. A settled shape is carved,
hit by rays and saved like any other.

This pass specifies it; it is built after the save (see the plan), and
until then a fallen part stays a sleeping body and is not saved.

### 8. Found, and solid

- `Strata.Shapes.raycast(shapes, origin, direction, max)` answers the
  nearest standing shape a ray enters: its id, the voxel, the face and
  the distance. It tests the boxes of the shapes in the chunks the ray
  crosses, then steps the grid in the shape's own frame.
- `Strata.Query.raycast` stays the world's. A game takes the nearer of
  the world's hit, `Strata.Physics.raycast` (bodies) and the shapes'.
  `Strata.Shapes.raycast_all` does that for the common case.

Solidity is by proxy boxes fitted to a prototype's solid, non-leaf
voxels, as the population design rules; walkers and entities sweep
against them. Bodies meet props by promotion: a static shape within
reach of an awake body is handed to physics as a pinned body (the same
grid) and taken back when everything near it sleeps. Both are specified
here and built last; until then a walker and a falling trunk pass
through a standing prop.

### 9. Saved

`Strata.Save` already keeps named sections per region
(`set_region_section`). A region's section `shapes` holds the memory of
its chunks: for each chunk with any, its key and its records (hash and
`gone`, or hash and a grid, or a settled shape with its pose). A grid is
written as its sizes, voxel size, palette and run-length-encoded palette
bytes. `Strata.Shapes.save(shapes, store)` sets the section of each
region whose memory changed; `sync` reads a region's section the first
time a chunk of that region loads. Decoding answers a `Result` and never
panics; a section that does not decode is dropped and counted.

### 10. Budget

- Listing a chunk's props: within the scatter layer's cost today,
  measured, on a worker.
- A standing prop: no per-frame cost in March. In the renderer, one
  body-table row a frame.
- 5,000 static props in view at 0.25 m on the 1 m island hold the frame
  budget in light mode 4. (The population design asked for 20,000; the
  body table holds 16,384 rows and the island's near field holds fewer
  trees than that. The number is measured and the gap stated.)
- A blast on a prop: carved and drawn within two frames, the March work
  under 5 ms for an 8 m tree.
- Syncing shapes with the stream: under 0.2 ms of main thread a frame
  when nothing loads.

Each is measured with the load average beside it and not judged above a
load of 8. Results go in `RESULTS.md`.

### 11. Testing

- Shape: a ray through a turned shape hits the voxel a brute-force scan
  finds; a carve takes exactly the voxels within the sphere; bytes
  decode to the same grid, and hostile bytes answer `Err`.
- Population: `props_with` over a chunk equals `instances_with` narrowed
  to it; the union over the chunks of a column loses and repeats none; a
  prop layer stamps nothing at level 0 and stamps as before above; the
  same declaration in a world at the detail scale stamps.
- Shapes: an unedited prop holds no grid; a carved prop survives its
  chunk leaving and returning; the standing part is held; a prop whose
  ground is dug away is loose; a cut trunk gives a body sharing the
  prop's voxels above the cut.
- Save: carve, save, load: the same shapes stand.
- The viewer: a golden scene of the forest as props, and one of a cut
  tree fallen.

### 12. What follows

- **Stage 5 (growth on surfaces).** Attach, hang and climb place shapes
  against other shapes. With one type, a vine on a trunk is a shape
  whose pose is given in its host's frame, and the host's hash is its
  parent in the memory.
- **Stage 8 (falling parts).** A building stamped as a prop falls by the
  same carve and `add_shape`. A building stamped into the world falls by
  `Strata.Support` and physics as today.
- **Character parts.** A rig part given as a shape in kinematic mode,
  drawn by the body path or the figure path as its palette says.

## Risks

- **Rows and slots.** Every instance part is a slot and every instance a
  body row. A dense forest at a large radius exceeds 16,384. The viewer
  measures what it uses; raising the table is a constant in C.
- **Light history.** The renderer treats a body that appears as having
  moved. Thousands appearing as chunks load may unsettle the light for a
  few frames. Measured in the viewer.
- **Two sessions on physics.** The additions are at the edges; the merge
  from main is done before the final gate.

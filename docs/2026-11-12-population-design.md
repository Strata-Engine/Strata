# Strata population: design

Status: design, awaiting approval. Nothing here is built.
Date: 2026-11-12
Follows: `2026-11-11-generation-design.md` (the island, the atlas, features,
templates) and builds on the animation, entities and physics designs of
2026-11-11.
Research: `reports/Procedural generation for Strata.md` and its notes; the
survey of free-standing grids on main in the generation ledger.

## Intent

The island has trees and ruins because the viewer wires them in by hand: a
tree kind here, a site loop there. Every new thing a world should hold, a
fern, a boulder, a lamp post, a house, would be more hand wiring. This
project replaces the wiring with one system. A game declares what exists,
where it may stand and how densely, and the engine places it: ground cover,
plants large and small, rocks, decorations, buildings. The engine supplies
mechanisms and names nothing. The first content written against it, as a
game would write it, is a living island: grass, flowers, ferns and
mushrooms underfoot, shrubs and better trees, giant mushrooms and
vine-hung caves; and a small set of urban tools.

## Decisions the owner made

1. This is a generic engine system. Games provide values, templates, zones
   and rules to generate their own plants, buildings, rocks, decorations
   and whatever else.
2. Authoring is hybrid: a March API builds declarations at start-up; the
   engine evaluates the frozen result; a data-file loader sits on top for
   tuning and reloading.
3. The first content aims at both ground cover and showpiece flora.
4. Plants are drawn at 0.25 m by default, and the game can set that. The
   shape is combined: small ground cover is cross or model blocks; larger
   plants are their own fine voxel grids, static until disturbed, then
   carved voxel by voxel or handed to physics.
5. Trees gain a space-colonisation generator.
6. For cities the engine offers tools a game can use, for a city builder
   in the manner of SimCity or for small urban FPS maps. It does not
   become a city-builder engine.
7. Enterable buildings are an option of the building tool.
8. From earlier designs: the library names no game concept; a chunk is a
   pure function of its key and the seed; no game code runs on a hot
   path; frame rate stays the priority.

Everything else below is the designer's call.

## Goals

- A game adds a new kind of thing to its world by declaring it, with no
  change to the engine and no code that runs per chunk.
- The same declaration works at any world scale and at any detail scale,
  because every number in it is in metres.
- What stands where is deterministic and chunk-local: two chunks agree
  about a plant on their border without asking each other, and a layer
  can depend on another layer (ferns under canopy) the same way.
- Ground cover costs almost nothing per column. A forest of fine-voxel
  plants costs no more to draw than the same forest of figures.
- A plant or building that is blasted carves like the world does, and the
  carving is remembered like an edited chunk.
- The urban tools work both when a world is generated and while a game
  runs, as edits.
- Trees and ruins move onto the system and the viewer's hand wiring goes.

## Non-goals

- A whole-city planner, districts, traffic, demand, economy, citizens,
  addresses or routines. The tools return the data a game needs to build
  those.
- Growth over time, seasons, or plants that spread. A layer is a function
  of the seed; a game that wants growth changes what it declares.
- Wind and animation of fine-voxel plants. Cross blocks sway in the
  vertex shader as grass does today.
- Falling trees in the first stages. They need a body with its own voxel
  size and the fracture the physics design has not built; this design
  says where they join.
- Wave function collapse. Box grammars and authored kits cover buildings;
  the hook for another generator kind is there.
- An editor.

## Anti-goals

- A game callback evaluated per column, per voxel or per instance.
- A rule language so general it is a second programming language. Rules
  are sums and products of curves over fields, and comparisons.
- A module in `lib/strata` that names a plant, a rock or a house.
- A second placement path beside this one. When this lands, nothing in
  the viewer places a feature by hand.
- Detail that only exists at one voxel scale.

## Constraints

- This is planned and built on main after the far field (generation plan
  Task 12) has landed there, because layers and props are declared per
  level.

- March `origin/main`; the gaps in `GAPS.md` apply, most of all that a
  shared value must not be threaded through a hot loop (S15) and that
  `&&` and `||` do not guard a read (S16).
- Island chunk generation is over its budget today and has only been
  measured on a loaded machine. This project must not start until a
  clean measurement exists, and every stage reports what it adds per
  chunk.
- Fine grids are drawn and lit by the figure path the animation design
  built: per-rig voxel size, shared shapes, sun shadows by shadow map, at
  most 64 in the traced rays, 4,096 live figures today. Bodies have no
  voxel size of their own, cannot be carved once made, and are capped at
  256 awake.
- March first. Pushing March's performance is one of the engine's goals,
  so a measured gap becomes a reduced case for the March work with a C
  comparison beside it, not a move to C. C is at most a marked stopgap
  that names the gap it works around and keeps the March version to
  switch back to.

## The design

### 1. The shape of it

A game builds one value at start-up, a `Population`, and hands it to its
generator. It holds four kinds of declaration:

- **fields**: named numbers a rule can read at a place;
- **zones**: named areas a rule can be limited to;
- **shapes**: the things that can be placed, each made once by a
  generator or imported;
- **layers**: what is placed, by which placer, under which rule, in what
  order.

`Strata.Population.freeze` checks the declarations and compiles them into
flat tables. From then on the population is immutable data that the chunk
generator closes over, as it closes over the atlas. Game code has stopped
running by the time the first chunk is asked for.

**Where it joins generation.** A game's terrain chain stays the game's:
the island's heights, density, caves and materials remain in the viewer,
and nothing of them moves into the library. The generator calls the
population at two points and hands it what it has computed. This is my
call; the alternative, moving the island chain into the library, would
make a game concept of it.

- `Population.ground(p, key, seed, heights)`: before the density is
  built. The game passes its column heights for the chunk and its margin;
  the population returns them adjusted under sites, footprints and roads.
- `Population.place(p, key, seed, chunk, ground)`: after the chunk's
  voxels and materials are filled. `ground` is a record of arrays the
  game fills from what it already knows (surface height, the surfaces of
  section 2, water line, slope, and one array per built-in field it can
  supply). The population returns the chunk with blocks and stamps
  written, and the chunk's list of props.

A built-in field a game does not supply reads as zero, and `freeze`
reports any rule that uses one.

### 2. Fields

A field is a number per ground column, in metres or in the unit range.
The engine evaluates every field a population uses once per chunk, into
an array the chunk's task owns, and rules read those arrays.

Underground is handled by surfaces, not by a third dimension of fields.
The surface scan already finds every floor and ceiling in a column; the
game passes them as a short list per column (height, floor or ceiling,
depth below the top surface, whether the sky is open above). A rule is
evaluated per surface: the column's fields plus that surface's own
depth and openness. So a cave with three floors in one column gets three
answers, and field storage stays two-dimensional. This is my call.

Built in, from what generation already computes: altitude, slope, depth
of water above, distance to a river, distance to a lake, distance to the
coast, drainage (the atlas's measure of how wet the ground is), region
id, depth below the surface for underground columns, open sky above.

Declared by the game:

- a noise field: kind, octaves, period in metres, seed salt;
- a blend of other fields through a curve;
- a cover field: how much of a named layer stands near this column,
  within a radius. Because placement is a pure hash, cover is computable
  by enumerating that layer's candidates around the chunk, with no
  neighbour state. This is what lets one layer depend on another.

### 3. Zones

A zone answers yes or no for a column, or for a voxel when it is
underground. Kinds: a set of region ids; a band of a field (within 12 m
of a river; above 95 m); the ground inside or around an atlas site;
underground at a depth range; a polygon in metres (a lot, a park); and
unions, intersections and complements of zones. Zones compile to
conditions over field arrays, so they cost what fields cost.

### 4. Rules

A rule gives a layer its density and its limits. It is data: a product
of curves, each curve a few points mapping a field's value to a factor
between 0 and 1, times a base density per square metre; plus filters,
each a comparison of a field with a number or membership of a zone. No
branching beyond that, no loops, no game functions.

`freeze` turns every rule into a short program over field arrays. For a
chunk the engine runs each program once across its columns. The
vocabulary is deliberately the one Minecraft's placed features and
Luanti's decorations settled on, because games compose those without
inventing logic.

### 5. Shapes and generators

A shape is a small box of voxels with a voxel size in metres, holding
either block ids from the game's registry or palette colours. A shape
set is several variants of one thing; an instance picks one by its hash.

A layer may weight its variants by zone (one layer of trees whose mix
of species differs by region), and each layer has a salt the game can set,
so a port can keep the hashes it had.

A generator makes shapes from parameters at start-up. Kinds, each a
library mechanism with every length in metres:

- **import**: a `.vox` file, as today;
- **branching**: trunk, tapering branches, leaf masses; the generator the
  trees use now;
- **colonise**: space colonisation. Attraction points are scattered in an
  envelope (a few envelope kinds: ellipsoid, cone, flattened dome, a
  hanging curtain); branches grow from the root toward the points they
  are nearest, points die when reached, radii come from the pipe model.
  Trunks, crowns that avoid themselves, and shrubs with a low envelope
  and several roots all come from this one kind;
- **revolve**: a profile turned about an axis with a bend: stems and
  caps;
- **rosette**: arcs from a centre, each drooping: ferns, tussocks, palms;
- **blob**: an ellipsoid displaced by noise, cut flat underneath: stones
  and boulders;
- **grammar**: a box split into floors, bays and openings by rules: the
  building tool of section 10;
- **made by the game**: a March function that returns a shape. It runs at
  start-up only.

Generators take the voxel size they are asked for. A shape set is built
at each scale it is used at.

### 6. How a placed thing exists in the world

Three representations, chosen by the engine from the shape's size and
the layer's detail scale, and overridable by the game:

- **block**: the shape fits in one world voxel. The engine registers it
  as a cross or model block and the layer writes block ids into the
  chunk. This is ground cover. It costs a voxel write.
- **stamp**: the layer's detail scale is the world's scale. The shape is
  blitted into the chunk as a template, as trees are today, and is world
  voxels from then on.
- **prop**: the detail scale is finer than the world's. The instance
  keeps its own grid. Section 9 says how it is drawn, lit, carved and
  remembered.

A layer's detail scale defaults to 0.25 m. A world at 0.25 m therefore
stamps everything; a world at 1 m gets props. The declarations are the
same in both.

On the viewer's 1 m island this means that once props exist (stage 4),
the trees and large plants become props, as the owner's decision on
detail scale implies. Until then they are stamps at the world's scale.
Section 9 keeps what stamps give today: blocking, shade under the
canopy, carving, and a presence in the far field.

### 7. Placers

A placer decides where instances of a layer stand. Each is a pure
function of world position, seed and the layer's tables:

- **cover**: per surface, a hash against the rule's density picks
  whether it holds something and which variant. For blocks. Cover layers
  are exclusive in their declared order: an earlier layer's block keeps
  its place.
- **scatter**: hashed cells with jitter, at most one instance a cell, a
  padded enumeration so instances cross chunk borders. This is
  `Strata.Feature` as built.
- **patch**: scatter whose density is gated by a cellular noise, so
  things come in clumps and rings with gaps between.
- **site**: the atlas chooses places with constraints a chunk cannot
  check, and the ground is drawn up to meet the footprint. As built for
  ruins.
- **attach**: instances stand on, under or against the instances of
  another layer: on a trunk, under a canopy's edge, on top of a rock. The
  host layer's candidates are enumerated by the same hash.
- **hang**: from a ceiling or an overhang, a column downward of a hashed
  length. For underground and for canopies.
- **climb**: from a hashed start on a surface, a bounded walk over solid
  faces under a few pulls (up, down, toward light, random). The walk
  reads the chunk generator's density, not neighbouring chunks, and its
  length bounds the padding. For vines and roots.

### 8. Layers and their order

A layer names a shape set, a placer, a rule, a detail scale and its
dependencies. `freeze` orders layers so that a layer comes after every
layer whose cover it reads or whose instances it attaches to, and refuses
a cycle. Within a chunk, layers run in that order.

### 9. Props

A chunk's props are a pure function of its key and the seed: the list of
instances whose root stands in it, each a position, a rotation, a shape
variant and a hash. The stream yields the list with the chunk and drops
it with the chunk.

**Drawn and lit.** Props go through the figure path with a static pose:
one shape is one rig part, shared by every instance, at the layer's voxel
size. They cast sun shadows through the shadow map. The figure budget is
4,096 live figures, which a forest exceeds; this design adds a batch of
static instances beside the animated figures, drawn the same way with no
per-frame update, sized by the far field's rings. Beyond the near ring a
prop is drawn as its coarse stamp in the far field's chunks, as trees are
at level 1.

**Levels.** Each layer says how far it is seen. Block layers exist at
level 0 only. Stamp and prop layers are written into coarser chunks as a
coarse stamp for as long as the shape is at least two coarse voxels
across, and dropped beyond; a layer may set the level itself.

**Found.** Props have their own index, a list per loaded chunk with
boxes, and do not use `Strata.Entities`, so a game need not open
entities to have plants and a forest does not spend entity rows. A ray
walks the chunks it crosses, tests every box on its way in order, then
the exact voxel by the shape's own grid.

**Solid.** A prop blocks movement through a proxy: at start-up each
shape gets a few boxes fitted to its solid voxels (a trunk, a cap, a
boulder), and leaves and thin parts get none. Walkers, entities and
bodies collide with the boxes; water ignores props. So a forest of props
is walked around, not through, and brushed past at the leaves. This is
my call, and the cheapest form of solidity: exact fine-voxel collision
in the entities kernel, in physics contacts and in fluid is several
times the work and is not in this design.

**Shade.** A prop draws its own sun shadow through the shadow map, but
the sky light under a canopy comes from the world's light volume, which
props are not in. So each prop also writes a coarse, undrawn occupancy
of its leaf mass into the light volume, at the world's scale, and the
ground under a forest of props is as dim as under stamped trees. My
call.

**Edits that reach a prop.** Any edit that removes voxels inside a
prop's box carves it. An edit that places blocks inside the box does not
change the prop. If the ground under a prop's root voxels is removed,
the whole prop is loose (below).

**Carved.** A blast that reaches a prop makes it an edited prop: its
shape is copied into a grid of its own, the blast removes voxels, and
the grid is split into connected parts from its root voxels. The rooted
part stays, drawn from its own grid. Edited props are remembered with
their chunk, keyed by the instance's hash, the way edited chunks are, so
an unedited prop costs nothing to keep.

**Fallen.** Parts no longer joined to the root should become bodies and
fall. That needs a body with its own voxel size and carving of a live
grid, which the physics design has not built. Until it is, loose parts
become spray and are gone. The join is one call, "make this grid a body
at this pose and scale", and this design asks the physics work for it
rather than building a second physics.

### 10. Urban tools

Three small mechanisms and one generator kind, each with two faces: a
declaration evaluated at generation, and an edit applied while a game
runs that returns the ordinary outcome.

- **Roads.** A graph of nodes and segments in metres, each segment with a
  width and a surface id. The engine rasterises it into voxels, draws
  the ground to it within a margin, cuts or fills along slopes up to a
  grade, and bridges water. It returns the graph, so a game can route on
  it.
- **Blocks and lots.** From a road graph, the enclosed blocks; from a
  block, lots by recursive splitting of its oriented box, each lot
  touching a road. Lots are polygons, so they are zones.
- **Ground adaptation.** Flatten, terrace or cut under any footprint.
  The beard under ruins becomes one use of this.
- **The grammar generator.** A lot and a style give a building, at the
  level the style asks for:
  1. a shell: mass, roof, door and window positions marked;
  2. hollow: floors, stairwells aligned between storeys, real openings;
  3. rooms: interior walls and doors, placed greedily by score with the
     rule that every room is reachable from the entrance;
  4. furnished: each room is a zone, and ordinary layers fill it.
  Each level returns its data: openings, the stair graph, the room
  graph.

A template can also be stamped as an edit, which is the one new edit this
needs.

What the two faces promise to share: the structure's own voxels (the
road surface, the building) and the level of the ground under and around
it. At generation the game's materials and caves follow the adjusted
heights. At run time an edit only sets voxels, so cut and filled ground
takes a material the tool is given, and caves are not re-derived. My
call; matching the terrain voxel for voxel would mean re-running a
game's generator inside an edit.

### 11. What a game writes

In March, at start-up, in the shape of:

    let p0 = Population.new()
    let moist = Population.field_curve(p0, "moist", drainage(), curve(...))
    let wood = Population.zone_regions(moist, "wood", [0, 2])
    let oaks = Population.shapes(wood, "oak", colonise(oak_values()), 6)
    let p1 = Population.layer(oaks, "oaks", scatter(6.0, 0.6),
               rule(0.02, [by("moist", wet_curve())], [inside("wood")]))
    Population.freeze(p1)

The names and exact signatures are the builder's; the shape is the
point: values in, a frozen population out.

The data file is the same declarations in text, one section per field,
zone, shape set and layer, with the generator's values under its shape
set. It is the island's parameter file grown to hold structure. The
viewer reloads it on regenerate. A shape made by a game function cannot
be named in the file; everything else can.

### 12. What moves

The viewer's trees become a layer over a colonise shape set (broadleaf)
and a branching one (conifers). Its ruins become a site layer over
imported shapes. Its tall grass and lantern posts become cover layers.
`Strata.Feature` and `Strata.Template` stay as the mechanisms under
scatter and stamp. The hand wiring in the viewer is deleted in the stage
that replaces it.

The port must look the same, not be the same voxel for voxel: goldens
are compared, looked at, and re-stored where only the placement hash
moved. Zone-weighted variants, exclusive cover and settable salts exist
so the difference is small. My call.

## Budget and targets

- Fields and rules for a surface chunk: 0.1 ms. Cover layers: 0.1 ms for
  all of them together. Scatter and stamp layers: what trees cost today,
  measured first.
- Building a population at start-up, with 12 shape sets of 6 variants at
  0.25 m: under 2 s; a colonise tree of 8 m: under 50 ms.
- 20,000 static props in view at 0.25 m in a 1 m world hold the frame
  budget, with sun shadows.
- A blast on a prop: the edited prop is drawn carved within two frames,
  and costs no more than a blast on world voxels of the same size.
- A road of 200 m rasterised as an edit: under 20 ms off the frame.

Each is measured with the load average beside it, and is not judged
above a load of 8.

## Testing

- Rules: a compiled rule equals its straightforward evaluation on random
  fields; a filter never passes a column outside its zone.
- Placement: every placer gives the same instances for a box and for its
  halves; a layer that reads cover gives the same result whichever chunk
  asks; `freeze` refuses a cycle.
- Shapes: each generator kind is connected from its root, stands on its
  base, fits its declared bounds, and is the same for the same seed; a
  colonise crown has no branch passing through another.
- Representations: the same declaration at world scale 1 m and 0.25 m
  puts the same things in the same places.
- Props: an edited prop survives its chunk unloading and returning; the
  rooted part is one connected piece; an unedited prop holds no grid.
- Urban: a rasterised road is continuous across chunk borders; every lot
  touches a road; at level 3 every room is reachable; the generation
  face and the edit face of each tool produce the same structure voxels
  and the same ground level.
- Underground: a column with three cave floors gets three independent
  answers from a cover layer; a hang layer starts only from ceilings.
- Props: a walker cannot pass through a trunk's proxy and can pass
  through leaves; the sky light under a prop canopy is within a tenth of
  the same canopy stamped.
- The port: the island's forest and ruins before and after are compared
  by golden, and the viewer holds no placement code of its own.

## Order of work

Each stage ends with something to look at and a measurement.

1. **The core.** Fields, zones, rules, `freeze`, layers and their order;
   the cover, scatter and site placers; the block and stamp
   representations; the data file. Trees, ruins, tall grass and posts
   moved onto it with no change to what is drawn.
2. **Ground cover.** Patch placer; rosette, revolve and blob generators;
   a first set of content: grasses by moisture, flowers in patches,
   ferns in shade, small mushrooms, stones. Cover fields.
3. **Better large plants.** The colonise generator; shrubs; giant
   mushrooms; all as stamps at the world's scale.
4. **Props.** Detail scale; static fine grids drawn and shadowed; found
   by rays; carved and remembered.
5. **Growth on surfaces.** Attach, hang and climb; vine-hung caves and
   canopies.
6. **Urban tools, part one.** Roads, blocks and lots, ground adaptation,
   the grammar at levels 1 and 2, the stamp edit; a small generated
   urban map and a road drawn at run time as the proofs.
7. **Urban tools, part two.** Levels 3 and 4.
8. **Falling.** With the physics work: loose parts of props and of
   buildings become bodies.

Stages 1 to 3 need nothing that does not exist. Stage 4 extends the
figure path. Stage 8 waits on physics.

## Left to the builder

- Names and signatures of the API, and the data file's syntax.
- How fields are stored per chunk and how rule programs are encoded.
- The envelope kinds and constants of the colonise generator.
- How the static prop batch shares code with figures, and how the
  proxy boxes reach the walker, the entities kernel and physics.
- The grammar's rule format.

## Risks

- **Chunk time.** Generation is already over budget. Fields and cover are
  cheap by design, but scatter with cover dependencies enumerates
  candidates more than once. Stage 1 measures before anything is added.
- **Props are the largest unknown.** Twenty thousand static fine grids
  need a batch the figure path does not have, and their place in the
  traced lighting is limited to shadows from the sun. If the batch proves
  hard, stage 4 falls back to stamps at the world's scale and props wait.
- **The rule vocabulary may prove too small.** The escape is a field made
  by the game at start-up as a table over the atlas, not a callback.
- **Two sessions on one seam.** Carving and falling overlap the physics
  work. This design takes one call from it and builds no physics.
- **Climb needs the density far from the chunk.** Its walk length bounds
  the cost; a long vine is several short ones.

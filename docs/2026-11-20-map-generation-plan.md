# Map generation (strata_maps) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** bounded tactical maps generated from a seed and layer settings:
terrain, rivers that flood when breached, roads, lots, buildings from a
grammar, set pieces, trees, lava, ruin, objective sites, and a tactical
check that rejects unplayable maps.

**Architecture:** Strata core gains the urban tools (roads, lots, ground
adaptation, the building grammar), trees as a library module, and stamping
a template as an edit. The plugin `plugins/strata_maps/` runs the pipeline
over plain data (a height field, masks, lists of placed things), turns the
result into a `Strata.Terrain.Gen` the stream evaluates per chunk, produces
the ruin pass as edits, places objective sites, and checks the map with
`strata_tactics` on a scratch world. An example shows maps and floods.

**Tech Stack:** March (forge), Strata's terrain, template, edit, fluid,
support and population modules; `strata_tactics` for the check.

**Spec:** `docs/2026-11-20-map-generation-design.md`. Rulings made on the
way go in `docs/maps-ledger.md` (created in Task 1).

## Global Constraints

- `. scripts/env.sh` before forge; windowed runs need `MARCH_PIN_MAIN=1`.
- Commit after each task; merge into main only after the final review and
  with the owner's go-ahead (memory: main is checked out in the main
  folder).
- Strata core names no game concept: no mission, Steel Rain style or
  archetype in `lib/strata/`. `strata_maps` uses only public Strata and
  `strata_tactics`.
- Scale: world 0.4 m a voxel; tiles of 8 voxels (3.2 m); storeys 8 voxels;
  maps up to 64 x 64 tiles (512 voxels) plus a 16-tile ring each side (a
  768-voxel square); relief up to 100 voxels (40 m).
- Generation is deterministic in (seed, settings); a 64 x 64 map with every
  layer generates in under 3 s per attempt in release; at most 8 attempts.
- A map enters the world as a `Strata.Terrain.Gen` (pure: chunk key, seed
  to chunk); only the ruin pass is edits. `generate` never edits the game's
  world.
- Every knob is a Float 0..1.
- Art is placeholder built in code; block ids come from the game.
- March pitfalls (GAPS.md): no `opaque`, `by`, `on`, `loop`, `within` as
  names; never name a type `Unit` (S52); constructors do not resolve
  through aliases and three-segment constructor paths do not parse as
  patterns (give readers and builder functions); one global type
  namespace (prefix types: `Map...`, `Bld...`); aliases unique across all
  test suites and Strata's library (check `grep -rh "as X$" lib test`);
  lift lambdas nested in another lambda's let-chain into helpers (S53); a
  function of three Ints is `Int -> Int -> Int -> Bool` (S54);
  `List.fold_left(xs, acc, f)`; `Math.min/max` do not exist
  (`Math.min_int`, or write `minf`).
- `scripts/verify.sh` stays green and gains `plugins/strata_maps` and
  `plugins/strata_maps/example`.

## Review Focus

1. **Settings that leave a layer no room** (a settlement of 1.0 on a canyon
   base, a river wider than the map, lava 1.0 with a city): the map is
   still made, the layer reports what it could not place, and nothing
   overlaps (no building in a river or lava). Pinned in Tasks 13 and 17.
2. **Seeds whose check fails eight times** (relief 1.0 and water 1.0
   cutting the map in two): `generate` answers the best attempt with the
   report of what failed, never loops or crashes. Pinned in Task 21.
3. **Chunks at the ring's edge and beyond, and below the deepest valley or
   above the highest roof**: the generator answers terrain (flat
   continuation, air above, stone below) with no seam or hole. Pinned in
   Task 18.
4. **A breached bank at the map edge or next to a weir**: water floods
   inland and drains to the downstream sinks; nothing pours forever off
   the map's upstream edge. Pinned in Tasks 8 and 23.
5. **Ruin 1.0 on a dense city**: no floating voxels, no building left with
   an unsupported floor, rubble never on a spawn zone. Pinned in Tasks 19
   and 20.

---

## Part 1: Strata core

### Task 1: Trees as a library module

**Files:** create `lib/strata/trees.march` (moved from
`viewer/lib/trees.march`, module renamed `Strata.Trees`); modify the
viewer's callers; test `test/trees_test.march`; create
`docs/maps-ledger.md`.

**Interfaces:**
- Produces: `Strata.Trees.build(species : Int, seed : Int, voxel_size :
  Float, wood : Int, leaves : Int) : Strata.Template.Template` (species 0
  oak, 1 birch, 2 pine, 3 willow; the block ids are parameters now, not 7
  and 8), and `Strata.Trees.half(t) : Int` (the base column offset).

- [ ] **Step 1: Failing tests:** `"the same species, seed and voxel size
  build the same tree"` (equal `t_ids`); `"the base is the middle of the
  bottom row"` (a wood id at (half, 0, half)); `"wood and leaves use the
  ids given"`; `"pine and oak differ"`.
- [ ] **Step 2:** `forge test --filter Trees`; FAIL.
- [ ] **Step 3:** Move the module; parameterise the ids; the viewer calls
  `Strata.Trees.build(..., 7, 8)`.
- [ ] **Step 4:** `forge test`, `cd viewer && forge build --release`, and
  `scripts/golden.sh` for the tree scenes (forest-border, overview): no
  image changes. PASS.
- [ ] **Step 5:** Commit `feat(trees): procedural trees in the library`.

### Task 2: Stamping a template as an edit

**Files:** modify `lib/strata/edit.march`; test `test/edit_test.march`.

**Interfaces:**
- Produces: `Strata.Edit.stamp(w : Strata.World.World, t :
  Strata.Template.Template, ox : Int, oy : Int, oz : Int, rot : Int,
  stream : Int) : Strata.Edit.Outcome` — what `Template.blit` writes into a
  chunk, written into the world (holes keep what was there), every touched
  chunk dirty, the changes recorded.

- [ ] **Step 1: Failing tests:** `"stamping a template writes its blocks
  at the origin"`; `"holes keep the world"`; `"a stamp across a chunk
  border marks both chunks dirty"`; `"a quarter turn matches blit's"`.
- [ ] **Step 2:** FAIL. **Step 3:** Implement (per touched chunk:
  `Template.blit`, then record the changed cells). **Step 4:** PASS.
- [ ] **Step 5:** Commit `feat(edit): stamp a template as an edit`.

### Task 3: Height grids and ground adaptation

**Files:** create `lib/strata/heights.march`; test
`test/heights_test.march`.

**Interfaces:**
- Produces (`Strata.Heights`):
  - `type HeightGrid` — a square of n x n columns from voxel (x0, z0),
    heights in voxels (`NativeIntArr`). `grid(x0, z0, n, fill) :
    HeightGrid`, `get(g, x, z) : Int` (world voxels; outside: the nearest
    edge column), `set(g, x, z, h) : HeightGrid`, `size`, `origin`.
  - `flatten(g, cells : List((Int, Int)), level : Int, margin : Int,
    grade : Float) : HeightGrid` — the cells set to [level]; the ring of
    [margin] columns round them eased toward their old heights by at most
    [grade] voxels a column (cut and fill).
  - `quantise(g, step : Int) : HeightGrid` — every height rounded down to a
    multiple of [step].
  - `slope(g, x, z) : Int` — the largest height difference to a
    4-neighbour.

- [ ] **Step 1: Failing tests:** `"get outside answers the nearest edge"`;
  `"flatten sets the footprint and eases its margin within the grade"`;
  `"quantise rounds down to the step"`; `"slope of a step of 3 is 3"`.
- [ ] **Step 2:** FAIL. **Step 3:** Implement. **Step 4:** PASS.
- [ ] **Step 5:** Commit `feat(heights): height grids and ground adaptation`.

### Task 4: Roads

**Files:** create `lib/strata/roads.march`; test `test/roads_test.march`.

**Interfaces:**
- Produces (`Strata.Roads`):
  - `type RoadGraph` — nodes (x, z metres), segments (node a, node b,
    width metres, surface id). `graph() : RoadGraph`, `add_node(g, x, z) :
    (RoadGraph, Int)`, `add_segment(g, a, b, width, surface) : RoadGraph`,
    `segments(g) : List((Int, Int, Float, Int))`, `node_at(g, i) : (Float,
    Float)`.
  - `at(g, x : Float, z : Float) : Option((Int, Float))` — the segment
    covering the point (by distance to its centre line within half its
    width) and its surface id; the first in segment order.
  - `adapt(g, h : Strata.Heights.HeightGrid, scale : Float, grade : Float)
    : Strata.Heights.HeightGrid` — each segment's centre line given a
    height profile no steeper than [grade] (rise over run) between its
    end heights, the road's columns set to it, a 2-column shoulder eased.
  - `bridges(g, h, water : Int -> Int -> Bool, scale) : List((Int, Float,
    Float, Int))` — for each segment crossing water: the segment, its
    start and end along it (metres) over water, and the deck height
    (voxels: the banks' road height).
  - `chunk_paint(c : Strata.Chunk.Chunk, k : Strata.Coord.Key, g, h, scale
    : Float, deck : Int) : Strata.Chunk.Chunk` — for the chunk's columns
    under a road: the road surface block at the adapted height, air above
    it for 3 m (cutting through what the terrain left), bridge decks of
    block [deck] over water. Chunk-local: reads only the graph and grid.

- [ ] **Step 1: Failing tests:** `"at finds a point on a road and not one
  beside it"`; `"adapt keeps a road within its grade over a hill"`;
  `"a road across a river reports one bridge with the banks' height"`;
  `"chunk_paint writes the surface and clears above it"`; `"two chunks
  either side of a road agree at the border"` (the columns' heights match).
- [ ] **Step 2:** FAIL. **Step 3:** Implement. **Step 4:** PASS.
- [ ] **Step 5:** Commit `feat(roads): road graphs, adaptation, bridges`.

### Task 5: Blocks and lots

**Files:** create `lib/strata/lots.march`; test `test/lots_test.march`.

**Interfaces:**
- Produces (`Strata.Lots`):
  - `type Lot = Lot(Int, Int, Int, Int, Int)` — a rectangle in voxels (x0,
    z0, x1, z1 inclusive) and its street side (0 +x, 1 -x, 2 +z, 3 -z);
    readers `lot_box`, `lot_street`, `lot_area`.
  - `blocks(roads : Strata.Roads.RoadGraph, x0 : Int, z0 : Int, n : Int,
    scale : Float, buildable : Int -> Int -> Bool) : List((Int, Int, Int,
    Int))` — the buildable regions between roads as rectangles (the
    connected non-road buildable columns, each split into maximal
    rectangles).
  - `split(block : (Int, Int, Int, Int), roads, scale, min_side : Int,
    max_side : Int, seed : Int) : List(Lot)` — recursive splits across the
    longer side at a hashed point, stopping under [max_side]; a piece with
    no road frontage is merged back; pieces under [min_side] dropped.

- [ ] **Step 1: Failing tests:** `"every lot touches a road on its street
  side"`; `"lots never overlap each other or a road"`; `"no lot is
  smaller than min_side"`; `"the same seed splits the same way"`.
- [ ] **Step 2:** FAIL. **Step 3:** Implement. **Step 4:** PASS.
- [ ] **Step 5:** Commit `feat(lots): blocks and lots`.

### Task 6: The building grammar, mass and facade

**Files:** create `lib/strata/building.march`; test
`test/building_test.march`.

**Interfaces:**
- Produces (`Strata.Building`):
  - `type BldStyle` — a record: `bs_storeys : (Int, Int)`, `bs_storey :
    Int` (voxels), `bs_coverage : Float`, `bs_setback : Int`, `bs_volumes
    : Int` (most volumes), `bs_bay : Int` (voxels), `bs_ground :
    List((Int, Float))` and `bs_upper : List((Int, Float))` (opening kinds
    with weights: 0 wall, 1 window, 2 shopfront, 3 door, 4 loading door),
    `bs_wall`, `bs_trim`, `bs_glass`, `bs_floor`, `bs_roof` (block ids),
    `bs_roof_kind : Int` (0 flat with parapet, 1 pitched, 2 saw-tooth),
    `bs_hall : Bool` (wide spans, low and long), `bs_clutter :
    List(Strata.Template.Template)`; `style(...)` with defaults.
  - `type BldPlan` — volumes (box in lot-local voxels, storeys), openings
    (wall side, bay, storey, kind), roof kind, the lot. `plan(lot :
    Strata.Lots.Lot, s : BldStyle, seed : Int) : BldPlan`; readers
    `volumes`, `openings`, `height(p) : Int`, `footprint(p) : List((Int,
    Int, Int, Int))`.
  - `solid(p : BldPlan, s : BldStyle, seed : Int) :
    Strata.Template.Template` — level 1: walls, roof, openings marked as
    glass or door blocks, the template's origin at the lot's low corner.

- [ ] **Step 1: Failing tests:** `"a building stays inside its lot"`;
  `"its storeys are within the style's range"`; `"the street side's
  ground floor has a shopfront or door"`; `"walls between volumes carry no
  openings"`; `"no opening is cut into a wall touching the lot edge
  without a street"`; `"a hall style gives one wide low volume"`; `"the
  same seed plans the same building"`.
- [ ] **Step 2:** FAIL. **Step 3:** Implement. **Step 4:** PASS.
- [ ] **Step 5:** Commit `feat(building): the grammar's mass and facade`.

### Task 7: The building grammar, hollow (level 2)

**Files:** modify `lib/strata/building.march`; test
`test/building_test.march`.

**Interfaces:**
- Produces: `hollow(p : BldPlan, s : BldStyle, seed : Int) :
  Strata.Template.Template` — level 2: floors at each storey, a stair core
  (one bay square) aligned through every storey, openings as holes (air;
  glass kept only where the style says), interior wall faces in
  `bs_trim`; and `stairs(p) : List((Int, Int, Int))` (the stair cells, for
  data).

- [ ] **Step 1: Failing tests:** `"every storey has a floor"`; `"the stair
  core is open through every floor and aligned"`; `"a window is a hole you
  can see through"` (a ray through it reaches the interior); `"the
  interior is air apart from floors and stairs"`.
- [ ] **Step 2:** FAIL. **Step 3:** Implement. **Step 4:** PASS.
- [ ] **Step 5:** Commit `feat(building): hollow buildings with floors and stairs`.

### Task 8: Source bodies at several levels (check, then fix if needed)

**Files:** test `test/fluid_test.march`; modify `lib/strata/fluid.march`
only if a test fails.

**Interfaces:**
- Consumes: `Strata.Fluid.new(r, families)` with a `sea()` family and a
  `sink()` block, `wake`, `step_world`.
- Produces: the rule, tested, that **each sea block fills only up to its
  own level**, and a ruling on how stretches are separated (weir walls of
  a non-fluid block between stretches, so nothing pours at rest).

- [ ] **Step 1: Failing or passing tests (record which):** `"two sea
  stretches at different levels, a weir between, stay at rest"` (no active
  cells after 50 steps); `"a breach beside the upper stretch floods the
  low ground up to that stretch's level and no higher"`; `"flood water
  reaching a sink at the edge is taken"`; `"a breach in a weir pours the
  upper stretch into the lower until the breach is level"`.
- [ ] **Step 2:** `forge test --filter Fluid`. If all pass, ledger it (no
  fluid change). If one fails, fix the fluid step minimally for that
  case, ledger the fix, and rerun until PASS.
- [ ] **Step 3:** Commit `test(fluid): seas at several levels`.

### Task 9: The tactics camera kept over the map

**Files:** modify `plugins/strata_tactics/lib/camera.march`; test
`plugins/strata_tactics/test/camera_test.march`.

**Interfaces:**
- Produces: `StrataTactics.Camera.with_bounds(c, x0 : Float, z0 : Float,
  x1 : Float, z1 : Float) : TacticsCamera` — the focus is clamped to the
  box after every pan, follow and shot.

- [ ] **Step 1: Failing test:** `"panning past the bounds stops at the
  edge"`. **Step 2:** FAIL. **Step 3:** Implement. **Step 4:** tactics
  suite PASS. **Step 5:** Commit `feat(tactics): camera bounds`.

---

## Part 2: strata_maps

Package `plugins/strata_maps/` (`type = "lib"`, deps `strata` `../..` and
`strata_tactics` `../strata_tactics`, `[ffi]` as `strata_tactics`). Flat
modules named `StrataMaps.*`. Test worlds as in `strata_tactics`' tests.

### Task 10: Package, settings and map data

**Files:** create `forge.toml`, `lib/strata_maps.march` (root),
`lib/settings.march`, `lib/data.march`, `test/settings_test.march`;
modify `scripts/verify.sh` (add `plugins/strata_maps` and its example).

**Interfaces:**
- Produces:
  - `StrataMaps.Settings`: `type MapKnobs = { mk_relief, mk_water,
    mk_settlement, mk_density, mk_vegetation, mk_hazard, mk_ruin : Float }`;
    `type MapSettings = { ms_seed : Int, ms_tiles : Int, ms_base : Int (0
    plains, 1 rolling, 2 highlands, 3 canyon, 4 plateau), ms_style : Int
    (index into the game's styles), ms_pattern : Int (0 grid, 1 arterial,
    2 broken), ms_step : Int (the height step in voxels, default 2), ms_knobs : MapKnobs }`; `type MapBlocks` (ids: stone, dirt,
    grass, sand, asphalt, concrete, water sea family base, sink, weir,
    lava, rubble, scorched, bridge deck); `type Archetype = { ar_name :
    String, ar_fixed : MapSettings -> MapSettings, ar_ranges : List((String,
    Float, Float)) }`; `blend(seed, ranges : List((String, Float, Float)),
    base : MapSettings) : MapSettings` (each named knob drawn in its range
    by a hash of the seed and its name); `apply(a : Archetype, seed, base)`.
  - `StrataMaps.Data`: `type MapData` — heights (`Strata.Heights
    .HeightGrid`), water (per column: level or -1, as `NativeIntArr`),
    weirs, roads, bridges, lots, buildings (lot, plan, template), set
    pieces (template index, origin, turn), trees (template index, origin),
    lava (mask), sites, reports (`List(String)`); `empty(settings,
    blocks) : MapData`; readers for each.
  - Sizes: `side_voxels(s) = (s.ms_tiles + 32) * 8`; playable from voxel
    `ring = 128` in both axes.

- [ ] **Step 1: Failing tests:** `"blend draws each knob within its
  range"`; `"the same seed blends the same"`; `"an archetype's fixed
  choices win over the blend"`; `"side_voxels of 64 tiles is 768"`.
- [ ] **Step 2–4:** FAIL, implement, PASS.
- [ ] **Step 5:** Commit `feat(maps): package, settings and map data`.

### Task 11: Base terrain

**Files:** create `lib/terrain_stage.march`; test `test/terrain_test.march`.

**Interfaces:**
- Produces: `StrataMaps.TerrainStage.run(d : MapData) : MapData` — the
  height field over the whole square for the base recipe, relief knob
  scaling the range from 6 voxels (one mech jump of gentle steps) to 100,
  quantised to `ms_step`, heights in [8, 8 + 100]; the ring eased toward the
  playable edge's mean height.

- [ ] **Step 1: Failing tests:** `"plains at relief 0 vary by no more than
  one step"`; `"relief 1 spans at least 80 voxels on highlands"`;
  `"canyon has a floor lower than its rims by most of the relief"`;
  `"heights are multiples of the step"`; `"the ring eases to the edge's
  mean"`; `"the same seed gives the same field"`.
- [ ] **Step 2–4:** FAIL, implement, PASS.
- [ ] **Step 5:** Commit `feat(maps): base terrain`.

### Task 12: Water

**Files:** create `lib/water_stage.march`; test `test/water_test.march`.

**Interfaces:**
- Produces: `StrataMaps.WaterStage.run(d : MapData) : MapData` — for water
  above 0: a river path from one edge to the opposite (a hashed walk down
  the height field, smoothed), width 6..62 voxels by the knob, carved 4..10
  voxels below its banks; split into stretches where the bed falls more
  than 2 voxels, each stretch's level its lowest bank less 2, a weir wall
  (one voxel thick, `weir` block, up to the upper level) between
  stretches; lakes in closed basins (`Strata.Flood`) at water ≥ 0.5;
  downstream edge columns of the river get a sink column in the ring.

- [ ] **Step 1: Failing tests:** `"a river runs edge to edge"`; `"its
  width follows the knob"`; `"every stretch is level and lower than its
  banks"`; `"a weir separates stretches whose levels differ"`; `"water 0
  makes no water"`; `"the downstream end has sinks"`.
- [ ] **Step 2–4:** FAIL, implement, PASS.
- [ ] **Step 5:** Commit `feat(maps): rivers and lakes`.

### Task 13: Roads and lots

**Files:** create `lib/settle_stage.march`; test `test/settle_test.march`.

**Interfaces:**
- Consumes: `Strata.Roads`, `Strata.Lots`, `Strata.Heights.flatten`.
- Produces: `StrataMaps.SettleStage.run(d : MapData) : MapData` — the
  settled area (a hashed disc or band covering the settlement knob's share
  of buildable ground: slope under 2 steps, off water and lava), a road
  graph in the pattern (grid: spacing by density 40..24 m; arterial: two
  crossing main roads and yard spurs; broken: a grid with hashed missing
  segments), adapted, bridges over water, lots split from blocks (min 8,
  max 40 voxels a side, scaled by density), a report line when the
  settlement found less room than asked.

- [ ] **Step 1: Failing tests:** `"settlement 0 makes no roads"`; `"every
  lot touches a road"`; `"no lot on water or a slope over the limit"`;
  `"a road across the river has a bridge"`; `"settlement 1 on a canyon
  reports too little room and places what fits"` (Review Focus 1).
- [ ] **Step 2–4:** FAIL, implement, PASS.
- [ ] **Step 5:** Commit `feat(maps): roads and lots`.

### Task 14: Buildings

**Files:** create `lib/build_stage.march`; test `test/build_stage_test.march`.

**Interfaces:**
- Consumes: `Strata.Building` (`plan`, `hollow`), `Strata.Heights.flatten`.
- Produces: `StrataMaps.BuildStage.run(d : MapData, styles :
  List(Strata.Building.BldStyle)) : MapData` — every lot gets a building of
  the settings' style, the ground flattened under its footprint to the
  footprint's mean height (quantised), density scaling storeys within the
  style; buildings listed with plan and template.

- [ ] **Step 1: Failing tests:** `"every lot has a building inside it"`;
  `"buildings stand on flattened ground"` (the footprint's columns equal);
  `"density 0 builds the style's lowest storeys, density 1 its highest"`.
- [ ] **Step 2–4:** FAIL, implement, PASS.
- [ ] **Step 5:** Commit `feat(maps): buildings`.

### Task 15: Set pieces

**Files:** create `lib/piece_stage.march`; test `test/piece_test.march`.

**Interfaces:**
- Produces: `type MapPiece = { mp_template : Strata.Template.Template,
  mp_zone : Int (0 yard: in a lot without a building, 1 high ground, 2
  road, 3 open ground), mp_per_map : (Int, Int), mp_needs_flat : Bool }`;
  `StrataMaps.PieceStage.run(d, pieces : List(MapPiece)) : MapData` —
  placed by zone with hashed turns, not overlapping buildings, water,
  lava, or each other.

- [ ] **Step 1: Failing tests:** `"pieces land only in their zone"`;
  `"pieces never overlap buildings or water"`; `"counts stay within
  per_map"`.
- [ ] **Step 2–4:** FAIL, implement, PASS.
- [ ] **Step 5:** Commit `feat(maps): set pieces`.

### Task 16: Vegetation

**Files:** create `lib/green_stage.march`; test `test/green_test.march`.

**Interfaces:**
- Consumes: `Strata.Trees.build`.
- Produces: `StrataMaps.GreenStage.run(d, trees :
  List(Strata.Template.Template)) : MapData` — trees by a hashed-cell
  scatter, cell size from vegetation (14 m scattered to 3.2 m thick
  forest), denser within 12 m of water, never on roads, lots' footprints,
  water, lava or its scorched ring, or slopes over 3 steps; street trees
  along exurb roads; grass on open ground.

- [ ] **Step 1: Failing tests:** `"vegetation 0 places no trees"`; `"no
  tree on a road or a footprint"`; `"vegetation 1 puts a tree on most
  tiles of open ground"`; `"trees are thicker by water"`.
- [ ] **Step 2–4:** FAIL, implement, PASS.
- [ ] **Step 5:** Commit `feat(maps): vegetation`.

### Task 17: Lava

**Files:** create `lib/hazard_stage.march`; test `test/hazard_test.march`.

**Interfaces:**
- Produces: `StrataMaps.HazardStage.run(d) : MapData` — lava pools in the
  lowest basins off water (canyon floors first), area up to 15% of the
  playable square at hazard 1; lava 2 voxels below the ground it fills; a
  3-voxel scorched ring (surface block `scorched`); lava never on a lot or
  road (those are placed earlier and lava avoids them: a report line when
  hazard asked for more than fit).

- [ ] **Step 1: Failing tests:** `"hazard 0 makes no lava"`; `"lava stays
  off roads, lots and water"`; `"a scorched ring surrounds every pool"`;
  `"hazard 1 with a city reports what did not fit"` (Review Focus 1).
- [ ] **Step 2–4:** FAIL, implement, PASS.
- [ ] **Step 5:** Commit `feat(maps): lava`.

### Task 18: The map as a terrain generator

**Files:** create `lib/world_gen.march`; test `test/world_gen_test.march`.

**Interfaces:**
- Produces: `StrataMaps.WorldGen.gen(d : MapData) : Strata.Terrain.Gen` —
  per chunk: ground to the height field (stone, dirt 3 deep, the surface
  block by what the column is: grass, sand by water, scorched, asphalt or
  concrete under roads and lots), water at stretch levels (the sea
  family's full block), weirs, sinks, lava, roads (`Strata.Roads
  .chunk_paint`), then every template overlapping the chunk (buildings,
  pieces, trees, by a per-chunk index built once) with
  `Strata.Template.blit`; beyond the square, flat ground at the ring's
  edge height; stone below, air above.

- [ ] **Step 1: Failing tests:** `"a chunk's columns match the height
  field"`; `"a building spanning four chunks is whole when they are
  joined"`; `"a chunk beyond the ring is flat ground"` and `"a chunk under
  the deepest valley is stone, one over the highest roof air"` (Review
  Focus 3); `"the same data gives the same chunks"` (hash); `"water
  columns hold the sea block at the stretch's level"`.
- [ ] **Step 2–4:** FAIL, implement, PASS.
- [ ] **Step 5:** Commit `feat(maps): the map as a terrain generator`.

### Task 19: The ruin pass

**Files:** create `lib/ruin_stage.march`; test `test/ruin_test.march`.

**Interfaces:**
- Consumes: `Strata.Edit.blast`, `Strata.Support.loose`, `Strata.Edit
  .stamp`.
- Produces: `StrataMaps.RuinStage.run(d : MapData, w :
  Strata.World.World) : (List(Strata.Plugin.Request), MapData)` — on a
  scratch world built from the generator: for ruin r, each building gets
  blasts with probability r (count 1..4 by r, on corners and upper storeys
  by a hash), what `Support.loose` lists is removed, and rubble heaps
  (rubble block, cone of the removed volume's size) are stamped on the
  ground below; craters on open ground at r > 0.6; answered as requests
  for the game to apply after the stream builds the world.

- [ ] **Step 1: Failing tests:** `"ruin 0 makes no requests"`; `"after the
  pass nothing is left unsupported"` (Support.loose of the result is
  empty) (Review Focus 5); `"rubble lies below where buildings broke"`;
  `"the same seed ruins the same way"`.
- [ ] **Step 2–4:** FAIL, implement, PASS.
- [ ] **Step 5:** Commit `feat(maps): the ruin pass`.

### Task 20: Objective sites

**Files:** create `lib/sites.march`; test `test/sites_test.march`.

**Interfaces:**
- Produces: `type SiteRule = { sr_name : String, sr_kind : Int (0 zone, 1
  point, 2 building), sr_tiles : Int, sr_edge : Int (-1 none, 0..3),
  sr_far_from : List((String, Int)) (site, tiles), sr_high : Bool,
  sr_in_settlement : Bool, sr_open_sky : Bool, sr_count : Int }`;
  `type MapSite = { st_name : String, st_tiles : List((Int, Int)) }`;
  `StrataMaps.Sites.place(d, grid : StrataTactics.Grid.TileGrid, rules :
  List(SiteRule)) : MapData` — rules in order, each on standable tiles
  meeting it, chosen by hash among candidates; a rule that cannot be met is
  a report line; zones never on water, lava or rubble.

- [ ] **Step 1: Failing tests:** `"two spawn zones land on opposite
  edges"`; `"far_from keeps the distance"`; `"high picks the highest
  candidate band"`; `"no zone on water, lava or rubble"`; `"an
  impossible rule is reported, not placed"`.
- [ ] **Step 2–4:** FAIL, implement, PASS.
- [ ] **Step 5:** Commit `feat(maps): objective sites`.

### Task 21: The tactical check and `generate`

**Files:** create `lib/check.march`; modify `lib/strata_maps.march`; tests
`test/check_test.march`, `test/generate_test.march`.

**Interfaces:**
- Consumes: every stage; `StrataTactics.Grid`, `Reach`, `Sight`.
- Produces:
  - `type CheckReport = { cr_ok : Bool, cr_routes : Int, cr_unreached :
    List(String), cr_cover : Float, cr_sight : Int, cr_why : List(String) }`;
    `StrataMaps.Check.run(w, grid, d : MapData, mover :
    StrataTactics.Reach.Mover) : CheckReport` — routes between the first
    two zone sites = edge-disjoint tile paths (up to 3); every site reached
    from both; cover share of standable tiles with half or full cover on
    some edge in [0.08, 0.6]; at least one clear line between the zones'
    tiles; `cr_ok` when routes ≥ 2 and nothing unreached and cover in range.
  - `early(d, mover) : Bool` — the spawns' edges connected over the
    height field alone (before buildings), for a cheap early reject.
  - `StrataMaps.generate(s : MapSettings, b : MapBlocks, styles, pieces,
    trees, rules : List(SiteRule), mover) : (Strata.Terrain.Gen,
    List(Strata.Plugin.Request), MapData, CheckReport)` — up to 8 attempts
    (seed, seed + 1, ...), each: stages 11–17, early check, generator,
    scratch world over the playable square, ruin, grid, sites, check; the
    first passing attempt, else the best (most routes, then fewest
    unreached) with `cr_ok = false` and its reasons.
  - `StrataMaps.load_archetypes(text : String) : Result(List(Archetype),
    String)` — `[archetype name]` sections of `knob = lo hi` and `base =`,
    `style =`, `pattern =` lines.

- [ ] **Step 1: Failing tests:** `"a single bridge over a wide river is one
  route and fails"`; `"an open plain passes"`; `"generate returns a
  passing map for an ordinary blend"`; `"a map that cannot pass returns
  the best attempt with reasons after 8 tries"` (Review Focus 2);
  `"load_archetypes reads a section and reports a bad line"`.
- [ ] **Step 2–4:** FAIL, implement, PASS. Budget test: 64 x 64, every
  layer at 0.5, one attempt under 3 s release (asserted at 12 s in the
  test build), timed and ledgered.
- [ ] **Step 5:** Commit `feat(maps): the tactical check and generate`.

### Task 22: The example: Steel Rain's styles and archetypes

**Files:** create `plugins/strata_maps/example/` (`forge.toml`, `run.sh`,
`lib/maps_example.march`, `assets/archetypes.txt`); `docs/maps-*.png`.

**What it does:** Steel Rain's three styles (industrial, city quarter,
exurb, as the design describes) and placeholder set pieces (tank, crane,
pipe run, containers, wrecked vehicle, tower) built in code; five
archetypes in `archetypes.txt` (Training Fields: plains, relief 0.1,
vegetation 0.2, settlement 0; River Town: rolling, water 0.6, exurb 0.5;
Lava Foundry: canyon, hazard 0.7, industrial 0.3; Ruined Quarter: city
1.0, ruin 0.7; Highland Pass: highlands, relief 0.9, vegetation 0.6); the
map generated, streamed, the tactics camera over it with bounds, sites
drawn as overlay tiles, the check's report on screen; keys: N next seed,
A next archetype, B blend.

- [ ] **Step 1:** Build. **Step 2:** Scripted: one picture per archetype
  (`STRATA_FRAMES`, `STRATA_SHOT`, an env var for the archetype); look at
  each; store `docs/maps-<archetype>.png`; ledger timings.
- [ ] **Step 3:** Commit `feat(maps): an example with Steel Rain's styles`.

### Task 23: Floods in play

**Files:** modify `plugins/strata_maps/example/lib/maps_example.march`;
`docs/maps-flood.png`.

**What it does:** the example runs `Strata.Fluid` each frame (families
from `MapBlocks`), wakes it with every applied edit, and passes its
outcomes to `StrataTactics.world_changed`; F blasts the bank nearest the
pointer. Scripted: River Town, a bank blasted at frame 20, a picture at
frame 300 showing the flood and the overlay's flooded tiles.

- [ ] **Step 1:** Implement. **Step 2:** Scripted picture; check the flood
  spread inland and stopped at the stretch's level; a breach at the map
  edge drains into the sinks (Review Focus 4); store the picture.
- [ ] **Step 3:** `scripts/verify.sh`; PASS.
- [ ] **Step 4:** Commit `feat(maps): floods in play`.

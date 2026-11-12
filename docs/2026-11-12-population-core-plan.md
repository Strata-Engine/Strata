# Strata Population Core Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A game declares what stands in its world (fields, zones, shapes, layers) and the engine places it; the island's trees, ruins, tall grass and lantern posts are placed that way and the viewer's hand wiring is gone.

**Architecture:** `Strata.Population` is a pure library module. Game code builds declarations at start-up and `freeze` compiles them into flat tables the chunk generator closes over. The game's generator calls `Population.ground` before it builds density (to adjust heights under sites) and `Population.place` after it has filled the chunk (to write cover blocks and stamps), handing over a `Ground` record of arrays it has already computed. Rules are data: products of curves over per-column field arrays, plus filters. Placement is by hash, so it is chunk-local and one layer can read another's cover.

**Tech Stack:** March (see Global Constraints for which compiler), the existing `Strata.Feature`, `Strata.Template`, `Strata.Seed`, `Strata.Noise` and `Strata.Atlas`.

**Spec:** `docs/2026-11-12-population-design.md`, stage 1 of its Order of work ("The core"). Stages 2 to 8 each get their own plan.

## Global Constraints

- This plan covers the design's stage 1 only: fields, zones, rules, `freeze`, layers and their order; the cover, scatter and site placers; the block and stamp representations; levels; the data file; and the port of trees, ruins, tall grass and posts. No props, no new generators, no patch, attach, hang or climb placers, no urban tools.
- Order against other work: Tasks 1 to 8 change no existing file except `lib/strata/template.march` (Task 7) and may run while the chunk-performance task is under way. Task 9 (the port) rewrites the viewer's feature code and must start only after the performance task's restructuring of the column stage and the march-main switch are both on main; rebase onto main first.
- March first: a measured slowness becomes a reduced case under `bench/march/` with a C comparison and a `GAPS.md` entry, not a move to C. No new C in this plan.
- No game callback is evaluated per column, per voxel or per instance. A game function may run at start-up only. Nothing in `lib/strata` names a game concept (no tree, grass, ruin, post, biome).
- Every number in a declaration is a `Float` in metres or a unit-range factor. The only voxel conversion is at the chunk boundary through the world's scale.
- Deterministic and chunk-local: `place` for a key depends only on the frozen population, the key, the seed and the `Ground` handed to it; a layer straddling a chunk border is identical from both sides.
- GAPS apply: never guard an array read with `&&` or `||`; constructor and type names are unique across the library; a callee cannot fill its caller's array (return the array); a shared value is not threaded through a hot loop (copy what a task needs into arrays it owns); under March 0.5.0 `Check.all` cannot fail, so every property is an ordinary loop with direct assertions.
- Integer arithmetic keeps every intermediate under 2^62 (March 0.7.0 traps on overflow; 0.5.0 wraps). Use `Strata.Seed` for all hashing.
- Costs are measured by CPU time (`/usr/bin/time -l`, a run minus a baseline run), with the load average recorded; wall-clock figures above a load of 8 are "not judged".
- `scripts/verify.sh` prints PASS at the end of every task. Goldens are looked at before they are stored.
- Commit after each task. Merge to `main` after each reviewed task (the owner's standing instruction).

## Review Focus

1. A rule that names a field, zone or layer that was never declared, or layers whose dependencies form a cycle: `freeze` returns `Err` naming it; nothing panics. (Task 1.)
2. A built-in field the game did not supply in `Ground`: it reads as zero and `freeze` has already reported which rules use it; `place` does not read a missing array. (Tasks 2 and 3.)
3. An instance on a chunk border at world scales 1 m, 0.5 m and 0.25 m, at every rotation: both chunks write the same voxels and none twice. This is where the parked tree-clipping defect at 0.25 m lives. (Tasks 4 and 9.)
4. A population with no layers, and a chunk no layer reaches: `place` returns the chunk unchanged and costs under 5 µs of CPU. (Task 3.)
5. Two cover layers that both want a surface, and a density above 1 or below 0: the earlier layer keeps the surface; density is clamped. (Task 3.)

---

### Task 1: Declarations and `freeze`

**Files:** `lib/strata/population.march`, `test/population_test.march`

**Interfaces — Produces:**
```
Strata.Population.Pop                                   -- declarations being built (opaque)
Strata.Population.Frozen                                -- the compiled, immutable result (opaque)
new() : Pop

-- fields: a number per ground column
field_ground(p, name : String, slot : Int) : Pop        -- a built-in the game supplies in Ground; slots: 0 altitude (m), 1 slope (rise over run), 2 water_depth (m), 3 river_dist (m), 4 lake_dist (m), 5 coast_dist (m), 6 wetness (unit), 7 region (id), 8 depth (m below the top surface), 9 sky (0 or 1)
field_noise(p, name, kind : Int, octaves : Int, period : Float, salt : Int) : Pop
field_curve(p, name, of : String, c : PopCurve) : Pop   -- another field through a curve
field_cover(p, name, layer : String, radius : Float) : Pop   -- how many of a layer's instances stand within radius, over the layer's densest packing, clamped to 0..1

PopCurve; curve(points : List((Float, Float))) : PopCurve   -- piecewise linear, flat beyond its ends; at least one point

-- zones: yes or no for a column
zone_regions(p, name, ids : List(Int)) : Pop
zone_band(p, name, field : String, lo : Float, hi : Float) : Pop     -- lo <= field < hi
zone_ring(p, name, sites : List(PopSite), inner : Float, outer : Float) : Pop   -- within inner..outer metres of a site's centre
zone_all(p, name, of : List(String)) : Pop;  zone_any(...);  zone_not(p, name, of : String) : Pop

-- rules
PopRule; rule(base : Float, by : List((String, PopCurve)), inside : List(String), outside : List(String)) : PopRule
  -- density per square metre = base * product of curve(field) over `by`; passes only inside every zone of `inside` and outside every zone of `outside`

-- shapes
shapes_blocks(p, name, ids : List(Int)) : Pop                       -- one-voxel things: block ids, a variant per id
shapes_templates(p, name, ts : List(Strata.Template.Template)) : Pop
weights(p, shapes : String, zone : String, w : List(Float)) : Pop   -- variant weights inside a zone; without one, variants are equally likely

-- layers
layer_cover(p, name, shapes : String, r : PopRule, salt : Int) : Pop
layer_scatter(p, name, shapes : String, r : PopRule, cell : Float, jitter : Float, salt : Int) : Pop
layer_site(p, name, shapes : String, sites : List(PopSite), margin : Float, salt : Int) : Pop
PopSite; site(xm : Float, zm : Float, floor : Float, radius : Float, index : Int) : PopSite

freeze(p : Pop) : Result(Frozen, String)
layer_names(f : Frozen) : List(String)      -- in the order they run
missing_ground(f : Frozen) : List(Int)      -- built-in slots that rules use; the game checks it supplies them
```
`freeze` rejects, with a message naming the offender: a duplicate name within a kind; a rule, zone, field or layer that names something undeclared; a curve with no points; a negative radius, cell or margin; a jitter above 0.6; a cycle through `field_cover` (layer A's rule reads a cover of layer B whose rule reads a cover of A). Layers run in declaration order, except that a layer is moved after every layer whose cover its rule reads.

- [ ] **Step 1: failing tests** in `test/population_test.march`:
  - `freeze(new())` is `Ok` and `layer_names` is empty;
  - two cover layers declared a then b run in that order; when a's rule reads a cover field of b, the order is b then a;
  - each rejection above yields `Err` whose message contains the offending name (one test per rejection; the cycle message names both layers);
  - `missing_ground` lists exactly the slots of the `field_ground` fields that some rule or zone reaches, through `field_curve` chains too;
  - `curve([(0.0, 0.0), (10.0, 1.0)])` evaluated at -5, 0, 5, 10, 20 gives 0, 0, 0.5, 1, 1 (expose `curve_at(c, x) : Float`).
- [ ] **Step 2:** run `forge test --filter Population`; expect FAIL (module missing). Implement.
- [ ] **Step 3:** `scripts/verify.sh` PASS. Commit `feat(population): declarations and freeze`.

### Task 2: Field tables and rule programs

**Files:** `lib/strata/population.march`, `test/population_test.march`

**Interfaces — Produces:**
```
Strata.Population.Ground    -- what the game hands over for an area of columns
ground(x0 : Int, z0 : Int, n : Int, scale : Float, level : Int, tops : NativeIntArr, waters : NativeIntArr) : Ground
  -- an n x n area of columns whose corner is world voxel (x0, z0) at this level's voxel size; tops: the ground top voxel per column; waters: the water line voxel per column, or a value below tops for none
with_field(g : Ground, slot : Int, values : NativeF32Arr) : Ground   -- one built-in field over the same n x n area
Strata.Population.Table     -- every field a Frozen uses, evaluated over a Ground (opaque)
fields(f : Frozen, seed : Int, g : Ground) : Table
field_at(t : Table, name : String, col : Int) : Float       -- for tests and tools
zone_at(f : Frozen, t : Table, name : String, col : Int) : Bool
density_at(f : Frozen, t : Table, layer : String, col : Int) : Float   -- the layer's rule at a column, clamped to 0 or above; 0 where a filter fails
```
`fields` evaluates each field once over all n x n columns into an array the caller's task owns: ground fields are copied (zeros when the slot was not supplied); noise fields come from one `Strata.Noise.field2` call each, seeded by `Strata.Seed.mix(seed, salt)`, at frequency `1 / period`; curve fields map their source array; cover fields are filled by Task 6 and read as zero until then. Rules are compiled by `freeze` into a flat list of (field index, curve) and (zone index, wanted) pairs and evaluated per column with no name lookups.

- [ ] **Step 1: failing tests:**
  - a ground field supplied as a ramp reads back through `field_at`; a slot not supplied reads 0 everywhere;
  - a noise field is the same for two `Ground`s that overlap, at the columns they share (by world position), and differs for another seed;
  - `zone_band` on the ramp is true exactly on `lo <= v < hi`; `zone_not`, `zone_all`, `zone_any` combine as their names say; `zone_ring` is true between its radii of a site and false elsewhere; `zone_regions` matches the region slot's ids;
  - `density_at` equals a straightforward evaluation written in the test (base times each curve, zero outside a zone) at 500 columns of pseudo-random fields; a product that would be negative is 0;
  - evaluating 32 x 32 columns with 4 fields and 3 layers: record the CPU time and instructions per call in the test output (no assertion).
- [ ] **Step 2:** run; expect FAIL. Implement.
- [ ] **Step 3:** `scripts/verify.sh` PASS. Commit `feat(population): field tables and compiled rules`.

### Task 3: The cover placer and block shapes

**Files:** `lib/strata/population.march`, `test/population_test.march`

**Interfaces — Produces:**
```
with_surfaces(g : Ground, counts : NativeIntArr, ys : NativeIntArr, kinds : NativeIntArr) : Ground
  -- every surface of every column: counts[col] surfaces for a column, stored in order from the top in ys and kinds (kind 0 floor, 1 ceiling) at offsets the prefix sum of counts gives; without it each column has one floor at tops[col]
place(f : Frozen, seed : Int, k : Strata.Coord.Key, c : Strata.Chunk.Chunk, g : Ground) : Strata.Chunk.Chunk
  -- this task: cover layers only. Requires g to cover the chunk's 32 x 32 columns.
```
A cover layer visits each floor surface of each column of the chunk. Where the voxel above the surface is air and lies inside the chunk, the layer rolls `Strata.Seed.unit(Strata.Seed.cell(seed, salt, wx, wy, wz))` at the surface's world voxel against `density * scale * scale` (density is per square metre; a column is `scale` metres a side), clamped to 0..1, and on success writes the variant's block id into the voxel above. The variant is picked by `Strata.Seed.next` of the same hash against the weights of the first weighted zone the column is in, else uniformly. Layers run in order and a later layer never overwrites a voxel an earlier layer wrote. Cover layers place nothing above level 0.

- [ ] **Step 1: failing tests:**
  - a population with no layers returns the chunk unchanged (same `Chunk.hash`); so does a chunk wholly above or below every surface (Review Focus 4); the CPU time of the no-layer call is printed;
  - one cover layer at density 1 per square metre on a flat floor at scale 1.0 fills every air voxel above the floor and nothing else; at density 0 it fills none; at 0.25 it fills between 20% and 30% of 1024 columns;
  - at scale 0.5 the same layer at 0.25 per square metre fills between 4% and 9% of the columns of a 32 x 32 chunk (per square metre, not per column);
  - two layers that both pass everywhere: every surface holds the first layer's block (Review Focus 5); a density of 7.0 behaves as 1.0;
  - a rule limited to a zone places nothing outside it;
  - weights `[1.0, 0.0]` inside a zone give only the first variant there and both variants elsewhere;
  - a column with two floors (a cave under a hill, supplied through `with_surfaces`) gets a roll on each, independently; a ceiling gets none;
  - the same key and seed give the same chunk; a chunk and its neighbour agree with one chunk generated over both (cover does not depend on which chunk asks);
  - a level-1 key gets nothing.
- [ ] **Step 2:** run; expect FAIL. Implement.
- [ ] **Step 3:** `scripts/verify.sh` PASS. Commit `feat(population): cover layers of blocks, exclusive in order`.

### Task 4: The scatter placer and stamps

**Files:** `lib/strata/population.march`, `test/population_test.march`

**Interfaces — Consumes:** `Strata.Feature.kind`, `candidates`, `p_xm`, `p_zm`, `p_index`, `p_rot`, `p_stream`; `Strata.Template.bounds`, `blit`, `t_sx`, `t_sz`, `t_voxel_size`.

**Interfaces — Produces:**
```
reach(f : Frozen) : Float            -- metres: the largest half-width of any template of any scatter or site layer, plus half a voxel of the largest template voxel size
pad(f : Frozen, scale : Float) : Int -- columns of Ground a chunk needs beyond its own on each side so every instance that can reach it is judged: ceil(reach / scale) + 1
PopInstance; i_layer(i) : Int; i_xm(i) : Float; i_zm(i) : Float; i_top(i) : Int; i_variant(i) : Int; i_rot(i) : Int; i_stream(i) : Int
instances(f : Frozen, seed : Int, t : Table, g : Ground, layer : String) : List(PopInstance)
  -- every instance of a scatter layer whose base column lies in g, with the ground top it stands on
place(...)   -- now also scatter layers: requires g to cover the chunk plus pad(f, scale) columns on each side
```
A scatter layer is a `Strata.Feature.kind(cell, reach of its shapes, jitter, salt, 1.0)`; its `density` argument is the rule's density at the candidate's column times `cell * cell`, clamped to 0..1, read from the `Table` (so one candidate a cell at most, present with probability density times the cell's area). A candidate whose rule density is 0 does not exist. The variant comes from `p_index` against the zone weights as in Task 3. The instance stands with its template's base centre on the candidate's column at `tops[col] + 1`, turned by `p_rot`. `place` blits every instance whose rotated `bounds` meet the chunk, in layer order, with `p_stream` as the blit's stream. Templates whose `t_voxel_size` differs from the Ground's voxel size are refused at `freeze` time by `fit_all` below, not here.
```
fit_all(f : Frozen, scale : Float) : Result(Frozen, String)   -- every template through Strata.Template.fit at this world scale; Err naming the shape set of a template that cannot be fitted
```

- [ ] **Step 1: failing tests:**
  - `pad` for a 15-voxel-wide template at 1 m is 9 (half-width 7, reach 7.5, ceil + 1); at 0.25 m with the same template fitted from 1 m it is 31;
  - `instances` over an area equals the union of `instances` over its two halves (same instances, same order within a cell);
  - an instance exists in a cell at density `1 / (cell * cell)` and above, and never where a filter fails at its column; with jitter 0.6 no instance leaves its cell;
  - **Review Focus 3:** a 9 x 12 x 9 asymmetric template, a layer dense enough that an instance straddles the border between two chunks, at world scales 1.0, 0.5 and 0.25 (the template fitted from 1 m) and all four rotations (found by searching seeds): the voxels the two chunks hold together are exactly the fitted template's non-hole voxels at the instance's place, none missing, none doubled;
  - a cell at negative coordinates is its own cell (an instance at x = -3 m is not the mirror of one at +3 m);
  - two scatter layers in order: where their stamps overlap the later layer's voxels are on top, and the result is the same whichever chunk is generated first;
  - `fit_all` at 0.3 m with a template authored at 1 m is `Err` naming the shape set.
- [ ] **Step 2:** run; expect FAIL. Implement.
- [ ] **Step 3:** record `place`'s CPU time for a chunk with about 20 instances in reach (printed). `scripts/verify.sh` PASS. Commit `feat(population): scatter layers stamped across chunk borders`.

### Task 5: The site placer and ground adaptation

**Files:** `lib/strata/population.march`, `test/population_test.march`

**Interfaces — Produces:**
```
adapt(f : Frozen, x0 : Int, z0 : Int, n : Int, scale : Float, heights : NativeF32Arr) : NativeF32Arr
  -- column heights in metres for an n x n area, returned with every site layer's ground drawn to its sites: inside a site's radius the height is the site's floor; across the layer's margin beyond it the height blends linearly back to the given height
adapted(f : Frozen, x0 : Int, z0 : Int, n : Int, scale : Float) : NativeU8Arr   -- 1 for every column adapt changed or would hold at a floor; the game uses it to keep its own rules (caves, detail) off sites
place(...)   -- now also site layers
```
(The design calls the first of these `Population.ground`; it is `adapt` here because `ground` builds the record.) A site layer blits, for each of its sites whose footprint can reach the chunk, the variant `index mod variants` turned by `Strata.Seed.cell(seed, salt, index, 0, 0) mod 4`, with its base centre on the site's centre at the voxel above the site's floor. Where two sites' margins overlap, every site within reach contributes and the nearest site's floor wins inside its radius; no "nearest site to the area's centre" shortcut.

- [ ] **Step 1: failing tests:**
  - on a 10 m ramp, inside a site's radius every column is at the floor; at the outer edge of the margin the height is unchanged; halfway across the margin it is the mean of the two; columns beyond are untouched;
  - `adapt` over an area equals `adapt` over a larger area cropped to it (no dependence on the area asked for), with two sites whose margins overlap in view;
  - `adapted` is 1 exactly where `adapt` differs from its input or the column is inside a radius;
  - a site's template stands with its lowest row on the voxel above the floor, and every column of its footprint has ground directly under it after the game re-derives tops from the adapted heights (the test does that re-derivation);
  - a site straddling a chunk border is identical from both chunks; sites at index 0, 1, 2, 3 with three variants pick variants 0, 1, 2, 0.
- [ ] **Step 2:** run; expect FAIL. Implement.
- [ ] **Step 3:** `scripts/verify.sh` PASS. Commit `feat(population): site layers with the ground drawn up to them`.

### Task 6: Cover fields: one layer reading another

**Files:** `lib/strata/population.march`, `test/population_test.march`

**Interfaces — Produces:** `fields` now fills `field_cover` fields; `pad` grows by the largest `field_cover` radius of any field a layer's rule reaches, in columns.

A cover field of a scatter layer at a column is the number of that layer's instances whose base lies within `radius` metres, divided by `pi * radius^2 / cell^2` (the most a jittered grid can hold there), clamped to 1. It is computed from `instances` of the source layer over the Ground, so it needs the source layer's own fields over the padded area; layers therefore evaluate in `layer_names` order, each with the fields it needs. A cover field of a cover layer is that layer's rule density times `scale^2`, clamped to 1 (the chance a column holds it), with no enumeration.

- [ ] **Step 1: failing tests:**
  - a dense scatter layer over the left half of an area only (by zone): its cover field is above 0.5 well inside the left half, 0 more than `radius` into the right half, and between across the edge;
  - an understory cover layer whose rule multiplies by that cover field places blocks only under the scatter layer's instances' surroundings, and nothing far from them;
  - the understory's blocks in a chunk are the same whether the chunk's Ground is exactly chunk plus `pad` or larger (the cover field does not depend on the area handed over);
  - the same from the neighbouring chunk on a shared border column;
  - `pad` includes the cover radius; the layer order from Task 1 holds (the scatter layer runs first even when declared second).
- [ ] **Step 2:** run; expect FAIL. Implement.
- [ ] **Step 3:** record the CPU time of `fields` with one cover field of radius 6 m over a chunk plus pad (printed). `scripts/verify.sh` PASS. Commit `feat(population): cover fields let a layer read another`.

### Task 7: Levels

**Files:** `lib/strata/template.march`, `lib/strata/population.march`, `test/template_test.march`, `test/population_test.march`

**Interfaces — Produces:**
```
Strata.Template.coarsen(t : Template, k : Int, rank : NativeIntArr) : Template
  -- the template at k times its voxel size: each k x k x k cube becomes the id of highest rank in it (rank[id]; 0 means hole and never wins over a ranked id); cubes are aligned to the template's origin corner; the result's voxel size is k times t's
Strata.Population.seen_to(p : Pop, layer : String, level : Int) : Pop     -- the coarsest level a layer is placed at; default for stamp layers: the coarsest level at which the widest variant is still at least two coarse voxels across; block layers: 0
place(...)   -- at a level above 0 (Ground.level), stamp layers within their seen_to blit their coarsened templates at the instances level 0 decides
```
At level L a scatter or site layer's instances are exactly level 0's (same cells, same hashes, the ground top taken from the Ground it is handed, whose voxels are `scale * 2^L`). A coarsened instance is aligned to the world's coarse voxel grid: the template is placed at its fine world position and each coarse world voxel takes the highest-ranked id of the fine voxels it covers, so a coarse stamp matches the fine one wherever both are drawn. `rank` is supplied per shape set: `ranks(p, shapes : String, rank : NativeIntArr) : Pop`, default every non-zero id ranked by its id.

- [ ] **Step 1: failing tests** `test/template_test.march`: `coarsen` by 2 of a 4 x 4 x 4 template with one voxel of id 7 in a cube of id 8s gives 7 when rank[7] > rank[8] and 8 otherwise; an all-hole cube stays a hole; an odd size rounds the last cube up (a 5-wide template is 3 wide); the voxel size doubles.
- [ ] **Step 2: failing tests** `test/population_test.march`:
  - the default `seen_to` of a layer whose widest template is 9 m, in a 1 m world, is 2 (9 m is 2.25 voxels of 4 m, 1.1 voxels of 8 m);
  - for an instance straddling the edge between a level-0 chunk and a level-1 chunk: every fine voxel of the instance inside the level-1 chunk's box is covered by a coarse voxel of the instance in the level-1 chunk (the union covers the instance);
  - a layer is absent above its `seen_to`; a block layer is absent above level 0;
  - a level-2 chunk generated alone equals the same key generated after its children.
- [ ] **Step 3:** run both; expect FAIL. Implement.
- [ ] **Step 4:** `scripts/verify.sh` PASS. Commit `feat(population): stamp layers at coarser levels, coarsened by rank`.

### Task 8: The data file

**Files:** `lib/strata/population_file.march`, `test/population_file_test.march`

**Interfaces — Produces:**
```
Strata.PopulationFile.Named = Named(List((String, List(Strata.Template.Template))), List((String, List(Strata.Population.PopSite))))
  -- what a file cannot hold: template lists and site lists the game made, by name
named(templates, sites) : Named
read(text : String, n : Named) : Result(Strata.Population.Pop, String)     -- Err carries the line number and what was wrong
```
The format is line-oriented text, parsed in March with no new C. A section starts with a header and runs to the next header; inside it, `name = value` lines; `#` starts a comment:
```
[field wet]            kind = curve      of = wetness     curve = 0:0 0.4:0.2 1:1
[field patches]        kind = noise      noise = 0   octaves = 2   period = 40   salt = 7
[field shade]          kind = cover      layer = forest   radius = 6
[zone wood]            kind = regions    ids = 0 2
[zone shore]           kind = band       field = coast_dist   lo = 0   hi = 12
[zone yard]            kind = ring       sites = camps    inner = 0   outer = 20
[zone damp_wood]       kind = all        of = wood shore
[shapes blades]        kind = blocks     ids = 11
[shapes big]           kind = templates  from = broadleaf
[weights big wood]     w = 3 1 0
[layer lawn]           kind = cover      shapes = blades  base = 0.3   by = wet:0:0,1:1   inside = wood   outside = shore   salt = 26
[layer forest]         kind = scatter    shapes = big     base = 0.02  cell = 6   jitter = 0.6   salt = 21   seen_to = 2
[layer ruins]          kind = site       shapes = stones  sites = camps   margin = 3   salt = 28
```
The ten built-in ground fields are pre-declared under the names in Task 1's slot list. A header may be followed by its settings on the same line or on the lines below it. Unknown kinds, unknown setting names, a missing required setting, a template or site list not in `Named`, and a malformed number are errors, not warnings: a typo must not silently drop a forest.

- [ ] **Step 1: failing tests:**
  - the example above, with `Named` supplying `broadleaf`, `stones` and `camps`, reads to a `Pop` that freezes with `layer_names` equal to `lawn, forest, ruins` (declaration order: nothing depends on anything); with `by = shade:0:1,1:0.2` added to the lawn's rule the order becomes `forest, lawn, ruins`, because `shade` is a cover of `big`'s layer `forest`;
  - reading the file and building the same declarations through the API give populations that place identically on 5 chunks;
  - each error kind yields `Err` with the right line number (one test each): unknown kind, unknown setting, missing `shapes`, `from = nothing`, `base = 0.x3`, a section header with no name;
  - comments, blank lines, and settings split across lines are accepted.
- [ ] **Step 2:** run; expect FAIL. Implement.
- [ ] **Step 3:** `scripts/verify.sh` PASS. Commit `feat(population): declarations from a text file`.

### Task 9: The island's content moves onto it

**Files:** `viewer/lib/strata_viewer.march`, `viewer/assets/island.gen` (the settings stay), `viewer/assets/island.pop` (new: the population), `viewer/test/island_test.march`, `viewer/test/walker_test.march`, `viewer/test/lakes_test.march`, `scripts/golden.sh` (only if a scene must be re-stored), `bench/chunk_stages/` (if present on main)

**Precondition:** the performance task's restructuring and the march-main switch are on main; rebase first.

**Interfaces — Consumes:** everything Tasks 1 to 8 produce; `StrataViewer.Trees.build` (the viewer's branching generator stays in the viewer as a game-made shape set for now; stage 3 replaces it).

**What moves, and what must stay the same:**
- **Tree layer** (`forest`): scatter, cell and jitter from the settings, salt 21, variants the 12 tree templates with zone weights per region reproducing `species_of`'s mix (three templates a species, equal within a species). Its rule reproduces `island_tree`'s refusals as filters over ground fields: not on a lake or river column, not within a site's beard, slope at most the settings' angle sampled a metre each side, altitude at least 41.5 m, the region's density. The game computes those fields in its `Ground` from what `area_lv` already has.
- **Ruin layer** (`ruins`): site layer over the atlas's sites, variants wall, tower, arch in that order (so `index mod 3` matches today), margin 3 m, salt 28. `Population.adapt` replaces `site_for` and `beard_go`; `adapted` replaces the calm-0 marking under sites.
- **Post layer** then **grass layer**: cover layers, posts first so a post keeps its column. Today's rules: posts rolled per one-metre cell with salt 25 on its centre column; tall grass where `grassy(g, hm)` and `roll(wx, wz, seed, 26) > 1 - p_tall_grass`, only on a grass block (id 3). The cover placer rolls per surface voxel with `Seed.cell(seed, salt, wx, wy, wz)`; today's rolls are per column. The port may therefore move posts and grass.
- **Levels:** `coarse_trees`, `level0_tree`, `tree_code`, `coarse_code` and the window tables are deleted in favour of Task 7, with `ranks` giving wood over leaves.
- **Stamps for blasts:** `Strata.Terrain.stamps` replays the `ruins` layer only, as today.
- **Deleted from the viewer:** `has_post`, the grass roll in `finish_go`, `tree_kind`, `species_of`, `pick_at`, `reaches`, `island_tree`, `island_trees`, `trees_into`, `ruins_into`, `site_for`, `beard_go`, `tree_pad`, `slope_step`, `coarse_trees`, `level0_tree` and the coarse-tree tables. After this task, a grep of the viewer for `Strata.Feature.` and `Strata.Template.blit` finds nothing outside `island.pop` loading and tests.

**The standard for "the same":** trees and ruins are placed by the same hashes (same salts, same cell, same `Strata.Feature`), so they must stand exactly where they stand now, at every level: the stored chunk hashes in `viewer/test/island_test.march` that contain no tall grass or posts must not change. Posts and tall grass may move (their roll changes from per column to per surface voxel); hashes and goldens that change for that reason alone are re-recorded after looking, and the ledger names each one and says which decision moved.

- [ ] **Step 1: measure first.** With `STRATA_CHUNK_TIMES=1`, record the island surface chunk's CPU time and instructions (a run minus a baseline run) before any change, with the load average.
- [ ] **Step 2: failing viewer tests** in `viewer/test/island_test.march`:
  - the population file `viewer/assets/island.pop` reads and freezes, and `missing_ground` is empty against what the viewer supplies;
  - for 40 trees found on the current main (their cells, templates, rotations and base voxels recorded as literals in the test before the port), the ported island has each of them at the same place with the same template and rotation;
  - the three ruins nearest the origin stand at the same voxels with the same template and turn;
  - **the parked defect:** at 0.25 m a voxel, a tree straddling a chunk border is whole (no voxel missing, none doubled) at every rotation: the test `walker_test` has at 1 m, run at 0.25 m;
  - the cross-scale test gains 0.25 m beside 0.5 m, and checks trees both ways (every tree at one scale exists at the other);
  - a grep-style check: the viewer module source contains none of the deleted function names (read the file in the test).
- [ ] **Step 3:** run; expect FAIL. Port. Keep `island.gen` as the settings file; `island.pop` holds the layers and is reloaded on regenerate with it.
- [ ] **Step 4:** run the viewer suite. For each stored chunk hash that changed, check which of its chunks holds posts or tall grass; a changed hash of a chunk with neither is a defect to fix, not to re-record.
- [ ] **Step 5: goldens.** `scripts/golden.sh`; for each failing golden look at old and new; re-store only where the difference is where posts and tall grass stand. Scenes that show a forest crossing a border, a ruin on a slope, the ring boundary and the island from its hill must be pixel-identical or differ only by grass and posts.
- [ ] **Step 6: measure again** as in Step 1 and record both in `RESULTS.md` and `docs/population-ledger.md`. The port must not cost more than 0.3 ms of CPU a surface chunk over the Step 1 figure; if it does, find where with `sample`, and reduce any March cause to a case under `bench/march/`.
- [ ] **Step 7:** `scripts/verify.sh` PASS. Commit `feat(population): the island's trees, ruins, grass and posts are declared; the viewer's placement code is gone`.

### Task 10: Record

**Files:** `README.md`, `RESULTS.md`, `GAPS.md`, `docs/population-ledger.md`

- [ ] **Step 1:** README: what a population is, the calls a game makes (`freeze`, `fit_all`, `pad`, `adapt`, `fields`, `place`), the `Ground` a game must supply and how it sizes it, the file format with the island's own file as the example, the limits (stage 1: blocks and stamps only; no props; cover fields cost an enumeration).
- [ ] **Step 2:** RESULTS: the design's stage-1 targets against what was measured, by CPU time with the load average: fields and rules for a surface chunk (target 0.1 ms), all cover layers together (0.1 ms), scatter and stamp layers against the pre-port figure, building the island's population at start-up.
- [ ] **Step 3:** the ledger holds every ruling made during the plan; GAPS gains any March gap met, each with its reduced case.
- [ ] **Step 4:** `scripts/verify.sh` PASS. Commit `docs(population): the core measured; README, RESULTS, ledger`.

---

## Out of scope for this plan

Props and detail scale; polygon zones (they arrive with lots); the patch, attach, hang and climb placers; the colonise, revolve, rosette, blob and grammar generators and any new plant content; roads, lots and buildings; falling pieces; thermal reach in metres and the column-stage restructuring (the performance task); range checks on `island.gen`.

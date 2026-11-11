# Strata Generation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** One seed makes one island with downhill rivers, lakes in real basins, caves, cliffs, forests across chunk borders and stamped ruins; the player blasts into it; the island is visible from its hills through the far field.

**Architecture:** One global pass per seed builds an immutable coarse atlas (`Strata.Atlas`: mask, regions, stream-power erosion, priority-flood drainage, rivers, lake levels, sites). Every chunk stays a pure function of key and seed that closes over the atlas: height from the atlas plus damped detail, a 3D density on a 4 x 8 x 4 lattice, ridge-noise caves, a per-column material scan, hashed-cell features clipped from padded enumeration, templates blitted with masks. Noise is FastNoiseLite behind the C FFI, crossed per array. Everything is parameterised in metres; the voxel scale is one constant. The far field is Part F of the renderer design, with the generator run at `scale * 2^level`.

**Tech Stack:** March (origin/main via `scripts/env.sh`), C99 single headers `FastNoiseLite.h` (MIT) and `ogt_vox.h` (MIT) vendored under `native/`, the C Vulkan shim `native/strata_vk.c`, GLSL built by `native/build_shaders.sh`.

**Spec:** `docs/2026-11-11-generation-design.md`

## Global Constraints

- Work on branch `generation` cut from the current branch. Commit after each task.
- `scripts/verify.sh` prints PASS at the end of every task.
- GAPS S1 to S15 apply. In particular: every test module using `Check.all` declares `needs IO.Clock` and `needs IO.Process`; a module that spawns tasks declares `needs IO.Spawn`; aliases are unique per target across the whole suite; an extern result is never bound to `_`; no exponent float literals; every extern is wrapped in an ordinary `fn` in its own module.
- Spiked and proven before this plan was written: a `type = "lib"` package may list `[ffi] sources` in its own `forge.toml` and `forge test` links them; an extern runs inside `task_spawn`; a native array arrives in C as a pointer to a 32-byte header with the length at byte 16 and data at byte 32 (`narr_len`, `narr_data` in `strata_vk.c`). Consumers still list the C sources themselves (S11): `demo/forge.toml` and `viewer/forge.toml` gain each new source the task that adds it names.
- New C is compiled with `-ffp-contract=off` and no fast-math flag, added to the `[ffi] link` line of every package that lists it.
- Every generation parameter is a `Float` in metres. The only voxel conversion is `Strata.Coord.to_metres(scale, v)` / `to_voxel` at the chunk boundary. A test that changes the scale and expects the same island is in Task 2.
- Seeds: one `Int` world seed; every derived seed and per-cell stream comes from `Strata.Seed` (Task 2). No `seed + small_int` anywhere new.
- Golden images are looked at before they are stored. Existing goldens pass unchanged unless a task says otherwise.
- Machine timings are only trusted when `uptime` shows a load average under 8; otherwise record them as not judged.

## Review Focus

1. Coordinates far from the origin and negative: a chunk at x = -100 000 m must be continuous with its neighbour and a hashed cell at a negative coordinate must hash as its own cell, not its mirror. (Tests in Tasks 1 and 9.)
2. The island's edge and beyond the atlas: a chunk outside the atlas at any level is sea, and no atlas read goes out of range. (Tests in Tasks 3 and 11.)
3. A feature straddling a chunk border at every rotation: both chunks write the same voxels and no voxel is written twice. (Test in Task 9.)
4. A lake bed met by a cave or by detail noise: water must not drain into the ground. (Soak check in Task 10 and a unit test in Task 7.)
5. A blast on a chunk border, on a stamped ruin, and under water: only touched chunks are dirtied, the structure layer agrees with the chunk, water wakes. (Tests in Task 10.)

---

### Task 1: The noise substrate in C

**Files:** `native/FastNoiseLite.h` (vendored, MIT notice kept), `native/strata_noise.c`, `lib/strata/ffi/noise.march`, `lib/strata/noise.march`, `forge.toml` (lib: `[ffi] sources = ["native/strata_noise.c"]`, `link = ["-ffp-contract=off"]`), `demo/forge.toml`, `viewer/forge.toml`, `test/noise_test.march`, `native/tests/noise_test.c`

**Interfaces — Produces:**
```
Strata.Noise.Settings = Settings(kind : Int, octaves : Int, lacunarity : Float, gain : Float, fractal : Int, warp : Float)
  kind: 0 OpenSimplex2 (rotated, ImproveXZ), 1 OpenSimplex2S, 2 Cellular (cell value), 3 Cellular (F2 - F1)
  fractal: 0 none, 1 fBm, 2 ridged;  warp: domain-warp amplitude in metres, 0 for none
Strata.Noise.settings(kind, octaves, lacunarity, gain, fractal, warp) : Settings
Strata.Noise.field2(s : Settings, seed : Int, freq : Float, x0 : Float, z0 : Float, step : Float, w : Int, d : Int, out : NativeF32Arr) : Int
  -- fills out[i + w * j] with noise at (x0 + i * step, z0 + j * step) in metres; freq is cycles per metre; returns w * d, or 0 if out is too small
Strata.Noise.field3(s, seed, freq, x0, y0, z0, sx, sy, sz, w, h, d, out) : Int   -- out[i + w * (j + h * k)]; returns w * h * d or 0
Strata.Noise.sample2(s, seed, freq, x, z) : Float;  sample3(s, seed, freq, x, y, z) : Float   -- single samples, for tests and sparse use
Strata.Noise.value2 / fbm / ridged_fbm / hash2 / smooth / lerp / clamp01   -- unchanged
```
Output range is [-1, 1] for simplex kinds and fBm, [0, 1] for ridged, cell value in [-1, 1].

- [ ] **Step 1: failing C test** `native/tests/noise_test.c`: `sn_field2` over a 4 x 4 grid equals `sn_sample2` at each point; a field at origin (-100000, 0) differs from one at (0, 0); a 32 x 32 x 32 `sn_field3` call with a short array returns 0 and writes nothing.
- [ ] **Step 2:** run via `scripts/verify.sh`'s C loop; expect FAIL. Implement `strata_noise.c`: `int64_t sn_field2(int64_t kind, int64_t octaves, double lac, double gain, int64_t fractal, double warp, int64_t seed, double freq, double x0, double z0, double step, int64_t w, int64_t d, void *out)`, `sn_field3` likewise with three origins, three steps and three sizes, `double sn_sample2(...)`, `sn_sample3(...)`. Rotation type `FNL_ROTATION_IMPROVE_XZ_PLANES` for kind 0 in 3D. Seeds are truncated to 32 bits inside C.
- [ ] **Step 3: failing March tests** in `test/noise_test.march`: `field2` 8 x 8 equals `sample2` at each point within 0.000001; the same call twice is identical; `field3` with a short array returns 0; ridged output in [0, 1] and fBm in [-1, 1] over 400 points; the existing tests still pass.
- [ ] **Step 4:** run `forge test --filter noise`; expect FAIL. Implement the extern module and wrappers; add the `[ffi]` block to the lib's `forge.toml` and the source and link flag to the demo's and viewer's.
- [ ] **Step 5:** `scripts/verify.sh` PASS. Commit `feat(noise): FastNoiseLite behind the FFI, fields filled per array`.

### Task 2: Seeds, metres, and the island of the moment

**Files:** `lib/strata/seed.march`, `test/seed_test.march`, `viewer/lib/strata_viewer.march`, `demo/lib/strata_demo.march`, `viewer/test/walker_test.march`, `test/determinism_test.march`, `test/golden/` (re-stored once)

**Interfaces — Produces:**
```
Strata.Seed.mix(seed : Int, salt : Int) : Int                     -- a derived 64-bit seed; PCG-style (multiply, xorshift) hash of seed and salt
Strata.Seed.cell(seed : Int, salt : Int, x : Int, y : Int, z : Int) : Int   -- the per-cell stream's first value
Strata.Seed.unit(h : Int) : Float                                 -- [0, 1) from a hash
Strata.Seed.next(h : Int) : Int                                   -- the next value of a stream
```
The viewer's and demo's `height` take metres: `height(xm : Float, zm : Float, seed : Int) : Float` in metres, with every frequency in cycles per metre and every amplitude in metres, using `Strata.Noise.sample2` of kind 0 for base and hills and kind 1 ridged for ridges, and `Strata.Seed.mix` for the three seeds. `terrain` converts with the world's scale. The lakes, trees and sea level convert to metres likewise (sea at 40 m, lake grid 192 m).

- [ ] **Step 1: failing tests** in `test/seed_test.march`: `mix(7, 1) != mix(7, 2)`; `cell(7, 1, -1, 0, 0) != cell(7, 1, 1, 0, 0)` and `!= cell(7, 1, 0, 0, -1)`; over 100 000 cells `unit` falls into ten equal bins each within 2% of a tenth; `next` applied 1000 times visits no value twice.
- [ ] **Step 2:** run; expect FAIL. Implement.
- [ ] **Step 3: failing test** `test/determinism_test.march`: a fixed list of 12 chunk keys from the demo's generator at seed 7 hash to stored values (store them in this step after one look); the demo's generator at scale 1.0 and at scale 0.5 give the same ground height in metres at 50 columns within one voxel of the finer scale.
- [ ] **Step 4:** convert the viewer's and demo's terrain to metres and the new noise (`Strata.Noise.value2` and friends stay but nothing in the viewer or demo calls them). Viewer tests pass (`forge test` in `viewer/`).
- [ ] **Step 5:** `scripts/golden.sh --update`; look at every image: the same kind of landscape, different in detail. Record "goldens re-stored on OpenSimplex2" in the ledger. `scripts/verify.sh` PASS. Commit `feat(gen): seeds mixed, terrain in metres, OpenSimplex2 in the viewer and demo`.

### Task 3: The atlas: grid, mask, regions, sampling

**Files:** `lib/strata/atlas.march`, `test/atlas_test.march`, `demo/lib/strata_demo.march` (atlas layer images)

**Interfaces — Produces:**
```
Strata.Atlas.Def = Def(n : Int, step : Float, island_radius : Float, region_size : Float, region_count : Int, sea : Float)
  -- n samples a side, step metres a sample, centred on the origin; sea is the sea level in metres
Strata.Atlas.def(n, step, island_radius, region_size, region_count, sea) : Def
Strata.Atlas.Atlas  -- opaque: Def plus NativeF32Arr height, NativeI32Arr region, NativeF32Arr water (lake level or -1), NativeI32Arr receiver (index or -1), NativeF32Arr drainage (m^2), NativeF32Arr river_width (m, 0 for none), List(Site)
Strata.Atlas.build(d : Def, seed : Int) : Atlas       -- this task: mask and regions only; height = mask * 60 m of warped fBm; water, receiver, drainage, river_width filled by Tasks 4 and 5
Strata.Atlas.n(a) : Int;  step(a) : Float;  def(a) : Def
Strata.Atlas.height_at(a : Atlas, xm : Float, zm : Float) : Float     -- bilinear; the sea level minus 20 m beyond the grid
Strata.Atlas.region_at(a : Atlas, xm : Float, zm : Float) : Int       -- nearest sample; -1 beyond the grid or in the sea
Strata.Atlas.lake_at(a, xm, zm) : Float;  river_width_at(a, xm, zm) : Float;  drainage_at(a, xm, zm) : Float
Strata.Atlas.hash(a : Atlas) : Int
Strata.Atlas.layer_ppm(a : Atlas, layer : Int) : String   -- 0 height, 1 region, 2 water, 3 drainage, 4 river width: a P6 image n x n
```

- [ ] **Step 1: failing tests** in `test/atlas_test.march` with `def(64, 4.0, 100.0, 40.0, 5, 0.0)` at seed 7: every sample farther than `island_radius + 2 * step` from the centre is at or below sea; some sample is above sea; `region_at` beyond the grid is -1 and inside the island is in 0..4 and every region appears; `height_at` at a sample's own position equals the sample and halfway between two samples is their mean; `height_at` at (10 000, 10 000) is sea minus 20; `build` twice gives the same `hash`, seed 8 a different one.
- [ ] **Step 2:** run; expect FAIL. Implement: mask `e = lerp(e, 1 - d, 0.7)` with `d` the distance over `island_radius`, `e` fBm of kind 0 warped by `warp = 0.3 * island_radius`; regions by kind 2 (cell value) at frequency `1 / region_size` warped by `0.25 * region_size`, the cell value quantised to `region_count`.
- [ ] **Step 3:** the demo writes `atlas_<layer>.ppm` for each layer after a run (`File.write`, as `surface_a.ppm`). Look at the height and region images.
- [ ] **Step 4:** `scripts/verify.sh` PASS. Commit `feat(atlas): the island's mask and regions, sampled in metres`.

### Task 4: The atlas: stream-power erosion with lakes

**Files:** `lib/strata/atlas.march`, `lib/strata/erosion.march`, `test/erosion_test.march`

**Interfaces — Produces:**
```
Strata.Erosion.Params = Params(iterations : Int, dt : Float, k : Float, m : Float, n_exp : Float, uplift : Float, talus_lo : Float, talus_hi : Float)
Strata.Erosion.params(...) : Params;  Strata.Erosion.default_params() : Params   -- 50, 2.5e5 written out, 5.61e-7 written out, 0.4, 1.0, 5e-4 written out, 15.0, 60.0 (degrees)
Strata.Erosion.run(p : Params, n : Int, step : Float, height : NativeF32Arr, mask : NativeF32Arr, strength : NativeF32Arr, workers : Int) : NativeF32Arr
  -- height in place semantics: returns the eroded array; uplift is uplift * mask; talus angle = lerp(talus_lo, talus_hi, strength)
Strata.Erosion.receivers(n : Int, step : Float, height : NativeF32Arr) : NativeIntArr   -- lowest of 8 neighbours or -1 (a pit or the edge)
Strata.Atlas.build now runs erosion between mask and the rest; Def gains `erosion : Strata.Erosion.Params`
```
Each iteration: receivers; pits become lake nodes that spill at their lowest pass (Cordonnier's lake tree, O(n + m log m)); drainage area accumulated down the tree; the implicit update `h' = (h + dt * U + dt * k * A^m * h_rcv / step) / (1 + dt * k * A^m / step)` in topological order; then a thermal pass that lowers any slope steeper than the talus angle by half the excess. Rows are split across `workers` tasks for the receiver and thermal passes; the tree walk runs on one.

- [ ] **Step 1: failing tests** in `test/erosion_test.march`: on a 32 x 32 cone with a uniform mask, after `run` with default params every land sample's height is at or above its receiver's (no uphill flow) and the mean height dropped; a flat plate with a single pit: the pit's receiver after one iteration is on the lake's spill path, not -1; `run` is deterministic with 1 and with 4 workers (same array); runtime of 50 iterations on 128 x 128 printed.
- [ ] **Step 2:** run; expect FAIL. Implement.
- [ ] **Step 3:** the atlas test from Task 3 still passes; the demo's `atlas_0.ppm` shows valleys. Record the 512 x 512 build time in the ledger against the 60 s target.
- [ ] **Step 4:** `scripts/verify.sh` PASS. Commit `feat(atlas): stream-power erosion with lake nodes`.

### Task 5: The atlas: priority-flood, rivers, lakes, sites

**Files:** `lib/strata/atlas.march`, `lib/strata/flood.march`, `test/flood_test.march`, `test/atlas_test.march`

**Interfaces — Produces:**
```
Strata.Flood.fill(n : Int, height : NativeF32Arr, sea : Float) : (NativeF32Arr, NativeIntArr)
  -- (spill heights: height raised to the level water must reach to leave; receivers over the filled surface). Edge and sea cells are the outlets. Barnes 2014, binary heap on Float.
Strata.Atlas.Def gains river_min_area : Float (m^2), lake_margin : Float (m), site_count : Int, site_spacing : Float (m), site_flat : Float (m of height range over 3 x 3 samples)
Strata.Atlas.Site = Site(xm : Float, zm : Float, floor : Float, radius : Float, region : Int, index : Int)
Strata.Atlas.sites(a) : List(Site);  Strata.Atlas.site_near(a, xm, zm, within : Float) : Option(Site)
Strata.Atlas.rivers(a) : List(List((Float, Float, Float)))   -- polylines of (xm, zm, width)
```
After erosion: `fill`; a sample whose spill height exceeds its height by more than `lake_margin` is lake water at `spill - lake_margin`; drainage accumulated over the filled receivers; a sample with area over `river_min_area` is river with `width = 2.0 * (area / river_min_area) ^ 0.375` metres (Manning's 3/8 power, 2 m at the threshold); polylines traced from each river head to the sea. Sites: candidates ranked by `Seed.cell`, taken in order when flat enough, spaced, not river or lake, until `site_count` or none left.

- [ ] **Step 1: failing tests** in `test/flood_test.march`: a 16 x 16 bowl entirely above sea with no outlet but the edge: every cell's spill height is the rim's minimum and following receivers from any cell reaches the edge; a plate with a channel to the edge: spill equals height everywhere; sea cells (below `sea`) are outlets.
- [ ] **Step 2:** run; expect FAIL. Implement `Strata.Flood`.
- [ ] **Step 3: failing tests** in `test/atlas_test.march` (64 x 64, seed 7): every land sample reaches sea or the edge through receivers; every lake sample's level is below its spill height; along every river polyline the atlas height never rises; every site is at least `site_spacing` from every other, not on river or lake, and `site_near` finds it within its radius; the atlas hash is stable.
- [ ] **Step 4:** run; expect FAIL. Implement. Look at `atlas_2.ppm` (water) and `atlas_4.ppm` (river width).
- [ ] **Step 5:** `scripts/verify.sh` PASS. Commit `feat(atlas): priority-flood, rivers by drainage, lakes at spill height, sites`.

### Task 6: The island in the viewer, with its atlas cached

**Files:** `lib/strata/atlas.march` (save/load), `test/atlas_test.march`, `viewer/lib/strata_viewer.march`, `viewer/test/lakes_test.march`, `scripts/golden.sh`, `demo/lib/strata_demo.march`

**Interfaces — Produces:**
```
Strata.Atlas.to_bytes(a : Atlas) : NativeU8Arr;  Strata.Atlas.from_bytes(b : NativeU8Arr) : Option(Atlas)   -- a version byte, the Def, the arrays, the sites
Strata.Atlas.cache_name(d : Def, seed : Int) : String    -- "atlas-<seed>-<hash of Def>.bin"
Strata.Atlas.load_or_build(dir : String, d : Def, seed : Int, workers : Int) : Atlas   -- reads dir/cache_name, else builds and writes it (needs IO.FileSystem; lives in the viewer and demo, not the library, if the capability cannot be charged to a library module — the builder checks)
viewer: STRATA_ISLAND=1 (default 1; 0 keeps the old terrain for the lake goldens until Task 10 retires them); STRATA_ATLAS_DIR (default ".march/atlas")
viewer terrain: column height = Atlas.height_at + detail, detail = 6 m fBm (kind 0, 4 octaves, 1/40 cycles per metre) * damp, damp = clamp01(dist_to_river / (2 * river_width) ) * (lake ? 0 : 1); lakes are the fluid family at the atlas lake level; river and sea cells are the sink id 5 up to the river's surface (atlas height) or sea level
viewer scenes 22 (a river mouth from the sea side) and 23 (a lake above a river, from its bank); the test lake for scenes 18 and 19 becomes the largest atlas lake within 400 m of the origin
```

- [ ] **Step 1: failing tests** in `test/atlas_test.march`: `from_bytes(to_bytes(a))` has the same hash as `a`; a truncated byte array gives `None`; `cache_name` differs for a different Def.
- [ ] **Step 2:** run; expect FAIL. Implement.
- [ ] **Step 3: failing viewer tests** in `viewer/test/lakes_test.march`: with the island on, a column inside an atlas lake has fluid at the lake level and sand below; a column on a river is sink water with its surface at the atlas height; a column 2 m from a river bank has detail amplitude under 1 m (sample the height with and without detail); the same chunk generated twice is identical.
- [ ] **Step 4:** run; expect FAIL. Implement the viewer's island terrain and the cache. The scripted breach (`STRATA_BREACH`) uses the atlas lake.
- [ ] **Step 5: scenes.** 22 and 23 at frame 60; 18 and 19 re-stored with the atlas lake (`STRATA_BREACH=30`, frames 60, 360, 1200): the lake drains through the breach, the river below keeps its level. Look at each, then store. Other goldens run with `STRATA_ISLAND=0` in `golden.sh` and pass unchanged.
- [ ] **Step 6:** `scripts/verify.sh` PASS. Commit `feat(gen): the island in the viewer — atlas height, rivers and sea as sink, lakes at spill level, cached atlas`.

### Task 7: Density, lattice, caves, thermal erosion, the surface scan

**Files:** `lib/strata/lattice.march`, `lib/strata/surface.march`, `lib/strata/thermal.march`, `test/lattice_test.march`, `test/surface_test.march`, `test/thermal_test.march`, `viewer/lib/strata_viewer.march`, `viewer/test/lakes_test.march`

**Interfaces — Produces:**
```
Strata.Lattice.Dims = (4, 8, 4) voxels a cell: 9 x 5 x 9 samples a chunk, in NativeF32Arr of 405, index i + 9 * (j + 5 * k)
Strata.Lattice.fill(samples : NativeF32Arr, out : NativeF32Arr) : Int   -- trilinear to 32768 values, flat index as Strata.Coord.index; precomputed weights; returns 32768
Strata.Lattice.bounds(samples) : (Float, Float)                         -- min and max over the samples
Strata.Thermal.relax(h : NativeF32Arr, n : Int, step : Float, talus : NativeF32Arr, passes : Int) : NativeF32Arr   -- n x n column heights (34 for a chunk plus halo); each pass moves half the excess over the talus slope downhill
Strata.Surface.scan(density : NativeF32Arr, above : NativeF32Arr, rule) : NativeI32Arr
  -- density 32768 for the chunk, `above` the 1024 values one voxel above it; rule : (depth : Int, y_local : Int, under_water : Bool) -> Int, called per voxel; depth counts from the surface and resets on air; -1 depth for an air voxel
viewer: density = (H - y) * squash + A * noise3, squash 1.0, A 12 m, noise kind 0 at 1/60 cycles per metre, 3 octaves; tunnels = max(|a|, |b|) - 0.08 with a and b kind 1 at 1/25 cycles per metre; caverns = kind 0 at 1/90 cycles per metre over 0.55, both only below H - 6 m except where an entrance noise (kind 0, 1/120) exceeds 0.6; regions scale the cave thresholds by a per-region factor
viewer: early-outs: a chunk wholly above max(H) + A / squash is air, wholly below min(H) - A / squash - 40 m is stone, by Chunk.uniform
viewer scenes 24 (a cliff with scree) and 25 (a cave mouth)
```
The anti-goal on callbacks: `rule` is called once per voxel of a non-uniform chunk only, in the chunk's own task, and the viewer's rule is a small decision tree; the design accepts this as the per-column material tree.

- [ ] **Step 1: failing tests** `test/lattice_test.march`: samples all 1.0 fill to all 1.0; a linear ramp in x fills to the exact ramp at every voxel (within 0.0001); `bounds` of a ramp are its ends; time of `fill` over 1000 calls printed and under 60 µs a call in release (record; the design's risk).
- [ ] **Step 2:** run; expect FAIL. Implement with weights precomputed once per call as 32 x 32 x 32 index and fraction tables built on first use.
- [ ] **Step 3: failing tests** `test/thermal_test.march`: a vertical step of 10 m over one 0.25 m column with talus 45 degrees becomes a slope of at most 45 degrees after 20 passes; total height is conserved within 0.001; a slope under the talus is unchanged.
- [ ] **Step 4:** run; expect FAIL. Implement.
- [ ] **Step 5: failing tests** `test/surface_test.march`: a density that is solid below y = 10 and air above gives depth 0 at y = 10, 1 at y = 9, -1 above; a solid slab under an air gap under solid (an overhang) gives depth 0 on both solid tops; a one-voxel ceiling (solid, air below) gets a depth of 1000 (the sentinel); the chunk's top row uses `above` to find whether y = 31 is a surface.
- [ ] **Step 6:** run; expect FAIL. Implement.
- [ ] **Step 7: failing viewer tests** in `lakes_test.march`: no cave voxel within 4 m below any lake bed (Review Focus 4) over 20 lakes' chunks; a chunk 200 m above the ground is `uniform(0)` and one 200 m below is `uniform(1)`; a chunk generated alone equals one generated after its neighbours.
- [ ] **Step 8:** run; expect FAIL. Wire density, caves, thermal and the scan into the viewer's `terrain`. Measure chunk generation in the demo (`terrain: ... a chunk`) and record it against 1 ms median, 3 ms worst, and the air and solid early-outs against 20 µs (the bench prints them separately by chunk form).
- [ ] **Step 9: scenes.** 24 and 25 at frame 60, looked at and stored. `scripts/verify.sh` PASS. Commit `feat(gen): 3D density on a lattice, ridge caves, thermal cliffs, the surface scan`.

### Task 8: Templates and `.vox` import

**Files:** `native/ogt_vox.h` (vendored, MIT notice kept), `native/strata_vox.c`, `native/tests/vox_test.c`, `native/tools/make_ruins.c`, `lib/strata/ffi/vox.march`, `lib/strata/template.march`, `test/template_test.march`, `viewer/assets/ruins/*.vox` (CC0, from the tool), `demo/forge.toml`, `viewer/forge.toml`

**Interfaces — Produces:**
```
Strata.Template.Template = Template(sx : Int, sy : Int, sz : Int, ids : NativeI32Arr, prob : NativeU8Arr, yslice : NativeU8Arr, voxel_size : Float)
  -- id 0 is a hole (never written); prob 255 always; yslice per y row, 255 always
Strata.Template.make(sx, sy, sz, ids, voxel_size) : Template;  with_prob(t, i, p);  with_yslice(t, y, p)
Strata.Template.blit(c : Strata.Chunk.Chunk, k : Strata.Coord.Key, t : Template, ox : Int, oy : Int, oz : Int, rot : Int, stream : Int) : Strata.Chunk.Chunk
  -- world voxel origin, rot 0..3 about Y, stream a Seed value deciding probabilities; clipped to the chunk; writes only non-hole voxels
Strata.Template.bounds(t, ox, oy, oz, rot) : (Int, Int, Int, Int, Int, Int)
Strata.Template.from_vox(bytes : NativeU8Arr, palette_to_id : NativeI32Arr, voxel_size : Float) : Option(Template)
  -- the whole scene flattened through its nTRN/nGRP/nShp graph; palette index -> block id (0 for unmapped)
Strata.Ffi.Vox: x_vox_size(bytes, n) : Int (packed sx | sy << 10 | sz << 20, 0 on failure), x_vox_fill(bytes, n, out_palette_u8, cap) : Int  -- flattened scene, one palette index per voxel in x + sx * (y + sy * z)
native/tools/make_ruins.c: writes wall.vox, tower.vox, arch.vox (each under 48 voxels a side) with ogt_vox's writer
```

- [ ] **Step 1: failing C test** `native/tests/vox_test.c`: write a two-model scene with ogt_vox (a 2 x 2 x 2 cube at the origin and a translated, rotated copy), read it back through `sv_vox_size`/`sv_vox_fill`: the flattened grid holds both at the expected cells; a byte array that is not a `.vox` returns 0.
- [ ] **Step 2:** run via the C loop; expect FAIL. Implement `strata_vox.c` (`OGT_VOX_IMPLEMENTATION`) and the tool; run the tool to write `viewer/assets/ruins/`.
- [ ] **Step 3: failing tests** `test/template_test.march`: a 3 x 3 x 3 template with a hole in the middle blitted onto a stone chunk leaves the middle stone; the four rotations of an L-shape land where expected; prob 0 voxels are never written and prob 255 always; a blit straddling two chunks writes, across both, exactly the template's non-hole count; `from_vox` of `wall.vox` (read with `File.read`) has the tool's size and maps palette 1 to the id given.
- [ ] **Step 4:** run; expect FAIL. Implement; add `native/strata_vox.c` to the three `forge.toml`s.
- [ ] **Step 5:** `scripts/verify.sh` PASS. Commit `feat(template): voxel templates with holes, probabilities and rotation; .vox import through ogt_vox`.

### Task 9: Hashed-cell features and sites with beards

**Files:** `lib/strata/feature.march`, `test/feature_test.march`, `viewer/lib/strata_viewer.march`, `viewer/test/walker_test.march`, `scripts/golden.sh`

**Interfaces — Produces:**
```
Strata.Feature.Kind = Kind(cell : Float, radius : Float, jitter : Float, salt : Int, chance : Float)   -- metres; jitter <= 0.6
Strata.Feature.Placed = Placed(xm : Float, zm : Float, index : Int, rot : Int, stream : Int)
Strata.Feature.candidates(k : Kind, seed : Int, x0 : Float, z0 : Float, x1 : Float, z1 : Float, density) : List(Placed)
  -- every cell overlapping the box grown by radius; density : (xm, zm) -> Float in [0, 1] multiplies chance; index is a Seed value for the game to pick a template
viewer: trees are a Kind(6 m cell, 4 m radius, 0.6, salt, 0.35) with a per-region density; each tree is a template from a start-up library of 8 (trunk heights 4..8 m, canopy radii 2..3.5 m, built in code at the world's voxel size); the interior-only rule is deleted
viewer: sites from the atlas are blitted as the ruin templates; the column heights within a site's radius + 3 m are blended to the site floor (linear over the 3 m margin) before density, in `terrain`
viewer scenes 26 (a forest across a chunk border, camera on the border line) and 27 (a ruin on a slope)
```

- [ ] **Step 1: failing tests** `test/feature_test.march`: candidates for a box and for its two halves: the union of the halves' lists equals the whole's (same cells, same values); a cell at negative coordinates is not the mirror of the positive one (Review Focus 1); jitter keeps every placement inside its cell; chance 0 gives none, chance 1 one per cell; the same seed gives the same list.
- [ ] **Step 2:** run; expect FAIL. Implement.
- [ ] **Step 3: failing viewer tests** `walker_test.march`: a tree whose canopy straddles a chunk border: the voxels the two chunks hold are the template's, with none missing and none doubled (Review Focus 3), at each of the four rotations; a site's chunk has the ruin's floor row at the site floor and the ground meets it at every column of the footprint.
- [ ] **Step 4:** run; expect FAIL. Implement in the viewer.
- [ ] **Step 5: scenes.** 26 and 27 at frame 60, looked at and stored; `forest` and `full-forest` goldens are expected to change (trees now cross borders) and are re-stored after looking.
- [ ] **Step 6:** `scripts/verify.sh` PASS. Commit `feat(gen): features by hashed cells across borders; ruins on sites with the ground pulled up`.

### Task 10: Blasts, the structure layer, regenerate, the island soak

**Files:** `lib/strata/terrain.march`, `lib/strata/edit.march`, `test/terrain_test.march`, `test/edit_test.march`, `viewer/lib/strata_viewer.march`, `scripts/soak.sh`, `scripts/golden.sh`, `RESULTS.md`, `README.md`, `docs/generation-ledger.md`

**Interfaces — Produces:**
```
Strata.Terrain.Stamps = Stamps(Strata.Coord.Key -> Int -> NativeI32Arr)   -- per voxel the site index + 1, 0 for none; the game's replay of its template pass
Strata.Terrain.with_stamps(g : Gen, s : Stamps) : Gen;  Strata.Terrain.stamps(g : Gen, k : Key, seed : Int) : NativeI32Arr   -- all zeros for a generator without stamps
Strata.Edit.blast(w : World, cx, cy, cz : Int, radius : Int, seed : Int, id : Int) : Outcome
  -- a sphere whose radius varies by up to 30% with a hashed per-direction noise (Seed.cell of the voxel's direction octant and a ring index); otherwise fill_sphere
viewer: left click is a blast of radius 6 voxels (1.5 m at 0.25 m); spray of 200 particles in the removed voxels' mean colour, upward; STRATA_REGEN=1 drops every unedited chunk and regenerates after resetting the fluid; the old terrain (STRATA_ISLAND=0) is deleted and the goldens that needed it re-stored on the island
viewer: every generator parameter (atlas Def, noise settings, cave thresholds, feature kinds, per-region factors) is read from `viewer/assets/island.gen`, one `name = value` a line parsed in March, at start and again on regenerate; a missing name takes the code's default and is printed once
viewer scene 28 (a crater on a ruin)
```

- [ ] **Step 1: failing tests** `test/terrain_test.march`: `stamps` of a generator without stamps is all zeros; with a stamps function it returns what the function gives; `test/edit_test.march`: `blast` dirties only the chunks its bounds touch, on a border exactly two (Review Focus 5); the same seed gives the same crater; radius never exceeds 1.3 times the given.
- [ ] **Step 2:** run; expect FAIL. Implement.
- [ ] **Step 3: failing viewer test:** blasting a ruin's wall leaves `stamps` agreeing with the chunk (every voxel that is still the ruin's id has a stamp, every carved one is air); a blast under lake water wakes the fluid (`active_count` above 0 after `wake`).
- [ ] **Step 4:** run; expect FAIL. Implement: blast on click, spray, regenerate, the parameter file, delete the old terrain path and `STRATA_ISLAND`. A viewer test reads a file that sets the sea level to 50 and checks the generator's sea is 50.
- [ ] **Step 5: soak.** `scripts/soak.sh` flies the island circuit at 0.25 m, blasting every 100 frames, and after 1000 fluid steps asserts no atlas lake lost more than 1% of its amount (`Strata.Fluid.amount` over each lake's box) and the usual frame and step assertions. Run it; record.
- [ ] **Step 6:** goldens: scene 28 stored; every golden that was run with the old terrain is re-stored on the island after looking. `RESULTS.md` gets the design's target table (atlas build, chunk median and worst at 0.25 m and 0.5 m, air and solid chunk time, soak worst frame, lake loss). README: the island, the atlas cache, the generator calls a game makes, the scale rule. The ledger records rulings.
- [ ] **Step 7:** `scripts/verify.sh` PASS. Commit `feat(gen): blasts, the structure layer replayed, regenerate, soak over an island`.

### Task 11: Levels in the generator and the stream

**Files:** `lib/strata/terrain.march`, `lib/strata/stream.march`, `lib/strata/world.march`, `test/terrain_test.march`, `test/stream_test.march`, `viewer/lib/strata_viewer.march`, `demo/lib/strata_demo.march`

**Interfaces — Produces:**
```
Strata.Terrain.gen_levels(f : Key -> Int -> Chunk, max_level : Int) : Gen;  Strata.Terrain.max_level(g : Gen) : Int   -- `gen` is `gen_levels(f, 0)`
Strata.Coord.key_at(cx, cy, cz, level) : Key;  Strata.Coord.parent(k) : Key;  Strata.Coord.children(k) : List(Key)   -- eight, level - 1
Strata.Stream.Config gains radii per level: config_levels(radius_h : Int, radius_v : Int, apply_budget, workers, far_h : Int, far_v : Int, levels : Int)
  -- levels 1..levels each load a box of far_h x far_v chunks of their own size around the focus; level 0 as before. `config` is `config_levels(.., 0, 0, 0)`.
Strata.Stream.take_meshes returns meshes at every level; Strata.Mesh.level(m) carries the key's level
Strata.World: an edited level-0 chunk is coarsened into its ancestors when they are generated or regenerated: World.coarsen(w, k : Key) : Chunk takes the max-occupancy voxel of each 2^L cube over loaded or cached fine chunks, else the generated one; an edit marks ancestors stale (World.stale(w) : List(Key)) and the stream regenerates them before their next mesh
viewer/demo generator: the chain at scale * 2^level: atlas step unchanged, detail noise the same in metres, caves skipped above level 0, trees as one canopy-colour voxel at level 1 and none above, sites' beards at every level, sea beyond the atlas at every level
```

- [ ] **Step 1: failing tests** `test/terrain_test.march`: `max_level` of `gen` is 0; `run` of a level-2 key on a generator declaring level 1 gives `uniform(0)`; `test/stream_test.march`: with `config_levels(2, 1, 8, 4, 2, 1, 2)` a settled stream holds level-0, level-1 and level-2 chunks and `world_hash` is order-independent across two focus paths; an edit at level 0 makes its level-1 and level-2 ancestors stale and after a tick they are regenerated with the edit coarsened in (a 2 x 2 x 2 carve shows as one air voxel at level 1).
- [ ] **Step 2:** run; expect FAIL. Implement.
- [ ] **Step 3: failing viewer test:** a level-1 chunk's solid set contains the coarsening of its eight level-0 children on 10 random keys within one voxel (the design's approximation); a level-3 key outside the atlas is sea (Review Focus 2); a level-1 chunk generates in under 1 ms (printed by the demo's bench with a level argument).
- [ ] **Step 4:** run; expect FAIL. Implement the viewer's and demo's level support.
- [ ] **Step 5:** `scripts/verify.sh` PASS (no golden changes: nothing draws levels yet). Commit `feat(far): generator levels, per-level stream boxes, edits coarsened into ancestors`.

### Task 12: The far field drawn

**Files:** `native/strata_vk.c`, `native/shaders/quad.vert`, `lib/strata/ffi/vk.march`, `lib/strata/render.march`, `lib/strata/slots.march`, `test/slots_test.march`, `viewer/lib/strata_viewer.march`, `scripts/golden.sh`, `scripts/soak.sh`, `RESULTS.md`, `README.md`, `docs/generation-ledger.md`

**Interfaces — Produces:**
```
sv_chunk_begin(slot, ox, oy, oz, scale) : the chunk's voxel size in metres; the vertex shader multiplies local positions by it; textures repeat per fine voxel (uv scaled by 2^level)
Strata.Slots.plan applies Part F's which-chunk-draws rule: a key is drawn unless all eight children are held and meshed; a level-0 key is drawn only when its seven siblings are held; a departed key's slot is kept until its parent's mesh is uploaded (Slots.retained(s) : Int)
Strata.Render.sync unchanged in signature; it frees retained slots when parents arrive
viewer: default config_levels(10, 3, 8, 4, 10, 1, 4) at 0.25 m (rings to 1.3 km); STRATA_LEVELS=n overrides; fog from the renderer design blends the outer ring
viewer scenes 29 (the island from its highest hill) and 30 (a ring boundary over terrain, camera low); the soak records the reference scene's chunk count and worst frame
```

- [ ] **Step 1: failing tests** `test/slots_test.march`: a plan with a level-1 key and seven of its children held draws the level-1 key and none of the children; with all eight it draws the children and hides the parent; when the eight children leave, the parent is drawn and the children's slots are retained until the parent's upload action is performed; over a scripted sequence of arrivals and departures no region is drawn twice or not at all (a checker over the keys' boxes).
- [ ] **Step 2:** run; expect FAIL. Implement in `Slots`.
- [ ] **Step 3:** the shim and shader: `sv_chunk_begin` takes the scale; `quad.vert` scales positions and texture repeats; `build_shaders.sh` run. The light volume stays level 0: a coarse chunk is lit by sun and sky only (`set_light_box` unchanged; coarse chunks are flagged in the chunk table and skip the ray pass).
- [ ] **Step 4: scenes.** 29 and 30 at frame 120 (the rings need time to arrive; `run_until_settled` first), looked at and stored. Every level-0 golden unchanged with `STRATA_LEVELS=0` in `golden.sh`.
- [ ] **Step 5: measure.** The soak at the default levels: chunk count (target about 8400), worst frame against the budget, level-1 chunk generation time. `RESULTS.md` rows: far-field chunk count, worst frame, far shore visible (yes or no with scene 29). README: the far field, `config_levels`, the limits (lit without rays, seams of one coarse voxel). Ledger rulings.
- [ ] **Step 6:** `scripts/verify.sh` PASS. Commit `feat(far): rings of coarser chunks drawn, one level per region, meshes retained across the hand-off`.

---

## Out of scope for this plan

Cities and interiors, water sources and sinks on bounded maps, rigid debris, saving worlds, random-walk carvers, wave function collapse, roads between sites, lighting the far field with rays, smooth terrain.

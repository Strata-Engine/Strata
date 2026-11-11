# Feature and structure placement for procedural voxel worlds

Scope: how shipped voxel games and the literature place "features" (trees, boulders, plants) and "structures" (villages, dungeons, ruins) on generated terrain, with emphasis on determinism and chunk-border behaviour. Target context: 32³ chunks generated as a pure function of (chunk key, seed); trees currently confined to the chunk interior so canopies never cross a border.

Locality legend used throughout: **[LOCAL]** = computable from world coordinates + seed with no neighbour data; **[NEIGHBOURS]** = needs data (or at least the *candidate list*) of adjacent chunks; **[GLOBAL]** = needs a world-wide or region-wide pass / sequential dependency.

---

## Key question 1: Scatter placement — deterministic per-chunk RNG, Poisson-disc / jittered grids, density from noise, biome-dependent rates; how a feature decides its own existence from world coordinates alone

### Takeaway
Every shipped system that stays deterministic hashes (seed, region coords, salt/index) into a private RNG stream per chunk/cell/feature; the cheap, border-safe way to make neighbouring chunks agree is a jittered (or hashed) grid where each cell owns at most one feature and the cell's global coordinates alone decide the feature's existence and offset. True Poisson-disc (Bridson) gives better blue noise but is inherently sequential and therefore needs padding or a tiling trick to be chunk-local.

### Cited Findings

**Per-chunk / per-feature RNG streams (Minecraft)**
- Minecraft's `WorldgenRandom.setDecorationSeed` takes the level seed and a chunk's minimum block X/Z and returns a seed for that chunk's decoration pass; `setFeatureSeed` takes that decoration seed, a feature index, and a decoration-step ordinal "to give each feature its own random stream"; the index is "the cumulative index of the generating feature within the biome's list of features" — [WorldgenRandom javadoc (1.21.x)](https://aldak.netlify.app/javadoc/1.21.1-21.1.x/net/minecraft/world/level/levelgen/worldgenrandom)
- Known weakness of that scheme per the same javadoc: "the salt-based approach does not do a good job of this, and issues can arise from the salt being small with features that have the same decoration step and are close together in the feature lists" (i.e. correlated feature placements) — [WorldgenRandom javadoc](https://aldak.netlify.app/javadoc/1.21.1-21.1.x/net/minecraft/world/level/levelgen/worldgenrandom)
- PaperMC ships a "Configurable feature seeds" patch that lets operators fix a seed per configured feature, feeding it through `setDecorationSeed` + `setFeatureSeed` — [PaperMC patch](https://ayakael.net/mirrors/papermc/src/commit/3f2159e43cac68211316ea2822e8411140b2b834/patches/server/0800-Configurable-feature-seeds.patch)
- Structure-set placement is randomised by a `salt` ("a non-negative integer that randomizes placement, similar to a salt in cryptography"); the `frequency_reduction_method` `default` "depends on seed, position, and salt", `legacy_type_1`/`legacy_type_3` "depends only on seed and position", `legacy_type_2` uses "fixed salt 10387320" — [Minecraft Wiki: Structure set](https://minecraft.wiki/w/Structure_set)

**Minecraft placed-feature modifiers (the per-chunk scatter vocabulary)** — all from [Minecraft Wiki: Placed feature](https://minecraft.wiki/w/Placed_feature):
- A biome-referenced placed feature starts "once on the northwest corner of each chunk at the bottom layer of the world"; modifiers run in list order and "each placement attempt applies placement modifiers separately".
- `count` (0–4096; stacks multiplicatively), `rarity_filter` (`chance`: pass with probability 1/chance), `in_square` (adds random 0–15 to X and Z), `heightmap` (snap Y to one above `MOTION_BLOCKING`, `MOTION_BLOCKING_NO_LEAVES`, `OCEAN_FLOOR`, `OCEAN_FLOOR_WG`, `WORLD_SURFACE`, `WORLD_SURFACE_WG`), `offset` (each axis −16..16).
- `noise_based_count`: count = `ceil((noise(x / noise_factor, z / noise_factor) + noise_offset) * noise_to_count_ratio)`, nothing when noise ≤ 0.
- `noise_threshold_count`: samples `noise(x / 200, z / 200)`; uses `below_noise` count if below `noise_level`, else `above_noise`.
- `biome` (pass only if the biome at the position lists this feature — keeps features inside biome edges), `block_predicate_filter`, `surface_water_depth_filter` (`max_water_depth`), `surface_relative_threshold_filter`, `environment_scan` (`up`/`down`, `max_steps` 1–32), `count_on_every_layer` (0–256).
- Since JE 26.4 snapshot 1 the wiki states: "feature placement can only occur within a 3×3 chunk region".

**Luanti/Minetest decorations (grid-divided scatter)** — from [Luanti lua_api.md, Decoration definition](https://raw.githubusercontent.com/luanti-org/luanti/master/doc/lua_api.md):
- `deco_type` is `"simple"`, `"schematic"` or `"lsystem"`; `place_on` node(s); `sidelen` = "Size of the square X/Z divisions of the mapchunk" (adjusted if the mapchunk size is not divisible by it); `fill_ratio` = decorations per surface node, used only when `noise_params` is absent; "Values of 10.0 or more enable full coverage"; `noise_params` gives density per division ("A value of 10.0 or more enables full coverage with a faster method").
- Filters: `biomes` (all if omitted), `y_min`/`y_max` (inclusive, measured at the `place_on` node), `spawn_by` + `num_spawn_by` (−1 = any), `check_offset` ∈ {−1,0,1}, flags `liquid_surface`, `force_placement`, `all_floors`, `all_ceilings`.
- Simple type: `decoration` node (list → random pick), `height` (default 1) / `height_max` (0 = fixed; else lower bound of a random range; [1, 32767]), `param2`/`param2_max`, `place_offset_y`. Schematic type: `schematic` (`.mts` path, inline table, or registered ID), `replacements`, `flags` `place_center_x/y/z`, `rotation` `"0"|"90"|"180"|"270"|"random"`, `place_offset_y`. L-system type: `treedef` (same as `core.spawn_tree`).
- Luanti noise params: fields `offset, scale, spread, seed, octaves, persistence, lacunarity`, defaults `seed = 12345`, `octaves = 3`, `persistence = 0.6`; `eased` flag "Maps noise gradient values onto a quintic S-curve before performing interpolation", `absvalue` flag — [Luanti lua_api.md, Noise Parameters](https://raw.githubusercontent.com/luanti-org/luanti/master/doc/lua_api.md)
- Luanti ore scatter rule-of-thumb: `clust_scarcity` = "Chance of an ore spawning in a node is 1 in this value. For an average spacing of d, use d × d × d"; `clust_num_ores` 1–32767; `clust_size` 0–32767 — [Luanti lua_api.md, Ore definition](https://raw.githubusercontent.com/luanti-org/luanti/master/doc/lua_api.md)
- A Luanti "mapchunk" "is usually 5x5x5 mapblocks" "(80x80x80 nodes) and is the volume of world generated in one operation by the map generator"; since 5.15.0 the `chunksize` setting "can be a vector, instead of a single integer" — [Luanti lua_api.md](https://raw.githubusercontent.com/luanti-org/luanti/master/doc/lua_api.md)

**Hashed-grid (one feature per cell) placement — chunk-local by construction**
- Frozen Fractal ("Around the World", 1×1 km chunks): "Each grid cell contains the center of at most one plant", whose species and position are "computed deterministically from the hash of the cell's global coordinates"; neighbouring chunks "may not exist yet, so the method can't depend on them". Per-biome coverage is a threshold 0..1 on a single simplex-noise layer; each species has a base occurrence weight; a per-species slope limit rejects cliffs; a ports/clearing mask ramps spawn probability from 0 inside an inner radius to 1 at an outer radius. Example: 3×3 cells per chunk, species "green" (small radius, weight 1) and "blue" (large radius, weight 0.5) — [Frozen Fractal: Planting trees](https://frozenfractal.com/blog/2025/11/28/around-the-world-27-planting-trees/)
- Same post on overlap pruning: hash-based "largest hash wins" pruning failed because pruning chains made results "depend, at least a little bit, on chunk creation order" and biased toward small plants; final decision: "overlapping plants are okay" — [Frozen Fractal](https://frozenfractal.com/blog/2025/11/28/around-the-world-27-planting-trees/)
- Zylann's godot_voxel docs: seed an RNG "with a hash of the block's 2D coordinates, combined with the world seed" (world seed alone "would put trees at the same positions in every block"); each 16-block column block picks `rng.randi_range(0, 2)` trees at random X/Z; altitude obtained by "computing the height function again" at (x,z) — [voxel-tools: Procedural generation](https://voxel-tools.readthedocs.io/en/latest/procedural_generation/)

**Jittered grid vs Poisson disc (Red Blob Games)** — [Jittered grid vs Poisson disc](https://redblobgames.com/x/1830-jittered-grid):
- Jittered grid: square or hex grid, each point offset by `jitter * spacing * (random() - 0.5)`. Two conflicting constraints: even angle distribution needs "`jitter ≥ 0.9`", avoiding close points/gaps needs "`jitter ≤ 0.6`"; conclusion "There's no `jitter` value that gives me an even distribution of angles while avoiding distance outliers" and "Jittered grids are easy to implement but the output isn't great."
- Poisson disc "prevents points from being too close together" but "sometimes allows points to be too far"; default slack "`max` to be `2*min`"; tries default 30, "Values above 30 do not seem to improve quality", 5 tries "allows it to run 2–4X faster".
- Comparison table: jittered/low jitter = tight distances, non-uniform angles; jittered/high jitter = the reverse; Poisson/low slack = both good.
- The author says the best blue noise comes from Poisson disc or Lloyd relaxation, "Both can be slow", and cites 2019–2021 algorithmic improvements on Bridson — [Red Blob (via search)](https://redblobgames.com/x/1830-jittered-grid)

**Bridson 2007 (Fast Poisson Disk Sampling in Arbitrary Dimensions)** — [PDF](https://www.cs.ubc.ca/~rbridson/docs/bridson-siggraph07-poissondisk.pdf):
- Inputs: domain extent in Rⁿ, minimum distance r, constant k "(typically k = 30)".
- Step 0: background grid with cell size "bounded by r/√n, so that each grid cell will contain at most one sample" (array of ints, −1 = empty).
- Step 1: initial sample uniform in domain; push onto an "active list".
- Step 2: while active list non-empty, pick random index i, generate up to k points "uniformly from the spherical annulus between radius r and 2r around xᵢ"; accept the first farther than r from all existing samples (grid lookup), else remove i from the active list.
- "Step 2 is executed exactly 2N−1 times to produce N samples", so O(N) total; the paper contrasts with jittered/stratified sampling "(which reduces but doesn't eliminate clustering)".
- Implementations: `poisson_diskus` (Rust) documents "the recommended value is 30" attempts; `fast_poisson` (Rust); `poisson_disc` (PyPI, arbitrary dimensions) — [lib.rs fast_poisson](https://lib.rs/crates/fast_poisson); [poisson_diskus](https://github.com/pjohansson/poisson_diskus); [pypi poisson-disc](https://pypi.org/project/poisson-disc/)

**Cutting a feature off at the border is the failure mode of non-deterministic placement**
- "if tree locations are random then you'll have half-trees at chunk borders" (hence "all the placement logic has to be deterministic") — [r/VoxelGameDev thread](https://pol1.lr.ggtyler.dev/r/VoxelGameDev/comments/1hb1emb/understanding_how_terrain_generation_works_with/m1dcp39/?context=3)
- Pyrite (hobby engine) sidesteps borders by rule: "A tree's bounding box cannot exceed the chunk's boundaries" — [Pyrite terrain docs](https://shivamkr12.github.io/Pyrite/terrain.html)

### Inferences
- For a pure `(chunk key, seed) → chunk` function, the hashed/jittered-grid scheme is the natural fit: choose a cell size ≥ the largest feature footprint, hash `(seed, cellX, cellZ, salt)` for existence, offset, species and rotation; a chunk then enumerates all cells overlapping its footprint padded by the maximum feature radius (see Q2). Neighbouring chunks agree because they evaluate the same hash for the same cell.
- Minecraft's `noise_based_count` / `noise_threshold_count` and Luanti's `noise_params`-per-division are the standard "density map from noise" idiom: a low-frequency 2D noise (Minecraft samples at x/200, z/200 for threshold-count) modulates a per-chunk count or per-cell probability. Biome-dependent rates are expressed as per-biome feature lists (Minecraft) or per-decoration `biomes` filters (Luanti), evaluated at the candidate position so biome edges are respected without neighbour data (the biome function itself must be a pure function of world coords).
- Bridson's sequential dependence (active-list growth from a random seed point) means a straight implementation is **[GLOBAL]** within whatever region it runs. To make it [LOCAL] one runs it per *padded* region and clips, which is deterministic but not seamless at padded-region edges (points near the edge differ from the run in the neighbouring region), or one uses the layered scheme of Q5 (modifying-in-blocks) — nothing in the sources gives a seamless deterministic Poisson-disc tiling for free; the jittered grid is the usual compromise.
- Red Blob's jitter numbers translate directly: with cell size c and jitter 0.6, min distance between two features ≈ 0.4c and max ≈ 1.6c√2; so a 32-block chunk divided into 8-block cells (Luanti default-style `sidelen`) gives roughly 3–13 block spacing.
- Minecraft's own feature-seed weakness (small salts → correlated streams) argues for a strong 64-bit mixer (e.g. splitmix/xxhash-style) over `seed ^ (x*k1 + z*k2)` constructions when deriving per-feature streams.

### Gaps
- Red Blob's dedicated Poisson-disc article (`/x/1830-poisson-disk/`) returned 404; the jittered-grid notes are explicitly "unfinished" per the author.
- I found no primary source stating Minecraft's exact `setDecorationSeed`/`setFeatureSeed` arithmetic (the LCG constants); only the javadoc descriptions. The decompiled source would be needed.
- No source quantified the visual difference between hashed-grid and Poisson-disc placement for *voxel* trees specifically.

---

## Key question 2: Crossing chunk borders — Minecraft's 3×3 feature window and proto-chunks; overflow/deferred-edit buffers; padded-region-and-clip; interior-only features; trade-offs

### Takeaway
Minecraft solves borders by generating in stages with a dependency "pyramid": a chunk reaches `features` only after its 3×3 neighbourhood is at `carvers`, and features may write into that 3×3 (structure *references* use a 17×17 window). The purely functional alternatives are (a) **padded generation**: each chunk enumerates feature candidates from a neighbourhood large enough to cover the maximum feature radius and clips the voxels that land inside itself (deterministic, no neighbour state, costs re-generating each feature up to 9× or caching templates), or (b) interior-only placement (what Strata does now). Deferred overflow buffers are order-dependent unless tightly controlled.

### Cited Findings

**Minecraft generation steps and neighbour radii**
- Chunk statuses in generation order (current wiki): `empty, structure_starts, structure_references, biomes, terrain, features, initialize_light, light, spawn, full`; the statuses `noise`, `surface`, `carvers` "have been replaced by `minecraft:terrain`" (26.3 snapshot 10); "All status except `minecraft:full` are used for chunks called *proto-chunks*"; `PostProcessing` lists block positions "that need to receive an update when a *proto-chunk* turns into a full chunk" — [Minecraft Wiki: Chunk format](https://minecraft.wiki/w/Chunk_format)
- Per-stage access, as documented by a modder's breakdown (community source, pre-26.3 stage names) — [neoforged/Documentation issue #347](https://github.com/neoforged/Documentation/issues/347):
  - `STRUCTURE_STARTS`: computes structure positions from raw biome samples and a 1×1 "column" height sample; "This chunk may only see itself, accessing beyond itself throws a crash."
  - `STRUCTURE_REFERENCES`: gives each chunk references to structure starts whose bounding boxes intersect it; reads a "17 x 17 dependency chunk area" set by the ChunkPyramid, i.e. 8 chunks each way; the author derives a structure size cap of 272×272 blocks.
  - `BIOMES`: caches biomes at 2×2×2 per section; reads 17×17 structure data.
  - `NOISE`: first block-placing stage; "It may only place blocks within itself"; sees 3×3 biomes.
  - `SURFACE`, `CARVERS`: place only within own chunk; see 3×3 biomes / 17×17 structure data.
  - `FEATURES`: trees, ores, grass, flowers, plus the structure pieces from the start/reference stages; sees a 3×3 area of carvers and "Can place blocks in neighboring chunks, giving it a total space of 3x3 chunks"; heightmaps are primed first; it is "the final block-placement stage".
  - Then `INITIALIZE_LIGHT`, `LIGHT`, `SPAWN`, `FULL` (ProtoChunk → LevelChunk).
- Minecraft wiki (JE 26.4 snapshot 1) independently: "feature placement can only occur within a 3×3 chunk region" — [Minecraft Wiki: Placed feature](https://minecraft.wiki/w/Placed_feature)
- Older (1.12-era) design, from the Streams mod author: decorations are generated in "a chunk-sized area which sits at the middle of 4 freshly-generated chunks" with the current chunk at the northwest, giving an "8-block wide 'buffer zone'"; the offset idiom is `chunk.x + random.nextInt(16) + 8`; decorations with radii over 8 blocks spill, and spilling "causes neighboring chunks to generate, which can cascade" ("performance-killing cascading generation"). Features/structures have "a radius of 8 chunks", so a chunk checks a "17-chunk-wide box centered on itself"; structures cannot inspect terrain because "the game would go into an infinite loop as each of these chunks checked an 8-chunk radius around themselves" — which is why villages get basements in ravines — [delvr gist](https://gist.github.com/delvr/bfac43cd48675ec74b905ffe640a6a11)
- Spigot bug reports observed that loading one chunk triggered neighbour generation at a 2-chunk radius (one report) or suspected 1 chunk in 1.16.5 (another) — [SPIGOT-4862](https://hub.spigotmc.org/jira/si/jira.issueviews:issue-html/SPIGOT-4862/SPIGOT-4862.html); [SPIGOT-6348](https://hub.spigotmc.org/jira/si/jira.issueviews:issue-html/SPIGOT-6348/SPIGOT-6348.html)
- Sponge API describes `STRUCTURE_STARTS` as "A chunk where the structures to be placed in the chunk are being determined and primed for placement" and `STRUCTURE_REFERENCES` as the final validity check — [SpongeAPI ChunkStates](https://jd.spongepowered.org/spongeapi/8.2.0/org/spongepowered/api/world/chunk/ChunkStates.html)

**Padded generation / enumerate neighbour candidates (no neighbour *state* needed)**
- godot_voxel: "Instead of just considering trees in the current block, we can also check trees that would generate in neighbor blocks" (the 8 neighbours); each tree is pre-generated into a `VoxelBuffer` with known bounds; "Once we know the bounds of each tree, we can check if they intersect with the current block" and only those are pasted with `paste_masked` so tree air doesn't overwrite terrain; because trees are recomputed several times, "it is possible to optimize this step by pre-generating (or making by hand) a bunch of trees ahead of time". Limitation: "the engine generates cubic blocks, so when generating a given 16x16x16 voxel, you can't access what's below, you only know what's inside the area of the block" — [voxel-tools: Procedural generation](https://voxel-tools.readthedocs.io/en/latest/procedural_generation/)
- Reddit commenter: "generate the locations of all the trees in your neighbouring chunk without generating the entire neighbouring chunk", caching each data layer; this doesn't solve all inter-chunk dependencies (e.g. erosion) — [r/VoxelGameDev](https://red.127.pp.ua/r/VoxelGameDev/comments/1hb1emb/understanding_how_terrain_generation_works_with/m1ibvk5/?context=3)
- The search radius must be finite: trees crossing borders need their "radius to be bounded to some degree" — [r/VoxelGameDev](https://redlib.hackliberty.org/r/VoxelGameDev/comments/1gka5yp/trees_in_block_games)
- Order-dependence warning: resolving overlaps with a fixed ordering across chunks "would make placement depend, at least a little bit, on chunk creation order" — [Frozen Fractal](https://frozenfractal.com/blog/2025/11/28/around-the-world-27-planting-trees/)

**Interior-only**
- Pyrite: "A tree's bounding box cannot exceed the chunk's boundaries" — [Pyrite](https://shivamkr12.github.io/Pyrite/terrain.html)

**Determinism of lazy chunked generators (from the WFC literature, applies generally)**
- Boris the Brave: in lazy chunk generators that constrain each new chunk against whatever neighbours happen to exist, "even using the same random seed, you'll see different results"; a fixed dependency tree per block restores determinism — [Infinite Modifying in Blocks](https://boristhebrave.com/2021/11/08/infinite-modifying-in-blocks)

### Inferences
- Minecraft's pipeline is **[NEIGHBOURS]** with a graded radius: terrain/biomes need a 3×3 biome window, features write into a 3×3 chunk window, structures reference a 17×17 window. It is only tractable because proto-chunks persist partially generated state; it is not a pure per-chunk function.
- The "padded region and clip" approach is **[LOCAL]** in the strict sense required by Strata: chunk C evaluates candidates in the cells overlapping `bounds(C) ⊕ R_max`, generates each candidate's voxel set (or looks it up in a template cache), and writes only the voxels inside C. Correctness requires: (1) candidate existence/parameters depend only on world coords + seed; (2) the ground height at the candidate's base is a pure function of (x,z) — fine for heightmap terrain, not for terrain with 3D caves/overhangs unless the feature's base test is also evaluable from the column function; (3) R_max is a hard cap (Minecraft's analogue: features ≤ 8 blocks over the edge in 1.12, 3×3 chunks now). Cost: with 32-block chunks and R_max ≈ 8 (a 5–7 block canopy per Minecraft), the padded footprint is 48×48 = 2.25× the area, so ~2.25× the candidate evaluations; template blitting makes each candidate O(voxels in intersection).
- Deferred overflow buffers ("write the overhang into a pending edit list for the neighbour") are **[NEIGHBOURS]** and introduce ordering hazards: whichever chunk generates first decides whether its tree or the neighbour's later feature wins the shared voxels, unless writes are commutative (e.g. priority by feature hash) — which is exactly the order-dependence Frozen Fractal rejected. They also add persistent state outside the pure function. They are only safe if applied *before* the neighbour's own feature pass with a deterministic merge rule.
- For 3D (cubic) chunks the vertical axis is the same problem as the horizontal one: a tree whose base is in the chunk below must be enumerable from the chunk above (godot_voxel's "you can't access what's below"). The padded approach handles this if candidates are keyed by (x,z) column cells and the base height is a pure function.

### Gaps
- The exact per-status neighbour radii come from a community-maintained issue (neoforged #347) and a 1.12-era gist; I could not retrieve the `ChunkStatus`/`ChunkPyramid` source constants directly. Treat "3×3 for features, 17×17 for structure references" as well-corroborated but version-dependent.
- No source gave measured overhead numbers (ms/chunk) for padded-vs-interior tree placement.
- I found no shipped voxel game documenting a deferred-overflow-buffer design in detail; it appears only as forum suggestions.

---

## Key question 3: Tree and plant generators — Minecraft tree shapes, L-systems in voxels, space colonisation, parametric trees, Luanti's L-system API, per-tree generation vs template blitting

### Takeaway
Shipped voxel games use cheap parametric trunk+foliage placers (Minecraft) or pre-authored schematics with per-node probabilities (Luanti), both [LOCAL] given a seed; Luanti also ships a full turtle-interpreted L-system (`core.spawn_tree`) with stochastic rule symbols. Space colonisation (Runions 2007) gives the most natural crowns but is iterative over hundreds–thousands of attraction points and is better used offline to bake templates than per chunk.

### Cited Findings

**Minecraft tree feature (data-driven, per-tree procedural)**
- Trunk placers: `straight_trunk_placer, forking_trunk_placer, giant_trunk_placer, mega_jungle_trunk_placer, dark_oak_trunk_placer, fancy_trunk_placer, bending_trunk_placer, upwards_branching_trunk_placer, cherry_trunk_placer, poplar_trunk_placer`; fields `base_height` 0–32, `height_rand_a` 0–24, `height_rand_b` 0–24 ("a random value between 0 and this value (inclusive) is added to the height of the trunk"); straight has `trunk_width` (default 1); bending has `bend_length` 1–64 — [Minecraft Wiki: Tree definition](https://minecraft.wiki/w/Tree_definition); [wiki mirror](https://wiki.ronlab.site/content/minecraftwiki_en_all_maxi_2025-11/Tree_definition)
- Foliage placers share `radius` 0–16 and `offset` 0–16 (int providers); `blob` adds `height` 0–16; `spruce` has `trunk_height` 0–24; `random_spread` has `foliage_height` 1–512 and `leaf_placement_attempts` 0–256 — [Minecraft Wiki: Tree definition](https://minecraft.wiki/w/Tree_definition)
- `minimum_size`: `two_layers_feature_size` (`limit` default 1, 0–81; `lower_size` default 0; `upper_size` default 1; 0–16) or `three_layers_feature_size` — Fabric wiki: TwoLayersFeatureSize "is used to see how tall the tree can be without clipping into blocks" — [Minecraft Wiki: Tree definition](https://minecraft.wiki/w/Tree_definition); [Fabric wiki: trees](https://wiki.fabricmc.net/tutorial:trees)
- Decorators: `trunk_vine` (75% per side of each trunk block), `leave_vine` (`probability`), `cocoa` (`probability`, then 25% per face), `beehive` (`probability`), `alter_ground` (block provider); `ignore_vines` default false — [Minecraft Wiki: Tree definition](https://minecraft.wiki/w/Tree_definition)
- Fabric tutorial example values: `new StraightTrunkPlacer(8, 3, 0)` (base, firstRandom, secondRandom), `new BlobFoliagePlacer(ConstantIntProvider.create(5), ConstantIntProvider.create(0), 3)` "(radius, offset from trunk, height)", `new TwoLayersFeatureSize(1, 0, 1)`; tree generation "is split into a TrunkPlacer, which generates the trunk, and a FoliagePlacer, which generates the leaves" — [Fabric wiki: trees](https://wiki.fabricmc.net/tutorial:trees)
- In-game shapes: trees range "from a bare minimum of 1 log block for jungle bushes" up to 30 blocks (mega jungle); canopies are "roughly spherical clusters of leaves about 5–7 blocks across"; leaves grow "1 block higher than the highest log block" (fancy oak: 3 higher); since 18w22a natural leaves persist "at a distance of up to 6 blocks from logs, instead of 4"; a sapling needs "at least 6 blocks of space above it"; if an obstruction is hit the tree "fails and retries on a later pass"; acacia "About 8 blocks tall" with diagonal trunks; dark oak "Thick 2×2 trunks", "typically 6–8 blocks tall"; mega pines "31 blocks tall" (conflicts with the 30 max stated on the same page) — [Minecraft Wiki: Tree](https://minecraft.wiki/w/Tree)
- Big/fancy oak (community reverse-engineering): main trunk plus branches with parameters `heightLimit`, `trunkHeight`, `leafDistanceLimit` ("set to 4 for sapling growth, though appears to be set to 5 for natural growth"); branches must fit inside a sphere whose radius scales with the height limit; leaves are a sphere of radius `leafDistanceLimit` around each branch end — [inxomnyaa gist](https://gist.github.com/inxomnyaa/806dbdf820f11c3ff8e55464dc12038a)
- A third-party add-on's defaults for a simple tree: log height 4 ± 2, leaf radius 2, leaf height 3 (not vanilla values) — [ccpacks docs](https://ccpacks-docs.readthedocs.io/en/latest/features/tree_gen/)

**Luanti L-system trees (`core.spawn_tree(pos, treedef)`)** — [Luanti L-system trees](https://api.luanti.org/l-system-trees/):
- `treedef` fields: `axiom`; `rules_a..rules_d` (expansions for A–D); `trunk`, `leaves`, `leaves2`, `leaves2_chance` (0–100 % to swap leaves for leaves2); `angle` (degrees); `iterations` (2–32767, "usually 2 to 5"); `random_level` (random 0..value subtracted from iterations, never below 2, "Usually 0 to 3"); `trunk_type` `"single"|"double"|"crossed"` (1 node, 2×2, or 3×3 cross trunk); `thin_branches` (bool); `fruit`, `fruit_chance` (0–100); `seed` (s32, engine-generated if omitted).
- Symbols: `G` forward pen-up; `F` forward drawing trunk/branch; `f` forward drawing leaves (100%); `T` forward drawing trunk only; `R` forward placing fruit; `A B C D` → rule sets; `a b c d` → rule sets with 90/80/70/60 % probability; `+ -` yaw right/left by `angle`; `& ^` pitch down/up; `/ *` roll right/left; `[` push state; `]` pop state.
- Apple-tree example: axiom `"FFFFFAFFBF"`, `angle=30`, `iterations=2`, `random_level=0`, `trunk_type="single"`, `thin_branches=true`, `fruit_chance=10`, rules_a beginning `"[&&&FFFFF&&FFFF]"`, rules_b beginning `"[&&&++FFFFF&&FFFF]"`.
- Decorations of `deco_type = "lsystem"` take the same `treedef` — [Luanti lua_api.md](https://raw.githubusercontent.com/luanti-org/luanti/master/doc/lua_api.md)

**Luanti schematics (template blitting with per-voxel probability)** — [Luanti schematics](https://api.luanti.org/schematics/):
- A schematic is a `.mts` file or a table with `size` (3D vector), `data` (flat `[z [y [x]]]` list of `{name, prob, param2, force_place}`), and `yslice_prob` (`{ypos, prob}` per horizontal slice, default 255).
- `prob` (alias `param1`) in [0,255]: "A probability value of `0` or `1` means that node will never appear", "`254` or `255` means the node will always appear", else "(p / 256 * 100)" percent "when the schematic is placed on the map"; `force_place` (default false) overwrites existing nodes; flags `place_center_x/y/z`, `force_placement` ("Schematic nodes other than "ignore" will replace existing nodes").
- Decoration schematics support `rotation` `"0"|"90"|"180"|"270"|"random"` and `replacements` — [Luanti lua_api.md](https://raw.githubusercontent.com/luanti-org/luanti/master/doc/lua_api.md)

**Space colonisation (Runions, Lane, Prusinkiewicz 2007)** — [PDF](https://www.algorithmicbotany.org/papers/colonization.egwnp2007.pdf):
- Begins "with an initial configuration of N attraction points (usually hundreds or thousands) and one or several tree nodes"; an attraction point influences the closest tree node if within "radius of influence dᵢ"; a new node is placed "at a distance D from v, in the direction defined as the average of the normalized vectors toward all the sources s ∈ S(v)", i.e. v′ = v + D n̂; optional bias vector g for weight/tropism; an attraction point is removed when a node is closer than the "kill distance d_k".
- Terminates "when all attraction points have been removed, when no nodes are within the radius of influence of the remaining attraction points, or when" a step limit is reached.
- Parameter examples (Fig. 3–5): N = 12000, d_k = 2D; N = 1500, d_k = 2D; N = 12000, d_k = 20D; N = 375, d_k = 20D; dᵢ = ∞ vs dᵢ = 17D (shrubs) and dᵢ = 8D (trees); "Decreasing N and increasing d_k yields crowns that are increasingly sparse"; small dᵢ gives "a wiggly or gnarly appearance".
- Branch width by the pipe model: r^n = r₁^n + r₂^n computed basipetally from tip radius r₀ (exponent n stated as a parameter) — [PDF](https://www.algorithmicbotany.org/papers/colonization.egwnp2007.pdf)
- Proximity tests are done with a 3D Voronoi/Delaunay (CGAL) in the paper — [PDF](https://www.algorithmicbotany.org/papers/colonization.egwnp2007.pdf)

**L-systems (Prusinkiewicz & Lindenmayer)**
- Core idea from the 1986 paper: generate a string with an L-system and "interpret this string as a sequence of commands which control a 'turtle'" — [Graphical applications of L-systems](https://algorithmicbotany.org/papers/graphical.gi86.html)
- Bracketed branching example strings of the form `FF[+FF][-FF]FF`; Lowclouds' JS port of the book's models includes a 3D turtle — [Lowclouds/lsystem](https://github.com/Lowclouds/lsystem)
- Parametric L-systems (Hanan 1992 dissertation) attach numeric parameters to symbols — [Hanan 1992](https://www.algorithmicbotany.org/papers/hanan.dis1992.pdf)

### Inferences
- Locality: Minecraft trunk/foliage placers, Luanti schematics and Luanti L-systems are all **[LOCAL]** given a per-tree seed (Luanti's `treedef.seed`; Minecraft's feature RNG). Space colonisation is also deterministic given a seed but is iterative with N in the hundreds–thousands and nearest-node queries per step; its natural use in a chunked engine is offline: bake a library of N voxelised trees per species, then blit (Luanti-style schematics with `prob`/`yslice_prob` give cheap per-instance variation and rotation ×4).
- Cost ordering per tree instance (inferred from the mechanics): schematic blit (O(voxels in template∩chunk)) < parametric trunk+blob (O(trunk + radius³)) < L-system (string rewriting with branching factor^iterations, then turtle walk) ≪ space colonisation.
- Border budget: Minecraft's canopies of 5–7 blocks across imply R_max ≈ 3–4 horizontally for small trees, ~8+ for fancy oaks/branches; this bounds the padding of Q2.
- Luanti's probabilistic symbols (`a b c d` at 90/80/70/60 %) and `random_level` give stochastic variety from one grammar without authoring more templates; the engine's fallback seed generation is the thing to avoid in a pure function — always pass `seed`.

### Gaps
- Vanilla per-species Minecraft numbers (oak `straight 4/2/0` + `blob r=2, offset 0, height 3`, birch `5/2/0`) are commonly cited in modding circles but I could not confirm them from the wiki pages fetched; only the schema ranges and the Fabric tutorial's example (8/3/0, blob 5/0/3) are sourced.
- The `prob` value 1 in Luanti is documented as "never appear" though `(p/256)` would suggest ~0.4 %; this is as documented.
- I could not obtain the pipe-model exponent value Runions used (the formula r^n = r₁^n + r₂^n is quoted; n not confirmed in the extracted text).
- "Sapling"-style parametric trees (Weber & Penn 1995) were not researched due to call budget; the Blender Sapling add-on is the usual reference.

---

## Key question 4: Structures — Minecraft structure sets (spacing/separation/salt), structure starts by region, jigsaw; Dwarf Fortress sites; dungeon generation (BSP, rooms-and-corridors, random walk); template placement with rotation

### Takeaway
Minecraft decides structure *existence* per spacing×spacing chunk cell from (seed, cell, salt) — a [LOCAL] decision — then builds the structure's piece list once (jigsaw, up to 128-block radius) and lets every chunk within 8 chunks look it up ([NEIGHBOURS]/region cache). Dungeon-interior algorithms (BSP, random walk) are region-local and deterministic if seeded by the structure's start cell. Dwarf Fortress sites are a [GLOBAL] history simulation and not chunk-local.

### Cited Findings

**Minecraft structure sets** — [Minecraft Wiki: Structure set](https://minecraft.wiki/w/Structure_set):
- `random_spread`: "Starting from chunk 0 0" the game builds a grid at `spacing` chunks; "Generation is attempted once per spacing×spacing chunk region"; each position is offset separately on X and Z.
- `spacing` (0–4096) = average distance in chunks; `separation` (0–4096, strictly < spacing) = minimum distance; "The maximum distance between neighbors is 2×spacing − separation".
- `spread_type` `linear`: offset uniform in 0..(spacing − separation − 1); `triangular`: sum of two such values, halved, floored (offsets cluster toward the middle).
- `salt`: non-negative integer; `frequency` (default 1.0) = probability of attempting; `exclusion_zone` {`chunk_count` 1–16, `other_set`}; `locate_offset`.
- Default values (separation, spacing, salt, spread): villages 8, 34, 10387312, linear; end cities 11, 20, 10387313, triangular; ocean monuments 5, 32, 10387313, triangular; woodland mansions 20, 80, 10387319, triangular; pillager outposts 8, 32, 165745296, linear, frequency 20 %, exclusion zone 10 chunks from villages; buried treasure 0, 1, 0, linear, frequency 1 %, locate_offset (9,0,9).
- `concentric_rings` (strongholds): `distance` in units of 6 chunks, `count`, `spread`; Nth ring gets `spread × (N² + 3N + 2) / 6` attempts; strongholds: distance 32, count 128, spread 3; `preferred_biomes` searched in a 112-block-radius square in 4×4 steps.

**Minecraft jigsaw structures** — [Minecraft Wiki: Jigsaw structure](https://minecraft.wiki/w/Jigsaw_structure):
- Start piece from `start_pool`, rotation and template "randomly, based on the chunk coordinates and the world seed"; start position is the "0, 0 chunk coordinate of the starting chunk"; `project_start_to_heightmap` makes `start_height` an offset from the heightmap.
- Expansion: queue processed in order; each piece's jigsaw blocks handled by selection priority (random among equal); target pool tried in weighted random order, then the template pool's fallback; pool 1 skipped once `size` (0–20) depth is reached; candidate must have a jigsaw block facing outward with matching target name and compatible orientation; terrain-matching pieces moved vertically to the terrain; piece must stay within the outer bounding box and not intersect other pieces.
- Bounding box: side `2a + 1` with `a = max_distance_from_center.horizontal` (1–128, or 1–116 when `terrain_adaptation` ≠ none); vertical up to 4064; the 20w28a data-pack note "They have a maximum radius of 80 blocks"; ancient city radius 128 ([verify] on the wiki).
- `terrain_adaptation`: `beard_thin` (pillager outposts, villages), `beard_box` (ancient cities), `bury` (strongholds), `encapsulate` (trial chambers), default `none`.
- Pool aliases `direct`, `random`, `random_group` resolved before generation.
- Village streets are "street" pieces spanning the village with "terminator" pieces ending the network — [Village house (Fandom wiki)](https://minecraft.fandom.com/wiki/Village_house); known jigsaw failures when a `terrain_matching` street changes elevation — [MC-303542](https://mojira.dev/MC-303542)
- Bedrock describes `terrain_adaptation` options `beard_thin`/`beard_box` as placing "a terrain beard under the structure" — [Lithostitched custom structures](https://www.mintlify.com/Apollounknowndev/lithostitched/guides/custom-structures)

**Structure pipeline vs chunks**
- `structure_starts` runs on the chunk alone; `structure_references` reads 17×17; `features` places pieces whose boxes intersect the chunk — [neoforged #347](https://github.com/neoforged/Documentation/issues/347); 1.12-era: "Structures are generated once, stored in memory, and built gradually as chunks that intersect them are populated"; villages and strongholds "are spaced far apart to avoid overlap" — [delvr gist](https://gist.github.com/delvr/bfac43cd48675ec74b905ffe640a6a11)
- Chunk NBT: `structures.starts` holds unfinished structures, "Completely generated structures are *removed* by setting their *id* to `INVALID`"; `References` stores "Coordinates of chunks that contain *Starts*" — [Minecraft Wiki: Chunk format](https://minecraft.wiki/w/Chunk_format)

**Dwarf Fortress sites**
- World-gen stages listed on the wiki include "placing civilizations, making cave civilizations, ... placing megabeasts, placing good/evil, placing caves, prehistory generation" followed by history simulation; the max number of new sites civilisations can expand to is a parameter and "High site numbers increase the duration of history generation by a huge amount of time"; seeds are recorded in gamelog.txt and worlds are reproducible from seed + end year — [DF wiki v0.31 Advanced world generation](https://www.dwarffortresswiki.org/index.php/v0.31:Advanced_world_generation); [40d Advanced World Generation](https://dwarffortresswiki.org/index.php/40d:Advanced_World_Generation)

**Dungeon interiors**
- BSP: "predictable control over room counts and sizes because each split deterministically bounds future space"; one developer found BSP gave "lots more rooms and less corridor space" than brute-force room placement — [pulsegeek](https://pulsegeek.com/articles/dungeon-generation-algorithms-patterns-and-tradeoffs/); [blenderartists](https://blenderartists.org/t/bsp-dungeon-generation/614970)
- Drunkard's walk: carve until a step budget runs out; "organic" cave-like output, "extremely fast to implement", weak structural guarantees ("too random" per one GameDev.net poster) — [dinogame](https://dinogame.gg/blog/how-procedural-dungeons-work/); [gamedev.net](https://gamedev.net/forums/topic/637643-dungeon-generation-method/)
- Common hybrid: "BSP for rooms + drunkard's walk for corridors", often followed by cellular-automata smoothing; the PCG Book chapter 3 covers these classic constructive methods — [pulsegeek](https://pulsegeek.com/articles/dungeon-generation-algorithms-patterns-and-tradeoffs/); [PCG Book ch. 3](https://www.pcgbook.com/chapter03.pdf)

### Inferences
- Minecraft's structure-set decision is exactly the hashed-grid of Q1 at a coarser scale: cell = spacing×spacing chunks; existence = `frequency` test on hash(seed, cell, salt); position = cell origin + offset(s) in [0, spacing − separation). This is **[LOCAL]** per cell and therefore per chunk (a chunk can compute which cell it is in and whether that cell's structure overlaps it). With villages at spacing 34 / separation 8, any two villages are ≥ 8 and ≤ 60 chunks apart.
- Building the piece list (jigsaw expansion) is a per-*structure* computation that must be deterministic from (seed, start cell): Minecraft caches it in proto-chunks; a pure engine can instead recompute it in every chunk that intersects the structure's max bounding box (radius up to 80–128 blocks = up to 3–4 chunks of 32 each side), memoising by start cell. The cost is bounded by `size` ≤ 20 depth and pool sizes; the heightmap-projected start and `terrain_matching` pieces require a pure column-height function.
- Terrain adaptation ("beard") is the one part that is **not** feature-pass-only: it modifies terrain *under* the structure during terrain generation, so a pure engine must evaluate "is there a structure here?" during the terrain stage too — feasible with the hashed-cell lookup above, which is why Minecraft runs `structure_starts` before `terrain`.
- Dungeon-interior algorithms (BSP, random walk) are [LOCAL] to the structure's region once seeded; BSP is preferable when the dungeon must fit a fixed box (e.g. 3×3 chunks) because the splits bound every room.
- Dwarf Fortress-style site placement is **[GLOBAL]** (history simulation with site-count caps and rejection parameters) and would have to be a world-creation pre-pass whose output (a site list) is then consulted by chunks.

### Gaps
- I did not find a primary description of Dwarf Fortress's per-site terrain-suitability or spacing algorithm; only parameter-level documentation.
- The Minecraft wiki jigsaw page does not state the village `size` (commonly cited as 6) nor the `structure_references` mechanics; `step` is marked "more information needed".
- Rotation handling for templates: Luanti documents 0/90/180/270/random for schematics (Q3); for Minecraft structure templates the per-piece rotation is stated only as chosen "randomly, based on the chunk coordinates and the world seed".

---

## Key question 5: Wave Function Collapse and Model Synthesis — models, voxel uses (Townscaper, Bad North, Caves of Qud), chunked/infinite WFC and contradictions, deterministic per-chunk WFC, cost, libraries

### Takeaway
WFC/model synthesis are constraint solvers that fail (contradict) by nature; the only published scheme that is both infinite and deterministic is Merrell's "modifying in blocks" lifted to a fixed layered dependency tree (Boris the Brave), costing ~4× the work of a single pass; naive lazy per-chunk WFC constrained against whatever neighbours exist is non-deterministic. Per-chunk WFC with free (unconstrained) borders is deterministic and [LOCAL] but visibly seams.

### Cited Findings

**Algorithm (Gumin)** — [mxgmn/WaveFunctionCollapse](https://github.com/mxgmn/WaveFunctionCollapse):
- Read NxN patterns from the input ("a typical value of N is 3"), optionally augmented with rotations/reflections; output cells start in superposition; observe "a wave element with the minimal nonzero entropy", collapse, propagate.
- Simple tiled model = the 1×2 special case using adjacency data; "We implement a tile symmetry system to reduce the sizes of inputs."
- Contradiction = "all the coefficients for a certain pixel become zero"; a contradictory run ends "without returning anything"; "The problem of determining whether a certain bitmap allows other nontrivial bitmaps satisfying condition (C1) is NP-hard, so it's impossible to create a fast solution that always finishes"; "In practice, however, the algorithm runs into contradictions surprisingly rarely."
- "WFC algorithm in higher dimensions works completely the same way as in dimension 2, though performance becomes an issue."
- Applications named: Bad North (Stålberg's heuristic "tries to select such tiles that the resulting observed zone is navigable at each step"), Townscaper (WFC + marching cubes on irregular grids), Caves of Qud (Brian Bucklew applies WFC "in multiple passes"), MarkovJunior (3D simple tiled model).
- Ports: fast-wfc (Fehr & Courant, C++) "significantly improved the running time of WFC, by an order of magnitude for the overlapping model"; Emil Ernerfeldt C++; Rust: Sean Leffler (overlapping), Stephen Sherratt (library), Elwann Guillemot (overlapping + GUI); C: Jakubowski et al. (tiled), Krystian Samp (single-file overlapping), Vladimir Pleskonjić (single-header).

**Model synthesis (Merrell)** — [paulmerrell.org/model-synthesis](https://paulmerrell.org/model-synthesis/):
- "inspired by texture synthesis"; papers: 2007 i3D "Example-Based Model Synthesis", 2008 TOG "Continuous Model Synthesis", 2009 PhD dissertation, 2010 TVCG "Model Synthesis: A General Procedural Modeling Algorithm"; "The two algorithms are nearly identical and produce very similar results" (WFC 2016 vs model synthesis 2007); model synthesis focused more on 3D.

**Constraint solving mechanics and remedies (Boris the Brave)**
- Contradiction: "If we ever get a variable with an empty domain, it means it is not possible to assign that variable"; backtracking restores state and removes the guessed value ("Every time you backtrack you reduce the domain of at least one variable"); with good tiles "it's rarely necessary to backtrack so we can skip implementing that"; propagation via a queue and per-cell/tile/side support counts ("If the count ever drops to zero, then we know the tile cannot be placed"); "Because WFC only constrains nearby tiles, it rarely generates large scale structures" — [WFC explained](https://www.boristhebrave.com/2020/04/13/wave-function-collapse-explained/)
- "It's a fact of any constraint based approach that you will face contradictions." Remedies: restart ("Hopefully they are rare enough you'll succeed in a few attempts"), backtracking ("Can still get stuck if the problem space is too large"), modifying in blocks ("generate the level one chunk at a time, and do restarts per chunk"), CDCL (professional solvers, usually too hard to implement); overlapping model "more costly to evaluate than adjacencies"; arc-consistency solvers "pretty quick" — [Constraint-Based Tile Generators](https://www.boristhebrave.com/2021/10/31/constraint-based-tile-generators/)

**Infinite / chunked WFC** — [Infinite Modifying in Blocks](https://boristhebrave.com/2021/11/08/infinite-modifying-in-blocks):
- Modifying in blocks: "subdividing the output into small blocks", generating one at a time with each block constrained to join the already-generated ones; "the blocks actually overlap each other, so that each block replaces some of the tiles previously generated".
- Infinite deterministic scheme: 4 layers on a grid; layer 1 blocks are separated by a gap and independent; layer 2 is the same grid offset 50 % in x (each block overlaps exactly two layer-1 blocks); layer 3 offset 50 % in y; layer 4 offset 50 % in x again; "Each layer 4 block needs a total of 12 blocks from earlier layers evaluated before it can be computed"; by layer 4 "every tile has been replaced at least once"; the approach is "essentially doing 4x as much work as a simpler approach".
- Determinism: results depend only on each block's fixed dependency tree, so evaluation order doesn't matter; contrast: lazy chunk generators are non-deterministic ("even using the same random seed, you'll see different results").
- Failure handling: if a block fails "you can carry forward tiles from the earlier layers"; optional initial fill with a known repeating pattern; without layer-1 constraints "you lose some of the robustness guarantees"; "Any patterns or special constraints that are naturally larger than the size of a block are simply impossible"; increasing block size "is usually sufficient to hide most block artifacts in practice" — [Infinite Modifying in Blocks](https://boristhebrave.com/2021/11/08/infinite-modifying-in-blocks)
- Forum opinions (not established results): make chunk-edge tiles "accept anything so each chunk can be computed independently"; or re-run WFC when a neighbour loads, "which can make edges visibly change"; one commenter doubts canonical deterministic infinite tiles are possible — [r/proceduralgeneration](https://redlib.hbubli.cc/r/proceduralgeneration/comments/11wltau/infinite_wave_function_collapse_level_generation)
- A "Nested WFC" paper claims that "complete and sub-complete tileset preparation" avoids conflicts and backtracking for aperiodic deterministic infinite content (not verified) — [core.ac.uk](https://core.ac.uk/works/145310998)

**Shipped uses** — [Game Developer: How Townscaper works](https://www.gamedeveloper.com/game-platforms/how-townscaper-works-a-story-four-games-in-the-making):
- Townscaper grid: "taking a fixed hexagonal grid, breaking it up into quads, then breaking it up into quads again", then relaxing points; tiles are "corner segments" placed marching-cubes style; WFC "optimised for working in real-time"; "the Wave Function Collapse identifies the possible modules for that cell, and then afterwards it will decorate it"; "it is actually allowed to fail silently".
- Bad North: islands generated before play; navigability heuristic + validation, "the generator will restart" on failure; "over 300 tiles"; "the larger the map, the longer this all takes to execute"; "occassionally you'll get a level that seems broken"; a reskin of Townscaper would need "somewhere in the region of 500 tiles".
- Stålberg talks: "Beyond Townscaper" (Sweden Game Conference 2021, https://youtu.be/Uxeo9c-PX-w) and Everything Procedural 2018 (Bad North) — [HSWFC slides](https://graphics.tudelft.nl/Publications-new/2024/AB24/HSWFC.CL.slides.pdf); [records.sigmm.org](https://records.sigmm.org/?p=12589)

### Inferences
- Locality classification: single-region WFC = **[GLOBAL]** within the region; per-chunk WFC with free borders = **[LOCAL]** but seamed; lazy per-chunk WFC constrained by existing neighbours = **[NEIGHBOURS]** and non-deterministic; Boris's layered modifying-in-blocks = **[LOCAL]** in the pure-function sense (each output block has a fixed, finite dependency tree computable from coordinates), at ~4× cost plus up to 12 dependency blocks per output block, with restart-per-block for contradictions.
- For a 32³-chunk engine, the practical fit is WFC at the *structure* level (one building/ruin solved inside a bounded box seeded from its structure cell, as in Q4) rather than WFC over the open world; that keeps contradiction handling local (restart with the next sub-seed, fall back to a template on repeated failure, as Townscaper "fails silently").
- 3D tiled WFC cost scales with cells × tiles × propagation; Gumin flags higher-dimensional performance as "an issue", and fast-wfc's order-of-magnitude gain applies to the overlapping model; the simple tiled model with symmetry reduction is the realistic voxel choice.

### Gaps
- No source gave wall-clock numbers for 3D WFC on voxel-sized problems.
- I did not retrieve Merrell's 2021 WFC-vs-model-synthesis comparison PDF or the dissertation's block-size recommendations.
- The Reddit thread content was summarised from search snippets; direct fetch was blocked (HTTP 403).
- Caves of Qud's multi-pass WFC details beyond Gumin's one-line description were not researched.

---

## Key question 6: Grammars and shape grammars for buildings (CGA shape, Müller et al. 2006) and voxel applications

### Takeaway
CGA shape (Müller et al. 2006) models buildings as recursive split/repeat/component rules on scoped boxes, which maps naturally onto voxel AABBs; it is deterministic given a seed and [LOCAL] to a structure footprint. I could not retrieve the paper's full text, so rule examples are from secondary sources.

### Cited Findings
- The paper "Procedural Modeling of Buildings" (Müller, Wonka, Haegler, Ulmer, Van Gool, SIGGRAPH 2006) presents "CGA shape, a novel shape grammar for the procedural modeling of computer graphics architecture"; its stated contributions: "the actual definition of the split including the repeat split and the scaling of rules", plus a component split for 1/2/3-D shapes — [muller_buildings.pdf (course copy, abstract)](https://course.ccs.neu.edu/cs5150f13/readings/muller_buildings.pdf)
- Repeat rules are "an extension of split rules, which allow a set of products to be repeated as many times as fits in the context of the rule during evaluation" — [Zweig 2013 Brown thesis](https://cs.brown.edu/research/pubs/theses/ugrad/2013/zweig.pdf)
- Earlier split grammars (Wonka et al.) derive a facade "using a sequence of split and repeat commands to subdivide a planar shape"; CGA added environmental queries such as occlusion for window/door placement — [LIPP 2008](https://www.cg.tuwien.ac.at/research/publications/2008/LIPP-2008-IEV/LIPP-2008-IEV-paper.pdf)
- Practical tutorial: facade split vertically, repeat split for floors and windows, "useful to scale the split operation to shapes of unknown size (e.g., floors in a building)" — [GameDev.net: Procedural Modeling of Buildings with Shape Grammars](https://gamedev.net/tutorials/programming/engines-and-middleware/procedural-modeling-of-buildings-with-shape-grammars-r4596)

### Inferences
- In a voxel setting the CGA "scope" (position + 3 axes + size) is literally an integer AABB; splits become integer partitions with remainder handling, and repeats become `floor(size / unit)` copies. The "insert" operation maps to schematic blitting (Q3). Because derivation is a tree seeded from the structure's start cell, it is deterministic and [LOCAL] to the structure box; the only cross-chunk concern is the same as any structure (Q4): every chunk intersecting the box must re-derive (or memoise) the same tree.
- Grammar derivations are cheap relative to WFC (no search, no contradictions), which makes them a safer default for buildings than WFC in a pure-function engine; WFC adds value when adjacency variety (irregular ruins, dungeons) matters more than architectural regularity.

### Gaps
- Direct fetch of the 2006 PDF failed twice (404 on the NEU copy, size limit on the Wonka copy); exact rule syntax (`Subdiv`, `Repeat`, `Comp`, relative sizes `r`, snap lines, `occ()`), rule counts and timings are not quoted here.
- I found no documented shipped *voxel* game using CGA-style grammars; voxel applications are inferred.

---

## Key question 7: Roads and path networks between structures; villages following terrain

### Takeaway
Minecraft does not build inter-village roads; its streets are jigsaw "street" pieces inside one structure, with terrain adaptation handled by heightmap projection, `terrain_matching` pieces and a "beard" that reshapes terrain under the structure. Inter-structure roads exist only as mods or research and are [GLOBAL]/[NEIGHBOURS] by nature.

### Cited Findings
- Village streets are jigsaw pieces spanning the village, ended by "terminator" pieces — [Village house (Fandom)](https://minecraft.fandom.com/wiki/Village_house); example road-connector pools (plaza → road pieces via named connectors) — [Iris jigsaw docs](https://www.mintlify.com/VolmitSoftware/Iris/advanced/jigsaw-structures); [MCreator tutorial](https://mcreator.net/node/124767)
- Jigsaw terrain handling: `project_start_to_heightmap`, terrain-matching pieces "moved vertically to align with the terrain", `minecraft:gravity` processor for terrain-matching columns; `terrain_adaptation` `beard_thin` (villages/outposts), `beard_box`, `bury`, `encapsulate` — [Minecraft Wiki: Jigsaw structure](https://minecraft.wiki/w/Jigsaw_structure)
- Known failure modes: jigsaw generation failing when a `terrain_matching` street changes elevation — [MC-303542](https://mojira.dev/MC-303542); Bedrock village structures not adapting to terrain — [MCPE-145659](https://bugs-legacy.mojang.com/browse/MCPE-145659); `terrain_matching` structures overlapping paths — [MCPE-187995](https://bugs-legacy.mojang.com/browse/MCPE-187995)
- Inter-village roads only via mods: RoadWeaver "generates roads between villages or custom structures" — [RoadWeaver](https://www.curseforge.com/minecraft/mc-mods/roadweaver-tec3); a research paper notes earlier road generators "didn't adapt roads to terrain" — [CWI paper](https://ir.cwi.nl/pub/35899/35899.pdf)
- The 1.12-era note on why structures ignore terrain (checking terrain would recurse through 8-chunk radii) is the root reason villages don't follow ravines — [delvr gist](https://gist.github.com/delvr/bfac43cd48675ec74b905ffe640a6a11)

### Inferences
- A village "following terrain" is achievable chunk-locally if (a) the structure's piece positions are decided from a pure column-height function at jigsaw time (Minecraft's heightmap projection), and (b) the terrain stage can query "which structure cell covers me" to apply a beard/flatten before features — i.e. structure existence must be decided *before* terrain, exactly Minecraft's ordering.
- Roads between structures require knowing both endpoints; with the hashed structure-cell scheme, a chunk can find the nearest structure cells deterministically (bounded search over neighbouring cells) and compute a path with a pure cost function over the column-height function. That is [LOCAL] in principle but the path computation (e.g. A* over heightmap) is a region-sized cost repeated per chunk unless memoised per (cellA, cellB) pair. No source documented this in a shipped voxel game.

### Gaps
- I found no primary description of how RoadWeaver or any shipped voxel game routes roads over terrain deterministically.
- No source gave Minecraft's exact beard falloff numbers.

---

## Key question 8: Decoration/surface rules — surface block by biome, slope, altitude, noise; Minecraft 1.18 surface rules

### Takeaway
Minecraft 1.18+ surface rules are a per-column decision tree (`sequence`/`condition`/`block`) over conditions that are all functions of the column, biome, noise and depth counters — fully [LOCAL] per column and a direct template for Strata; gravel/sand patches are noise-threshold bands on a `surface` noise.

### Cited Findings — [Minecraft Wiki: Surface rule](https://minecraft.wiki/w/Surface_rule)
- Rule types: `bandlands` (badlands terracotta bands), `block` (`result_state`), `condition` (`if_true`, `then_run`), `sequence` ("Tries surface rules in order, only the first that matches is applied").
- Conditions: `above_preliminary_surface` (keeps grass out of noise caves), `biome` (`biome_is`), `hole` (surface depth 0), `noise_threshold` (`noise`, `min_threshold`, `max_threshold` — "Computes the noise value of the current column using a specified noise and checks if it is between the min and max threshold"), `not`, `steep` ("steep north or east faces of mountains"), `stone_depth` (`surface_type` floor/ceiling, `offset`, `add_surface_depth`, `secondary_depth_range` adding `map(surface_secondary(X,0,Z), -1, 1, 0, secondary_depth_range)`), `temperature` (cold enough for snow), `vertical_gradient` (`random_name`, `true_at_and_below`, `false_at_and_above`; between anchors success probability `(false_at_and_above - Y) / (false_at_and_above - true_at_and_below)`), `water` (`offset`, `surface_depth_multiplier` −20..20, `add_stone_depth`; passes if no water above), `y_above` (`anchor`, `surface_depth_multiplier`, `add_stone_depth`).
- Surface depth = `floor(surface(X,0,Z) × 2.75 + 3.0 + positional_noise(X,0,Z) × 0.25)` using the `minecraft:surface` noise; secondary depth in −1..1 from `minecraft:surface_secondary`; generator tracks `stoneDepthAbove`, `stoneDepthBelow`, `waterHeight`.
- Known artefact: surface rules and multi-noise biomes sample noise at different scales, so patterns can look offset — [MC-239790](https://bugs.mojang.com/browse/MC-239790)
- Patchy ground-cover pattern (community docs): a `noise_threshold` on `minecraft:surface` placing gravel when the noise is between −1.0 and −0.5, nested under a `stone_depth`/biome condition, injected above the fallback sequence — [Lithostitched common patterns](https://www.mintlify.com/Apollounknowndev/lithostitched/guides/common-patterns)
- Minecraft `NOISE`/`SURFACE` stages place only within their own chunk and see a 3×3 biome window (for biome blending at edges) — [neoforged #347](https://github.com/neoforged/Documentation/issues/347)
- Luanti's equivalent for surface scatter is the decoration system (`fill_ratio`, `noise_params`, `place_on`, `spawn_by`, `y_min`/`y_max`) and for sub-surface the ore types `scatter`, `sheet`, `puff`, `blob`, `vein`, `stratum` — [Luanti lua_api.md](https://raw.githubusercontent.com/luanti-org/luanti/master/doc/lua_api.md)

### Inferences
- Every surface-rule condition is a pure function of (x, z, y, biome(x,z), noise(x,z), depth-from-surface counters computed while scanning the column top-down), so the entire system is **[LOCAL]** per column; the only neighbour-dependent input is `steep` (needs adjacent column heights), which is still [LOCAL] if the column-height function is pure. Biome blending at chunk edges is why Minecraft gives these stages a 3×3 biome window; a pure biome function removes that need.
- "Slope" rules (Minecraft's `steep`) and altitude rules (`y_above`) plus a per-biome noise-threshold band for patches cover the requested gravel/flower-patch behaviour; the surface-depth formula (≈3–6 blocks, jittered by noise) is the standard "dirt thickness" recipe.

### Gaps
- The exact vanilla threshold values for gravel/sand patches, stone shore, and the bedrock/deepslate `vertical_gradient` anchors are not on the wiki page fetched; the −1.0..−0.5 gravel band is from a third-party guide, not vanilla data. Vanilla `overworld.json` would need to be read directly.

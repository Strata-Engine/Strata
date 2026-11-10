# Strata Voxel Core Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build project 1 of Strata: a standalone, headless March voxel core in `~/code/strata` with a block registry, 32-cubed palette chunks, streaming, edits, a greedy mesher emitting 8-byte quads, occupancy masks, raycast and collision, and a demo that measures it.

**Architecture:** A forge library package `strata` (modules `Strata.*`) plus a small C memory helper, and a separate forge app `demo/` that depends on it by path. Games supply a block registry and a pure terrain function; the core never names a game concept. Generation and meshing run on worker actors; edits return change records instead of calling back.

**Tech Stack:** March `origin/main` (built from `~/code/march` in a separate worktree), forge, clang for one C file. Load the `march-lang`, `march-test`, `build-test` and `forge-search` skills before writing March.

**Spec:** `docs/superpowers/specs/2026-11-10-strata-design.md` in the cube_forge worktree `/Users/80197052/code/cube_forge/.claude/worktrees/fps-drops-block-pop-in-3a7c2f`. Task 1 copies it and this plan into `~/code/strata/docs/`. Read both.

**Reference code (read-only, copy and adapt, never move or edit):** `lib/cube_forge/` in that worktree. Its `GAPS.md` lists March pitfalls already hit; read the headings before starting.

## Global Constraints

- Everything is created under `~/code/strata`. cube_forge and `~/code/march`'s current checkout are not modified.
- Compile against March `origin/main`. Do not install over the existing `~/.opam/march` toolchain; use binaries from the separate worktree through `scripts/env.sh`.
- Do not change March or forge. Work around gaps inside Strata and record each in `~/code/strata/GAPS.md` (number, symptom, workaround, minimal repro).
- The gate, run as `scripts/verify.sh`, is `forge check`, `forge build --release`, `forge lint --strict`, `forge test`, in the library and in `demo/`. Every task ends with the gate green.
- No panic reachable from library code. Every lookup is total with a documented default.
- No `Strata.*` module names a game concept, and none needs a window or GPU capability.
- Chunk side is the compile-time constant 32. Voxel scale (metres per voxel) is a runtime world value.
- Block ids are 0..65535. Id 0 is air. Any id that is unregistered or out of range behaves as air everywhere.
- Face numbering everywhere: 0 +y, 1 -y, 2 +x, 3 -x, 4 +z, 5 -z.
- Test style: `mod Strata.Test.X do ... import Test ... describe/test ... Test.assert_eq_int(actual, expected, "msg")`, as in cube_forge's `test/`. Use `Check.all` property tests where a task says "property".
- Commit after every task, in the `~/code/strata` repository, on a branch named `voxel-core`.

## Review Focus

Inputs the spec implies but does not spell out. Each has a test in the task named.

1. **Negative world coordinates.** Chunk and local coordinates must use floor division, so voxel -1 is local 31 of chunk -1. Tests in Tasks 2, 8, 10.
2. **Edits on a chunk border or corner.** The neighbouring chunks' meshes change too (faces and corner shading); a stale seam is the visible bug. Test in Task 8.
3. **Palette growth and shrink.** The 257th distinct id must promote a chunk without losing data; setting a block to the id it already has must not change the chunk's form or hash. Test in Task 4.
4. **Garbage ids from a game's terrain function.** Ids above the registered count, negative, or above 65535 must mesh, collide and raycast as air without a panic. Tests in Tasks 3, 4, 7.
5. **Focus teleport while workers are busy.** Results for chunks now outside the box are discarded; queued edits for them survive; the world hash still matches a run that went straight there. Test in Task 9.

---

## File Structure

```
~/code/strata/
  forge.toml                 lib package "strata"
  README.md  GAPS.md
  scripts/env.sh             puts March-main march/forge on PATH
  scripts/verify.sh          the gate
  docs/                      copies of the spec and this plan
  native/strata_mem.c        memory helper, no graphics include
  lib/strata/coord.march     Strata.Coord
  lib/strata/tree.march      Strata.Tree      (copied)
  lib/strata/math/*.march    Strata.Math.*    (copied)
  lib/strata/noise.march     Strata.Noise     (copied)
  lib/strata/blocks.march    Strata.Blocks
  lib/strata/chunk.march     Strata.Chunk
  lib/strata/ffi/mem.march   Strata.Ffi.Mem
  lib/strata/store.march     Strata.Store
  lib/strata/occupancy.march Strata.Occupancy
  lib/strata/quad.march      Strata.Quad
  lib/strata/mesh.march      Strata.Mesh
  lib/strata/terrain.march   Strata.Terrain
  lib/strata/world.march     Strata.World
  lib/strata/edit.march      Strata.Edit
  lib/strata/stream.march    Strata.Stream
  lib/strata/query.march     Strata.Query
  test/*_test.march          one per module
  test/support/naive_mesh.march   per-face oracle for the mesher
  demo/forge.toml            app "strata_demo", strata = { path = ".." }
  demo/lib/strata_demo.march
```

---

### Task 1: Toolchain, scaffold and a compatibility smoke test

**Files:**
- Create: `forge.toml`, `README.md`, `GAPS.md`, `scripts/env.sh`, `scripts/verify.sh`, `docs/` copies
- Create (copied from cube_forge, renamed `CubeForge.` to `Strata.`): `lib/strata/tree.march`, `lib/strata/math/{vec3,mat4,quat}.march`, `lib/strata/noise.march` and their tests (`tree`, `math`, noise coverage from `terrain_test` if separable)

**Interfaces:**
- Produces: `Strata.Tree`, `Strata.Math.Vec3`, `Strata.Math.Mat4`, `Strata.Math.Quat`, `Strata.Noise` with cube_forge's signatures unchanged; `scripts/verify.sh` exiting 0 on a clean gate.

- [ ] **Step 1:** `git -C ~/code/march fetch origin main`, then `git -C ~/code/march worktree add --detach ~/code/march-main origin/main`. Build `march` and `forge` there following that repository's own build instructions. Do not run any install step.
- [ ] **Step 2:** Write `scripts/env.sh` exporting `PATH` (and `MARCH_LIB_PATH`/stdlib variables if the build needs them) so `march --version` and `forge --version` resolve to `~/code/march-main`. Verify: `source scripts/env.sh && which march` prints a path under `~/code/march-main`.
- [ ] **Step 3:** `mkdir ~/code/strata && git init`, branch `voxel-core`. Create the lib package with `forge new --lib` (or by hand if the generator's layout differs from the tree above). Copy the spec and this plan into `docs/`.
- [ ] **Step 4:** Copy the five cube_forge modules and their tests, renaming the namespace. Strip any `needs Window` and any alias of `CubeForge.Ffi.Window`; if a copied module needs a C blit, replace it with a plain March loop for now.
- [ ] **Step 5:** Write `scripts/verify.sh`: sources `env.sh`, runs the four gate commands in the repository root, then in `demo/` when that directory exists; prints PASS or the first failing command and exits non-zero on failure.
- [ ] **Step 6:** Run `scripts/verify.sh`. Expected: PASS with the copied tests green. Any failure caused by March main differing from 0.5.0 is fixed here and recorded in `GAPS.md`.
- [ ] **Step 7:** README: one paragraph on what Strata is, the March commit hash verified against, how to run the gate. Commit.

### Task 2: Coordinates

**Files:** Create `lib/strata/coord.march`, `test/coord_test.march`

**Interfaces — Produces:**
```
fn side() : Int                      -- 32
fn volume() : Int                    -- 32768
fn chunk_of(v : Int) : Int           -- floor(v / 32)
fn local_of(v : Int) : {Int | 0 <= _ && _ < 32}
fn index(x : {Int | 0 <= _ && _ < 32}, y : {Int | 0 <= _ && _ < 32}, z : {Int | 0 <= _ && _ < 32})
    : {Int | 0 <= _ && _ < 32768}    -- x + 32 * (z + 32 * y)
type Key = Key(Int, Int, Int, Int)   -- cx, cy, cz, level (always 0 in project 1)
fn key(cx : Int, cy : Int, cz : Int) : Key
fn key_of_voxel(x : Int, y : Int, z : Int) : Key
fn kx(k : Key) : Int   fn ky   fn kz   fn level
fn key_eq(a : Key, b : Key) : Bool
fn key_hash(k : Key) : Int
fn to_voxel(scale : Float, metres : Float) : Int    -- floor(metres / scale)
fn to_metres(scale : Float, v : Int) : Float        -- v * scale, the voxel's low corner
```

- [ ] **Step 1: failing tests.** `chunk_of(-1) == -1`, `local_of(-1) == 31`, `chunk_of(31) == 0`, `chunk_of(32) == 1`, `chunk_of(-32) == -1`, `chunk_of(-33) == -2`; `index(31,31,31) == 32767`; `to_voxel(0.1, -0.05) == -1`; `to_voxel(1.0, 2.99) == 2`. Property: for any `v` in -100000..100000, `chunk_of(v) * 32 + local_of(v) == v`.
- [ ] **Step 2:** Run `forge test`; expect failures for the missing module.
- [ ] **Step 3:** Implement. The release build must discharge the `index` refinement; if `local_of`'s refinement cannot be proved from `%`, clamp explicitly and note it in `GAPS.md`.
- [ ] **Step 4:** Gate green. Commit.

### Task 3: Block registry

**Files:** Create `lib/strata/blocks.march`, `test/blocks_test.march`

**Interfaces — Produces:**
```
type Opacity = Empty | Solid | SeeThrough
type Layer   = Opaque | Cutout | Translucent
type Style   = Cube | Cross | Model(Int) | NoMesh
type Box     = Box(Float, Float, Float, Float, Float, Float)   -- min xyz, max xyz in cell units 0..1
type Def                                                       -- opaque
fn def() : Def                       -- collidable, Solid, Opaque, Cube, texture 0 on all faces, no light
fn with_collide(d : Def, on : Bool) : Def
fn with_opacity(d : Def, o : Opacity) : Def
fn with_layer(d : Def, l : Layer) : Def
fn with_style(d : Def, s : Style) : Def
fn with_faces(d : Def, top : Int, bottom : Int, side : Int) : Def
fn with_face(d : Def, face : Int, tex : Int) : Def
fn with_emission(d : Def, r : Int, g : Int, b : Int, strength : Int) : Def   -- each 0..255
fn with_material(d : Def, roughness : Int, emissive : Int) : Def             -- each 0..255

type Registry                                                  -- opaque, flat native arrays inside
fn empty() : Registry                -- id 0 = air already registered
fn register(r : Registry, d : Def) : Result((Registry, Int), String)
fn register_family(r : Registry, d : Def, states : Int) : Result((Registry, Int), String)  -- contiguous ids, returns base
fn count(r : Registry) : Int
fn base_of(r : Registry, id : Int) : Int
fn state_of(r : Registry, id : Int) : Int
fn with_state(r : Registry, id : Int, s : Int) : Int           -- out-of-range s returns id unchanged
fn add_model(r : Registry, quads : NativeF32Arr, boxes : List(Box)) : (Registry, Int)
fn model_quads(r : Registry, m : Int) : NativeF32Arr            -- empty array for an unknown model
fn model_boxes(r : Registry, m : Int) : List(Box)               -- Nil for an unknown model

fn collides(r : Registry, id : Int) : Bool
fn opacity(r : Registry, id : Int) : Opacity
fn is_solid_opaque(r : Registry, id : Int) : Bool              -- opacity == Solid, the hot-path form
fn layer(r : Registry, id : Int) : Layer
fn style(r : Registry, id : Int) : Style
fn face_tex(r : Registry, id : Int, face : Int) : Int
fn emission(r : Registry, id : Int) : Int                      -- r | g<<8 | b<<16 | strength<<24
fn roughness(r : Registry, id : Int) : Int
fn emissive(r : Registry, id : Int) : Int
```
Air is: no collision, `Empty`, `NoMesh`, no light. `register` fails with `Err` once 65536 ids exist.

- [ ] **Step 1: failing tests.** Fresh registry: `count == 1`; every lookup for id 0 returns the air values. Register stone (`def()`): id 1, `collides`, `is_solid_opaque`, `style == Cube`. `with_faces(d, 3, 4, 5)`: `face_tex` returns 3 for face 0, 4 for face 1, 5 for faces 2..5; `face_tex(r, id, 9)` returns 0. `register_family(r, d, 8)` returns base `b`; `state_of(r, b + 5) == 5`; `base_of(r, b + 5) == b`; `with_state(r, b, 7) == b + 7`; `with_state(r, b, 8) == b`. Garbage ids -1, `count(r)`, 70000: every lookup returns the air values (Review Focus 4). `emission` packs as documented. Unknown model index returns empty quads and `Nil`.
- [ ] **Step 2:** Run; expect failure.
- [ ] **Step 3:** Implement with one native array per property, indexed by id, grown on register.
- [ ] **Step 4:** Gate green. Commit.

### Task 4: Chunk with three forms

**Files:** Create `lib/strata/chunk.march`, `test/chunk_test.march`

**Interfaces — Consumes:** `Strata.Coord.index`. **Produces:**
```
type Chunk                                       -- Uniform | Paletted | Direct, constructors private
fn uniform(id : Int) : Chunk
fn get(c : Chunk, x : {Int | 0 <= _ && _ < 32}, y : {..}, z : {..}) : Int
fn get_or(c : Chunk, x : Int, y : Int, z : Int, default : Int) : Int   -- default when outside 0..31
fn get_idx(c : Chunk, i : {Int | 0 <= _ && _ < 32768}) : Int
fn set(c : Chunk, x : {..}, y : {..}, z : {..}, id : Int) : Chunk
fn fill_box(c : Chunk, x0 : Int, y0 : Int, z0 : Int, x1 : Int, y1 : Int, z1 : Int, id : Int) : Chunk  -- inclusive, clipped to the chunk
fn form(c : Chunk) : Int                         -- 0 uniform, 1 paletted, 2 direct
fn uniform_id(c : Chunk) : Int                   -- the id, or -1 when not uniform
fn compact(c : Chunk) : Chunk                    -- smallest form that holds the content
fn hash(c : Chunk) : Int                         -- depends on content only, never on form or palette order
fn to_flat(c : Chunk) : NativeI32Arr             -- 32768 ids, for the mesher
fn bytes(c : Chunk) : Int                        -- storage size, for the demo report
```
Ids outside 0..65535 are stored as 0. `set` to the id already present returns a chunk with the same form and hash.

- [ ] **Step 1: failing tests.** `uniform(7)`: `get` anywhere is 7, `form == 0`, `bytes` under 64. One `set` to another id: `form == 1`, that voxel reads back, all others read 7. Set 257 distinct ids: `form == 2`, all 257 read back (Review Focus 3). Overwrite everything with one id then `compact`: `form == 0`. `set(c, x, y, z, get(c, x, y, z))` leaves `form` and `hash` unchanged. `hash(a) == hash(b)` when `a` and `b` hold equal content built in different orders and forms. `set(.., 70000)` and `set(.., -3)` read back 0. `fill_box` clipped: a box from -5 to 40 fills the whole chunk and it compacts to uniform. Property: a random sequence of sets applied to a chunk and to a plain reference `NativeI32Arr` agree on every `get_idx`.
- [ ] **Step 2:** Run; expect failure.
- [ ] **Step 3:** Implement. Paletted is a 32768-byte `NativeU8Arr` of palette indices plus a palette of up to 256 ids; Direct is a `NativeI32Arr`. Use plain March loops; Task 5 swaps in C copies where it pays.
- [ ] **Step 4:** Gate green. Commit.

### Task 5: C memory helper and the consumer FFI path

**Files:** Create `native/strata_mem.c`, `lib/strata/ffi/mem.march`, `test/mem_test.march`; modify `forge.toml` (`[ffi] sources = ["native/strata_mem.c"]`), `lib/strata/chunk.march`

**Interfaces — Produces:**
```
fn u8_blit(dst : NativeU8Arr, di : Int, src : NativeU8Arr, si : Int, n : Int) : NativeU8Arr
fn i32_blit(dst : NativeI32Arr, di : Int, src : NativeI32Arr, si : Int, n : Int) : NativeI32Arr
fn u8_fill(dst : NativeU8Arr, di : Int, n : Int, v : Int) : NativeU8Arr
```
Module needs `IO.Foreign` only. Each extern is wrapped in an ordinary `fn` (a qualified extern call fails at link; see cube_forge `GAPS.md`). Out-of-range arguments leave `dst` unchanged; the C side must not abort.

- [ ] **Step 1: failing tests.** Blit copies exactly `n` elements at the offsets; `n <= 0` and out-of-range offsets return `dst` unchanged; fill writes `v & 255`.
- [ ] **Step 2:** Run; expect failure.
- [ ] **Step 3:** Implement, adapting `cf_u8_blit` from cube_forge's `native/cf_shim.c` (keep its copy-on-write discipline: write in place only when the reference count is 1, otherwise copy first rather than abort).
- [ ] **Step 4:** Use the blits in `Chunk` where a whole array is copied or converted. Chunk tests still pass.
- [ ] **Step 5:** Confirm whether forge on main builds a path dependency's `[ffi]` sources: make a throwaway app that depends on strata by path and calls `u8_blit`. Record the result in `GAPS.md` and write the exact `[ffi]` block a consumer needs into `README.md`.
- [ ] **Step 6:** Gate green. Commit.

### Task 6: Chunk store

**Files:** Create `lib/strata/store.march`, `test/store_test.march`

**Interfaces — Consumes:** `Coord.Key`, `key_eq`, `key_hash`. **Produces:**
```
type Store(a)
fn empty(fill : a) : Store(a)
fn get(s : Store(a), k : Key) : a          -- fill when absent
fn has(s : Store(a), k : Key) : Bool
fn put(s : Store(a), k : Key, v : a) : Store(a)
fn remove(s : Store(a), k : Key) : Store(a)
fn size(s : Store(a)) : Int
fn keys(s : Store(a)) : List(Key)
fn fold(s : Store(a), acc : b, f : fn(b, Key, a) -> b) : b
```
Persistent. Structure is the builder's choice (a hash trie over `key_hash` is the expected shape; `Strata.Tree` is available).

- [ ] **Step 1: failing tests.** Absent key returns fill; put then get; put twice keeps `size` at 1; remove of an absent key is a no-op; keys differing only in sign or only in `level` are distinct; an older version is unchanged after a put on the newer one. Property: a random sequence of put and remove agrees with a reference association list on `get`, `has` and `size`.
- [ ] **Step 2:** Run; expect failure. **Step 3:** Implement. **Step 4:** Gate green. Commit.

### Task 7: Occupancy, quad packing and the mesher

**Files:** Create `lib/strata/occupancy.march`, `lib/strata/quad.march`, `lib/strata/mesh.march`, `test/support/naive_mesh.march`, `test/occupancy_test.march`, `test/quad_test.march`, `test/mesh_test.march`

**Interfaces — Consumes:** `Blocks` lookups, `Chunk.to_flat`, `Chunk.uniform_id`, `Store`. **Produces:**
```
-- Strata.Occupancy
fn of_chunk(r : Registry, c : Chunk) : NativeU8Arr     -- 4096 bytes; bit Coord.index(x,y,z) set when opacity is Solid
fn bit(o : NativeU8Arr, x : Int, y : Int, z : Int) : Bool   -- false outside 0..31
fn count(o : NativeU8Arr) : Int

-- Strata.Quad: one quad is two 32-bit words; the bit layout is private to this module
fn pack0(x : Int, y : Int, z : Int, w : Int, h : Int, face : Int) : Int   -- 28 bits used; w, h are 1..32, stored minus one
fn pack1(tex : Int, ao : Int, flags : Int) : Int       -- tex 16 bits; ao 8 bits, four 2-bit values, corner 0 lowest; flags 8 bits
fn x_of(w0 : Int) : Int   fn y_of   fn z_of   fn w_of   fn h_of   fn face_of
fn tex_of(w1 : Int) : Int   fn ao_of(w1 : Int) : Int   fn flags_of(w1 : Int) : Int
-- accessors mask their input: a word read back from a NativeI32Arr may be sign-extended
fn pack_instance0(x : Int, y : Int, z : Int, model : Int) : Int   -- model -1 for a cross
fn pack_instance1(id : Int) : Int

-- Strata.Mesh
type Around                                            -- the 27 chunks centred on the one being meshed
fn around_of(s : Store(Chunk), k : Key) : Around
fn has_all(s : Store(Chunk), k : Key) : Bool           -- all 27 present
type Meshed
fn build(r : Registry, a : Around) : Meshed
fn quads(m : Meshed, l : Layer) : NativeI32Arr         -- 2 words per quad
fn quad_count(m : Meshed, l : Layer) : Int
fn instances(m : Meshed) : NativeI32Arr                -- 2 words per cross or model block
fn instance_count(m : Meshed) : Int
fn occupancy(m : Meshed) : NativeU8Arr                 -- the centre chunk's mask
fn level(m : Meshed) : Int                             -- 0
fn bytes(m : Meshed) : Int
```

Mesher rules (the spec leaves these to the plan):
- A face of block A toward neighbour B is emitted when A's style is `Cube` and B is not solid-opaque, except when A and B are the same id (so a body of water has no inner faces). The face goes to A's layer.
- Corner shading per face corner uses cube_forge's `Mesher.corner_ao` (0..3) and counts only solid-opaque neighbours, including those in neighbouring chunks.
- Two faces merge only when they share block id, face direction and all four corner values. A quad's `w` runs along the face's first in-plane axis and `h` along the second; document the axis table at the top of `quad.march`.
- `Cross` and `Model` blocks emit one instance each and no quads. `NoMesh` and unregistered ids emit nothing.
- A centre chunk that is uniform and whose six face neighbours are uniform with the same solid-opaque id (or which is uniform air) returns an empty mesh without calling `to_flat`.

`test/support/naive_mesh.march` is the oracle: for each voxel and face it applies the first rule directly and returns, per layer and face direction, the count of unit faces, plus a raster (set of covered unit faces).

- [ ] **Step 1: failing tests, occupancy and quad.** Occupancy bit equals `is_solid_opaque(get)` for every voxel of a random chunk; a see-through block gives no bit; `count` of a uniform stone chunk is 32768. Quad fields round-trip at their extremes: `x,y,z` 0 and 31, `w,h` 1 and 32, `face` 0..5, each corner value 0..3, `tex` 0 and 65535.
- [ ] **Step 2: failing tests, mesher.** (a) One stone block alone in air: 6 quads, each 1 by 1, all corner values 3. (b) A full 32 by 32 stone floor one block thick in an otherwise air world: top and bottom are one 32 by 32 quad each. (c) Uniform stone surrounded by uniform stone: zero quads. (d) Uniform stone with air above: exactly one top quad of 32 by 32. (e) Two adjacent see-through blocks of the same id: no face between them; of different ids: a face on each. (f) A cross block: one instance, zero quads. (g) A block at local (0,0,0) with a stone block in the diagonal neighbour chunk: the shared corner's value drops below 3. (h) A chunk filled with id 60000, unregistered: zero quads (Review Focus 4). (i) Property over random chunks built from a small registry with opaque, cutout, translucent and cross blocks: for every layer and face direction the summed `w * h` of the greedy quads equals the oracle's unit-face count, and expanding the greedy quads back to unit faces gives exactly the oracle's raster with no face covered twice.
- [ ] **Step 3:** Run; expect failure.
- [ ] **Step 4:** Implement. Slice-by-slice greedy merging as in cube_forge's `Mesher`, or bitmask-based; builder's choice. Push quads into growable `NativeI32Arr` buffers.
- [ ] **Step 5:** Gate green. Commit.

### Task 8: Terrain contract, world and edits

**Files:** Create `lib/strata/terrain.march`, `lib/strata/world.march`, `lib/strata/edit.march`, `test/world_test.march`, `test/edit_test.march`

**Interfaces — Consumes:** `Registry`, `Chunk`, `Store`, `Coord`. **Produces:**
```
-- Strata.Terrain
type Gen                                               -- wraps a pure function
fn gen(f : fn(Key, Int) -> Chunk) : Gen                -- (chunk key, seed) -> chunk
fn run(g : Gen, k : Key, seed : Int) : Chunk
```
If March main cannot send a function value to an actor, switch `Gen` to an interface the game implements, keep `run`'s signature, and record the gap.
```
-- Strata.World
type World
fn new(r : Registry, g : Gen, seed : Int, scale : Float) : World
fn registry(w : World) : Registry   fn terrain(w : World) : Gen   fn seed(w : World) : Int   fn scale(w : World) : Float
fn chunks(w : World) : Store(Chunk)                    -- fill is Chunk.uniform(0)
fn is_loaded(w : World, k : Key) : Bool
fn get_block(w : World, x : Int, y : Int, z : Int) : Int      -- 0 when the chunk is not loaded
fn insert(w : World, k : Key, generated : Chunk) : World      -- records the generation hash, restores a cached edited chunk if one exists, then applies queued edits
fn unload(w : World, k : Key) : World                  -- an edited chunk moves to the cache; an unedited one is dropped
fn is_edited(w : World, k : Key) : Bool                -- current hash differs from the generation hash, or edits are queued
fn cache_size(w : World) : Int
fn pending_size(w : World) : Int                       -- chunks with queued edits

-- Strata.Edit
type Change = Cell(Int, Int, Int, Int, Int)            -- x, y, z, old id, new id
            | BoxFill(Int, Int, Int, Int, Int, Int, Int)     -- inclusive corners, new id
            | SphereFill(Int, Int, Int, Int, Int)            -- centre, radius in voxels, new id
type Outcome
fn world(o : Outcome) : World
fn dirty(o : Outcome) : List(Key)                      -- loaded chunks whose mesh must be rebuilt, no duplicates
fn changes(o : Outcome) : List(Change)
fn deferred(o : Outcome) : List(Key)                   -- chunks whose part of the edit was queued
fn set_block(w : World, x : Int, y : Int, z : Int, id : Int) : Outcome
fn fill_box(w : World, x0 : Int, y0 : Int, z0 : Int, x1 : Int, y1 : Int, z1 : Int, id : Int) : Outcome
fn fill_sphere(w : World, cx : Int, cy : Int, cz : Int, radius : Int, id : Int) : Outcome   -- voxels with squared distance <= radius squared
fn set_many(w : World, cells : List((Int, Int, Int, Int))) : Outcome                        -- x, y, z, id; one Cell change each
```
Rules: a `set_block` that does not change the id returns no dirty chunks and no changes. `dirty` holds every loaded chunk containing a changed voxel, plus each loaded neighbour (face, edge or corner) that has a changed voxel within one voxel of the shared boundary. A bulk edit yields a single `BoxFill` or `SphereFill` change. The part of any edit that falls in an unloaded chunk is queued on that chunk's key in order and reported in `deferred`. A chunk whose content returns to its generation hash is no longer edited.

- [ ] **Step 1: failing tests, world.** `get_block` is 0 before `insert` and the generated value after. `unload` of an untouched chunk leaves `cache_size` 0; of an edited chunk, 1; a later `insert` of the same key restores the edited content, not the freshly generated one.
- [ ] **Step 2: failing tests, edit.** Interior edit: `dirty` is exactly the one chunk; one `Cell` with the right old and new ids. No-op edit: empty `dirty` and `changes`. Edit at local (0, 5, 5): `dirty` has the chunk and its -x neighbour. Edit at local (0, 0, 0): `dirty` has 8 chunks (Review Focus 2). Edit at world (-1, -1, -1) lands in chunk (-1, -1, -1) local (31, 31, 31) (Review Focus 1). Sphere of radius 12 centred on a chunk corner: one `SphereFill`, 8 dirty chunks, `get_block` at the centre and at distance 12 on an axis is the new id, at distance 13 is not. `fill_sphere` with radius 0 changes exactly one voxel; negative radius changes nothing. Box spanning a loaded and an unloaded chunk: the unloaded key is in `deferred`, `pending_size == 1`, and after `insert` of that chunk its voxels inside the box hold the new id and `pending_size == 0`. Edit then revert: `is_edited` false, and `unload` drops it. Property: random `set_many` agrees with applying each cell through `set_block`.
- [ ] **Step 3:** Run; expect failure. **Step 4:** Implement. **Step 5:** Gate green. Commit.

### Task 9: Streaming

**Files:** Create `lib/strata/stream.march`, `test/stream_test.march`

**Interfaces — Consumes:** `World`, `Edit.Outcome`, `Mesh.build`, `Mesh.has_all`, `Terrain.run`. **Produces:**
```
type Config = Config(Int, Int, Int, Int)     -- radius_h, radius_v (chunks), apply_budget (results per tick), workers
type Stream
fn start(w : World, c : Config) : Stream                       -- spawns the workers; needs IO.Spawn
fn stop(s : Stream) : Unit
fn set_focus(s : Stream, points : List((Float, Float, Float))) : Stream   -- metres
fn tick(s : Stream) : Stream
fn apply(s : Stream, o : Outcome) : Stream                     -- adopts o's world and schedules o's dirty chunks for remeshing
fn world(s : Stream) : World
fn take_meshes(s : Stream) : (Stream, List((Key, Meshed)))     -- finished meshes since the last take, newest per key
fn take_unloads(s : Stream) : (Stream, List(Key))
fn settled(s : Stream) : Bool                                  -- nothing wanted, in flight or waiting to apply
fn run_until_settled(s : Stream, max_ticks : Int) : Stream
fn world_hash(s : Stream) : Int                                -- over loaded chunks; independent of load order
fn loaded_count(s : Stream) : Int
```
Behaviour:
- The wanted set is the union, over focus points, of the box `radius_h` chunks out in x and z and `radius_v` in y around the point's chunk. Chunks are unloaded once they are more than one chunk beyond every box.
- Each `tick`: request generation for wanted, unloaded, not-in-flight keys, nearest to a focus first; take up to `apply_budget` finished results; a generated chunk goes through `World.insert`; a result for a key no longer wanted is discarded; then request meshes for loaded chunks that need one and satisfy `Mesh.has_all`.
- Workers run `Terrain.run` and `Mesh.build`. A mesh result is dropped if its chunk or any of its 26 neighbours changed after the request was sent, and the request is reissued.
- `tick` never blocks waiting for a worker. `run_until_settled` may sleep between ticks.

- [ ] **Step 1: failing tests.** With a flat terrain function (stone below y = 0, air above), radius 2 by 1, one focus at the origin, `run_until_settled`: `loaded_count == 5 * 5 * 3`; every chunk at least one inside the box edge has a mesh in `take_meshes`; `take_meshes` again is empty. Move the focus 64 m in x: old chunks appear in `take_unloads`, new ones load. Determinism: the same seed and focus path with `workers` 1 and with 4, and `apply_budget` 1 and 64, give the same `world_hash`. Edit then leave then return: the edit is still there. `apply` of a `set_block` outcome: exactly the outcome's dirty chunks reappear in `take_meshes` and the new mesh reflects the edit. Teleport (Review Focus 5): set the focus, tick once, move the focus 100 chunks away, settle; `loaded_count` is one box, `world_hash` equals a fresh stream settled at the far point, and an edit queued at the first location before leaving is applied when the focus returns. Empty focus list: everything unloads and it settles.
- [ ] **Step 2:** Run; expect failure.
- [ ] **Step 3:** Implement. Start from cube_forge's `Streamer` actor pattern (`spawn`, `send`, `Actor.call`). `Chunk`, `Meshed` and `Around` must be sendable; if a persistent value cannot be sent, send its native arrays and rebuild on the other side.
- [ ] **Step 4:** Gate green. Commit.

### Task 10: Raycast and collision

**Files:** Create `lib/strata/query.march`, `test/query_test.march`

**Interfaces — Consumes:** `World.get_block`, `World.scale`, `Blocks`. **Produces:**
```
type Hit = Miss | Hit(Int, Int, Int, Int, Float)       -- voxel x, y, z; face entered (0..5); distance in metres
fn raycast(w : World, ox : Float, oy : Float, oz : Float, dx : Float, dy : Float, dz : Float, max : Float, stop : fn(Int) -> Bool) : Hit
fn targetable(r : Registry) : fn(Int) -> Bool          -- true for any id whose style is not NoMesh
type Swept = Swept(Float, Float, Float, Bool, Bool, Bool)   -- new centre in metres; blocked on x, y, z
fn sweep(w : World, x : Float, y : Float, z : Float, hx : Float, hy : Float, hz : Float, dx : Float, dy : Float, dz : Float) : Swept
```
Rules: positions and distances are metres; the direction need not be normalised. The ray steps voxel to voxel. `stop(id)` decides whether a block ends the ray; a `Cube` or `Cross` block is hit as its whole cell, a `Model` block only where the ray meets one of its boxes. `sweep` moves a box of half extents `hx, hy, hz` by the motion, one axis at a time (y, then x, then z, as cube_forge's `Player` does), stopping at blocks for which `Blocks.collides` holds; `Model` blocks collide by their boxes, `Cross` blocks never. A zero direction or non-positive `max` returns `Miss`; zero motion returns the start unblocked.

- [ ] **Step 1: failing tests.** Ray straight down onto a floor at y = 0 from 5 m up at scale 1.0: hits the floor voxel, face 0, distance 4.0 (the top of voxel y = 0 is at 1.0). Same scene at scale 0.1: distance scales accordingly. Ray starting at (-0.5, 0.5, 0.5) heading -x hits a block at voxel x = -3 with face 2 (Review Focus 1). Ray that exits loaded chunks: `Miss`. `stop` that rejects the first block type: the ray passes through to the next. Model block with a half-height box: a ray through the upper half misses it. Property: on random small worlds, `raycast` agrees with an oracle that samples the ray every 1/64 voxel, on hit-or-miss and on the voxel hit. Sweep: a box falling onto the floor stops with its base at the floor's top and `blocked y`; sliding along a wall keeps the free axis' motion; a 0.6 m wide box passes a 1-voxel gap at scale 1.0 and is blocked by the same gap at scale 0.5.
- [ ] **Step 2:** Run; expect failure.
- [ ] **Step 3:** Implement, adapting cube_forge's `raycast.march` and the collision half of `player.march`.
- [ ] **Step 4:** Gate green. Commit.

### Task 11: Demo, measurements and write-up

**Files:** Create `demo/forge.toml`, `demo/lib/strata_demo.march`, `RESULTS.md`; modify `README.md`, `scripts/verify.sh` (already runs `demo/` when present)

**Interfaces — Consumes:** the whole public surface above. The demo names no cube_forge module.

The program:
- registers air plus at least stone, soil, a see-through translucent liquid as an 8-state family, a cutout leaf, one emitting block, and one cross plant;
- supplies a noise heightmap terrain function built on `Strata.Noise`, with liquid filling below a fixed level and plants scattered on soil;
- **Run A, scale 1.0:** `Config(6, 3, 8, 4)`; one focus walking 256 m along +x in 64 steps, settling after each; then `set_block` at the surface under the focus, then `fill_sphere` of radius 12 with air at the surface, each applied and settled;
- **Run B, scale 0.1:** `Config(3, 2, 8, 4)`; one focus at the origin; `fill_sphere` radius 20 with air, applied and settled;
- prints for each run: chunks loaded; uniform, paletted and direct counts; total voxel bytes and mean per chunk; total mesh bytes, total quads, bytes per quad; mean and worst terrain generation time per chunk; mean and worst mesh time per chunk; time from `apply` to the edited meshes arriving, for the single edit and for the sphere; `cache_size`; `world_hash`;
- writes `surface_a.ppm`: a top-down image of Run A's loaded area, one pixel per column, coloured by the topmost non-air block's id and shaded by height.

- [ ] **Step 1:** Write `demo/forge.toml` with `strata = { path = ".." }` and the `[ffi]` block from Task 5's finding.
- [ ] **Step 2:** Implement the program. Timing uses the monotonic clock available on March main (find it with the `forge-search` skill).
- [ ] **Step 3:** Run it twice. Expected: both runs complete; `world_hash` is identical across the two executions for each run; bytes per quad is exactly 8; `surface_a.ppm` opens and shows terrain with a visible crater. Convert it to PNG (`sips -s format png`) and look at it.
- [ ] **Step 4:** Write `RESULTS.md`: the printed numbers, the machine, the March commit, and the comparison the spec asks for. The cube_forge baseline is the most recent entry in its `RESULTS.md` whose mesher includes ambient occlusion; state which entry was used and compare on equal voxel volume (two 16 by 256 by 16 chunks equal four Strata chunks). If Strata is slower, say so and give the measured cause. If single-edit latency exceeds 16.7 ms, say so and note that sub-chunk dirty regions are the planned remedy; do not build them in this plan.
- [ ] **Step 5:** README: quick start, the consumer `[ffi]` block, the module table, a 30-line example of registering blocks and streaming a world, known gaps.
- [ ] **Step 6:** Gate green in the library and `demo/`. Commit.

---

## Out of scope for this plan

Save format, any light field, smooth terrain, level of detail (the `level` fields stay 0), sub-chunk remeshing, and anything that opens a window. These belong to later projects in the spec.

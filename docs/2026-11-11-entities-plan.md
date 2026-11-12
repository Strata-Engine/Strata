# Strata Entities Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** One id for one thing in the world: spawned, moved in one of four ways, shown with its figure, body and sounds when a camera is near, simulated without them when none is, and removed; for tens of thousands at once, with the game's own numbers riding along.

**Architecture:** C (`native/strata_ent.h`, logic; `native/strata_ent.c`, the extern wrapper holding one world) owns every entity as rows in packed arrays and runs every per-entity loop: movement, the ray and the box sweep against its own one-bit-a-voxel copy of the loaded blocks, the seen test, the spatial grid, the touch tests. March (`Strata.Entities`) holds the kind table, feeds the kernel the changed chunks, and does the work that is bounded by transitions rather than by entity count: giving and taking figures, bodies and voices as entities enter and leave view. The game calls Strata; Strata never calls the game.

**Tech Stack:** March (origin/main via `scripts/env.sh`), C with no Vulkan, GLFW, physics or audio includes.

**Spec:** `docs/2026-11-11-entities-design.md`

## Global Constraints

- Branch `claude/strata-engine-gaps-ec17fc`. Commit after each task. After each task's review is clean and the gate passes, merge the branch into `main` through a temporary worktree (`git worktree add <scratch> main`, `git merge --no-ff`, gate, `git worktree remove`). Never push.
- `scripts/verify.sh` prints PASS at the end of every task (`STRATA_NO_GPU=1 scripts/verify.sh` where no GPU is to hand; say which was run).
- **Task 10 starts only when the animation plan's drawing is on `main`** (`grep -n "fn figures" lib/strata/render.march` on `main` finds it). Until then stop after Task 9 and report.
- GAPS S1 to S23 apply (read `GAPS.md`): no `opaque`, `by`, `on`, `loop`, `within`, `spawn`, `linear` as names (so the call is `Strata.Entities.add`, not `spawn`); `needs IO.Clock` and `needs IO.Process` in every test module; every alias unique across the whole suite (`grep -rn "^ *alias" lib test viewer animcheck soundcheck demo` before choosing; `En`, `Fen`, `Ech` are intended); externs wrapped in ordinary functions; extern March names unique across the program (prefix `xe_`); full module paths for Unit calls; no extern result bound to `_`; a stamp line in `strata_ent.c` for `strata_ent.h` (S14), checked by `native/stamp_ent.sh --check` as `native/stamp_anim.sh` does.
- C prefix `en_` for everything in `strata_ent.h` and `strata_ent.c`; the wrapper's exported names are `en_x_*`. All state lives inside `EnWorld`; `strata_ent.c` holds the one world a process has, as `strata_anim.c` does.
- No C type crosses into March: Int, Float, `NativeI32Arr`, `NativeF32Arr`, `NativeU8Arr` only.
- No panic reachable from library code. A dead or unknown id reads as defaults (position 0, kind -1, slots 0, part ids -1, `alive` false) and a write to it is ignored and counted in the refusals stat.
- Fixed values: capacity 65,536 by default, at most 1,048,576; kinds 62; slots 8 float and 8 int a kind; voices 2 an entity; routes 256 points each, 262,144 points in all by default; grid cell 4 m; events at most 65,536 a tick, the rest dropped and counted; body budget 128 by default; grounding search 16 m up and down; seen distance at most `(radius_h - 2) * chunk_metres`; unseen hysteresis one `chunk_metres`.
- Time is the `dt` passed, never the wall clock. The same calls with the same `dt` give the same `en_hash` on the same build and machine.
- Rulings made while building go in `docs/entities-ledger.md`, in the form `docs/animation-ledger.md` uses.
- Timings are trusted only when `uptime` shows a load average under 8; otherwise recorded as not judged. A missed target is recorded in `RESULTS.md`, not hidden.

## Review Focus

1. **Stale ids.** An id kept after its entity was removed, then used after the row was reused by a new entity: every read gives defaults, every write is ignored and counted, and the new entity is untouched. Test: Task 1 ("stale ids").
2. **Bad numbers.** A NaN or infinite position, velocity or speed; `dt` of 0, negative, or 10 s. NaN and infinity are refused and counted; `dt` 0 changes nothing; negative is treated as 0 and counted; a 10 s step of a flying entity still stops at the first block on its path. Tests: Task 1 ("bad numbers"), Task 3 ("dt edge cases").
3. **Full tables.** Adding to a full world, a 257-point route, routes past the point pool, a 63rd kind, more than 65,536 events in a tick. Each is refused (`-1`) or dropped, counted, and nothing else is disturbed. Tests: Task 1 ("full world"), Task 4 ("event overflow"), Task 5 ("route limits").
4. **Removal inside the game's loop.** The game takes `each(e, kind)`, then removes entities as it walks the array, including ones later in it, and adds new ones. Later reads of the removed ids give defaults; no event of the next tick names a removed id; an entity removed and an entity touching it in the same tick produce no touch. Test: Task 4 ("removal in the loop").
5. **The ground goes away.** The chunk under a seen walker is unloaded or its mask dropped in the same frame; a walker is added at y = -1,000,000, far outside any loaded chunk; a seen walker stands where a sphere edit removes the floor. The first two are held or unseen and never fall without end; the third falls and lands as a walker should. Test: Task 6 ("the ground goes away").

---

### Task 1: The entity kernel, headless

**Files:**
- Create: `native/strata_ent.h`, `native/tests/ent_test.c`, `docs/entities-ledger.md`

**Interfaces — Produces** (C, static functions in `strata_ent.h`):
```c
#define EN_KINDS 62
#define EN_SLOTS 8
#define EN_VOICES 2
typedef struct EnWorld EnWorld;                      /* every array allocated in en_open */
EnWorld *en_open(int capacity, int route_points, int bodies, double seen_cap /*metres*/, double chunk_metres, double voxel_metres);
void     en_close(EnWorld *);
/* kind ints: mover (0 placed, 1 flying, 2 walking, 3 body), unseen (0 live, 1 freeze, 2 remove), divisor >= 1,
   float slots, int slots, gravity 0/1, has figure 0/1, has body 0/1, voices 0..2.
   kind floats: hx, hy, hz (half extents, metres), step height, lifetime (<= 0 none), seen distance (<= 0: seen_cap), gravity m/s2 */
int      en_kind(EnWorld *, const int32_t ints[9], const float floats[7]);     /* index, or -1 */
int64_t  en_add(EnWorld *, int kind, double x, double y, double z, double facing);   /* id, or -1 */
void     en_remove(EnWorld *, int64_t id, int reason /*0 game, 1 lifetime, 2 unseen rule*/);
int      en_alive(const EnWorld *, int64_t id);
int      en_kind_of(const EnWorld *, int64_t id);                              /* -1 if dead */
/* which: 0 position, 1 velocity (3 doubles); 2 facing, 3 age (1 double) */
int      en_get(const EnWorld *, int64_t id, int which, double out[3]);        /* 0 if dead */
void     en_set(EnWorld *, int64_t id, int which, const double v[3]);
double   en_float(const EnWorld *, int64_t id, int n);   void en_set_float(EnWorld *, int64_t id, int n, double v);
int64_t  en_int(const EnWorld *, int64_t id, int n);     void en_set_int(EnWorld *, int64_t id, int n, int64_t v);
/* part: 0 figure, 1 body, 2 voice 0, 3 voice 1; -1 for none */
int64_t  en_part(const EnWorld *, int64_t id, int part); void en_set_part(EnWorld *, int64_t id, int part, int64_t v);
int      en_each(const EnWorld *, int kind, int max, int64_t *ids);            /* live ids of a kind, in row order; count */
/* a whole column of one kind, in en_each order. col: 0 position (3 floats a row), 1 velocity (3), 2 facing (1), 10+n float slot n (1) */
int      en_column(const EnWorld *, int kind, int col, int max_rows, float *out);
void     en_set_column(EnWorld *, int kind, int col, int rows, const float *in);   /* rows must equal the live count, else refused */
uint64_t en_hash(const EnWorld *);                   /* FNV-1a over the live rows' columns and the routes */
double   en_stat(const EnWorld *, int which);        /* 0 live, 1 seen, 2 waiting for ground, 3 refused calls, 4 negative dt,
                                                        5 events dropped, 6 last refusal reason (EN_R_*), 7 entities over the body budget */
```
An id packs a row and that row's generation; `en_remove` raises the generation. Positions are stored as doubles (an island 2 km across at 10 cm voxels needs more than a float carries); `en_column` hands them out as floats.

- [ ] **Step 1: failing tests** in `ent_test.c` (`CHECK` style of `anim_test.c`, prints `ent_test: ok`):
  - kinds: a kind with 3 float and 2 int slots is index 0; a kind asking for 9 float slots, divisor 0, mover 4, or a body with mover other than 3 is `-1` with stat 6 naming the reason; the 63rd kind is `-1`;
  - add and read: `en_add(kind 0, 1.5, 2.5, 3.5, 0.25)` then position reads (1.5, 2.5, 3.5), facing 0.25, velocity 0, age 0, kind 0, every slot 0, every part -1;
  - slots: float slot 2 set to 7.5 reads 7.5; slot 3 on a 3-slot kind is refused and reads 0;
  - **stale ids** (Review Focus 1): add A, remove A, add B (takes A's row); `en_alive(A)` is 0; `en_set` through A leaves B's position unchanged and raises stat 3 by 1; every read through A gives the defaults;
  - **bad numbers** (Review Focus 2): `en_set` position with a NaN or an infinity is refused and counted; `en_add` at a NaN is `-1`;
  - **full world** (Review Focus 3): capacity 4, a fifth `en_add` is `-1` and counted; after one remove a further add succeeds;
  - each and columns: 5 of kind 0 and 3 of kind 1 interleaved; `en_each(1)` gives the 3 in row order; `en_column(1, 0)` gives their 9 position floats in that order; `en_set_column` with 2 rows is refused; with 3 rows every position reads back;
  - hash: two fresh worlds given the same 1,000 scripted adds, sets and removes have equal `en_hash`; changing one slot changes it.
- [ ] **Step 2:** run (the loop in `scripts/verify.sh` picks up `native/tests/*_test.c` by itself) `STRATA_NO_GPU=1 scripts/verify.sh`; expect FAIL (`strata_ent.h` missing).
- [ ] **Step 3:** implement `strata_ent.h`. Live rows of a kind must be walkable without scanning dead rows (the anti-goal: cost follows what exists).
- [ ] **Step 4:** run; expect PASS.
- [ ] **Step 5:** create `docs/entities-ledger.md` with the header, the id packing and the column layout chosen. Commit `feat(entities): the entity kernel, headless — rows, ids, kinds, slots, columns`.

### Task 2: Entities from March, and entcheck

**Files:**
- Create: `native/strata_ent.c`, `native/stamp_ent.sh`, `lib/strata/ffi/ent.march`, `lib/strata/entities.march`, `test/entities_test.march`, `entcheck/forge.toml`, `entcheck/lib/entcheck.march`, `scripts/ent_check.sh`
- Modify: `scripts/verify.sh` (add `entcheck` to the app loop; run `scripts/ent_check.sh` after `scripts/anim_check.sh`), `README.md` (an "Entities" section; the consumer `[ffi]` block gains `native/strata_ent.c`)

**Interfaces**
- Consumes: Task 1's `en_*`.
- Produces (`Strata.Entities`, alias `En`; `Strata.Ffi.Ent`, alias `Fen`, capability `Ent`):
```
type Kind                                   -- pure data; built with the with_ functions
fn kind() : Kind                            -- placed, no parts, unseen rule live, divisor 1, no lifetime, no slots
fn with_mover(k : Kind, mover : Int, hx : Float, hy : Float, hz : Float, step_height : Float, gravity : Float) : Kind
fn placed() : Int  fn flying() : Int  fn walking() : Int  fn bodied() : Int          -- 0, 1, 2, 3
fn with_unseen(k : Kind, rule : Int, divisor : Int) : Kind
fn lives() : Int  fn freezes() : Int  fn goes() : Int                                 -- 0, 1, 2
fn with_lifetime(k : Kind, seconds : Float) : Kind
fn with_slots(k : Kind, floats : Int, ints : Int) : Kind
fn with_seen_distance(k : Kind, metres : Float) : Kind
fn valid(k : Kind) : Bool                   -- the checks en_kind makes, answerable without a world

type Entities                               -- the kinds, the body budget, the open flag
fn open(kinds : List(Kind), capacity : Int, bodies : Int, radius_h : Int, chunk_metres : Float, voxel_metres : Float) : Entities
fn close(e : Entities) : Entities
fn is_open(e : Entities) : Bool             -- false if any kind was refused or the kernel could not allocate
fn add(e : Entities, kind : Int, x : Float, y : Float, z : Float, facing : Float) : (Entities, Int)
fn remove(e : Entities, id : Int) : Entities
fn alive(e : Entities, id : Int) : Bool     fn kind_of(e : Entities, id : Int) : Int
fn position(e : Entities, id : Int) : (Float, Float, Float)      fn set_position(e, id, x, y, z) : Entities
fn velocity(e : Entities, id : Int) : (Float, Float, Float)      fn set_velocity(e, id, x, y, z) : Entities
fn facing(e : Entities, id : Int) : Float   fn set_facing(e, id, a) : Entities   fn age(e : Entities, id : Int) : Float
fn float(e : Entities, id : Int, n : Int) : Float   fn set_float(e, id, n, v) : Entities
fn int(e : Entities, id : Int, n : Int) : Int       fn set_int(e, id, n, v) : Entities
fn each(e : Entities, kind : Int) : NativeIntArr
fn column(e : Entities, kind : Int, col : Int) : NativeF32Arr    fn set_column(e, kind, col, a : NativeF32Arr) : Entities
fn col_position() : Int  fn col_velocity() : Int  fn col_facing() : Int  fn col_float(n : Int) : Int   -- 0, 1, 2, 10 + n
fn hash(e : Entities) : Int                 -- en_hash folded to its low 62 bits
fn stats(e : Entities) : (Int, Int, Int, Int)   -- live, seen, waiting for ground, refused
fn stat(e : Entities, which : Int) : Float
```
`open` computes the kernel's `seen_cap` as `(radius_h - 2) * chunk_metres` (at least one `chunk_metres`). `with_figure`, `with_body`, `with_sound` come in Tasks 7 and 8. entcheck: `ENTCHECK_SCENE=name ./entcheck` prints `scene name: ok` or the failure and exits 1.

- [ ] **Step 1:** `test/entities_test.march`, pure only (no kernel call): `kind()` is valid; a kind with 9 float slots, divisor 0, or a box with a zero half extent on a walking mover is not; each `with_` function changes only its own fields.
- [ ] **Step 2:** entcheck scenes, each failing first: `basics` (the Task 1 add-and-read, slot and stale-id checks through `Strata.Entities`); `columns` (1,000 entities of one kind, velocity column written in one call, read back equal); `determinism` (the Task 1 scripted run twice in one process through `open`/`close`, equal `hash`); `closed` (every call on a closed world is ignored or gives defaults).
- [ ] **Step 3:** run `STRATA_NO_GPU=1 scripts/verify.sh`; expect FAIL.
- [ ] **Step 4:** implement the wrapper, the bindings, `Strata.Entities`, `entcheck` and `scripts/ent_check.sh` (modelled on `scripts/anim_check.sh`: stamp check, release build, every scene). `entcheck/forge.toml` lists `../native/strata_ent.c`, `../native/strata_anim.c` and `../native/strata_noise.c` with `-I../native` and `-ffp-contract=off`.
- [ ] **Step 5:** run; expect PASS. Commit `feat(entities): Strata.Entities over the kernel, entcheck`.

### Task 3: The kernel's blocks, the ray, flying and placed movers

**Files:**
- Modify: `native/strata_ent.h`, `native/strata_ent.c`, `native/tests/ent_test.c`, `lib/strata/ffi/ent.march`, `lib/strata/entities.march`, `lib/strata/stream.march`, `test/stream_test.march`, `entcheck/lib/entcheck.march`, `scripts/ent_check.sh`

**Interfaces — Produces**
```c
void en_block_table(EnWorld *, int n, const uint8_t *collides);   /* per block id: 0 passes, 1 stops as its whole cell */
void en_chunk(EnWorld *, int cx, int cy, int cz, const int32_t *ids /*32768, Coord.index order*/);
void en_chunk_uniform(EnWorld *, int cx, int cy, int cz, int id);
void en_chunk_drop(EnWorld *, int cx, int cy, int cz);
int  en_solid(const EnWorld *, int vx, int vy, int vz);           /* 1 solid, 0 clear, -1 no chunk held */
/* first solid voxel along the ray; out: voxel x, y, z, face (Strata.Query's numbering), and *t metres. 1 hit, 0 miss, -1 left the held chunks */
int  en_ray(const EnWorld *, const double o[3], const double d[3], double max, int32_t out[4], double *t);
void en_tick(EnWorld *, int nfocus, const double *focus /*3 each*/, double dt);
/* events since the last tick, in order. ints: code, a (id), b (id or 0), i0, i1, i2, i3; floats: x, y, z */
int  en_events(const EnWorld *, int max, int64_t *ints /*7 each*/, double *floats /*3 each*/);
```
Event codes: 1 touch (a, b, point), 2 block hit (a; i0..i2 the voxel, i3 the face; point), 3 entered view, 4 left view, 5 route finished, 6 removed (i0 the reason), 7 body lost, 8 blocked.
```
Strata.Stream.take_changed(s : Stream) : (Stream, List(Strata.Coord.Key), List(Strata.Coord.Key))
    -- chunks loaded or edited, and chunks unloaded, since the last take; independent of take_meshes and take_unloads
Strata.Entities.blocks(e : Entities, s : Strata.Stream.Stream) : (Entities, Strata.Stream.Stream)
Strata.Entities.blocks_world(e : Entities, w : Strata.World.World) : Entities     -- every chunk of w, for tests and tools
Strata.Entities.step(e : Entities, focus : List((Float, Float, Float)), dt : Float) : Entities   -- the kernel's tick alone, no parts
type Event      fn events(e : Entities, w : Strata.World.World) : List(Event)
fn ev_code(v : Event) : Int  fn ev_a(v) : Int  fn ev_b(v) : Int  fn ev_point(v) : (Float, Float, Float)
fn ev_voxel(v) : (Int, Int, Int)  fn ev_face(v) : Int  fn ev_block(v) : Int  fn ev_reason(v) : Int
```
A mask bit is set where `Strata.Blocks.collides` is true and the style is not cross (`style_index != 1`); a model block that collides is its whole cell (the design's one stated difference from `Strata.Query`). `ev_block` is read from the world by `events`, since the kernel holds no ids. `Strata.Ffi.Ent.ray(ox, oy, oz, dx, dy, dz, max, out : NativeI32Arr) : Float` (distance; -1.0 miss, -2.0 left the held chunks) carries `en_ray` to entcheck; Task 5 adds `Strata.Ffi.Ent.sweep(x, y, z, hx, hy, hz, dx, dy, dz, out : NativeF32Arr) : Int` (out: position then three blocked flags) the same way. Until Task 6, `en_tick` treats an entity as seen when it is within its kind's seen distance of any focus, measured horizontally, with no hysteresis; no focus means every entity is unseen. In this task `en_tick` does: age and lifetimes (removal reason 1, event 6); placed movers nothing; flying movers `position += velocity * dt` with gravity added to velocity first if the kind asks, and, when seen, `en_ray` along the step: on a hit the entity is put at the hit point with velocity 0 and event 2 is written; unseen flying entities move with no ray. A flying entity that has hit stays where it is until the game moves or removes it.

- [ ] **Step 1: failing C tests:** masks (a chunk with one solid voxel at local (3, 4, 5) of chunk (1, 0, -1): `en_solid` is 1 there, 0 beside it, -1 in chunk (9, 9, 9); a uniform solid chunk; dropping it gives -1); ray (from above a solid floor straight down hits the floor's top voxel, face 0, at the right `t`; a ray entering at a chunk border and a negative-coordinate chunk hits the right voxel; a ray leaving the held chunks answers -1; a zero direction is a miss); flying (velocity (10, 0, 0), `dt` 0.1, no blocks: x advances 1.0 a tick; gravity 10 from rest for 1 s in 60 ticks falls 5 m within 0.1; a seen flyer aimed at a wall 3 m away stops at the wall and one event 2 names the wall voxel and face; the same flyer unseen passes through); lifetime 0.5 s is removed on the tick its age passes 0.5 with event 6 reason 1; **dt edge cases** (Review Focus 2): `dt` 0 moves nothing and ages nothing, `dt` -1 the same and stat 4 is 1, one `dt` of 10 s at 100 m/s toward a wall 3 m away still stops at the wall.
- [ ] **Step 2: failing March checks:** `test/stream_test.march` gains: after settling a stream around one focus, `take_changed` lists every loaded chunk once and no unloads; a second take is empty; after `Strata.Stream.adopt` of a one-block edit it lists that chunk; after moving the focus far away it lists the old chunks as unloaded; `take_meshes` and `take_unloads` still give what they gave. entcheck scene `ray`: a flat-ground-with-pillars world built with `Strata.Edit`, 2,000 rays from fixed pseudo-random origins and directions (`Strata.Seed`), the kernel's answer (through a `Strata.Ffi.Ent` ray binding) and `Strata.Query.raycast` with a `collides`-and-not-cross stop predicate agree on hit or miss, voxel and face for every ray, and on distance within 1e-4. Scene `bullets`: 1,000 flyers fired at a wall, every one reports one block hit whose `ev_block` is the wall's block id.
- [ ] **Step 3:** run; expect FAIL. Implement. `blocks` passes a uniform chunk with `en_chunk_uniform` and any other with `Strata.Chunk.to_flat`; the block table is sent at `open` time by `blocks` on its first call (the registry comes from `Strata.Stream.world`). Run; expect PASS.
- [ ] **Step 4:** Commit `feat(entities): the kernel's copy of the blocks, the ray, flying and placed movers`.

### Task 4: The grid, near, raycast, touches; the bullet storm

**Files:** `native/strata_ent.h`, `native/strata_ent.c`, `native/tests/ent_test.c`, `lib/strata/ffi/ent.march`, `lib/strata/entities.march`, `entcheck/lib/entcheck.march`, `scripts/ent_check.sh`, `RESULTS.md`

**Interfaces — Produces**
```c
void en_touching(EnWorld *, int kind_a, int kind_b);
int  en_near(const EnWorld *, const double p[3], double radius, uint64_t kind_mask, int max, int64_t *ids);   /* sorted by id */
int64_t en_ray_entities(const EnWorld *, const double o[3], const double d[3], double max, uint64_t kind_mask, double *t);  /* nearest box hit, or -1 */
```
```
Strata.Entities.touching(e : Entities, kind_a : Int, kind_b : Int) : Entities
Strata.Entities.near(e : Entities, x : Float, y : Float, z : Float, radius : Float, kinds : Int) : NativeIntArr
Strata.Entities.raycast(e : Entities, ox, oy, oz, dx, dy, dz : Float, max : Float, kinds : Int) : (Int, Float)   -- (id or -1, metres)
Strata.Entities.mask(kinds : List(Int)) : Int
```
`en_tick` now ends with: rebuild the grid from the live rows; for each declared pair, test boxes (centre the position, half extents the kind's) and write one event 1 per touching pair per tick, `a` of the lower kind index. A flying entity's box is swept along the step it just made. Entities removed during the tick are in neither the grid nor any touch. `near` and `raycast` read the grid of the last tick.

- [ ] **Step 1: failing C tests:** `near` equals a brute-force filter for 5,000 scattered entities at 100 query points and three radii (0.5, 4, 37), including a point at negative coordinates and a mask that excludes a kind; `en_ray_entities` equals a brute-force nearest box hit for 500 rays; touches equal a brute-force pair test over 2,000 entities of two kinds; a flyer at 600 m/s with `dt` 1/60 passing a 0.4 m target between ticks reports the touch; a pair not declared reports nothing; the same pair reports once a tick while overlapping; **event overflow** (Review Focus 3): 70,000 touching pairs in one tick give 65,536 events and stat 5 is 4,464; **removal in the loop** (Review Focus 4): take `en_each`, remove every second id and add 10 new entities while walking it; reads of removed ids give defaults; the next tick's events name no removed id; an entity whose lifetime ends on the tick it would touch produces event 6 and no event 1.
- [ ] **Step 2:** entcheck scene `storm`: an open 200 m floor, 200 placed targets of kind 1, 20,000 flyers of kind 0 added over 100 ticks in rings from four emitters by writing columns, pair (0, 1) declared, every focus at the centre; the game loop removes each flyer named in a touch or block-hit event and adds a new one. Checks: live count stays 20,000 once filled; the count of touch events equals a brute-force count on ticks 150 and 300; `hash` equal across two runs. Prints the median and worst `step` time over ticks 200 to 600.
- [ ] **Step 3:** run; expect FAIL. Implement. Run; expect PASS.
- [ ] **Step 4:** record the storm's tick cost against the 2 ms target, and add-and-remove of 5,000 in one tick against 1 ms, in `RESULTS.md` under a new "Entities" heading. Commit `feat(entities): the grid, near, raycast and touch pairs; the bullet storm`.

### Task 5: The sweep, walking, routes

**Files:** `native/strata_ent.h`, `native/strata_ent.c`, `native/tests/ent_test.c`, `lib/strata/ffi/ent.march`, `lib/strata/entities.march`, `entcheck/lib/entcheck.march`, `scripts/ent_check.sh`

**Interfaces — Produces**
```c
/* move a box one axis at a time (y, then x, then z) against the masks; out position and blocked[3]. -1 if it would enter a column with no chunk held (nothing moved) */
int  en_sweep(const EnWorld *, const double p[3], const double h[3], const double d[3], double out[3], int blocked[3]);
int  en_route(EnWorld *, int n, const double *points /*3 each*/);     /* id, or -1 */
void en_route_free(EnWorld *, int route);                              /* freed once no entity follows it */
void en_follow(EnWorld *, int64_t id, int route, double speed, int loops);   /* route -1 stops following */
int  en_grounded(const EnWorld *, int64_t id);
```
```
Strata.Entities.route(e : Entities, points : List((Float, Float, Float))) : (Entities, Int)
Strata.Entities.free_route(e : Entities, route : Int) : Entities
Strata.Entities.follow(e : Entities, id : Int, route : Int, speed : Float, loops : Bool) : Entities
Strata.Entities.grounded(e : Entities, id : Int) : Bool
```
Walking, seen: gravity (the kind's) is added to the vertical velocity, capped at 60 m/s of fall; the horizontal velocity is the game's (`set_velocity`), or, when following, `speed` toward the next waypoint's x and z; the box is moved with `en_sweep`; a horizontal axis blocked while grounded is retried raised by the kind's step height and, if it then moves, set down again; landing sets grounded and vertical velocity 0. A waypoint is reached within 0.25 m horizontally. A follower that makes less than 0.1 m of progress toward its waypoint in 1 s writes event 8 once and keeps trying. Walking, unseen: with a route, the position moves along straight lines between waypoints at `speed`, y interpolated, touching no blocks; with none it stays. At the end: event 5, then stop, or start again if it loops. Facing turns to the direction of horizontal travel for followers. A kind's divisor `n` steps its unseen entities on every `n`th tick with `n * dt`, entities spread across ticks by row number.

- [ ] **Step 1: failing C tests:** sweep (a box dropped onto a floor rests with its base 1e-4 voxels above it; pushed at a wall it stops against it and `blocked` names the axis; it passes a cross-style voxel; into a column with no chunk it answers -1 and does not move); walking (falls 5 m and lands grounded within 0.02 m of the floor; walks up a 0.5 m step with step height 0.6 and is stopped by a 1.2 m one; walks off a ledge and falls); routes (an unseen follower at 2 m/s on a 3-point route of 10 m and 10 m legs is 10 m along after 5 s within 0.05 m, reports event 5 once at 10 s; looping it does not report and is back at the start at 20 s; a seen follower on flat ground reaches the end within 0.3 m; a seen follower facing a wall reports event 8 after 1 s); divisor 4 moves an unseen follower the same distance in 240 ticks as divisor 1 within 0.1 m; **route limits** (Review Focus 3): 257 points refused, a route that would pass the point pool refused, `follow` on a freed or unknown route ignored and counted, a route freed while followed lives until its last follower stops.
- [ ] **Step 2:** entcheck scene `sweep`: on the `ray` scene's world (no model blocks), 2,000 boxes of three sizes at fixed pseudo-random places and moves; `en_sweep` (through a binding) and `Strata.Query.sweep` agree on the end position within 1e-4 and on the blocked axes. Scene `walk`: 50 walkers follow a route across pillars and steps; all arrive; `hash` equal across two runs.
- [ ] **Step 3:** run; expect FAIL. Implement. Run; expect PASS.
- [ ] **Step 4:** Commit `feat(entities): the sweep, walking, routes and unseen movement`.

### Task 6: Seen and unseen, and setting on the ground

**Files:** `native/strata_ent.h`, `native/strata_ent.c`, `native/tests/ent_test.c`, `lib/strata/ffi/ent.march`, `lib/strata/entities.march`, `entcheck/lib/entcheck.march`, `scripts/ent_check.sh`

**Interfaces — Produces:** `en_seen(const EnWorld *, int64_t id)`, `Strata.Entities.seen(e, id) : Bool`. Events 3, 4 and 6 (reason 2) are now written. `en_tick`'s order is now the design's: lifetimes; movement of seen entities; movement of unseen entities due this tick; the seen and unseen switch; the grid; touches.

The switch, per entity, with `d` its kind's seen distance and `c` the chunk side: unseen becomes a candidate at horizontal distance `<= d` from any focus; seen becomes unseen at `> d + c`. A walking candidate waits (counted in stat 2) until every chunk of its column from 16 m below to 16 m above its position is held; then the nearest height, searching outward up and down from its own, where its box is clear and the voxel under it is solid is found; it is moved there, marked seen and grounded, and event 3 is written; with no such place within 16 m it stays unseen and is tried again next tick. Flying and placed candidates become seen at once, unmoved. Becoming unseen writes event 4, then the kind's rule: live keeps going, freeze stops movement and ageing until seen again, remove removes it (event 6, reason 2). A seen walker whose `en_sweep` answers -1 is held for that tick.

- [ ] **Step 1: failing C tests:** hysteresis (with `d` 40 and `c` 3.2: an entity at 39 m becomes seen, stays seen at 42 m, is unseen at 44 m, and a focus pacing between 41 m and 42 m causes no event after the first); grounding (a walker unseen at a height inside a hill is set on the hill's top; one above a pit is set on the pit's floor; one over a column of 40 m of air stays unseen and stat 2 is 1; one whose chunks are not yet held waits, then is set down on the tick after `en_chunk`); no focus leaves everything unseen; the three unseen rules; **the ground goes away** (Review Focus 5): `en_chunk_drop` of the chunk under a seen walker holds it in place, and it walks again when the chunk returns; a walker added at y = -1,000,000 never becomes seen and its y does not change over 600 ticks; a seen walker whose floor voxels are cleared by a new `en_chunk` falls and lands on the next floor down.
- [ ] **Step 2:** entcheck scene `jump`: a generated hilly world streamed headless around one focus (`Strata.Stream` with `Strata.Terrain` noise, as `demo` does); 500 walkers on routes 300 m away, unseen; the focus jumps to them; with `blocks` and `step` called each tick, within 30 ticks of the stream settling every one within seen distance is seen, grounded, and has `en_solid` 1 under its feet and 0 in its box; no tick's `step` exceeds 8 ms (printed; judged only under the load rule).
- [ ] **Step 3:** run; expect FAIL. Implement. Run; expect PASS.
- [ ] **Step 4:** Commit `feat(entities): seen and unseen, setting on the ground`.

### Task 7: Figures and voices; the arena

**Files:** `native/strata_anim.c`, `lib/strata/ffi/anim.march`, `lib/strata/anim.march`, `native/strata_ent.h`, `native/strata_ent.c`, `lib/strata/ffi/ent.march`, `lib/strata/entities.march`, `test/entities_test.march`, `entcheck/lib/entcheck.march`, `scripts/ent_check.sh`, `animcheck/lib/animcheck.march`, `scripts/anim_check.sh`, `RESULTS.md`

**Interfaces — Produces**
```
Strata.Anim.place_many(a : Anim, n : Int, figures : NativeI32Arr, poses : NativeF32Arr) : Anim   -- 7 floats a figure: x, y, z, then the turn x, y, z, w
Strata.Entities.with_figure(k : Kind, rig : Int) : Kind
Strata.Entities.with_sound(k : Kind, source : Int, group : Int, gain : Float) : Kind             -- up to two; a third is refused by valid
Strata.Entities.figure(e : Entities, id : Int) : Int      Strata.Entities.voice(e : Entities, id : Int, n : Int) : Int
Strata.Entities.tick(e : Entities, w : Strata.World.World, p : Strata.Physics.Physics, a : Strata.Anim.Anim, s : Strata.Sound.Sound,
                     focus : List((Float, Float, Float)), dt : Float) : (Entities, Strata.Physics.Physics, Strata.Anim.Anim, Strata.Sound.Sound)
```
```c
int en_seen_parts(const EnWorld *, int part, int max, int32_t *part_ids, float *poses /*7 each*/);   /* seen entities holding that part */
int en_wanting(const EnWorld *, int part, int max, int64_t *ids);   /* seen entities whose kind has the part and that lack it, nearest a focus first */
```
`tick` calls `step`, then: for each event 4 or 6 naming an entity with a figure or voices, `Strata.Anim.remove` and `Strata.Sound.stop` (fade 0.1) and clears the part; for each id of `en_wanting`, `Strata.Anim.spawn_at` at its position and facing (a turn about +y) and `Strata.Sound.play` per source, storing the ids; a figure answer of -1 leaves the part empty and counts the refusal for that kind (`stat` 10 + kind index); then one `place_many` for every seen figure and one `Strata.Sound.move` per live voice. A `remove` by the game takes its parts away at the next `tick` through its event 6. Physics is passed through untouched until Task 8.

- [ ] **Step 1: failing checks:** animcheck scene `place_many` (100 figures placed in one call have the same matrices as 100 `place` calls; a dead figure id in the array is skipped and counted). `test/entities_test.march`: `with_figure` and `with_sound` set only their fields; a third sound makes the kind invalid. entcheck scene `parts`: a kind with a one-part rig and one sound; 10 entities near the focus, 10 far: after one `tick` the near ones have `figure >= 0` and `voice(…, 0) >= 0` and `Strata.Anim.stats` counts 10 figures, the far ones have -1; moving the focus swaps them and the figure count is still 10; removing an entity frees its figure by the next tick; each figure's root matrix origin equals its entity's position after `Strata.Anim.update`. Scene `cap`: `Strata.Anim.open(8)`, 20 seen entities: 8 have figures, the 8 nearest the focus, the per-kind refusal stat is 12 after one tick, and when 4 with figures are removed 4 more get one on the next tick.
- [ ] **Step 2:** entcheck scene `arena`: a 120 m walled floor with pillars, 2,000 walkers of four kinds each with a rig of 6 parts and a looping walk clip, on looping routes, all seen. Checks: 2,000 figures; `hash` equal across two runs. Prints median and worst `tick` time over 600 ticks, and `Strata.Anim.update` time separately.
- [ ] **Step 3:** run; expect FAIL. Implement. Run; expect PASS.
- [ ] **Step 4:** record the arena against the 4 ms target in `RESULTS.md`. Commit `feat(entities): figures and voices follow their entities; the arena`.

### Task 8: The body mover

**Files:** `native/strata_ent.h`, `native/strata_ent.c`, `lib/strata/ffi/ent.march`, `lib/strata/entities.march`, `test/entities_test.march`, `entcheck/lib/entcheck.march`, `scripts/ent_check.sh`

**Interfaces — Produces**
```
Strata.Entities.with_driven_body(k : Kind, hx : Float, hy : Float, hz : Float) : Kind     -- sets the mover to bodied
Strata.Entities.with_dynamic_body(k : Kind, g : Strata.Body.Grid) : Kind                  -- sets the mover to bodied
Strata.Entities.body(e : Entities, id : Int) : Int
```
In `tick`, bounded by the body budget given at `open`: leaving view or removal calls `Strata.Physics.remove`; the nearest `en_wanting` bodied entities up to the budget get `Strata.Physics.add_driven` or `Strata.Physics.add` (centred on the entity, turned by its facing); the count over the budget goes to stat 7. Each tick, per entity with a body: if `Strata.Physics.exists` is false, clear the part and write event 7; a driven body is moved with `Strata.Physics.drive` by the entity's velocity and the entity's position set from the result; a dynamic body's `Strata.Physics.pose` at the latest step is copied to the entity's position. A bodied entity without a body (unseen, or over the budget) is treated as placed. The game still calls `Strata.Physics.begin` and `finish` itself.

- [ ] **Step 1: failing checks:** `test/entities_test.march`: both `with_` functions make the mover bodied; a body on a walking kind is invalid. entcheck scene `bodies`, using `Strata.Physics.step` on a small built world: a driven-body entity given velocity (2, 0, 0) for 1 s moves 2 m within 0.1 and its body's position equals the entity's; it is stopped by a wall; a dynamic-body entity dropped from 3 m lands and its entity position follows the body within 0.05 m; budget 4 with 10 seen bodied entities gives 4 bodies, the nearest, and stat 7 is 6; leaving view removes the body (`Strata.Physics.count` falls); `Strata.Physics.remove` behind the layer's back writes one event 7 and `body` reads -1.
- [ ] **Step 2:** run; expect FAIL. Implement. Run; expect PASS.
- [ ] **Step 3:** Commit `feat(entities): the body mover, within a body budget`.

### Task 9: Snapshot and restore; the city; results

**Files:** `native/strata_ent.h`, `native/strata_ent.c`, `native/tests/ent_test.c`, `lib/strata/ffi/ent.march`, `lib/strata/entities.march`, `test/entities_test.march`, `entcheck/lib/entcheck.march`, `scripts/ent_check.sh`, `RESULTS.md`, `README.md`

**Interfaces — Produces**
```
type Snapshot = Snapshot(NativeI32Arr, NativeF32Arr, NativeFloatArr)    -- ints, floats, doubles; layout in the ledger
Strata.Entities.snapshot(e : Entities) : Snapshot
Strata.Entities.restore(e : Entities, s : Snapshot) : (Entities, Bool)  -- false, and nothing changed, if the snapshot does not fit
```
```
Strata.Entities.restore_with(e : Entities, s : Snapshot, p : Strata.Physics.Physics, a : Strata.Anim.Anim, snd : Strata.Sound.Sound)
    : (Entities, Bool, Strata.Physics.Physics, Strata.Anim.Anim, Strata.Sound.Sound)
```
```c
int en_snapshot_sizes(const EnWorld *, int sizes[3]);                                   /* ints, floats, doubles needed */
int en_snapshot(const EnWorld *, int32_t *ints, float *floats, double *doubles);
int en_restore(EnWorld *, int ni, const int32_t *ints, int nf, const float *floats, int nd, const double *doubles);   /* 1, or 0 and nothing changed */
```
The ints begin with a version number, the capacity, the kind count and each kind's slot counts. A restore needs an open world with the same capacity and kinds whose slot counts match; it replaces every row, generation, free row and route, leaves every entity unseen with no parts, and does not touch the masks. `restore` answers false, changing nothing, while any entity holds a figure, body or voice. `restore_with` first removes every held part (as leaving view does), then restores.

- [ ] **Step 1: failing checks:** C: snapshot, restore into a fresh world, snapshot again: the arrays are byte-equal; ids handed out before still resolve after; a snapshot from capacity 1,024 into capacity 2,048, a wrong version, a kind with different slot counts, or a truncated array is refused and the world's hash is unchanged. `test/entities_test.march` property with `Check.all`: for a random script of adds, removes, sets, routes and `step` calls with no focus, snapshot at a random tick, restore into a second run, and both runs' `hash` are equal for the next 60 ticks. entcheck scene `restore`: an entity with a figure; `restore` answers false; `restore_with` succeeds, the figure count is 0, and one `tick` later the near entities have figures again.
- [ ] **Step 2:** entcheck scene `city`: the island (`Strata.Atlas`, as the viewer builds it, cached under `.march/atlas`) streamed headless around one focus; 10,000 walkers, unseen rule live with divisor 4, on 200 looping routes laid over the island's land (waypoints at land height from the atlas); run 600 ticks, print median and worst `tick` time; then jump the focus to the densest district, run until the stream settles plus 30 ticks, and check every walker within seen distance is seen and grounded with solid under it and its box clear. `hash` equal across two runs.
- [ ] **Step 3:** run; expect FAIL. Implement. Run; expect PASS.
- [ ] **Step 4:** `RESULTS.md`: the five rows of the design's budget table with measured figures, each marked met, missed or not judged. `README.md`: the Entities section gains the call table in the README's style and one paragraph on seen and unseen. Commit `feat(entities): snapshot and restore; the city; results`.

### Task 10: See it in the viewer (gated; see Global Constraints)

**Files:** `viewer/lib/strata_viewer.march`, `viewer/forge.toml` (add `../native/strata_ent.c`; `../native/strata_anim.c` if the animation work has not), `scripts/golden.sh`, `test/golden/`, `README.md`

- [ ] **Step 1:** merge `main`; read how the animation work draws figures (`Strata.Render` on `main`) and record the calls used in the ledger.
- [ ] **Step 2:** three scenes on the next free `STRATA_SCENE` numbers, each built from its entcheck scene's setup: the arena; the city, with key `J` jumping the camera to the far district; the storm, with the nearest 4,000 bullets given a one-voxel figure (kind seen distance set so about that many are seen). Each prints its `tick` cost once a second with `STRATA_STATS=1`.
- [ ] **Step 3:** scripted runs (`STRATA_HIDDEN=1 STRATA_OFFSCREEN=1 STRATA_PIXELS=1 STRATA_AUDIO=0`, fixed `STRATA_FRAMES`) of the arena and the city after its jump; look at each image, then store it as a golden. Existing goldens pass unchanged.
- [ ] **Step 4:** `scripts/verify.sh` prints PASS. Commit `feat(viewer): the arena, the city and the storm`.

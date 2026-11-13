# Particles Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A Strata particle effect system (effects as data, emitters, particles drawn on the GPU in closed form), used by the viewer's match for fireball trails and bursts and for burning that takes seconds.

**Architecture:** The GPU draws particles from a 64-byte record written once at birth and a table of up to 64 effects (closed-form motion, colour and size ramps, glowing or shaded). The shim (`native/strata_vk.c`, `native/strata_particles.h`) owns the ring, the table and a record writer that spawns a burst from a shape. `Strata.Particles` is pure March: effects, emitters, accumulators and a queue of spawn requests; `Strata.Render.particles` hands the queue to the shim. The match defines the effects, attaches emitters to fireballs and to burning cells, and schedules burning in pure `StrataViewer.Combat`.

**Tech Stack:** March main (`scripts/env.sh`, which sets `MARCH_HOME` so forge uses the March on PATH), C11 + Vulkan + GLSL (`native/build_shaders.sh`, glslang), forge test, `scripts/verify.sh`, `scripts/golden.sh`, `scripts/combat_check.sh`.

**Spec:** `docs/2026-11-12-particles-design.md`. Read it first; its numbers are the numbers.

## Global Constraints

- Build and test with March main only: `. scripts/env.sh` first in every shell (it exports `MARCH_HOME`); never a bare `forge` without it. Check `march --version` says 0.7.0 or later once a session.
- Integer hashes multiply with `int_wrapping_mul` where a product can pass 2^62 (March main traps on overflow).
- `spray` keeps its signature and its pictures: the goldens `lake-breached` and `lake-draining` pass unchanged, unblessed.
- No per-particle work on the CPU after birth; no per-frame upload of the ring.
- Deciding what to emit lives in March; the shim only writes records from a description. A slow March path is recorded in `GAPS.md` with numbers, not moved to C.
- Same calls, same particles: all randomness is a hash of a counter (`Strata.Particles`' own, and the record writer's seed); no clock, no `Random`.
- The ring holds 65 536 particles; a burst is at most 512 (`sp_clamp`); at most 64 effects; effect 0 is the built-in spray effect and cannot be redefined.
- Descriptor bindings 0 to 33 are taken; the effect table is binding 34.
- Viewer block ids: 1 stone, 2 dirt, 3 grass, 4 sand, 5 water, 6 snow, 7 wood, 8 leaves, 10 glass, 11 tall grass, 12 lantern post, 13 ice, 14-21 lake water. The two embers blocks are registered after the fluid: 22 burning wood, 23 burning leaves. Texture layers: 0-13 as in `viewer/lib/textures.march`; new 14 flame, 15 burning wood, 16 burning leaves. Confirm against `viewer/lib/strata_viewer.march` `registry()` before relying on them.
- Without `STRATA_COMBAT=1` the viewer changes only in that spray goes through the new path (pictures unchanged).
- `viewer/lib/strata_viewer.march` is shared: add calls and threaded values, keep logic in `viewer/lib/fx.march`, `match.march`, `combat.march`.
- Scripted runs are hidden: `MARCH_PIN_MAIN=1 STRATA_HIDDEN=1 STRATA_OFFSCREEN=1 STRATA_PIXELS=1 STRATA_AUDIO=0`. Never open a window.
- `forge lint --strict` clean, no warnings or hints from a build.
- March gaps that bite (GAPS.md): S26 `&&`/`||` evaluate both sides (nest `if`s); S27 type names unique across modules (prefix `Pt` in particles, `Fx` in the viewer's effects); S32 `by` is reserved, floats with exponents do not parse; S33 qualified constructors cannot be matched (export accessors).
- Commit on `claude/particle-effect-system-599ddd`, no attribution lines, never bare `git stash`. Gate: `scripts/verify.sh` prints `PASS`. After each reviewed task, merge into main through a temporary worktree (`git worktree add <scratch> main`, `git merge --no-ff`, gate, `git worktree remove`).

## Review Focus

Each line has its test in the task named.

1. A frame with dt 0 or a stopped emitter: nothing spawns, its carried fraction is kept (Task 3: `step_with_no_time_spawns_nothing`, `a_stopped_emitter_spawns_nothing`).
2. A moving emitter that jumps far in one frame (a fireball respawned, a teleport): it does not paint a trail across the world; past 4 m a frame it spawns at its new place only (Task 3: `a_jump_past_four_metres_draws_no_trail`).
3. More emission asked in a frame than a burst holds (a rate of 100 000 a second): spawned as several bursts of 512, never more than 4096 a frame per emitter, the rest dropped, not carried (Task 3: `a_huge_rate_is_capped_a_frame`).
4. A cell scheduled to burn is dug, built over or burned again before its end: it is not restored to what fire leaves, and not scheduled twice (Task 5: `a_cell_changed_while_burning_is_left_alone`, `a_cell_already_burning_is_not_scheduled_again`).
5. A match restarted while things burn and fireballs fly: every emitter stops, the schedule is emptied, embers left in the world are set to what fire leaves at once (Task 6: combat scene `ten matches` asserts no live emitters and no embers after each rematch).

---

### Task 1: The record, the effect table, the shaders; spray as effect 0

**Files:**
- Create: `native/strata_particles.h` (replaces `native/strata_spray.h`: keep `SprayRing` and its functions there, renamed nothing; add the record and effect layouts and the closed form in C)
- Delete: `native/strata_spray.h` (move its contents; update the `#include` in `strata_vk.c`)
- Modify: `native/strata_vk.c` (record size 16 floats; effect table uniform buffer at binding 34; two particle pipelines; `sv_spray` writes the new record)
- Modify: `native/shaders/spray.vert`, `native/shaders/spray.frag` (rename to `particle.vert`/`particle.frag`; update `native/build_shaders.sh`), regenerate `native/strata_shaders.h`
- Test: `native/tests/spray_test.c` → `native/tests/particles_test.c`

**Interfaces — Produces**
```c
#define PT_FLOATS 16          /* a record */
#define PT_EFFECTS 64
#define PT_EFFECT_FLOATS 40   /* an effect in the table, std140 */
/* record: [0..2] start (m) [3] birth (s) [4..6] velocity (m/s) [7] life (s)
 * [8] effect [9] size scale [10] seed 0..1 [11] rotation (rad) [12] layer override (-1 none) [13..15] 0 */
/* effect (10 vec4s, std140):
 * [0..3]   look (0 shaded, 1 glowing), layer, lit (0 per pixel, 1 per particle), colour keys used (1..4)
 * [4..19]  four colour keys (r, g, b, opacity)    [20..23] their ages (0..1)
 * [24..27] four sizes (m)                         [28..31] their ages (0..1)
 * [32..35] size keys used (1..4), drag k, wobble metres, wobble Hz
 * [36..39] ax, ay, az (m/s2), spin (rad/s)  */
static inline void pt_position(const float *rec, const float *effect, float t, float out[3]);  /* closed form, as the shader */
static inline void pt_effect_spray(float *effect);   /* effect 0: shaded, layer from the record, ballistic, gravity -9.8, no drag, opacity 1 fading over the last third, size 1 (the record's size scale is spray's 0.16 +- 0.07), lit per pixel */
```
- `int64_t sv_effect_set(int64_t index, void *floats40)` — copies one effect into the table (refuses index 0 and index >= 64; answers 1 or 0).
- `pt_position` with drag `k > 0`: `p0 + (v0 - a/k)(1 - e^{-kt})/k + (a/k) t`; with `k == 0`: `p0 + v0 t + a t²/2`; plus wobble `w * sin(2π f t + seed·2π)` on x and `w * sin(2π f t + seed·2π + 1.7)` on z.

- [ ] **Step 1: Write the failing C test** `native/tests/particles_test.c`: keep every check of `spray_test.c`; add
  - `pt_position` of effect 0 at t = 0.5 for start (1, 2, 3), velocity (0, 7, 0) equals (1, 2 + 3.5 - 1.225, 3) within 1e-5 (gravity 9.8, so `-4.9 t²` exactly as spray.vert has it);
  - with drag k = 2, accel (0, 3, 0), v0 = 0: at t = 10 the vertical speed (finite difference) is 1.5 within 1e-3 (a/k);
  - with k = 2 and t = 0 the position is the start;
  - wobble 0.1 m at 1 Hz, seed 0.25: x offset at t = 0 is 0.1 within 1e-6.
- [ ] **Step 2: Run** `cc -O1 -Wall -Wextra -Inative -o native/tests/bin/particles_test native/tests/particles_test.c -lm && native/tests/bin/particles_test` — Expected: fails to compile (no `strata_particles.h`).
- [ ] **Step 3: Implement** `native/strata_particles.h` (ring moved in, record and effect layouts, `pt_position`, `pt_effect_spray`), delete `strata_spray.h`, delete `spray_test.c`.
- [ ] **Step 4: Run** the test — Expected: `particles_test: ok`.
- [ ] **Step 5: Implement the renderer side.** In `strata_vk.c`: record of `PT_FLOATS` (vertex attributes four vec4); the effect table as a host-visible uniform buffer of `PT_EFFECTS * PT_EFFECT_FLOATS` floats at binding 34 (vertex and fragment stages), effect 0 written at open with `pt_effect_spray`; `sv_effect_set`; pipeline kind 6 becomes two pipelines, shaded (alpha blend, as now) and glowing (`src ONE, dst ONE`), both depth-tested, no depth write; the live span drawn by shaded then glowing, each vertex shader sending the other look's particles off screen. `sv_spray` writes the new record: the same jitter and hash calls as today, effect 0, size scale `0.16 + 0.07 h`, seed from `spray_hash(id, 9)`, layer override = `tex`. In the shaders: the closed form (as `pt_position`), ramps by age, layer = override if >= 0 else the effect's; shaded keeps today's fragment lighting when lit per pixel, and traces once in the vertex shader (passing sun and sky factors as flat varyings) when lit per particle; glowing outputs `colour * opacity`, multiplied by `1 - fog` and alpha 1 (so it adds). Add the next free stat index (after the last `which ==` in `sv_stat`; 20 at the time of writing): particles overwritten while still alive since the start (count a push whose slot's particle is younger than its own life). Add the extern in `lib/strata/ffi/vk.march`: `fn effect_set(index : Int, floats : NativeF32Arr) : Bool`.
- [ ] **Step 6: Run the goldens**: `. scripts/env.sh && (cd viewer && forge build --release) && STRATA_HIDDEN=1 STRATA_OFFSCREEN=1 scripts/golden.sh` — Expected: `golden: PASS` with no `--update`; in particular `lake-breached` and `lake-draining` pass. If a scene's stored draw count now differs only because live spray adds one draw (the glowing pass), update that count in `scripts/golden.sh` and say so in the commit.
- [ ] **Step 7: Update** `scripts/verify.sh` if it names `spray_test` (it globs `native/tests/*_test.c`; check). Run `scripts/verify.sh` — Expected: `PASS`.
- [ ] **Step 8: Commit** `feat(particles): a 64-byte record, an effect table at binding 34, shaded and glowing passes; spray is effect 0, its pictures unchanged`.

### Task 2: Spawning a burst from a shape, in the shim

**Files:**
- Modify: `native/strata_particles.h` (the pure writer), `native/strata_vk.c` (`sv_emit`), `lib/strata/ffi/vk.march`
- Test: `native/tests/particles_test.c`

**Interfaces — Consumes:** Task 1's record and ring. **Produces**
```c
/* Shape: 0 point, 1 sphere (radius a), 2 box (half sizes a, b, c). The start of particle i is spread
 * evenly along the segment (x0,y0,z0)->(x1,y1,z1) by (i + 0.5) / n, then offset inside the shape.
 * Velocity: (vx, vy, vz) plus a uniform offset in a ball of radius [spread].
 * Life: life * (1 + life_spread * u), u in -1..1; size scale 1 + size_spread * u; rotation u * pi.
 * All u from pt_hash(seed, i, salt). */
static inline float pt_hash(uint32_t seed, uint32_t i, uint32_t salt);           /* -1..1 */
static inline void pt_write(float *rec, int effect, int shape, const float seg[6], const float abc[3],
                            const float vel[3], float spread, float life, float life_spread, float size_spread,
                            uint32_t seed, uint32_t i, uint32_t n, float birth);
```
- `int64_t sv_emit(int64_t effect, int64_t shape, double x0, double y0, double z0, double x1, double y1, double z1, double a, double b, double c, double vx, double vy, double vz, double spread, int64_t count, double life, double life_spread, double size_spread, int64_t seed)` — clamps count with `sp_clamp`, writes records born at the renderer's time, answers how many. Refuses an effect index that was never set (answers 0).
- March: `fn emit(effect : Int, shape : Int, x0 : Float, y0 : Float, z0 : Float, x1 : Float, y1 : Float, z1 : Float, a : Float, b : Float, c : Float, vx : Float, vy : Float, vz : Float, spread : Float, count : Int, life : Float, life_spread : Float, size_spread : Float, seed : Int) : Int` in `Strata.Ffi.Vk`. If March cannot take twenty parameters to an extern, pass the floats in one `NativeF32Arr` of 18 and keep the same order; say which in the commit.

- [ ] **Step 1: Write failing C tests**: a sphere of radius 0.5 at a point: all 512 starts within 0.5 of it; a box (1, 0.1, 2): every start inside it; a segment (0,0,0)->(10,0,0) with a point shape and n = 10: starts at x = 0.5, 1.5, ... 9.5; the same seed twice gives identical records, another seed different ones; velocities within `spread` of the mean; lives within `life * (1 ± life_spread)`.
- [ ] **Step 2: Run** — Expected: compile failure (no `pt_write`).
- [ ] **Step 3: Implement** `pt_hash` (the `spray_hash` mixing with a seed), `pt_write`, `sv_emit`, the March extern and wrapper.
- [ ] **Step 4: Run** the C test — Expected: `particles_test: ok`. Run `scripts/verify.sh` — Expected: `PASS`.
- [ ] **Step 5: Commit** `feat(particles): bursts from a point, a sphere or a box along a segment, written by the shim from a seed`.

### Task 3: `Strata.Particles`

**Files:**
- Create: `lib/strata/particles.march` (`mod Strata.Particles`)
- Modify: `lib/strata/render.march` (`particles`), `README.md` (a Particles section with the example from the spec)
- Test: `test/particles_test.march` (copy the `needs` block of `test/emitters_test.march`)

**Interfaces — Consumes:** `Strata.Ffi.Vk.effect_set`, `Strata.Ffi.Vk.emit` (Tasks 1-2). **Produces**
```
type PtEffect          -- opaque; built from effect() and setters
type PtShape = PtPoint(Float, Float, Float) | PtSphere(Float, Float, Float, Float) | PtBox(Float, Float, Float, Float, Float, Float)
type PtSpawn           -- one queued burst; accessors below
type Particles         -- effects defined, emitters, the counter, the queue

fn effect() : PtEffect                                   -- shaded, layer 13, white opacity 1, size 0.2, no accel, no drag, lit per particle
fn glowing(e : PtEffect) : PtEffect
fn shaded(e : PtEffect) : PtEffect
fn lit_per_pixel(e : PtEffect) : PtEffect
fn layer(e : PtEffect, l : Int) : PtEffect
fn colour(e : PtEffect, keys : List((Float, Float, Float, Float, Float))) : PtEffect   -- (age, r, g, b, opacity); first four kept, sorted by age
fn size(e : PtEffect, keys : List((Float, Float))) : PtEffect                           -- (age, metres); first four kept
fn accel(e : PtEffect, ax : Float, ay : Float, az : Float) : PtEffect
fn drag(e : PtEffect, k : Float) : PtEffect
fn wobble(e : PtEffect, metres : Float, hz : Float) : PtEffect
fn spin(e : PtEffect, rad_per_s : Float) : PtEffect
fn floats(e : PtEffect) : List(Float)                    -- the 40 table floats, the layout of Task 1

fn new(seed : Int) : Particles
fn define(p : Particles, e : PtEffect) : (Particles, Int)            -- 1..63; -1 when full
fn point(x : Float, y : Float, z : Float) : PtShape
fn sphere(x : Float, y : Float, z : Float, r : Float) : PtShape
fn box(x : Float, y : Float, z : Float, hx : Float, hy : Float, hz : Float) : PtShape
fn burst(p : Particles, effect : Int, s : PtShape, v : (Float, Float, Float), spread : Float, count : Int, life : Float, life_spread : Float) : Particles
fn emitter(p : Particles, effect : Int, s : PtShape, v : (Float, Float, Float), spread : Float, rate : Float, life : Float, life_spread : Float) : (Particles, Int)
fn move(p : Particles, id : Int, s : PtShape) : Particles
fn set_rate(p : Particles, id : Int, rate : Float) : Particles
fn stop(p : Particles, id : Int) : Particles
fn step(p : Particles, dt : Float) : Particles            -- queues what each live emitter owes
fn live_emitters(p : Particles) : Int
fn queued(p : Particles) : List(PtSpawn)                  -- oldest first
fn drain(p : Particles) : Particles                       -- the queue emptied, the new effects marked sent
fn pending_effects(p : Particles) : List((Int, List(Float)))   -- defined and not yet sent
fn sp_effect(s : PtSpawn) : Int
fn sp_count(s : PtSpawn) : Int
fn sp_from(s : PtSpawn) : (Float, Float, Float)
fn sp_to(s : PtSpawn) : (Float, Float, Float)
fn sp_seed(s : PtSpawn) : Int
```
- In `Strata.Render`: `fn particles(p : Strata.Particles.Particles) : Strata.Particles.Particles` — sends `pending_effects` with `effect_set`, each `queued` spawn with `emit`, answers `drain(p)`.
- Rules: `step` gives an emitter `rate * dt + carried`, spawns the whole part as bursts of at most 512, at most 4096 a frame per emitter (the excess is dropped, the carried fraction set to the fractional part only). A burst's segment is from the emitter's last position to its current one; past 4 m apart, from the current position only. A burst's seed is `Strata.Seed.mix(p_seed, counter)`, the counter +1 each burst. `burst` queues at once with a segment of zero length. Size spread is 0.25 and life spread as given; spread of a burst is a ball.

- [ ] **Step 1: Write the failing tests** in `test/particles_test.march`, `describe "Strata.Particles"`:
  - `an emitter at 120 a second for 1/60 s queues 2` (sum of `sp_count` over `queued`);
  - `fractions carry: 50 a second for ten frames of 1/60 s queues 8, then 9 after the eleventh` (8.33 → 8, 9.17 → 9);
  - `step_with_no_time_spawns_nothing` (dt 0: empty queue, carry kept: then 1/60 s at 60 a second queues 1);
  - `a_stopped_emitter_spawns_nothing`, and `live_emitters` drops by one;
  - `a moving emitter spawns along its path`: moved from (0,0,0) to (1,0,0), `sp_from` (0,0,0), `sp_to` (1,0,0);
  - `a_jump_past_four_metres_draws_no_trail`: moved 10 m, `sp_from == sp_to ==` the new place;
  - `a_huge_rate_is_capped_a_frame`: 100 000 a second for 0.1 s queues 4096 in bursts of at most 512, and the next frame of 1/60 s queues at most 4096 again (no backlog);
  - `the same calls give the same seeds` (two `new(7)` runs, equal `sp_seed` lists) and `another seed gives others`;
  - `define refuses the 64th effect` (63 defined answer 1..63, the next -1);
  - `floats is 40 long and holds the look, layer and keys` (glowing → float 0 is 1.0; layer 14 → float 1 is 14.0; a colour key list of five keeps four);
  - `drain empties the queue and the pending effects`.
- [ ] **Step 2: Run** `. scripts/env.sh && forge test` — Expected: fails (no module `Strata.Particles`).
- [ ] **Step 3: Implement** `lib/strata/particles.march` and `Strata.Render.particles`.
- [ ] **Step 4: Run** `forge test` — Expected: all pass. `forge lint --strict` clean.
- [ ] **Step 5: README**: a Particles section (effects as data, the example of the spec, the caps, that particles meet nothing).
- [ ] **Step 6: Commit** `feat(particles): Strata.Particles, effects as data, emitters that carry their fractions and trail along their path; Render.particles`.

### Task 4: The viewer's effects, textures and embers blocks

**Files:**
- Create: `viewer/lib/fx.march` (`mod StrataViewer.Fx`)
- Modify: `viewer/lib/textures.march` (count 17; layers 14 flame — a soft teardrop, bright core, alpha to 0 at the edge; 15 burning wood — charred bark, glowing orange cracks; 16 burning leaves — leaves' holes kept, leaf pixels dark red to orange), `viewer/lib/strata_viewer.march` (`registry()` registers 22 burning wood: opaque, faces 15, emission (255, 120, 40, 160); 23 burning leaves: as leaves, faces 16, the same emission; a `Strata.Particles.Particles` threaded through `frame_loop`; `Strata.Render.particles` called once a frame after the match's frame; `STRATA_FX=1` scene below)
- Test: `viewer/test/fx_test.march`

**Interfaces — Consumes:** Task 3. **Produces**
```
type FxIds = { fx_flame : Int, fx_ember : Int, fx_smoke : Int, fx_spark : Int, fx_steam : Int }
fn define_all(p : Strata.Particles.Particles) : (Strata.Particles.Particles, FxIds)     -- once a run
fn burning_wood() : Int      -- 22
fn burning_leaves() : Int    -- 23
fn embers_for(block : Int) : Int   -- 7 and 3 -> 22, 8 -> 23, else -1 (tall grass 11 and ice 13 are not swapped)
```
- The five effects, starting values (tune by looking, record final values in the ledger): flame glowing, layer 14, colour (0, 1.0, 0.9, 0.5, 0.0) (0.15, 1.0, 0.6, 0.15, 0.9) (1.0, 0.6, 0.1, 0.0, 0.0), size (0, 0.25) (1, 0.05), accel (0, 3, 0), drag 2, wobble 0.08 m at 1.5 Hz; ember glowing, layer 13, orange (1.0, 0.5, 0.1) opacity 1 → 0, size 0.05, accel (0, -2, 0), drag 0.5; smoke shaded, layer 13, grey (0.25, 0.25, 0.25) opacity 0 → 0.5 at 0.2 → 0, size 0.3 → 1.2, accel (0, 0.6, 0), drag 0.8, wobble 0.2 m at 0.3 Hz; spark glowing, layer 13, (1.0, 0.9, 0.6) → (1.0, 0.4, 0.1), size 0.06 → 0.02, accel (0, -9.8, 0), drag 0.3; steam shaded, layer 13, white opacity 0 → 0.6 → 0, size 0.2 → 0.9, accel (0, 1.0, 0), drag 1.0.
- `STRATA_FX=1` (scripted, without a match): at frame 10 one of each effect is emitted 3 m ahead of the camera, side by side 1 m apart (a burst of 40 each, flame and smoke as emitters at 120 and 20 a second for the rest of the run); prints `fx: 5 effects defined`.

- [ ] **Step 1: Write failing tests** in `viewer/test/fx_test.march`: `define_all defines five distinct ids from 1`; `embers_for` cases (7 → 22, 3 → 22, 8 → 23, 11 → -1, 13 → -1, 1 → -1); `the registry holds 22 and 23 with emission` (`Strata.Blocks.emission(StrataViewer.registry(), 22) > 0`, same for 23; 23 cutout like 8: `Strata.Blocks.layer_index` equal).
- [ ] **Step 2: Run** `(cd viewer && forge test)` — Expected: fails.
- [ ] **Step 3: Implement** `fx.march`, the textures, the blocks, threading `Particles` and the `STRATA_FX` scene.
- [ ] **Step 4: Run** `(cd viewer && forge test)` — Expected: pass. Add a golden `fx-showcase:STRATA_FX=1 STRATA_SCENE=<the meadow scene used by render-meadow> STRATA_FRAMES=60 SAYS=fx:_5_effects_defined` to `scripts/golden.sh`; run it, look at `test/golden/fx-showcase.actual.ppm` (flames bright and rising, smoke grey and lit, sparks falling), tune, store it.
- [ ] **Step 5: Run** `scripts/verify.sh` — Expected: `PASS`.
- [ ] **Step 6: Commit** `feat(viewer): five effects, a flame texture, burning wood and burning leaves blocks; the STRATA_FX showcase`.

### Task 5: The burn schedule, pure

**Files:**
- Modify: `viewer/lib/combat.march`
- Test: `viewer/test/combat_test.march`

**Interfaces — Consumes:** `Combat.fire_changes`, `Combat.burns` (existing), `StrataViewer.Fx.embers_for` (Task 4). **Produces**
```
type CbBurn = { bn_x : Int, bn_y : Int, bn_z : Int, bn_was : Int, bn_left : Int, bn_catch : Float, bn_end : Float, bn_lit : Bool }
type CbFire = { cf_burns : List(CbBurn), cf_clock : Float }
type CbFireEvent = CbCatch(CbBurn) | CbOut(CbBurn) | CbNow(CbBurn)     -- export accessors: ev_kind (0 catch, 1 out, 2 now), ev_burn
fn fire_none() : CbFire
fn burn_most() : Int                                                     -- 256
fn burn_seconds(block : Int, h : Float) : Float                          -- h in 0..1: 11 -> 0.5 + 0.5h, 3 -> 1 + h, 8 -> 2 + h, 7 -> 4 + 2h, 13 -> 0.5 + 0.5h
fn catch_delay(metres : Float) : Float                                   -- 0.25 * metres (0.5 s at 2 m)
fn schedule(f : CbFire, read : Int -> Int -> Int -> Int, x : Float, y : Float, z : Float, seed : Int) : (CbFire, List(CbFireEvent))
fn fire_step(f : CbFire, read : Int -> Int -> Int -> Int, dt : Float) : (CbFire, List(CbFireEvent))
fn burning(f : CbFire) : Int
```
- `schedule` takes `fire_changes` at (x, y, z); skips cells already in the schedule; cells past `burn_most` come back as `CbNow` (apply at once, as before); the jitter `h` is a hash of the cell and `seed` (`Strata.Seed.cell` with salt 33). `fire_step` advances the clock: a cell whose `bn_catch` is reached and not lit comes back `CbCatch` and is marked lit; a lit cell whose `bn_end` is reached comes back `CbOut` and leaves the schedule — unless `read` shows it no longer holds `embers_for(bn_was)` (or, for 11 and 13, `bn_was`), in which case it leaves silently. A cell that is unlit and was changed meanwhile (read differs from `bn_was`) leaves silently too.

- [ ] **Step 1: Write failing tests** (`describe "the burn schedule"`): `burn_seconds` bounds for each block at h 0 and 1; `catch_delay(2.0) == 0.5`; `the centre cell catches on the first step` (dt 1/60); `a wood cell 1 m out catches after 0.25 s and goes out between 4.25 and 6.25 s`; `a_cell_already_burning_is_not_scheduled_again` (scheduling the same point twice keeps `burning` the same); `a_cell_changed_while_burning_is_left_alone` (after its catch, `read` answers 0 for it: no `CbOut` for it); `past 256 cells the rest burn now` (a read that answers wood everywhere within 2 m of each of 10 points: `burning == 256`, the others `CbNow`); `the same seed gives the same times`.
- [ ] **Step 2: Run** `(cd viewer && forge test)` — Expected: fails.
- [ ] **Step 3: Implement**.
- [ ] **Step 4: Run** — Expected: pass; `forge lint --strict` clean.
- [ ] **Step 5: Commit** `feat(combat): fire takes seconds: each cell catches by its distance and burns by its block, pure`.

### Task 6: The match: fireballs and fire

**Files:**
- Modify: `viewer/lib/match.march`, `viewer/lib/strata_viewer.march` (the match's frame takes and gives back the `Particles` and the `FxIds`), `scripts/combat_check.sh`, `scripts/golden.sh`
- Test: `scripts/combat_check.sh` scenes; goldens `combat-fire` (re-stored after looking), new `combat-flight`

**Interfaces — Consumes:** Tasks 3-5. **Produces:** `MtShot` gains `ms_fx : (Int, Int, Int)` (flame, smoke, ember emitter ids, -1 none); `MtMatch` gains `mt_fire : CbFire` and `mt_cells : List((Int, Int, Int, Int, Int))` (cell x, y, z, flame emitter, smoke emitter); `Match.frame` (or `match_frame`'s callee) takes `Strata.Particles.Particles` and `StrataViewer.Fx.FxIds` and answers the new `Particles`.

- In flight: on throw, three emitters at the hand (flame 200/s, smoke 30/s, ember 20/s; v (0, 0.3, 0), spread 0.4, flame life 0.5 ± 0.3, smoke 2.5 ± 0.3, ember 1.0 ± 0.4); moved to the entity each frame; stopped when the shot leaves `mt_shots` for any reason.
- Burst (`burst`): sparks 80 (sphere 0.3, v (0, 2, 0), spread 5, life 0.45 ± 0.3), flame 60 (sphere 0.5, v (0, 1, 0), spread 1.5, 0.6 ± 0.3), smoke 20 (sphere 0.5, v (0, 0.5, 0), spread 0.5, 3 ± 0.3). Over water: steam 40 only (the existing water test). Then `Combat.schedule` instead of applying `fire_changes`; the damage stays.
- Each frame: `Combat.fire_step`; `CbCatch` → set the cell to `embers_for(was)` where that is not -1 (as `mt_changes`), start a flame emitter (box 0.45 about the cell centre; 40/s for wood and leaves, 15/s for grass and tall grass; none for ice) and a smoke emitter at the cell top (8/s; for ice a steam burst of 15 and no emitters), tally the cell as burned; `CbOut` → set to `bn_left`, stop the cell's emitters, a smoke burst of 6; `CbNow` → set to `bn_left` and a smoke burst of 6, tallied.
- Restart (`again`, the ten-matches path): stop every shot and cell emitter, set each lit cell to `bn_left`, `fire_none()`.
- `report` adds `combat: fire burning <n> cells, <e> emitters live`; the ten-matches scene asserts `0 cells, 0 emitters` after each rematch.

- [ ] **Step 1: Update** `scripts/combat_check.sh`'s fire scene: run long enough for the trunk to finish (frames +600; measure), keep its asserts (wood ≥ 1, grass ≥ 1, bystander 10 to 35), and add: at a frame 0.3 s after the burst, `burning > 0` (the scene prints `combat: fire burning <n> cells` every 60 frames while any burn); the ten-matches assert above. Run — Expected: FAIL (no such lines).
- [ ] **Step 2: Implement** the match changes.
- [ ] **Step 3: Run** `scripts/combat_check.sh --build` — Expected: `combat: PASS`, each scene equal in its two runs.
- [ ] **Step 4: Goldens.** Add `combat-flight` (the fire scene shot while the fireball is mid-arc; find its frame from the throw at frame 30 and its 18 m/s over 10 m). Run `scripts/golden.sh`; `combat-fire` will differ (embers and flames where there were holes): look at the actual images (a lit trunk, flames, smoke, the ground lit orange about it; mid-arc a trail behind the ball), then store both with `--update` for those two only, and say so in the commit.
- [ ] **Step 5: Run** `scripts/verify.sh` — Expected: `PASS`.
- [ ] **Step 6: Commit** `feat(combat): fireballs trail flame and smoke and burst in sparks; what they burn catches and burns for seconds, glowing, before it is gone`.

### Task 7: Cost, docs, merge

**Files:**
- Modify: `RESULTS.md`, `README.md`, `GAPS.md` (only for gaps met)
- Create: `docs/particles-ledger.md`

- [ ] **Step 1: Measure**, hidden, 1280 x 800, light mode 4, two runs each, reporting the load average: (a) `STRATA_FOUNTAIN` or the `STRATA_FX` scene at 20 000 live particles: GPU ms of the two particle draws (stat 3 with and without), CPU of `Strata.Particles.step` + `Render.particles` a frame; (b) the combat fire scene while a trunk burns: live particles, emitters, March + shim ms, GPU ms; (c) that stat over the full bots-only match (overwritten while alive; should be 0). Above a load of 8, report and do not judge.
- [ ] **Step 2: Write** RESULTS (a Particles section: what is checked, the cost table, misses), README (link the design; `STRATA_FX`), the ledger (decisions, tuned values, review findings, checkpoints).
- [ ] **Step 3: Run** `scripts/verify.sh` — Expected: `PASS`.
- [ ] **Step 4: Commit** `docs(particles): measured, README, RESULTS and ledger`, then merge into main through a temporary worktree and run the gate there.

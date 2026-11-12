# Combat (free-for-all to 30) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A free-for-all match in the viewer, behind `STRATA_COMBAT=1`: the player with arrows and fireballs against eleven computer fighters who also fight each other, first to 30 kills.

**Architecture:** The rules (health, damage, credit, the table, what a fighter intends, what fire burns) are pure March in `StrataViewer.Combat`, unit tested. `StrataViewer.Match` is the glue: fighters and projectiles are `Strata.Entities`, drawn as `Strata.Anim` figures the match places each frame, on terrain read and edited through the stream. `strata_viewer.march` gains only calls into `Match`.

**Tech Stack:** March (toolchain via `scripts/env.sh`), `Strata.Entities`, `Strata.Anim`, `Strata.Fluid`, `Strata.Edit`, `Strata.Query`, `Strata.Ui`, `Strata.Text`; one small C and shader change (the figure tint).

**Spec:** `docs/2026-11-12-combat-design.md`. Read it first; its tables are the numbers.

## Global Constraints

- Without `STRATA_COMBAT=1` nothing changes: every stored picture, every key, `StrataViewer.Hud.slot_block`. All combat code paths start from one `Match.on(m)` test.
- `viewer/lib/strata_viewer.march` is shared by six sessions: add calls and one `frame_loop` parameter, no logic. No new `STRATA_SCENE` numbers; combat's own scenes are named by `STRATA_COMBAT_SCENE`.
- Scripted runs are hidden: `MARCH_PIN_MAIN=1 STRATA_HIDDEN=1 STRATA_OFFSCREEN=1 STRATA_PIXELS=1 STRATA_AUDIO=0`, `STRATA_ISLAND=1` (the match is on the island), fixed `STRATA_FRAMES`. Never open a window.
- Determinism: a scripted run steps 1/60 s a frame. Every random number is `Strata.Seed.mix(seed, n)` with `n` a counter kept in the match; no clock, no `Random`.
- March gaps that bite here (`GAPS.md`): S26 `||` and `&&` evaluate both sides in compiled code (nest `if`s where the right side must not run); S27 a type name must be unique across all modules (prefix combat's types `Cb`); else-if chains need one `end` an `if`; `on`, `none`, `within`, `spawn`, `linear` are not usable as parameter or function names where noted.
- Builds and tests stay free of warnings and hints; `forge lint --strict` is in the gate (an unused private function fails it).
- Descriptor bindings 17 to 33 and scene numbers are taken; the tint (Task 8) adds no binding.
- Commit in `~/code/strata` on branch `combat`, no attribution lines, never bare `git stash`. Gate: `STRATA_HIDDEN=1 STRATA_OFFSCREEN=1 scripts/verify.sh` prints `PASS`; do not pipe it into `tail` before a commit. After a golden failure list `test/golden/*.actual.ppm`: the gate prints only its tail.
- Block ids in the viewer's registry: 1 stone, 2 dirt, 3 grass, 4 sand, 5 water, 6 snow, 7 wood, 8 leaves, 10 glass, 11 tall grass, 13 ice. Confirm at `viewer/lib/strata_viewer.march:82-127` before relying on them.

## Review Focus

Each line has its test in the task named.

1. Two hits land on one fighter in the same tick and either would kill: one death, one kill, to the first in event order (Task 1: `one_death_a_tick`).
2. A shooter dies while its arrow is in flight: the arrow still hurts, and the kill is still the dead shooter's (Task 1: `credit_outlives_the_shooter`; Task 4 keeps the thrower's index on the projectile, not a figure id).
3. Two fighters reach 30 in the same tick: the winner is the one whose kill came first in event order, and nothing after it counts (Task 1: `first_to_thirty_wins`).
4. The player digs the ground from under a fighter, or places a block where one stands: it falls or is pushed up by the mover, never stuck inside, and the idle rule frees it otherwise (Task 4: scripted scene `trap`).
5. A second match after the first: every entity, figure and loose piece of the first is gone, so capacity does not run out by the tenth match (Task 9: `ten_matches`).

---

### Task 1: The rules

**Files:**
- Create: `viewer/lib/combat.march` (`mod StrataViewer.Combat`)
- Test: `viewer/test/combat_test.march` (`mod StrataViewer.Test.Combat`; copy the `needs` block of `viewer/test/walker_test.march`)

**Interfaces — Produces**
```
type CbKind = CbPlayer | CbRusher | CbShooter
type CbFighter = { cb_name : String, cb_kind : CbKind, cb_health : Int, cb_kills : Int, cb_deaths : Int,
                   cb_alive : Bool, cb_back_in : Float, cb_idle : Float }
type CbOutcome = CbIgnored | CbHurt | CbKilled(Int)          -- the fighter credited, or -1
type CbIntent = CbReturn | CbWander | CbChase(Int) | CbSwing(Int) | CbHold(Int) | CbRetreat(Int) | CbShoot(Int) | CbSwim(Int)

fn max_health(k : CbKind) : Int                               -- 100, 100, 60
fn arrow_damage() : Int                                       -- 35
fn swing_damage() : Int                                       -- 25
fn fire_damage(metres : Float) : Int                          -- 60 at 0, falling evenly to 20 at 2.0, 0 beyond
fn roster() : List(CbFighter)                                 -- 12: index 0 the player, 1..7 rushers, 8..11 shooters, all alive at full health
fn hit(fs : List(CbFighter), victim : Int, by : Int, damage : Int) : (List(CbFighter), CbOutcome)
fn tick(fs : List(CbFighter), dt : Float) : (List(CbFighter), List(Int))   -- counts cb_back_in down; the indices that come back now, at full health
fn winner(fs : List(CbFighter)) : Int                         -- the index holding 30 kills, else -1
fn standings(fs : List(CbFighter)) : List(Int)                -- indices by kills (more first), then deaths (fewer first), then index
fn idle(f : CbFighter, moved : Float, fought : Bool, dt : Float) : (CbFighter, Bool)   -- true when 20 s without 2 m moved or a hit given or taken
fn target(me : Int, at : List((Float, Float, Float)), alive : List(Bool), clear : Int -> Bool) : Int   -- nearest living other within 40 m for which clear(i); -1
fn intent(k : CbKind, off_field : Bool, swimming : Bool, tgt : Int, metres : Float, cool : Float) : CbIntent
fn ring(cx : Float, cz : Float) : List((Float, Float))        -- 16 columns: 8 at 36 m, 8 at 68 m turned half a step, about (cx, cz)
fn spawn_at(points : List((Float, Float, Float)), living : List((Float, Float, Float))) : Int    -- the point farthest from its nearest living fighter
fn burns(block : Int) : Int                                   -- what fire leaves: 0 for wood, leaves, tall grass, ice; 2 (dirt) for grass; -1 untouched
fn burn_cells(x : Float, y : Float, z : Float) : List((Int, Int, Int))   -- cells whose centre is within 2 m
fn tool(slot : Int) : Int                                     -- 0 the bow (slot 0), 1 fire (slot 1), else 2 + a block id: slots 2 and 3 are stone and dirt, slots 4 to 8 five of today's other hotbar blocks (the builder's pick)
```
`hit`: a victim not alive gives `CbIgnored` and changes nothing. A kill sets `cb_alive` false, `cb_back_in` 3.0, adds a death, and adds a kill to `by` unless `by == victim` or a winner already exists (then `CbKilled(-1)`). `by` may be dead. `intent`, in the design's order: off the field, `CbReturn`; swimming, `CbSwim(tgt)`; no target, `CbWander`; a rusher within 1.6 m with `cool <= 0`, `CbSwing`, else `CbChase`; a shooter under 12 m, `CbRetreat`, over 25 m, `CbChase`, else `CbShoot` when `cool <= 0` and `CbHold` otherwise.

- [ ] **Step 1: Write the failing tests** in `viewer/test/combat_test.march`:
  - `damage_table`: three `hit`s of `arrow_damage()` kill index 1 (a rusher) on the third and not the second; two kill index 8 (a shooter); `fire_damage(0.0) == 60`, `fire_damage(1.0) == 40`, `fire_damage(2.0) == 20`, `fire_damage(2.01) == 0`; one fire hit at 0 m kills a shooter, two a rusher.
  - `one_death_a_tick`: two `hit`s of 100 on index 3 by 1 then by 2: the first is `CbKilled(1)`, the second `CbIgnored`; index 3 has one death; index 2 has no kill.
  - `self_kill_credits_no_one`: `hit(fs, 0, 0, 100)` is `CbKilled(-1)`, kills all 0, index 0 one death.
  - `credit_outlives_the_shooter`: kill index 8, then `hit(fs, 1, 8, 100)` is `CbKilled(8)` and index 8 has one kill.
  - `first_to_thirty_wins`: with index 1 at 29 kills and index 2 at 29, a kill by 1 then a kill by 2: `winner` is 1, index 2 stays at 29.
  - `back_after_three_seconds`: after a kill, `tick` by 2.9 returns no one, by a further 0.2 returns the victim, alive at `max_health`.
  - `standings_order`: kills 5, 5, 7 with deaths 2, 1, 0 on indices 0, 1, 2 give `[2, 1, 0, ...]`.
  - `idle_rule`: 19.9 s of `idle(f, 0.0, false, _)` is false, 20.0 is true; a `moved` of 2.0 or `fought` true starts the count again.
  - `nearest_clear_target`: of living others at 10 m (not clear), 20 m (clear) and 50 m (clear), `target` is the one at 20 m; with none clear, -1; never `me`; never a dead one.
  - `intents`: one assertion per line of the `intent` rule above, plus a swimming rusher in reach gives `CbSwim`, not `CbSwing`.
  - `spawn_farthest`: of points at 10, 40 and 70 m from the one living fighter, `spawn_at` is the third.
  - `ring_shape`: 16 points, all between 30 and 72 m of the middle, no two within 10 m.
  - `fire_burns`: `burns(7) == 0`, `burns(8) == 0`, `burns(11) == 0`, `burns(13) == 0`, `burns(3) == 2`, and -1 for 1, 2, 4, 5, 6, 10; `burn_cells(0.5, 0.5, 0.5)` holds (0, 0, 0) and (1, 0, 0), not (2, 2, 0), and 33 cells in all.
  - `tools`: `tool(0) == 0`, `tool(1) == 1`, `tool(2) == 3`, `tool(3) == 4`.
- [ ] **Step 2:** Run `cd viewer && forge test test/combat_test.march`. Expected: FAIL, "Unknown module `StrataViewer.Combat`".
- [ ] **Step 3: Implement** the functions above in `viewer/lib/combat.march`. Fighters' names: twelve short ones, the player's "You".
- [ ] **Step 4:** Run it again. Expected: PASS, 14 tests.
- [ ] **Step 5: Commit** `feat(combat): the rules of a free-for-all, pure`.

### Task 2: New clips and small rigs

**Files:**
- Modify: `viewer/lib/figures.march`
- Test: `viewer/test/combat_test.march`

**Interfaces — Produces**
```
StrataViewer.Figures.clips(r)        -- now nine: the seven as they are, then flinch, fall (existing ids keep their places)
StrataViewer.Figures.bow() : Strata.Rig.Rig        -- one part, held in the hand socket as the torch is
StrataViewer.Figures.arrow() : Strata.Rig.Rig      -- one part, 0.9 m long along +z
StrataViewer.Figures.fireball() : Strata.Rig.Rig   -- one part, 0.4 m, palette emission as the torch's flame
StrataViewer.Figures.piece(part : Int) : Strata.Rig.Rig   -- the humanoid's part [part] alone, for falling apart (Task 8)
```
`flinch`: 0.25 s, the torso and head thrown back and returned, keyed on the upper-body parts only so it layers over the legs. `fall`: 0.6 s, the whole body to lying on its back, ending held. Write them as `swing` and `crouch` are written.

- [ ] **Step 1: Failing tests:** `nine_clips` (length 9; `Strata.Clip` durations of the eighth and ninth are 0.25 and 0.6); `flinch_spares_the_legs` (no key on a leg part); `small_rigs_are_whole` (each of `bow`, `arrow`, `fireball`, and `piece(p)` for every humanoid part has `refusals == 0` and one part).
- [ ] **Step 2:** Run; expected FAIL (no `bow`).
- [ ] **Step 3: Implement.** Check nothing else indexes `clips` past 6 or assumes its length (`grep -n "clip_ids\|clips(" viewer/lib`).
- [ ] **Step 4:** Run; expected PASS. Run `scripts/golden.sh` hidden: the figure scenes pass unchanged.
- [ ] **Step 5: Commit** `feat(figures): flinch and fall clips; bow, arrow, fireball and loose-part rigs`.

### Task 3: Entities in the viewer; fighters that stand, walk and choose

**Files:**
- Create: `viewer/lib/match.march` (`mod StrataViewer.Match`), `scripts/combat_check.sh`
- Modify: `viewer/forge.toml` (add `../native/strata_ent.c` to `sources`), `viewer/lib/strata_viewer.march`, `scripts/verify.sh` (run `combat_check.sh`)

**Interfaces**
- Consumes: Task 1 (`roster`, `ring`, `spawn_at`, `target`, `intent`), Task 2 (`clips`, `bow`), `Strata.Entities` (`kind`, `with_mover(walking())`, `open`, `blocks`, `step`, `add`, `set_velocity`, `set_facing`, `position`, `grounded`, `events`), `Strata.Query.raycast`, `StrataViewer.Figures` (`open`, `anim`, `rigs`, `dry_ground_at`).
- Produces:
```
type MtMatch                                         -- a record; opaque to the viewer
type MtInput = { mi_x : Float, mi_y : Float, mi_z : Float, mi_yaw : Float, mi_pitch : Float,
                 mi_shoot : Bool, mi_slot : Int, mi_table : Bool, mi_again : Bool }
fn off() : MtMatch                                   -- no match
fn is_on(m : MtMatch) : Bool
fn start(seed : Int, w : Strata.World.World, sx : Float, sz : Float, radius_h : Int) : MtMatch   -- (sx, sz): the shore start
fn crowd(m : MtMatch) : StrataViewer.Figures.Crowd   -- for Strata.Render.figures
fn frame(m : MtMatch, s : Strata.Stream.Stream, f : Strata.Fluid.Fluid, inp : MtInput, dt : Float)
       : (MtMatch, Strata.Stream.Stream, Strata.Fluid.Fluid)
fn middle(m : MtMatch) : (Float, Float)
fn fingerprint(m : MtMatch) : Int                    -- Strata.Entities.hash mixed with every fighter's health, kills and deaths
```
The middle: from (sx, sz) step 8 m at a time along the inland direction (away from the sea, as `shore_go` found it) until at least four fifths of 64 sample columns in the 80 m circle are dry (`dry_ground_at` answers), 160 m at most. Spawn points: `Combat.ring` columns set on `dry_ground_at`; a wet one is moved toward the middle 4 m at a time. Fighters' kinds: walking mover, box half extents (0.3, 0.9, 0.3), step height 1.05 m, gravity 20; seen distance 200 m; unseen rule `lives`. The player's kind: `placed`, the same box, set to the eye less 0.8 m each frame. `frame` here: `Entities.blocks`, set the player's box, every 15th frame (fighters staggered by index) choose `target` and `intent` and steer by velocity (run 4.2 m/s, walk 1.4 m/s; facing along the velocity), blocked a second (no headway of 0.5 m) sidestep left for half a second, then `Entities.step` with focus the player and the middle, then place each figure at its entity and play idle, walk or run by speed. Each shooter's figure has a `bow` figure attached at its hand socket (`Strata.Anim.attach`, as `Figures.add_torch` does). The player's first place is the spawn point nearest (sx, sz). No damage yet: `CbSwing` and `CbShoot` only face the target.

- [ ] **Step 1: The failing check.** `scripts/combat_check.sh` builds the viewer release and runs, hidden, `STRATA_COMBAT=1 STRATA_SHORE=1 STRATA_COMBAT_PLAYER=0 STRATA_LIGHT=0 STRATA_FRAMES=900` twice. It passes when both runs print the same two lines and the first matches: `combat: 12 fighters, 16 spawn points, middle (X, Z), dry N of 64` with N >= 52, then `combat: frame 900 fingerprint H, moved M m` with M > 100 (the fighters' summed horizontal travel: they walked) and no fighter outside 90 m of the middle. `STRATA_COMBAT_PLAYER=0`: the player's box is not added and the focus is the middle alone.
- [ ] **Step 2:** Run it. Expected: FAIL, no `combat:` line.
- [ ] **Step 3: Implement.** In the viewer: read `STRATA_COMBAT`; `Match.start` after the stream's first chunks are in (where `crowd_for` is called now; the match's crowd replaces `crowd` when on); one new `frame_loop` parameter `mt : StrataViewer.Match.MtMatch`; one `Match.frame` call after the stream's tick and before `Figures.step` (which then only runs `An.update`); the two `combat:` prints at start and at the last frame.
- [ ] **Step 4:** Run the check: PASS. Run the gate: PASS, every earlier picture unchanged.
- [ ] **Step 5: Commit** `feat(combat): the match's fighters as entities and figures, behind STRATA_COMBAT`. Tell the entities session (SendMessage to "Strata voxel engine gaps") that `strata_ent.c` is now in `viewer/forge.toml` and the entity world is opened by the match only when `STRATA_COMBAT=1`.

### Task 4: Shots, swings, deaths and the score

**Files:** Modify `viewer/lib/match.march`, `scripts/combat_check.sh`. Test: `viewer/test/combat_test.march`.

**Interfaces**
- Consumes: Task 1 (`hit`, `tick`, `idle`, `winner`, `arrow_damage`, `swing_damage`), Task 2 (`arrow`), `Entities` flying mover, `touching`, `near`, `events` (codes 1 touch, 2 block hit), `with_lifetime`, `remove`.
- Produces:
```
fn kills(m : MtMatch, fighter : Int) : Int          fn health(m : MtMatch, fighter : Int) : Int
fn winner(m : MtMatch) : Int                        fn table(m : MtMatch) : List((String, Int, Int))   -- name, kills, deaths, in standing order
pure, in Combat:  fn aim(seed : Int, n : Int, yaw : Float, pitch : Float, spread_deg : Float) : (Float, Float, Float)   -- a unit direction within the spread
                  fn lead(from, to, to_velocity, speed : Float) : (Float, Float)   -- yaw and pitch for a shot at [speed] to meet a mover, gravity 9.8 scaled by the arrow's kind
```
An arrow: flying mover, box 0.05 m, 60 m/s, gravity 3 m/s2, lifetime 80/60 s in flight; its int slot 0 holds the thrower's fighter index. On a touch with a fighter other than its thrower: `Combat.hit`, remove the arrow. On a block hit: it stays where it stopped for 5 s (velocity zero, lifetime 5), drawn; it hurts no one. The first step of every projectile is ray-tested by the match (`Strata.Query.raycast` over `speed * dt`) before the entity is added: a block in that step means the projectile lands there at once. A shooter with `CbShoot` fires with `lead` and `aim(…, 4.0)`, then cools 1.5 s. A rusher with `CbSwing` plays the swing clip and, 0.2 s in, hurts every living fighter within 1.6 m in the 90 degrees ahead of it (`Entities.near`), then cools 1.0 s. A dead fighter's entity and figure are removed (Task 8 adds the fall); `Combat.tick` brings it back at `spawn_at`. `Combat.idle` runs for computer fighters; a true answer moves the fighter to `spawn_at` at full health, no death counted. When `winner >= 0` nothing more is hit and fighters stand idle.

- [ ] **Step 1: Failing tests and checks.** Unit: `aim_stays_in_its_spread` (1,000 directions at 4 degrees are all within 4 degrees of the axis, unit length, and not all equal); `lead_meets_a_walker` (a target 20 m off crossing at 4.2 m/s: flying the answer for the computed time ends within 0.3 m of where the target then is). `combat_check.sh` gains: scene `STRATA_COMBAT_SCENE=duel` (two shooters set 15 m apart on level ground, no others, 1,200 frames): prints `combat: duel kills A B` with A + B >= 1 and the fingerprint equal across two runs. Scene `trap` (a rusher chasing a still shooter; at frame 60 the match itself digs the cell under the rusher three deep, at frame 300 places a block in the shooter's cell): at frame 1,500 neither is inside a solid cell and the line `combat: trap freed F` shows F >= 1 (the idle rule or the mover got the rusher out). The match: `STRATA_COMBAT_PLAYER=0 STRATA_FRAMES=54000` ends early at a winner and prints `combat: winner NAME 30` and twelve `combat: table` lines; two runs print the same lines.
- [ ] **Step 2:** Run; expected FAIL (no `duel` scene).
- [ ] **Step 3: Implement.**
- [ ] **Step 4:** Run the unit tests and the check: PASS. Note the match's frames-to-winner in the ledger; if no winner comes in 54,000 frames, that is a finding about the field or the fighters to fix here (design: "a match could not end" is a failure), not a number to raise.
- [ ] **Step 5: Commit** `feat(combat): arrows, swings, deaths and the count to thirty`.

### Task 5: Fire

**Files:** Modify `viewer/lib/match.march`, `scripts/combat_check.sh`, `scripts/golden.sh`. Test: `viewer/test/combat_test.march`.

**Interfaces**
- Consumes: Task 1 (`fire_damage`, `burns`, `burn_cells`), Task 2 (`fireball`), how `act` in `strata_viewer.march` applies a dig (the edit through the stream, the fluid woken with `Strata.Fluid.wake` over what changed), `StrataViewer.Figures.trace_last` and the torch's glow for a traced emissive figure.
- Produces: a fireball entity kind (flying, box 0.2 m, 18 m/s, gravity 9.8, lifetime 4 s; int slot 0 the thrower); `Match.throw_fire(m, fighter, yaw, pitch)` used by Task 7.

Where a fireball stops (a block hit, a touch with any fighter but its thrower, or its lifetime): every living fighter within 2 m, the thrower included, takes `fire_damage(distance)`, the one touched taking 60; then one edit of `burns` over `burn_cells`, skipping cells that are unloaded; the fluid is woken round the edit. A match runs with terrain support on, as `STRATA_SUPPORT=1` does, so a crown whose trunk burned comes down as loose bodies. In flight its figure is traced and emissive so it lights what it passes; if that costs more than 1 ms a frame with four in the air, it flies untraced and the ledger and `RESULTS.md` say so.

- [ ] **Step 1: Failing checks.** Unit: `fire_edit` (given a 5 x 5 x 5 world of wood with a grass floor and a stone core cell, the changes computed for a landing at the centre remove wood within 2 m, turn in-range grass to dirt, leave stone and every cell beyond 2 m). Scene `fire`: one fireball thrown from 10 m at the nearest tree's trunk 2 m up: prints `combat: fire burned N cells: wood A, leaves B, grass C` with A >= 1 and B >= 1, and a shooter (health 60) set 1 m from the landing is left with between 15 and 25 (it took about `fire_damage(1.0)`, 40). Golden `combat-fire` (the frame the fireball is 3 m from landing, `STRATA_LIGHT=4`): stored after looking at it.
- [ ] **Step 2:** Run; expected FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4:** Run: PASS. Look at `combat-fire.actual.ppm` (convert to PNG; the ground under the fireball is lit), store it.
- [ ] **Step 5: Commit** `feat(combat): fireballs burn wood, leaves, grass and ice, and light their way`.

### Task 6: Water

**Files:** Modify `viewer/lib/match.march`, `scripts/combat_check.sh`. Test: `viewer/test/combat_test.march`.

**Interfaces — Consumes:** `Strata.Fluid.depth_in`, `surface`, `flow`; `viewer/lib/walker.march` `wet` and `step_in` for how the player's body floats and climbs out; Task 1 `CbSwim`.

A computer fighter whose box is more than 0.55 under liquid (`depth_in`) is swimming: the match sets its entity's position itself each frame (velocity zeroed so the walking mover adds nothing): y eased to put its shoulders at `surface`, x and z moved 1.5 m/s toward its intent's goal plus `flow` times 1.0 m/s, the swim clip playing. Where the cell ahead is ground no more than one block above the surface, it is set on that ground and is walking again. Swimming, it neither swings nor shoots. A projectile's step is tested against fluid cells (the cells its segment crosses, `Strata.Blocks.is_fluid`): at the first, an arrow is removed and a fireball ends with no damage and no burn.

- [ ] **Step 1: Failing checks.** Unit: `shots_stop_at_water` (a segment from 2 m above a water cell to 2 m below it reports the surface point; one over dry cells reports none). Scene `channel` (a 6 m wide, 3 m deep channel of water dug and filled by the scene between a rusher and a still shooter, 1,800 frames): prints `combat: channel swam S frames, crossed 1, struck K` with S > 60 and K >= 1, and the rusher's lowest y while swimming within 1.2 m of the surface (it did not walk the bed).
- [ ] **Step 2:** Run; expected FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4:** Run: PASS. Run the Task 4 match check again: still a winner, still equal across two runs.
- [ ] **Step 5: Commit** `feat(combat): fighters swim; shots stop at the water`.

### Task 7: The player

**Files:** Modify `viewer/lib/match.march`, `viewer/lib/strata_viewer.march`, `scripts/combat_check.sh`.

**Interfaces**
- Consumes: Task 1 `tool`, Task 4 arrows, Task 5 `throw_fire`.
- Produces:
```
fn block_for(m : MtMatch, slot : Int) : Int          -- the block the viewer digs and places with in a match, -1 for a weapon slot
fn respawn(m : MtMatch) : (MtMatch, Option((Float, Float, Float)))   -- once after the player comes back: where to stand
fn player_down(m : MtMatch) : Float                  -- seconds left lying dead, 0 when alive
fn paused(m : MtMatch) : Bool                        -- the player more than 150 m from the middle
fn script_input(name : String, frame : Int, m : MtMatch) : MtInput   -- scripted players for the checks
```
In a match the viewer: fills `MtInput` from the eye, the left button's press edge and the slot; uses `block_for(slot)` where it uses `Hud.slot_block(slot)`, and neither digs nor places on a weapon slot; ignores the F key; moves the body to `respawn`'s point; holds the body still and drops the eye 1.2 m while `player_down > 0`. The player's arrow leaves the eye along the view with `aim(…, 0.5)`, 0.6 s between; fire with `aim(…, 3.0)`, 1.5 s between. Paused, `frame` steps nothing and counts no time.

- [ ] **Step 1: Failing checks.** Scripted players (scripted runs ignore real input): `STRATA_COMBAT_SCRIPT=archer` (the view turned to the nearest living fighter each frame, shooting whenever cool, on the `duel` ground with one still rusher 20 m off): `combat: archer shots S, kills 1` with S == 3 at frame 150. `STRATA_COMBAT_SCRIPT=victim` (the player stands; one rusher): the player's health reaches 0 after four swings, `combat: player died at frame F, back at frame G` with G - F == 180, standing within 1 m of a spawn point. `STRATA_COMBAT_SCRIPT=far` (the player's position reported 200 m from the middle from frame 60): the fingerprint at frame 600 equals the one at frame 60.
- [ ] **Step 2:** Run; expected FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4:** Run: PASS. Gate: PASS (the viewer without `STRATA_COMBAT` unchanged).
- [ ] **Step 5: Commit** `feat(combat): the player shoots, is hurt, dies and comes back`.

### Task 8: Being hit, and falling apart

**Files:** Modify `native/strata_anim.c`, `lib/strata/ffi/anim.march`, `lib/strata/anim.march`, `native/strata_vk.c`, `native/shaders/figquad.glsl` and the figure fragment path (`gbuffer.frag`), `viewer/lib/match.march`, `animcheck/lib/animcheck.march`, `scripts/golden.sh`.

**Interfaces — Produces**
```
Strata.Anim.tint(a : Anim, f : Int, r : Float, g : Float, b : Float, amount : Float) : Anim   -- the figure's colour mixed toward (r, g, b) by amount 0..1; 0 by default
```
The tint rides in the figure's per-instance data already uploaded each frame (four more floats); no new buffer, no new binding. Before starting, tell the animation session (SendMessage to "Strata animation support") what will change in its files and ask whether it has work in flight there; fold in what it answers.

In the match: a hit sets the victim's velocity along the shot (1.5 m for an arrow, up to 3 m at a fireball's centre, spent over 0.25 s), plays `flinch` on layer 1 (weight 1, not additive, the clip's own parts), and tints it pale (0.9, 0.9, 0.85) at 0.7 for 0.1 s. A fighter off the ground (`Entities.grounded` false for more than 0.1 s, or going up a step) plays `jump` until it lands. A death plays `fall` for 0.6 s with the entity held; then the figure is removed and each humanoid part becomes a `piece(part)` figure at the part's last origin (`Strata.Anim.origin`), thrown outward at 2 to 4 m/s with spin, moved by the match: gravity 20, one bounce losing 60% at the ground found by `Strata.Query.raycast` down, at rest after it, removed after 8 s. A hit plays an existing sound source at the victim if `viewer/sounds` holds one that fits a strike; no new sounds. (Design: physics bodies are 1 m grids here, so the pieces are the match's own.) Shirt colours: the tint at 0.0 amount cannot colour, so twelve humanoid rigs differing in the shirt's palette entry, finished at match start; each fighter's figure uses its own rig and that rig's nine clip ids.

- [ ] **Step 1: Failing checks.** animcheck scene `tint` (a figure's stored tint reads back; default 0; a removed figure's slot is clean for the next). Golden `figure-tinted` (scene 31's row with every other figure tinted red at 0.7): differs from `humanoid-poses` only on those figures. `combat_check.sh` scene `duel` gains: `combat: duel flinches N, falls M, pieces P` with N >= 1, M >= 1, P == 10 x M at most alive at once and 0 alive 9 s after the last death; and the fighter count of distinct rigs is 12.
- [ ] **Step 2:** Run; expected FAIL.
- [ ] **Step 3: Implement**, kernel and shader first, then the match.
- [ ] **Step 4:** Run: PASS. Look at `figure-tinted.actual.ppm`, store it. Gate: every other figure picture unchanged (a tint of 0 must cost nothing visible).
- [ ] **Step 5: Commit** `feat(anim): a figure's tint` and `feat(combat): knockback, flinch, the fall and the pieces` (two commits).

### Task 9: On screen; the end; another match

**Files:** Modify `viewer/lib/match.march` (or a new `viewer/lib/match_hud.march` if it passes 150 lines), `viewer/lib/strata_viewer.march`, `scripts/combat_check.sh`, `scripts/golden.sh`.

**Interfaces — Produces**
```
fn draw(m : MtMatch, u : Strata.Ui.Ui, show_table : Bool) : Strata.Ui.Ui     -- called where StrataViewer.Hud.build is, after it
fn again(m : MtMatch, w : Strata.World.World) : MtMatch                       -- a new match on the same field: every entity, figure and piece of the old one removed first
```
Drawn with the `Strata.Ui` and `Strata.Text` calls `viewer/lib/hud.march` uses: the health bar bottom left (200 x 14 points, above the hotbar's line; red under 30); top right "You K" and "NAME K" for the leader; kill lines "A felled B" for kills within 40 m of the player, newest lowest, four at most, gone after 4 s; the hotbar's first two slots showing a bow and a flame (two 16 x 16 pictures added to the hud atlas only in a match); a red edge for 0.2 s when the player is hit; "Paused: return to the field" when paused; Tab held, the table; at a winner, the winner's name, the table and "Press Enter for another". Enter then calls `again`.

- [ ] **Step 1: Failing checks.** `ten_matches`: `STRATA_COMBAT_PLAYER=0 STRATA_COMBAT_WIN=3 STRATA_COMBAT_REPEAT=10` (a match to 3, `again` at each winner, ten times): prints ten `combat: winner` lines and `combat: live entities E, figures F` after the tenth with E <= 12 + 16 and F <= 12 + 12 + 16 (fighters, bows, stuck arrows), the same as after the first. Goldens, `STRATA_UI=1 STRATA_LIGHT=4`: `combat-fight` (the `archer` script at frame 140: bar, counts, a kill line, weapons on the bar) and `combat-end` (a match to 3 at its winner, the table drawn).
- [ ] **Step 2:** Run; expected FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4:** Run: PASS. Look at both pictures before storing them.
- [ ] **Step 5: Commit** `feat(combat): the match on screen, its end, and another`.

### Task 10: Results, and landing it

**Files:** `RESULTS.md`, `README.md`, `GAPS.md` (anything new met), `docs/combat-ledger.md`.

- [ ] **Step 1:** Measure, hidden, `STRATA_LIGHT=4`, the `archer` view with all twelve fighting against the same view with `STRATA_FIGURES=12` and no match: frame time by `STRATA_FRAME_LOG` grouped by tick phase (not the fps line). Record both in `RESULTS.md`; more than 1 ms apart is a finding to fix or to name as a miss.
- [ ] **Step 2:** `README.md`: how to start a match (`MARCH_PIN_MAIN=1 STRATA_SHORE=1 STRATA_COMBAT=1 STRATA_LIGHT=4`), the keys, what is not in it (the design's list).
- [ ] **Step 3:** Merge `main`, run the gate, `git fetch . combat:main`; repeat while `main` moves. If `main` is checked out in another session's worktree, ask that session to release it.
- [ ] **Step 4:** Launch it for the user in a terminal tab (never from a background shell: the window needs the main thread) and say what was not checked by eye.

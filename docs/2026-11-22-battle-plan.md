# Steel Rain battle, stage 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A playable fight on a generated map with both sides played by hand: moving, cone-traced volleys that carve terrain and mech parts, falling ground and falling mechs, turn delay, heat, watches, emplacements, withdrawal and the Destroy mission, running headless with a screen that plays it.

**Architecture:** The fight is one value, `SteelRain.Fight.Fight`, changed only by `step(fight, order) -> (fight, events)`; it holds its own scratch world and never touches the renderer or `Strata.Anim`. General mechanics (cone shots, watch arcs, clock delay) go into `strata_tactics`; Steel Rain's numbers, damage, heat, missions and the screen go into `games/steel_rain`. The screen applies orders through `step` and plays the events back.

**Tech Stack:** March, forge, Strata (Query raycast, Edit blast/set_many/stamp, Support loose_box, Rig), `strata_tactics` (Clock, Reach, Grid, Sight, Area, Marks), `strata_kit` (Part, Build, Assemble, Damage), `strata_maps` (generate, RuinStage.scratch, Sites).

**Spec:** `docs/2026-11-22-battle-design.md` (approved 2026-11-22). Stage 1 of its Stages section. Ledger: `docs/battle-ledger.md`.

## Global Constraints

- Public Strata only in `strata_tactics` (the plugin rules); nothing Steel Rain-specific there.
- The world is 0.4 m a voxel, tiles 8 voxels; mech part voxels 0.1 m (the Warden's `SteelRain.Mech.voxel()` is 0.08 m today: use the function, never a literal).
- The headless fight must not need a window, the renderer or `Strata.Anim`; nothing in `SteelRain.Fight` and its helpers may `needs Anim` or call `Strata.Ffi`.
- Deterministic: same fight, same orders, same seed, same result. All randomness from `Strata.Seed.mix(fight seed, counter)`; no other source.
- An illegal order is rejected with a reason and leaves the fight unchanged.
- Rounds hit anything, allies included.
- A mech fires any or all of its intact weapons in one action, each at its own point; heat limits volleys.
- The screen decides nothing: every hit, fall, death and result comes from `step`.
- Every rules number lives in `SteelRain.Rules` as data.
- March pitfalls (march-lang skill): reserved words (`on within init with opaque by loop spawn send as`), no nested lambdas inside a lambda's or match arm's let-chain (lift helpers), no `&& match`, no `panic` in lib code (lint), test aliases unique across all test suites, `. scripts/env.sh` before forge, windowed runs need `MARCH_PIN_MAIN=1`.
- Gate per task: the package's `forge test` and `forge lint --strict` clean. No attribution lines in commits.

## Review Focus

1. A shot whose rounds leave the loaded world or the map (edge tiles, upward misses): a miss, never a crash or an out-of-range read. Pinned in Task 2.
2. An ally standing in a volley's cone: it is hit and the preview reports it. Pinned in Task 2 (shot) and Task 6 (fight).
3. A mover knocked out or immobilised by watch fire part way along its path: it stops on the tile where it was hit, the rest of the path is not walked, later watches do not fire on it. Pinned in Task 8.
4. A mech that falls onto a tile another unit holds: it takes the lowest free neighbour and both take the small damage. Pinned in Task 7.
5. Delay that would take CT below 0, and a volley that would land exactly at the heat cap: CT floors at 0; exactly at the cap is legal, one over is rejected unchanged. Pinned in Task 1 (delay) and Task 6 (heat).

---

### Task 1: Clock delay and watch arcs in strata_tactics

**Files:**
- Modify: `plugins/strata_tactics/lib/clock.march`
- Create: `plugins/strata_tactics/lib/watch.march`
- Test: `plugins/strata_tactics/test/clock_test.march`, `plugins/strata_tactics/test/watch_test.march`
- Modify: `docs/plugins.md` (one line each for `Clock.delay` and `Watch`)

**Interfaces:**
- Produces: `StrataTactics.Clock.delay(c : TurnClock, id : Int, amount : Int) : TurnClock` (CT minus amount, floored at 0; unknown id: unchanged).
- Produces: `type StrataTactics.Watch.Arc = { wa_owner : Int, wa_from : (Int, Int), wa_dir : Float, wa_width : Float, wa_range : Int }` (direction and width in radians, range in tiles); `Watch.arc(owner, from, dir, width, range) : Arc`; `Watch.covers(a : Arc, t : (Int, Int)) : Bool` (t within range, by tile-centre Euclidean distance, and within half the width either side of dir; the arc's own tile is not covered); `Watch.tiles(g : TileGrid, a : Arc) : List((Int, Int))` (covered tiles on the grid, row order); `Watch.triggered(arcs : List(Arc), t : (Int, Int), sees : Arc -> Bool) : List(Arc)` (the arcs covering t whose owner `sees` passes, in list order).

- [ ] **Step 1: Write the failing tests**

```march
-- clock_test.march, in the existing describe
test "delay takes CT away and stops at 0" do
  let c = Clock.add(Clock.add(Clock.start(Clock.settings()), 1, 8, 50), 2, 8, 10)
  Test.assert_eq_int(Clock.ct(Clock.delay(c, 1, 20), 1), 30, "50 - 20")
  Test.assert_eq_int(Clock.ct(Clock.delay(c, 2, 20), 2), 0, "floored at 0")
  Test.assert_eq_int(Clock.ct(Clock.delay(c, 9, 20), 1), 50, "unknown unit: nothing changes")
end
-- watch_test.march (alias TcW for StrataTactics.Watch)
test "an arc covers tiles inside its width and range, not its own tile" do
  let a = TcW.arc(1, (5, 5), 0.0, 1.5708, 4)   -- facing +x, 90 degrees wide
  Test.assert_true(TcW.covers(a, (8, 5)) && TcW.covers(a, (7, 6)), "ahead, inside")
  Test.assert_true(!TcW.covers(a, (5, 7)) && !TcW.covers(a, (10, 5)) && !TcW.covers(a, (5, 5)), "beside, past range, its own tile")
end
test "a step into a covered tile triggers the arcs whose owners see it, in order" do
  let a = TcW.arc(1, (5, 5), 0.0, 1.5708, 4)
  let b = TcW.arc(2, (9, 2), 1.5708, 1.5708, 4)  -- facing +z
  let hits = TcW.triggered([a, b], (8, 4), fn x -> x.wa_owner != 3)
  Test.assert_eq_int(List.length(hits), 2, "both")
  Test.assert_eq_int(List.length(TcW.triggered([a, b], (8, 4), fn x -> x.wa_owner == 2)), 1, "only the one that sees")
end
```

- [ ] **Step 2: Run them to see them fail**

Run: `. scripts/env.sh && cd plugins/strata_tactics && forge test --filter "delay takes|arc covers|covered tile"`
Expected: compile errors naming `Clock.delay` and `StrataTactics.Watch`.

- [ ] **Step 3: Implement `Clock.delay` and the `Watch` module** as the Interfaces say (direction 0 is +x, π/2 is +z; angles compared on the wrapped difference).

- [ ] **Step 4: Run the package's tests and lint**

Run: `forge test && forge lint --strict`
Expected: all pass, lint clean.

- [ ] **Step 5: Commit** `feat(tactics): clock delay and watch arcs`

### Task 2: Cone shots in strata_tactics

**Files:**
- Create: `plugins/strata_tactics/lib/shot.march`
- Test: `plugins/strata_tactics/test/shot_test.march`
- Modify: `docs/plugins.md`

**Interfaces:**
- Consumes: `Strata.Query.raycast(w, ox, oy, oz, dx, dy, dz, max, stop) : Hit` (`Miss | Struck(x, y, z, face, t)`), `Strata.Blocks.collides`.
- Produces:
  - `type StrataTactics.Shot.Box = { bx_unit : Int, bx_part : Int, bx_centre : (Float, Float, Float), bx_half : (Float, Float, Float), bx_yaw : Float }` (metres; a box turned by yaw about +y).
  - `type Shot.Spec = { sp_from : (Float, Float, Float), sp_at : (Float, Float, Float), sp_rounds : Int, sp_spread : Float, sp_max : Float, sp_near : Float }` (spread: the cone's half-angle in radians at this shot, the caller has already widened it; max: metres; near: near-miss distance in metres).
  - `type Shot.Struck = SVoxel(Int, Int, Int) | SBox(Int, Int, (Float, Float, Float)) | SNone` (a voxel; a unit, part and the point in metres; nothing).
  - `type Shot.Round = { rd_dir : (Float, Float, Float), rd_end : (Float, Float, Float), rd_struck : Struck, rd_near : List(Int) }` (unit ids passed within near distance without being struck).
  - `Shot.fire(w : Strata.World.World, boxes : List(Box), s : Spec, seed : Int) : List(Round)`: round k's direction is drawn from `Strata.Seed.mix(seed, k)` uniformly over the cone's disc around the aim; each round is traced to the nearer of the first colliding voxel (raycast) and the first box it enters; a box test is ray against oriented box.
  - `Shot.preview(w, boxes, s : Spec) : List((Struck, Float))`: no randomness; the aim plus 3 rings of 8 directions at 1/3, 2/3 and 1 of the spread (25 rays), grouped by what they strike (voxels collapse to one `SVoxel` per distinct voxel), each with its share of the 25.

- [ ] **Step 1: Write the failing tests** (alias TcS; worlds from `test/worlds.march`)

```march
test "a round aimed at a wall strikes the wall" do
  -- a flat world with a wall 3 voxels tall across x = 10 voxels
  let rs = TcS.fire(walled(), Nil, spec_at((2.0, 0.6, 4.0), (8.0, 0.6, 4.0), 1, 0.0), 7)
  Test.assert_true(struck_x(List.head(rs)) == 10, "the wall")
end
test "a round passes over a wall from high ground" do
  let rs = TcS.fire(walled(), Nil, spec_at((2.0, 3.0, 4.0), (8.0, 0.0, 4.0), 1, 0.0), 7)
  Test.assert_true(!struck_x_is(List.head(rs), 10), "over it")
end
test "a box between shooter and wall takes the round, whoever owns it" do
  let ally = { bx_unit: 1, bx_part: 0, bx_centre: (3.0, 0.6, 4.0), bx_half: (0.3, 0.6, 0.3), bx_yaw: 0.0 }
  let rs = TcS.fire(walled(), [ally], spec_at((2.0, 0.6, 4.0), (8.0, 0.6, 4.0), 1, 0.0), 7)
  Test.assert_true(struck_unit(List.head(rs)) == 1, "the ally")
end
test "a round passing close by a box reports a near miss" do
  let b = { bx_unit: 4, bx_part: 0, bx_centre: (5.0, 0.6, 4.6), bx_half: (0.2, 0.2, 0.2), bx_yaw: 0.0 }
  let rs = TcS.fire(walled(), [b], { spec_at((2.0, 0.6, 4.0), (8.0, 0.6, 4.0), 1, 0.0) with sp_near: 0.5 }, 7)
  Test.assert_true(List.member(List.head(rs).rd_near, 4), "near")
end
test "the same seed gives the same rounds; a wider cone spreads them" do
  let s = spec_at((2.0, 0.6, 4.0), (8.0, 0.6, 4.0), 6, 0.2)
  Test.assert_true(same_rounds(TcS.fire(walled(), Nil, s, 3), TcS.fire(walled(), Nil, s, 3)), "deterministic")
  Test.assert_true(distinct_voxels(TcS.fire(walled(), Nil, s, 3)) > 1, "spread")
end
test "rounds that leave the world miss" do
  let rs = TcS.fire(walled(), Nil, spec_at((2.0, 0.6, 4.0), (2.0, 50.0, 4.0), 4, 0.1), 1)
  Test.assert_true(List.all(rs, fn r -> is_none(r.rd_struck)), "no strike, no crash")
end
test "the preview shares add up to 1 and see an ally in the cone" do
  let ally = { bx_unit: 1, bx_part: 0, bx_centre: (5.0, 0.6, 4.0), bx_half: (0.3, 0.6, 0.3), bx_yaw: 0.0 }
  let p = TcS.preview(walled(), [ally], spec_at((2.0, 0.6, 4.0), (8.0, 0.6, 4.0), 1, 0.1))
  Test.assert_true(float_near(sum_shares(p), 1.0) && share_of_unit(p, 1) > 0.0, "shares, ally seen")
end
```

(The test helpers `walled`, `spec_at`, `struck_x`, `struck_x_is`, `struck_unit`, `is_none`, `same_rounds`, `distinct_voxels`, `sum_shares`, `share_of_unit`, `float_near` are the implementer's, in the test file or `worlds.march`.)

- [ ] **Step 2: Run them to see them fail** — `forge test --filter "round|preview"` in `plugins/strata_tactics`; expected: compile errors naming `StrataTactics.Shot`.
- [ ] **Step 3: Implement `StrataTactics.Shot`** as the Interfaces say. The cone disc sample: angle `2π·u1`, radius `spread·sqrt(u2)`, with u1, u2 from `Strata.Seed.unit(Strata.Seed.mix(seed, 2k))` and `(…, 2k + 1)`.
- [ ] **Step 4: Run the package's tests and lint** — expected: all pass, lint clean.
- [ ] **Step 5: Commit** `feat(tactics): cone shots traced through voxels and unit boxes`

### Task 3: Rules data and the mechs' stats

**Files:**
- Create: `games/steel_rain/lib/rules.march`
- Modify: `games/steel_rain/lib/mech.march` (tags on the Warden's parts; a second build)
- Test: `games/steel_rain/test/rules_test.march`

**Interfaces:**
- Produces, in `SteelRain.Rules`:
  - `type Weapon = { wp_name : String, wp_best : Float, wp_max : Float, wp_spread : Float, wp_grow : Float, wp_rounds : Int, wp_damage : Int, wp_carve : Int, wp_heat : Int, wp_delay : Int }` (metres; spread radians at best range; grow: extra radians per metre past best; carve: blast radius in world voxels).
  - `weapon(id : Int) : Option(Weapon)` — id from a part's `"weapon"` tag; 1 autocannon, 2 missile rack, 3 emplacement gun.
  - `type Numbers = { rn_near_delay : Int, rn_near_metres : Float, rn_moved_spread : Float, rn_suppressed_spread : Float, rn_hot_spread : Float, rn_heat_warn : Int, rn_heat_cap : Int, rn_overheat_ct : Int, rn_move_heat : Int, rn_side : Float, rn_rear : Float, rn_fall_safe : Int, rn_fall_core : Int, rn_fall_per_step : Int, rn_fall_ct_per_step : Int, rn_debris_per_voxel : Float, rn_bump : Int, rn_watch_width : Float, rn_watch_range : Int }`; `numbers() : Numbers`.
  - `speed(parts : List(StrataKit.Part.KitPart), b : StrataKit.Build.KitBuild) : Int` — the chassis's `"speed"` tag plus the movement piece's, less 1 per full 10% the summed `"weight"` exceeds 60% of the core's `"weight_limit"`; at least 1.
  - `shed(parts, b) : Int` — heat shed per turn: the summed `"heat_sink"` tags, less a quarter when summed `"power"` exceeds 90% of the core's `"power_limit"`.
  - `facing_factor(n : Numbers, shooter : (Float, Float), target : (Float, Float), target_yaw : Float) : Float` — 1.0 front (within 60° of the target's facing towards the shooter), `rn_rear` rear (within 60° of behind), `rn_side` otherwise.
  - `mover(parts, b) : StrataTactics.Reach.Mover` — from the movement piece's `"move"`, `"climb"`, `"drop"` tags and the build's height in world voxels.
- Produces, in `SteelRain.Mech`: tags on the Warden's parts — every part an `"armour"`; the torso `"core" 1`, `"speed"`, `"weight_limit"`, `"power_limit"`, `"heat_sink"`, `"sight"` (metres); legs `"move"`, `"climb"`, `"drop"`, `"speed"`; arm `"weapon" 1`; rack `"weapon" 2`; all `"weight"` and `"power"`. `striker() : KitBuild` — a second build of the same parts without the sensor (a second, different mech for tests). Starting values are the implementer's, sensible against these tests.

- [ ] **Step 1: Write the failing tests** (alias SrR)

```march
test "the Warden's speed comes from its torso and legs" do
  Test.assert_true(SrR.speed(SteelRain.Mech.parts(), SteelRain.Mech.build()) >= 1, "at least 1")
end
test "a heavier build is no faster" do
  Test.assert_true(SrR.speed(SteelRain.Mech.parts(), SteelRain.Mech.build()) <= SrR.speed(SteelRain.Mech.parts(), SteelRain.Mech.striker()), "the lighter striker is at least as fast")
end
test "shots from behind hit harder than from in front" do
  let n = SrR.numbers()
  Test.assert_true(SrR.facing_factor(n, (0.0, -5.0), (0.0, 0.0), 1.5708) > SrR.facing_factor(n, (0.0, 5.0), (0.0, 0.0), 1.5708), "rear over front")
end
test "every weapon the Warden carries has numbers" do
  Test.assert_true(Option.is_some(SrR.weapon(1)) && Option.is_some(SrR.weapon(2)) && Option.is_some(SrR.weapon(3)), "1, 2, 3")
end
```

- [ ] **Step 2: Run them to see them fail** — `. scripts/env.sh && cd games/steel_rain && forge test --filter "speed|heavier|behind|weapon the"`; expected: compile errors naming `SteelRain.Rules`.
- [ ] **Step 3: Implement `SteelRain.Rules` and the tags**, as the Interfaces say (yaw 0 faces +x, π/2 faces +z).
- [ ] **Step 4: Run the game's tests and lint** — expected: all pass (the existing mech, sculpt and walk tests too), lint clean.
- [ ] **Step 5: Commit** `feat(steel_rain): rules data and the Warden's stats`

### Task 4: Hulls: hit boxes and muzzles without the animator

**Files:**
- Create: `games/steel_rain/lib/hull.march`
- Test: `games/steel_rain/test/hull_test.march`

**Interfaces:**
- Consumes: `Strata.Rig` (`parents`, `pivot_at`, `anchor_at`, `size`, `rests`, `voxel_size`), `StrataKit.Assemble.assemble`, `KitMap` (`path_of`, `markers`).
- Produces, in `SteelRain.Hull`:
  - `type Hull = { hl_parts : List((List(String), (Float, Float, Float), (Float, Float, Float))), hl_muzzles : List((List(String), (Float, Float, Float))), hl_height : Float }` — per build node (path): its box's centre and half extents in metres in the mech's frame (origin under the mech on its floor, facing +x), the union of that node's rig parts at the rig's rest pose, raised by the stand height; muzzles: each `"muzzle"` effect marker's position by the node that carries it; the standing height.
  - `of(rig : Strata.Rig.Rig, m : StrataKit.Assemble.KitMap, stand : Float) : Hull`.
  - `boxes(h : Hull, unit : Int, at : (Float, Float, Float), yaw : Float, paths : List(List(String))) : List(StrataTactics.Shot.Box)` — the boxes of the nodes still on the mech (`paths`), placed at `at` turned by yaw; `bx_part` is the node's index in `h.hl_parts`.
  - `muzzle(h : Hull, path : List(String), at, yaw) : (Float, Float, Float)` — world metres; a node with no muzzle marker uses its box's centre.

- [ ] **Step 1: Write the failing tests** (alias SrH; the Warden from `SteelRain.Mech.assemble()` and `stand()`)

```march
test "the Warden's hull has a box for every build node, all above its floor" do
  let h = warden_hull()
  Test.assert_eq_int(List.length(h.hl_parts), StrataKit.Build.count(SteelRain.Mech.build()), "one per node")
  Test.assert_true(List.all(h.hl_parts, fn p -> box_bottom(p) >= -0.01), "nothing below the floor")
  Test.assert_true(float_near_tol(h.hl_height, SteelRain.Mech.height(), 0.2), "as tall as the Warden")
end
test "the legs' box is lower than the torso's" do
  Test.assert_true(centre_y(warden_hull(), legs_path()) < centre_y(warden_hull(), torso_path()), "legs below")
end
test "turning the mech a quarter turns its boxes" do
  let b0 = SrH.boxes(warden_hull(), 1, (0.0, 0.0, 0.0), 0.0, all_paths())
  let b1 = SrH.boxes(warden_hull(), 1, (0.0, 0.0, 0.0), 1.5708, all_paths())
  Test.assert_true(rotated_quarter(arm_centre(b0), arm_centre(b1)), "the arm moves from +x side to +z side")
end
```

- [ ] **Step 2: Run them to see them fail**; expected: compile errors naming `SteelRain.Hull`.
- [ ] **Step 3: Implement `SteelRain.Hull`**: each rig part's world rest transform is its parent's composed with its pivot, rest turn and anchor; its box spans its grid's size in voxels times the rig's voxel size.
- [ ] **Step 4: Run the game's tests and lint**.
- [ ] **Step 5: Commit** `feat(steel_rain): hulls for hits without the animator`

### Task 5: The fight: state, setup, turns and orders

**Files:**
- Create: `games/steel_rain/lib/fight.march`
- Test: `games/steel_rain/test/fight_test.march` (and `games/steel_rain/test/fights.march`: shared test setup, a small generated map)

**Interfaces:**
- Consumes: `StrataMaps.generate`, `StrataMaps.RuinStage.scratch`, `Strata.Plugin.apply`, `StrataMaps.grid`, `StrataTactics.Clock`, Task 3, Task 4.
- Produces, in `SteelRain.Fight`:
  - (No type may be named `Unit`: it shadows March's `Unit`.) `type Mech = { mc_id : Int, mc_side : Int, mc_kind : Int, mc_build : StrataKit.Build.KitBuild, mc_hull : SteelRain.Hull.Hull, mc_armour : List((List(String), Int)), mc_tile : (Int, Int), mc_yaw : Float, mc_heat : Int, mc_state : Int, mc_moved : Bool, mc_acted : Bool, mc_hit_this_round : Bool, mc_braced : Bool }` — side 0 the player, 1 the enemy; kind 0 mech, 1 emplacement; state 0 in, 1 withdrawn, 2 out; armour per build node still on the mech.
  - `type Fight = { fg_map : StrataMaps.Data.MapData, fg_world : Strata.World.World, fg_grid : StrataTactics.Grid.TileGrid, fg_parts : List(StrataKit.Part.KitPart), fg_mechs : List(Mech), fg_clock : StrataTactics.Clock.TurnClock, fg_watches : List(StrataTactics.Watch.Arc), fg_mission : Mission, fg_seed : Int, fg_shots : Int, fg_result : Option(Result_) }`; `type Mission = MNone` (Task 9 adds `MDestroy`); `type Result_ = { rs_won : Bool, rs_mechs : List((Int, Int, StrataKit.Build.KitBuild)) }` (id, state, build at the end).
  - `type Order = OMove(List((Int, Int))) | OFire(List((List(String), (Float, Float, Float)))) | OBrace | OWatch(List(String), Float) | OWithdraw | OEnd(Float)` — fire: (weapon node path, aim point in metres) pairs; watch: weapon path and direction; end: facing yaw.
  - `type Event = EMoved(Int, (Int, Int)) | ERound(Int, StrataTactics.Shot.Round) | ECarved(List(Strata.Edit.Change)) | EFell(List((Int, Int, Int, Int))) | EPartHit(Int, List(String), Int) | EPartLost(Int, List(String)) | EDelayed(Int, Int) | EHeat(Int, Int) | EOverheated(Int) | EMechFell(Int, (Int, Int), Int) | EWatchSet(Int) | EWatchFired(Int, Int) | EState(Int, Int) | ETurn(Int) | EEnded(Result_)`.
  - `start(d : StrataMaps.Data.MapData, w : Strata.World.World, parts : List(KitPart), side0 : List(KitBuild), side1 : List(KitBuild), seed : Int) : Fight` — the grid from `StrataMaps.grid`; side 0 on the `"spawn north"` site's tiles, side 1 on `"spawn south"`'s, facing each other; every mech on the clock at its `Rules.speed`; the clock advanced to the first turn.
  - `active(f : Fight) : Option(Int)`; `mech(f : Fight, id : Int) : Option(Mech)`.
  - `step(f : Fight, o : Order) : Result((Fight, List(Event)), String)` — in this task only `OEnd` and `OBrace` are accepted (others: `Err("not yet")`); `OEnd` sets facing, sheds heat (`Rules.shed`), ends the turn on the clock (`end_turn(moved, acted)`), clears the next unit's turn flags and advances to its turn (`ETurn`).
  - Errors are `Err(reason)`: a second move or second act in a turn, an order with no active unit, an order after the fight ended.

- [ ] **Step 1: Write the failing tests** (alias SrF; `SteelRain.Test.Fights.small()` builds a 16-tile map with the two spawn sites and returns a fresh fight with two Wardens a side, seed 3)

```march
test "a new fight has every mech on its spawn and a unit to act" do
  let f = SteelRain.Test.Fights.small()
  Test.assert_eq_int(List.length(f.fg_mechs), 4, "four mechs")
  Test.assert_true(Option.is_some(SrF.active(f)), "someone acts")
end
test "ending a turn passes it on and sheds heat" do
  let f = SteelRain.Test.Fights.with_heat(SteelRain.Test.Fights.small(), 20)
  match SrF.step(f, SrF.OEnd(0.0)) do
    Ok((f2, evs)) -> Test.assert_true(heat_of(f2, first_active(f)) < 20 && List.any(evs, is_turn), "shed, next turn")
    Err(e) -> Test.fail(e)
  end
end
test "an order the fight cannot take leaves it unchanged" do
  let f = SteelRain.Test.Fights.small()
  match SrF.step(f, SrF.OBrace) do
    Ok((f2, _)) -> Test.assert_true(Result.is_err(SrF.step(f2, SrF.OBrace)), "a second act is refused")
    Err(e) -> Test.fail(e)
  end
end
test "the same orders from the same fight give the same fight" do
  let f = SteelRain.Test.Fights.small()
  Test.assert_true(same_fight(run_orders(f, [SrF.OBrace, SrF.OEnd(0.0), SrF.OEnd(1.0)]), run_orders(f, [SrF.OBrace, SrF.OEnd(0.0), SrF.OEnd(1.0)])), "deterministic")
end
```

- [ ] **Step 2: Run them to see them fail**; expected: compile errors naming `SteelRain.Fight`.
- [ ] **Step 3: Implement `SteelRain.Fight`** as the Interfaces say, and `SteelRain.Test.Fights` (generate with `StrataMaps.generate`, the Rules mover, site rules for the two spawns; world = `RuinStage.scratch` with the ruin requests applied).
- [ ] **Step 4: Run the game's tests and lint**.
- [ ] **Step 5: Commit** `feat(steel_rain): the fight's state, setup and turns`

### Task 6: Volleys: rounds, terrain, damage, delay and heat

**Files:**
- Modify: `games/steel_rain/lib/fight.march`
- Create: `games/steel_rain/lib/ground.march` (terrain carving and falling rubble)
- Test: `games/steel_rain/test/volley_test.march`, `games/steel_rain/test/ground_test.march`

**Interfaces:**
- Consumes: `StrataTactics.Shot`, `Hull.boxes`/`muzzle`, `Rules`, `Strata.Edit.blast`/`set_many`, `Strata.Support.loose_box`, `StrataTactics.Grid.refresh`, `Clock.delay`.
- Produces:
  - `SteelRain.Ground.carve(w : Strata.World.World, x : Int, y : Int, z : Int, radius : Int, seed : Int, rubble : Int) : (Strata.World.World, List(Strata.Edit.Change), List((Int, Int, Int, Int)))` — the blast, then every loose piece in a box 12 voxels round the blast removed and dropped column by column onto the first solid voxel below as `rubble` blocks (each column's voxels keep their count); returns the world, the blast's changes, and the cells set by the fall (x, y, z, id; id 0 for removals).
  - `SteelRain.Ground.drop(w, cells : List((Int, Int, Int)), rubble : Int) : (Strata.World.World, List((Int, Int, Int, Int)))` — the same fall for given cells (wrecks and lost parts use it, Task 7).
  - `step(f, OFire(shots))`: legal when the mech has not acted, every path is an intact weapon node it carries, every aim is within that weapon's `wp_max` of its muzzle, and heat + the volley's heat ≤ `rn_heat_cap`. Weapons fire in the order given; for each: spread = `wp_spread` + `wp_grow` × metres past best + `rn_moved_spread` if moved + `rn_suppressed_spread` if hit since its last turn + `rn_hot_spread` if heat ≥ `rn_heat_warn`; rounds from `Shot.fire` with seed `Strata.Seed.mix(fg_seed, fg_shots)` (fg_shots +1 per weapon); each voxel strike → `Ground.carve` (`wp_carve`) and the grid refreshed; each box strike → damage `wp_damage × facing_factor` (rounded) off that node's armour, `wp_delay` CT off the target (halved if braced), the target marked hit; each near miss → `rn_near_delay`; a node at 0 armour → lost with its subtree (EPartLost; its cells handed to Task 7's drop when it exists — here simply removed from the build); the core lost → state out (EState, off the clock); heat added (EHeat); heat past `rn_heat_cap` cannot happen (refused). Facing set towards the first aim.
  - Overheat at turn end (`OEnd`): heat after shedding > `rn_heat_warn` + (`rn_heat_cap` − `rn_heat_warn`)/2 → `rn_overheat_ct` CT off and every node with a `"heat_damage"` tag loses that much armour (EOverheated).
  - States from damage: the core lost → out; the movement node lost → immobilised (cannot `OMove`, Task 8); no weapon node left → disarmed (cannot `OFire`/`OWatch`). Immobilised and disarmed are derived from the build, not stored.

- [ ] **Step 1: Write the failing tests** (alias SrV and SrG; fixtures in `fights.march`: `facing_pair()` two mechs 6 tiles apart in the open on flat ground, the active one side 0; `behind_wall()` the same with a wall 3 voxels tall between them)

```march
test "a volley at an exposed mech hits a part, delays it and heats the shooter" do
  match SrF.step(facing_pair(), SrF.OFire([(arm_path(), torso_point_of(facing_pair(), 2))])) do
    Ok((f2, evs)) -> Test.assert_true(List.any(evs, is_part_hit) && ct_of(f2, 2) < ct_of(facing_pair(), 2) && heat_of(f2, 1) > 0, "hit, delayed, hot")
    Err(e) -> Test.fail(e)
  end
end
test "a wall between them takes the rounds and wears away" do
  match SrF.step(behind_wall(), SrF.OFire([(arm_path(), torso_point_of(behind_wall(), 2))])) do
    Ok((f2, evs)) -> Test.assert_true(List.any(evs, is_carved) && !List.any(evs, is_part_hit) && wall_voxels(f2) < wall_voxels(behind_wall()), "the wall, not the mech")
    Err(e) -> Test.fail(e)
  end
end
test "an ally in the cone is hit" do
  let f = ally_between(facing_pair())
  match SrF.step(f, SrF.OFire([(arm_path(), torso_point_of(f, 2))])) do
    Ok((_, evs)) -> Test.assert_true(List.any(evs, fn e -> part_hit_on(e, ally_id())), "friendly fire")
    Err(e) -> Test.fail(e)
  end
end
test "a volley exactly at the heat cap is allowed; one over is refused unchanged" do
  let at = with_heat_left(facing_pair(), volley_heat())
  Test.assert_true(Result.is_ok(SrF.step(at, both_weapons(at))), "at the cap")
  Test.assert_true(Result.is_err(SrF.step(with_heat_left(facing_pair(), volley_heat() - 1), both_weapons(at))), "over")
end
test "shooting off the core takes the mech out" do
  Test.assert_eq_int(state_of(fire_until_core_lost(facing_pair()), 2), 2, "out")
end
test "a bank cut away drops what it held as rubble" do
  match SrG.carve(overhang_world(), 10, 4, 10, 2, 1, 8) do
    (w, _, fell) -> Test.assert_true(!List.is_empty(fell) && nothing_hangs(w), "fell, nothing left hanging")
  end
end
```

- [ ] **Step 2: Run them to see them fail**.
- [ ] **Step 3: Implement `SteelRain.Ground` and the fire order** as the Interfaces say.
- [ ] **Step 4: Run the game's tests and lint**.
- [ ] **Step 5: Commit** `feat(steel_rain): volleys through terrain and parts, delay and heat`

### Task 7: Mechs falling, wrecks and debris

**Files:**
- Modify: `games/steel_rain/lib/fight.march`
- Test: `games/steel_rain/test/fall_test.march`

**Interfaces:**
- Consumes: `Ground.drop`, `Grid.refresh`, `Rules.numbers`.
- Produces, in `SteelRain.Fight` (run after every change to the world inside `step`):
  - `footing(f : Fight, id : Int) : Fight` (with its events appended by the caller) — the spec's rule: tile floor dropped → fall to the new floor; tile no longer holds it (fewer than half its 64 columns within one step of its old floor, or no floor within its height) → slide to the lowest standable neighbour on the side the ground was lost (the neighbour whose floor dropped most; ties by direction order +x, −x, +z, −z) and fall to its floor; an occupied landing tile → the lowest free neighbour of it, and both units take `rn_bump` damage on their cores. A fall of h steps over `rn_fall_safe`: movement node damage `rn_fall_per_step × (h − rn_fall_safe)`, the core as well past `rn_fall_core`, and `rn_fall_ct_per_step × h` CT off (EMechFell(id, tile, h)).
  - Debris: cells dropped by `Ground` landing on a unit's tile damage its core by `rn_debris_per_voxel` × the cells landing there (rounded).
  - Wrecks and lost parts: a mech going out drops its hull's boxes as rubble cells (`Ground.drop` of the voxels of its boxes over its tile); a lost node drops its box the same way.

- [ ] **Step 1: Write the failing tests** (fixtures: `on_bank()` an enemy on a raised bank 3 steps over the shooter's ground; `on_bank_neighbour_held()` the same with a third mech on the tile below the bank)

```march
test "cutting the bank from under a mech drops it, hurt and delayed" do
  match SrF.step(on_bank(), fire_at_bank(on_bank())) do
    Ok((f2, evs)) -> Test.assert_true(List.any(evs, is_mech_fell) && floor_of_mech(f2, 2) < floor_of_mech(on_bank(), 2) && ct_of(f2, 2) < ct_of(on_bank(), 2) && legs_armour(f2, 2) < legs_armour(on_bank(), 2), "fell, delayed, legs hurt")
    Err(e) -> Test.fail(e)
  end
end
test "a mech falling onto another's tile takes a free one, both bumped" do
  match SrF.step(on_bank_neighbour_held(), fire_at_bank(on_bank_neighbour_held())) do
    Ok((f2, _)) -> Test.assert_true(tile_of(f2, 2) != tile_of(f2, 3) && core_armour(f2, 3) < core_armour(on_bank_neighbour_held(), 3), "apart, bumped")
    Err(e) -> Test.fail(e)
  end
end
test "a wreck becomes rubble on its tile" do
  let f = fire_until_core_lost(facing_pair())
  Test.assert_true(rubble_on_tile(f, tile_of(facing_pair(), 2)) > 0, "rubble")
end
```

- [ ] **Step 2: Run them to see them fail**.
- [ ] **Step 3: Implement footing, falls, debris and wreck drops** as the Interfaces say.
- [ ] **Step 4: Run the game's tests and lint**.
- [ ] **Step 5: Commit** `feat(steel_rain): mechs fall when their ground goes; wrecks and debris`

### Task 8: Moving, watches, bracing and withdrawing

**Files:**
- Modify: `games/steel_rain/lib/fight.march`
- Test: `games/steel_rain/test/move_test.march`

**Interfaces:**
- Consumes: `StrataTactics.Reach.find`/`path`, `Rules.mover`, `StrataTactics.Watch`, Task 6's single-weapon fire.
- Produces, in `step`:
  - `OMove(path)`: legal when not yet moved and the path equals `Reach.path` to its last tile (allies pass-through, enemies blocked, mover from `Rules.mover`; an immobilised mech cannot move). Walked tile by tile (EMoved); `rn_move_heat` heat per move; facing follows each step; on entering each tile, `Watch.triggered` with the side-1-vs-0 rule (only enemies' watches, whose owner can see the tile by `Sight.line` clear or partial within its torso's `"sight"` metres) fires each triggered watch's weapon at the mover's torso (a full shot as in Task 6, EWatchFired), and removes it; if the mover is out or immobilised after that, the walk stops there.
  - `OWatch(weapon, dir)`: an act; arc width `rn_watch_width`, range `rn_watch_range` tiles; replaces the unit's previous watch (EWatchSet). A watch lasts until its owner's next turn starts.
  - `OBrace`: an act; delay on it halved until its next turn.
  - `OWithdraw`: legal on a tile of its side's spawn site; state withdrawn, off the clock (EState).

- [ ] **Step 1: Write the failing tests** (fixtures: `open_pair()` a side-0 mech with a clear path of 6 tiles; `lane_watched()` the same with a side-1 mech holding a watch over tile 3 of that path; `lane_watched_twice()` two such watches; `lane_lethal()` a watch whose shot takes the mover out)

```march
test "a mech moves along a reachable path and gets warm" do
  match SrF.step(open_pair(), SrF.OMove(path6())) do
    Ok((f2, _)) -> Test.assert_true(tile_of(f2, 1) == last(path6()) && heat_of(f2, 1) > 0, "there, warm")
    Err(e) -> Test.fail(e)
  end
end
test "a path it cannot walk is refused" do
  Test.assert_true(Result.is_err(SrF.step(open_pair(), SrF.OMove(teleport_path()))), "refused")
end
test "walking into a watched lane draws fire once; two watches are a crossfire" do
  Test.assert_eq_int(watch_shots(SrF.step(lane_watched(), SrF.OMove(path6()))), 1, "one watch fires once")
  Test.assert_eq_int(watch_shots(SrF.step(lane_watched_twice(), SrF.OMove(path6()))), 2, "both fire")
end
test "a mover knocked out by watch fire stops where it was hit" do
  match SrF.step(lane_lethal(), SrF.OMove(path6())) do
    Ok((f2, _)) -> Test.assert_true(state_of(f2, 1) == 2 && tile_of(f2, 1) == List.nth(path6(), 3), "out on tile 3")
    Err(e) -> Test.fail(e)
  end
end
test "a mech on its own spawn edge withdraws" do
  match SrF.step(on_spawn(), SrF.OWithdraw) do
    Ok((f2, _)) -> Test.assert_eq_int(state_of(f2, 1), 1, "withdrawn")
    Err(e) -> Test.fail(e)
  end
end
```

- [ ] **Step 2: Run them to see them fail**.
- [ ] **Step 3: Implement the four orders** as the Interfaces say.
- [ ] **Step 4: Run the game's tests and lint**.
- [ ] **Step 5: Commit** `feat(steel_rain): moving, watches, bracing and withdrawing`

### Task 9: Emplacements, the Destroy mission and results

**Files:**
- Create: `games/steel_rain/lib/missions.march`
- Modify: `games/steel_rain/lib/fight.march`
- Test: `games/steel_rain/test/mission_test.march`

**Interfaces:**
- Consumes: `StrataMaps.Sites.SiteRule`, `Strata.Edit.stamp`, Task 6–8.
- Produces:
  - `SteelRain.Missions.destroy_rules() : List(StrataMaps.Sites.SiteRule)` — `"spawn north"`, `"spawn south"`, `"battery"` (3 single-tile sites, high, far from the north spawn by 10), `"supply"` (1 site of 2 × 2 tiles, in the settlement or open, far from the north spawn by 12).
  - `type Mission = MNone | MDestroy(List(Int), List((Int, Int, Int)), Int)` — emplacement ids, the supply structure's cells, how many cells it started with (Result_ is Task 5's).
  - `SteelRain.Missions.supply_structure() : Strata.Template.Template` — a small depot (about 2 × 2 tiles, 3 m tall) in code.
  - `Fight.start_destroy(d, w, parts, side0, seed) : Fight` — as `start`, side 1 being one emplacement per `"battery"` site (kind 1, one node, the emplacement gun: `Rules.weapon(3)`, armour from Rules, speed from Rules) and the depot stamped on the `"supply"` site; side-1 mechs optional (an extra list).
  - Emplacement turns are automatic inside `step`: when an emplacement's turn starts, it fires at the nearest side-0 mech it can see within its arc (arc: facing the north spawn, `rn_watch_width`), else holds a watch over that arc; then its turn ends. `step` keeps advancing while the active unit is an emplacement, so the caller only ever sees mech turns.
  - The win check after every step: Destroy is won when every emplacement is out, the depot has lost at least half its cells, and every side-0 mech still in is withdrawn or no side-1 mech is in; lost when no side-0 mech is in and it is not won (EEnded; `fg_result` set; later orders refused).

- [ ] **Step 1: Write the failing tests** (fixture: `destroy_small()` a 16-tile map from `destroy_rules()` with two Wardens)

```march
test "a Destroy fight has the battery and the depot on their sites" do
  let f = destroy_small()
  Test.assert_eq_int(count_kind(f, 1), 3, "three emplacements")
  Test.assert_true(depot_cells(f) > 0, "the depot stands")
end
test "emplacements act on their own: the player only sees mech turns" do
  Test.assert_true(List.all(turns_seen(destroy_small(), 12), fn id -> kind_of(destroy_small(), id) == 0), "only mechs")
end
test "knocking out the battery and half the depot, then withdrawing, wins" do
  Test.assert_true(won(play_destroy_script(destroy_small())), "won")
end
test "losing every mech loses" do
  Test.assert_true(lost(all_out(destroy_small())), "lost")
end
```

- [ ] **Step 2: Run them to see them fail**.
- [ ] **Step 3: Implement the mission, emplacements and the win check** as the Interfaces say. `play_destroy_script` is a test helper that issues legal orders (fire at the emplacements and depot, walk back, withdraw); it may take many orders.
- [ ] **Step 4: Run the game's tests and lint**.
- [ ] **Step 5: Commit** `feat(steel_rain): emplacements, the Destroy mission and results`

### Task 10: The screen: playing a fight by hand

**Files:**
- Modify: `games/steel_rain/lib/steel_rain.march` (the prototype becomes the fight screen)
- Create: `games/steel_rain/lib/screen.march` (views and playback, kept apart from the loop)
- Modify: `games/steel_rain/lib/walk.march` if the playback reuses it
- Test: `games/steel_rain/test/screen_test.march` (the parts with no window: event playback queue, view lists)

**Interfaces:**
- Consumes: everything above; `StrataTactics.Marks`, `StrataTactics.Camera`, `Strata.Overlay`, `StrataKit.Damage.carve`/`break_off`, `Strata.Gui` text as the tactics example uses.
- Produces:
  - `SteelRain.Screen.queue(evs : List(Fight.Event)) : List(Beat)` and `Screen.beat_done(...)` — events turned into timed beats (walk a tile, a tracer per round, carve, fall, part hit, part lost, delay tick); tested without a window: a volley's events give one tracer beat per round, then the carves, in order.
  - `Screen.cone_items(f : Fight, id : Int, weapon : List(String), aim : (Float, Float, Float)) : List(Strata.Overlay.Item)` — the cone from the muzzle and the `Shot.preview` shares drawn on what they strike (allies in the cone in a warning colour).
  - The loop: the active mech is shown; hover a tile → reach and path; click → `OMove`; number keys pick weapons into the volley, click a target → aims; Enter → `OFire`; W → watch (mouse sets direction); B brace; X withdraw (on the spawn edge); Space → `OEnd` with facing towards the pointer. The turn-order bar shows `Clock.forecast` with delays as they land; a panel shows the active mech's heat against warn and cap and each node's armour. Both sides are played by the person at the keyboard (side 1's turns tinted). The world shown is kept in step by applying the fight's `ECarved`/`EFell` changes to the streamed world as edits, never by recomputing them.
  - Scripted mode (as the prototype's): `STRATA_FRAMES=n` plays a fixed order list (`STRATA_SCRIPT` default: move, volley, end) and writes frame n − 3 to `STRATA_SHOT`; `STRATA_MAP_SEED` picks the map; the Destroy mission is the default.

- [ ] **Step 1: Write the failing tests** (alias SrS)

```march
test "a volley plays as one tracer per round, then the carves, in order" do
  let evs = volley_events(facing_pair())
  let beats = SrS.queue(evs)
  Test.assert_eq_int(count_tracers(beats), count_rounds(evs), "one tracer a round")
  Test.assert_true(tracers_before_carves(beats), "in order")
end
test "the cone view marks an ally inside it" do
  Test.assert_true(has_warning_item(SrS.cone_items(ally_between(facing_pair()), 1, arm_path(), torso_point_of(facing_pair(), 2))), "warned")
end
```

- [ ] **Step 2: Run them to see them fail**.
- [ ] **Step 3: Implement the screen** as the Interfaces say.
- [ ] **Step 4: Run the game's tests, lint, and a scripted run**

Run: `forge test && forge lint --strict && forge build --release && STRATA_OFFSCREEN=1 STRATA_FRAMES=240 STRATA_SHOT=docs/steel-rain-fight.png MARCH_PIN_MAIN=1 ./.march/build/release/steel_rain`
Expected: tests pass, lint clean, the run exits 0 and the picture shows the battery, the depot, mechs, a cone or tracers and carved ground.

- [ ] **Step 5: Commit** `feat(steel_rain): the fight screen, both sides by hand` (with `docs/steel-rain-fight.png`)

### Task 11: A whole fight headless, and its speed

**Files:**
- Create: `games/steel_rain/test/whole_fight_test.march`
- Create: `games/steel_rain/bench/` (forge package, release build: times `start_destroy` and a scripted fight to the end)
- Modify: `docs/battle-ledger.md`

**Interfaces:**
- Consumes: Tasks 5–9.

- [ ] **Step 1: Write the test** — `"a scripted Destroy fight plays to a result with no illegal order"`: from `destroy_small()`, a scripted player (the test's helper: each mech fires its weapons at the nearest emplacement or depot cell it can reach within range, else moves towards the battery, else ends; withdraws once the mission's targets are down) runs until `fg_result` is set or 400 orders; asserts a result was reached and no `step` returned `Err`.
- [ ] **Step 2: Run it**; if it fails, the fight code is wrong: fix it (systematic-debugging), not the test.
- [ ] **Step 3: The bench**: a 64-tile Destroy map, two Wardens, the same scripted player; prints the setup time and the whole fight's time. Expected under a minute (the spec's headless AI-against-AI budget, used here for a scripted fight); a slower result is profiled (`sample`) and fixed under the performance-first rule (March gaps get a reproducer and a GAPS.md entry).
- [ ] **Step 4: Run every suite**: the game's, `strata_tactics`', `strata_maps`', the root's, and `scripts/verify.sh`.
- [ ] **Step 5: Commit** `test(steel_rain): a whole Destroy fight headless; bench` with the ledger's timings.

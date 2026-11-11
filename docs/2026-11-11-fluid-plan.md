# Strata Fluids Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Finite water that flows downhill, pools, drains to the sea and empties a breached lake, drawn at its true level, with spray where it falls.

**Architecture:** A fluid is a family of eight block states, one per fill level. `Strata.Fluid` is a pure module: it keeps a set of active cells, and each step reads the world, computes moves, and returns one `Strata.Edit` outcome, so meshing, lighting and collision need no fluid path. The mesher writes a fluid quad's level into four spare bits of the quad and the vertex shader lowers the surface. Spray is a ring buffer of particles whose paths the vertex shader computes from the clock.

**Tech Stack:** March (origin/main via `scripts/env.sh`), the C Vulkan shim `native/strata_vk.c`, GLSL built by `native/build_shaders.sh`.

**Spec:** `docs/2026-11-10-fluid-design.md`

## Global Constraints

- Work on a new branch `fluids` cut from `lighting`. Commit after each task.
- `scripts/verify.sh` prints PASS at the end of every task.
- Every `test/*_test.march` module needs `needs IO.Clock` and `needs IO.Process` when it uses `Check.all`; aliases must be unique across the test suite (GAPS S1 to S14).
- After any shader edit run `native/build_shaders.sh` (it stamps `strata_vk.c`; GAPS S14).
- Golden images are looked at before they are stored. Existing goldens must still pass unchanged unless a task says otherwise.
- Eight fill levels to a voxel; level 8 is full. Amounts are counted in eighths.
- Defaults: 10 fluid steps a second, 4096 active cells a step, a way-down search of 8 cells, a fall of at most 4 cells a step.
- Deterministic for the same world, edits and focus points. No Vulkan type crosses into March.
- Machine timings are only trusted when `uptime` shows a load average under 8; otherwise record them as not judged.

## Review Focus

1. Water at a chunk border and at the edge of the loaded world: a move into an unloaded chunk must not happen and must not lose water.
2. Two cells moving into the same cell in one step: the receiver must not exceed 8 and nothing may be lost.
3. An edit that removes the block under still water far from any active cell: the water above must wake.
4. A sink below, beside and above flowing water; a sink must never change.
5. A huge edit (a 32-cube box) next to water: waking must stay bounded and must still find the water on the box's faces.

---

### Task 1: Fluid families and levels in the registry and mesher

**Files:** `lib/strata/blocks.march`, `lib/strata/quad.march`, `lib/strata/mesh.march`, `test/blocks_test.march`, `test/mesh_test.march`

**Interfaces — Produces:**
```
Strata.Blocks.register_fluid(r : Registry, d : Def) : Result((Registry, Int), String)   -- eight states; the base id is level 1, base + 7 is level 8
Strata.Blocks.fill(r : Registry, id : Int) : Int        -- 1..8 for a fluid state; 8 for any other block; 0 for air and unregistered ids
Strata.Blocks.is_fluid(r : Registry, id : Int) : Bool
Strata.Blocks.fluid_base(r : Registry, id : Int) : Int  -- the family's base id, or -1
Strata.Quad word0 bits 28-30: drop = 8 - fill (0 for a full or non-fluid quad)
```

- [ ] **Step 1: failing tests** in `test/blocks_test.march`: `register_fluid` returns a base whose eight ids have fills 1..8, `is_fluid` true, `fluid_base` the base; a plain block has fill 8 and `is_fluid` false; id 0 has fill 0; all eight states share one light class.
- [ ] **Step 2:** run `forge test --filter blocks`; expect FAIL (functions missing).
- [ ] **Step 3:** implement. The fill lives in the registry's per-id ints.
- [ ] **Step 4: failing tests** in `test/mesh_test.march`, on single-chunk worlds:
  - a lone level-3 fluid voxel emits six quads, each with drop 5;
  - a level-8 voxel beside a level-3 voxel of the same family: no face on the level-3 side toward the level-8 one, one face on the level-8 side toward the level-3 one;
  - fluid under the same family (a column) emits no top face on the lower voxel and drop 0 on the lower voxel's sides ("falling water draws full height");
  - a non-fluid quad has drop 0, and every existing mesh test still passes.
- [ ] **Step 5:** run; expect FAIL. Implement in the mesher: a face between two states of one family is drawn only from the higher level; a fluid voxel with the same family above it meshes as full.
- [ ] **Step 6:** `forge test`; expect all pass. `scripts/verify.sh` PASS. Commit `feat(fluid): fluid families, fill levels, and level-aware meshing`.

### Task 2: The rules, headless

**Files:** `lib/strata/fluid.march` (new), `lib/strata.march`, `test/fluid_test.march` (new)

**Interfaces — Produces:**
```
type Strata.Fluid.Fluid
Strata.Fluid.flowing() : Int = 0;  Strata.Fluid.sink() : Int = 1
Strata.Fluid.new(r : Registry, families : List((Int, Int))) : Fluid          -- (base id, kind)
Strata.Fluid.with_limits(f : Fluid, cap : Int, reach : Int, fall : Int) : Fluid   -- defaults 4096, 8, 4
Strata.Fluid.wake_cell(f : Fluid, x : Int, y : Int, z : Int) : Fluid
Strata.Fluid.step_world(f : Fluid, w : World, focus : List((Float, Float, Float))) : (Fluid, Strata.Edit.Outcome)
Strata.Fluid.active_count(f : Fluid) : Int
Strata.Fluid.amount(w : World, r : Registry, x0, y0, z0, x1, y1, z1 : Int) : Int   -- eighths of flowing fluid in a box (a test and measurement helper)
```
`step_world` reads the world as it stands, decides every move from that state, resolves moves into one cell in the fixed order down, -x, +x, -z, +z (a receiver takes from each in turn until it holds 8; what does not fit stays with the giver), and returns a single `Strata.Edit.set_many` outcome. Cells whose amount did not change and whose six neighbours did not change leave the active set; every changed cell and its six neighbours join it.

- [ ] **Step 1: failing tests**, each a hand-built world from a `Strata.Terrain.gen` of stone and air with fluid placed by edits, all cells woken:
  - **falls:** level 8 over air: after one step the cell below holds 8 and the cell is air;
  - **falls at most `fall` cells a step** and reports the same total;
  - **fills from below:** level 8 over level 5 gives 8 below and 5 above;
  - **spreads:** one level-8 voxel on a stone floor levels out to neighbours and the total is still 8;
  - **prefers a way down:** on a floor with an edge 3 cells away in +x, after enough steps all 8 eighths are below the edge and none went -x further than 1 cell;
  - **the film stays:** a single eighth on a flat floor wider than the reach is unchanged after 50 steps and `active_count` is 0;
  - **a hollow keeps its puddle:** water poured into a pit stays in the pit, at rest;
  - **sink:** flowing water above, beside and falling onto a sink block disappears; the sink's ids never change;
  - **two into one (Review Focus 2):** two level-8 cells either side of an empty cell over a pit: no cell ever exceeds 8.
- [ ] **Step 2: property tests** with `Check.all` over random heightmaps (16 x 16, heights 0..6), random water placements and 60 steps:
  - the amount is conserved exactly when there is no sink;
  - no cell exceeds 8 and no fluid sits above air at rest;
  - every case reaches `active_count == 0` within 400 steps;
  - two runs give equal `Strata.World` hashes step for step.
- [ ] **Step 3:** run `forge test --filter fluid`; expect FAIL (module missing).
- [ ] **Step 4:** implement `Strata.Fluid`. The active set is a `Strata.Store`-style persistent map keyed by cell; moves are computed into a second map and folded into one cell list.
- [ ] **Step 5:** `forge test`; all pass. `scripts/verify.sh` PASS. Commit `feat(fluid): the rules — fall, spread toward a way down, sink, rest`.

### Task 3: Waking, the cap, and the loaded edge

**Files:** `lib/strata/fluid.march`, `test/fluid_test.march`

**Interfaces — Produces:**
```
Strata.Fluid.wake(f : Fluid, w : World, o : Strata.Edit.Outcome) : Fluid   -- wakes fluid in and around what the edit changed
Strata.Fluid.waiting_count(f : Fluid) : Int      -- active cells a step left for later
Strata.Fluid.dormant_count(f : Fluid) : Int      -- active cells set aside in chunks that are not loaded
Strata.Fluid.events(f : Fluid) : List(Event)     -- what moved in the last step
Strata.Fluid.ev_x / ev_y / ev_z (e) : Int;  ev_eighths(e) : Int;  ev_fell(e) : Int;  ev_landed(e) : Bool
```
`wake` looks at each change record's bounds grown by one cell: every cell of it when the bounds hold at most 4096 cells, otherwise only the six faces of the grown box. An event is emitted for each cell that received falling water: where, how many eighths, how many cells that water has fallen without landing (carried per active cell), and whether it landed (the cell below is not air).

- [ ] **Step 1: failing tests:**
  - **an edit wakes water (Review Focus 3):** still water on a stone slab, nothing active; remove a slab block with `Strata.Edit.set_block`, call `wake`, step until rest: the water is below;
  - **a big edit (Review Focus 5):** a 32-cube `fill_box` of air beside a wall of water wakes at most 6 x 34 x 34 cells and the water flows in;
  - **the cap:** 100 falling cells with a cap of 10: one step moves 10 (the nearest the focus), `waiting_count` is 90, and after 10 steps all have moved; the amount is conserved throughout;
  - **the loaded edge (Review Focus 1):** water flowing toward a chunk that is not loaded stops at the border, `dormant_count` is above 0, the amount is conserved; insert the chunk and step: the water crosses;
  - **unload and return:** unload the chunk a falling cell is in, step 10 times, reload: it resumes and lands;
  - **events:** a level-8 cell dropped 6 cells onto stone yields a landing event with `ev_eighths` 8 and `ev_fell` 6.
- [ ] **Step 2:** run; expect FAIL. Implement.
- [ ] **Step 3:** `forge test`; all pass. `scripts/verify.sh` PASS. Commit `feat(fluid): waking from edits, a per-step cap, the loaded edge, events`.

### Task 4: Water moves in the viewer

**Files:** `lib/strata/fluid.march`, `viewer/lib/strata_viewer.march`, `viewer/lib/textures.march`, `viewer/test/walker_test.march`, `scripts/golden.sh`

**Interfaces — Produces:**
```
Strata.Fluid.step(f : Fluid, s : Stream, focus : List((Float, Float, Float))) : (Fluid, Stream)   -- step_world, then Strata.Stream.apply
viewer block ids: 5 becomes the sea (a sink, one id, drawn as before); lake water is a fluid family registered after ice
env STRATA_FLUID=0 turns stepping off; STRATA_BREACH=f breaches the test lake at frame f; STRATA_DRAIN=f opens its floor
viewer scenes 18 (the test lake from its bank) and 19 (the valley below it)
```
The viewer's terrain gains lakes above sea level: on a 192-cell grid, a cell whose hashed centre stands between heights 50 and 68 gets a round basin (radius 14 to 28, a bowl up to 16 deep) filled to one below the centre's height, with a rim raised to at least one above the water. The **test lake** is the first such lake in a fixed walk about the origin for seed 7 with radius at least 26; it holds about 20 000 voxels. The viewer steps the fluid ten times a second on the frame clock (scripted runs: every sixth frame), wakes it on every edit it makes, and prints `fluid: active a, waiting w, dormant d, step ms` with its periodic line.

- [ ] **Step 1: failing viewer tests:** terrain is still a pure function with lakes (same inputs, same chunk); a lake's rim encloses it (no fluid cell has air beside it at or below the water line); the sea id is not a fluid.
- [ ] **Step 2:** implement the lakes, the registry change and the stepping. Mode-0 goldens that show the sea must still pass unchanged; scenes with a lake in view are listed and re-stored after looking.
- [ ] **Step 3: scenes.** Scene 18 at frames 60, 360 and 1200 with `STRATA_BREACH=30` (the carve sphere of radius 4 at the bank's foot): full lake; a stream running down from the breach with the lake visibly lower; the lake empty with a wet trace at most. Scene 19 at frame 1200: water pooled in the valley or gone to sea. Look at each, then store.
- [ ] **Step 4:** a scripted run with `STRATA_BREACH=30` to frame 7200 prints the lake's remaining amount each 600 frames. Record against the targets: empty (under 2% and no cell above the breach floor holding more than one eighth) within 2 minutes; at rest (no active cells) within 5 seconds of the last change; worst frame; step time.
- [ ] **Step 5:** `scripts/verify.sh` PASS. Commit `feat(fluid): highland lakes, the sea as a sink, water stepping in the viewer`.

### Task 5: Partial levels drawn at their height

**Files:** `native/shaders/quad.vert`, `native/shaders/water.frag`, `native/strata_vk.c` (only if the stamp), `scripts/golden.sh`

- [ ] **Step 1:** in `quad.vert`, read drop = bits 28-30 of word 0. A top face (face 0) is lowered by drop / 8. A side face's upper edge is lowered by drop / 8 and its texture coordinate follows. A bottom face is unchanged.
- [ ] **Step 2:** `water.frag` and the light pass take the surface position from the lowered vertex, so shadows and reflections start from the true surface.
- [ ] **Step 3: scenes.** A hand-built staircase of levels 1 to 8 beside a full block (viewer scene 20): each step one eighth higher, no gaps between neighbours, no face inside the water. Scene 18 at frame 360 re-stored: the draining lake's surface sits below its rim by a fraction of a block where the level says so. Every other golden unchanged (drop is 0 for everything else).
- [ ] **Step 4:** `scripts/verify.sh` PASS. Commit `feat(fluid): partly full water draws at its level`.

### Task 6: Spray

**Files:** `native/strata_vk.c`, `native/shaders/spray.vert` (new), `native/shaders/spray.frag` (new), `native/build_shaders.sh`, `lib/strata/ffi/vk.march`, `lib/strata/render.march`, `viewer/lib/strata_viewer.march`, `viewer/lib/textures.march`, `native/tests/spray_test.c` (new), `scripts/golden.sh`

**Interfaces — Produces:**
```
C     int64_t sv_spray(double x, double y, double z, double vx, double vy, double vz, int64_t count, double spread, double life, int64_t tex)
March Strata.Render.spray(x, y, z, vx, vy, vz : Float, count : Int, spread : Float, life : Float, tex : Int) : Bool
C     sv_stat(8): live particles
```
A ring of 65 536 particles in a host-visible buffer, 32 bytes each: start position, birth time, start velocity, life, texture layer, size. `sv_spray` appends `count` particles (at most 512 a call) with velocities spread about (vx, vy, vz) by a hash of the ring index, born at the time last given to `set_time`. One instanced draw in the final pass, after water, of the ring's live span: the vertex shader places each as a camera-facing square at start + v t + (0, -9.8, 0) t^2 / 2 and collapses it when t is outside 0..life. The fragment shader takes the texture, lights it with the sun and the flat sky term, fades it over the last third of its life, and applies the same tone curve as water. No draw when nothing is live, so scenes without spray still issue their usual draw count.

The viewer turns events into spray: a landing event gives a burst at the landing cell, count = 2 x eighths (at most 64), speed = 1.5 + 0.9 x sqrt(cells fallen) m/s upward and outward, life 0.8 s; a falling event that has not landed gives one mist particle per 4 eighths with life 0.5 s. Texture layer 13 is a soft white dot.

- [ ] **Step 1: failing C test** `native/tests/spray_test.c` on the ring logic factored into `native/strata_spray.h` (no GPU): appending wraps; the live span is the particles younger than the longest life; a count over 512 is clamped; a full ring overwrites the oldest.
- [ ] **Step 2:** run it through `scripts/verify.sh`'s C test loop; expect FAIL. Implement the header, the shim, the shaders and the bindings.
- [ ] **Step 3: scenes.** Scene 21: a column of water released 12 cells above stone at frame 30, shots at frames 48 and 66: a burst where it lands, larger than the one from a 3-cell drop beside it. Scene 18 at frame 360 with spray along the breach's fall. Look at each, then store.
- [ ] **Step 4:** measure: 50 000 live particles (a scripted fountain) offscreen at 2560 x 1600 against the 120 fps target; GPU time with and without.
- [ ] **Step 5:** `scripts/verify.sh` PASS. Commit `feat(fluid): spray — a particle ring flown by the clock, fed by fluid events`.

### Task 7: Soak, measurements, documentation

**Files:** `scripts/soak.sh`, `viewer/lib/strata_viewer.march`, `RESULTS.md`, `README.md`, `GAPS.md`, `docs/`

- [ ] **Step 1:** the soak breaches the test lake at frame 300 and flies its circuit over it; it adds assertions that the fluid's amount never grows, that no step exceeded the cap, and prints the worst and mean fluid step time.
- [ ] **Step 2:** `RESULTS.md`: every row of the design's target table, met or missed, with the cause where missed, and the lighting cost while water moves (GPU time in scene 18 at frame 360 against the same scene at rest).
- [ ] **Step 3:** README: what fluids do, the `Strata.Fluid` calls a game makes each frame, the limits (no pressure, eight levels, loaded chunks only). Screenshots of the breached lake and a waterfall with spray into `docs/`.
- [ ] **Step 4:** `scripts/verify.sh` PASS. Commit `docs(fluid): results, README, soak with a draining lake`.

---

## Out of scope for this plan

Pressure, waves, more than one interacting fluid, rain and springs, evaporation, erosion, colliding particles, a save format, the far field.

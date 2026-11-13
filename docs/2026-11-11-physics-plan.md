# Strata Physics Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rigid voxel bodies that fall, tumble, collide, break by force and material, and come to rest tilted; terrain that comes loose when it loses support; bodies that dam water and sink in it; all drawn, shadowed and lit like the world, at 1 m and 10 cm voxels.

**Architecture:** `Strata.Physics` is a value the game keeps beside its stream and fluid. A step is a pure function of the world, the body state and the inputs handed to it; it runs on a worker (`begin`/`finish`, as the fluid does) over native arrays the job owns, and is never dropped. Everything it does to the world (loose terrain, broken blocks, snapped fragments) is one `Strata.Edit` outcome, so meshing, lighting and fluids follow. The renderer draws body parts in slots that carry a transform, the G-buffer carries a true normal, and rays step into each body's own voxel grid.

**Tech Stack:** March (origin/main via `scripts/env.sh`), the C Vulkan shim `native/strata_vk.c`, GLSL built by `native/build_shaders.sh`.

**Spec:** `docs/2026-11-11-physics-design.md`

## Global Constraints

- Branch `physics`, cut from `fluids`. Commit after each task. Execution is native, with one independent review at the end.
- `scripts/verify.sh` prints PASS at the end of every task. Build with March origin/main through `scripts/env.sh`; never install over `~/.opam/march`.
- Every scripted viewer run (goldens, soak, timings) uses `STRATA_HIDDEN=1 STRATA_OFFSCREEN=1 STRATA_PIXELS=1`. No window opens on the owner's screen.
- GAPS S1 to S15 apply: no `opaque`, `by`, `on`, `loop` as names; `needs IO.Clock` and `needs IO.Process` in every test module; aliases unique across the whole suite (use `Ph`, `Bd`, `Sp`, `Fr` for the new modules, checked against existing ones); full module paths for Unit calls; no shared value passed through a hot loop (read registry fields into task-owned arrays once per step).
- After any shader edit run `native/build_shaders.sh` (GAPS S14).
- Golden images are looked at before they are stored. Existing goldens must pass unchanged unless a task says otherwise.
- Units: metres, seconds, kilograms. Step 1/60 s. Gravity 9.8 m/s². Solver iterations 8. Awake cap 256. Total cap 4 096. Support off by default. Fragment: fewer than 8 voxels. Body grid at most 128 voxels a side. Support fill cap 200 000 voxels. Material defaults: density 2000 kg/m³, strength 50 000 J/m², span 4.0 m, piece size 0.5 m, not an anchor.
- Per-voxel and per-contact work runs on native arrays owned by one task. No `Strata.Store` or other persistent map inside a per-voxel or per-contact loop.
- Deterministic: the same world, edits and inputs at the same steps give equal `Strata.Physics.hash` every step. Nothing in a step may depend on worker timing.
- No Vulkan or C type crosses into March.
- Machine timings are trusted only when `uptime` shows a load average under 8; otherwise they are recorded as not judged.
- Rulings made while building go in `docs/physics-ledger.md`, in the form `docs/fluid-ledger.md` uses: the ruling, why, and what it costs.
- Physics depends on nothing from interface and text, audio, the far field, the save format or the cost pass. If a task finds it must, stop and say so.

## Review Focus

1. Ground dug out from under sleeping rubble: the rubble must wake and fall, not hang in the air. (Task 2 test "an edit under a sleeping body wakes it".)
2. A body resting on another body that is removed, snapped or broken: the upper one must wake and fall. (Task 3 test "a body on a body that leaves wakes".)
3. The same scene at 1 m and at 10 cm voxels: the motion in metres must match. A builder testing only 1 m will tune constants that are wrong at 10 cm. (Task 2 test "scale does not change the motion".)
4. A driven body (the player) standing on a body that starts to move or fall: it must ride it or fall with it, not float or sink into it. (Task 4 test "a driven body rides a moving body".)
5. A body that straddles a chunk border where one side is not loaded: it must not fall through the unloaded side, and must resume when it loads. (Task 2 test "the loaded edge".)

---

### Task 1: Materials and body grids

**Files:**
- Modify: `lib/strata/blocks.march`
- Create: `lib/strata/body.march`
- Test: `test/blocks_test.march`, `test/body_test.march` (new)

**Interfaces — Produces:**
```
Strata.Blocks.with_physics(d : Def, density : Int, strength : Int, span_cm : Int, piece_cm : Int, anchor : Bool) : Def   -- kg/m³, J/m², centimetres
Strata.Blocks.density(r : Registry, id : Int) : Int      -- 2000 default; 0 for any block that does not collide
Strata.Blocks.strength(r : Registry, id : Int) : Int     -- 50000 default
Strata.Blocks.span_cm(r : Registry, id : Int) : Int      -- 400 default
Strata.Blocks.piece_cm(r : Registry, id : Int) : Int     -- 50 default
Strata.Blocks.anchors(r : Registry, id : Int) : Bool
Strata.Blocks.max_span_cm(r : Registry) : Int            -- largest span of any registered colliding block

type Strata.Body.Grid                                     -- a dense box of block ids, each side 1..128
Strata.Body.grid(sx : Int, sy : Int, sz : Int) : Grid     -- all air
Strata.Body.grid_set(g : Grid, x : Int, y : Int, z : Int, id : Int) : Grid
Strata.Body.grid_get(g : Grid, x : Int, y : Int, z : Int) : Int     -- 0 outside the box
Strata.Body.size_x(g) / size_y(g) / size_z(g) : Int
Strata.Body.solid_count(r : Registry, g : Grid) : Int
Strata.Body.from_world(w : World, cells : List((Int, Int, Int))) : (Grid, Int, Int, Int)   -- the cells' blocks in their bounding box; the box's low corner in world voxels
type Strata.Body.Mass
Strata.Body.mass_of(r : Registry, g : Grid, scale : Float) : Mass
Strata.Body.mass_kg(m : Mass) : Float
Strata.Body.centre(m : Mass) : (Float, Float, Float)      -- metres from the grid's low corner
Strata.Body.inertia(m : Mass) : (Float, Float, Float, Float, Float, Float)   -- xx yy zz xy xz yz about the centre, kg m²
Strata.Body.surface(r : Registry, g : Grid) : NativeIntArr   -- indices x + sx * (y + sy * z) of colliding voxels with a non-colliding face neighbour or on the box face
Strata.Body.split(r : Registry, g : Grid) : List((Grid, Int, Int, Int))   -- face-connected colliding components, each in its own bounding box, with its offset in the parent grid
```
The physics values live in the registry's per-id ints beside the existing ones (grow the stride). A `Grid` is three sizes and one `NativeI32Arr`.

- [ ] **Step 1: failing tests** in `test/blocks_test.march`: a plain `def()` block reads density 2000, strength 50000, span 400, piece 50, not an anchor; `with_physics(def(), 2500, 2000, 600, 20, true)` reads back exactly; a non-colliding block reads density 0; `max_span_cm` of a registry holding spans 400 and 600 is 600; every existing blocks test still passes.
- [ ] **Step 2: failing tests** in `test/body_test.march`:
  - a 2 x 2 x 2 stone grid at scale 1.0: mass 16000.0, centre (1.0, 1.0, 1.0), inertia diagonal 16000 * 4 / 6 = 10666.67 (within 0.01), off-diagonal 0.0;
  - the same at scale 0.1: mass 16.0, centre (0.1, 0.1, 0.1), inertia diagonal 0.10667 (within 1e-5);
  - an L of three voxels: inertia equals brute force over voxel point masses plus each voxel's own m s² / 6 term, every component within 1e-6 relative;
  - a 3 x 3 x 3 solid grid has 26 surface voxels; a grid with a plant voxel counts it in neither mass nor surface;
  - `split`: two voxels with air between give two components; two voxels touching only at an edge give two; an L gives one, with offsets that put each voxel back where it was;
  - `from_world` of a 2 x 1 x 1 run of cells gives a 2 x 1 x 1 grid holding the world's ids and the run's low corner.
- [ ] **Step 3:** run `forge test --filter blocks` and `forge test --filter body`; expect FAIL (functions missing).
- [ ] **Step 4:** implement.
- [ ] **Step 5:** `forge test`; all pass. `scripts/verify.sh` PASS. Commit `feat(physics): block materials and body grids with mass and inertia`.

### Task 2: One body against the world

**Files:**
- Create: `lib/strata/physics.march`, `lib/strata/contact.march` (narrow phase and solver over native arrays)
- Test: `test/physics_test.march` (new)

**Interfaces — Consumes:** Task 1's `Grid`, `mass_of`, `surface`.

**Interfaces — Produces:**
```
type Strata.Physics.Config
Strata.Physics.config() : Config                     -- gravity 9.8, 60 steps a second, 8 iterations, awake cap 256, total cap 4096, support off, anchor depth -64 voxels
Strata.Physics.with_gravity(c : Config, g : Float) : Config
Strata.Physics.with_support(c : Config, yes : Bool, anchor_y : Int) : Config
Strata.Physics.with_caps(c : Config, awake : Int, total : Int) : Config
type Strata.Physics.Physics
Strata.Physics.new(r : Registry, c : Config) : Physics
Strata.Physics.dynamic() : Int = 0;  Strata.Physics.driven() : Int = 1
Strata.Physics.add(p : Physics, g : Grid, x : Float, y : Float, z : Float, q : Strata.Math.Quat.Quat, kind : Int) : (Physics, Int)   -- (x, y, z): where the grid's low corner is, metres; at rest
Strata.Physics.remove(p : Physics, id : Int) : Physics
Strata.Physics.push(p : Physics, id : Int, px : Float, py : Float, pz : Float, ix : Float, iy : Float, iz : Float) : Physics   -- impulse in N s at a world point, applied at the next step
Strata.Physics.wake(p : Physics, o : Strata.Edit.Outcome) : Physics   -- the edit is handed to the next step; bodies whose bounds it touches wake
Strata.Physics.step_world(p : Physics, w : World, focus : List((Float, Float, Float))) : (Physics, Strata.Edit.Outcome)   -- exactly one step
Strata.Physics.exists(p, id) : Bool;  is_asleep(p, id) : Bool
Strata.Physics.position(p, id) : (Float, Float, Float)    -- centre of mass, metres
Strata.Physics.rotation(p, id) : Strata.Math.Quat.Quat
Strata.Physics.velocity(p, id) : (Float, Float, Float);  spin(p, id) : (Float, Float, Float)
Strata.Physics.count(p) : Int;  awake_count(p) : Int;  steps(p) : Int
Strata.Physics.hash(p : Physics) : Int                    -- over ids, kinds, sleep flags and every state float as float_to_int(v * 1e9)
type Strata.Physics.Event
Strata.Physics.events(p) : List(Event)                    -- what the last step did
Strata.Physics.ev_kind(e) : Int                           -- 0 impact, 1 break, 2 loose, 3 splash, 4 deferred, 5 spray (GAPS S5: an index, not constructors)
Strata.Physics.ev_body(e) : Int;  ev_other(e) : Int (-1 the world);  ev_x/ev_y/ev_z(e) : Float;  ev_size(e) : Float   -- impulse N s for an impact; voxels for break, loose, spray; speed m/s for a splash
```
The step does, in this order: forces, contacts, solve, integrate, sleep (design §4 items 2 to 6; support and breaking arrive in Tasks 7 and 8). Integration is semi-implicit Euler. A body moving more than half a voxel in a step is sub-stepped. A body sleeps after 30 steps below 0.05 m/s and 0.05 rad/s (thresholds left to the builder; state them in the ledger). A chunk that is not loaded is a wall; a body touching one sleeps and is woken at the first step at which every chunk its bounds touch is loaded. The step reads what it needs from the registry into task-owned arrays once.

- [ ] **Step 1: failing tests**, worlds from a `Strata.Terrain.gen` of stone below y = 0 and air above, the 4 x 4 x 4 chunk box loaded as in `test/fluid_test.march`:
  - **free fall:** a 2 x 2 x 2 crate added at rest high in the air: after n = 30 steps its vertical velocity is -9.8 * 30 / 60 (within 1e-9) and it has dropped 9.8 * (1/60)² * n (n + 1) / 2 (within 1e-9);
  - **lands and sleeps:** the crate dropped from 5 m at scale 1.0 has its lowest voxel within 0.05 m of y = 0 at step 240, never penetrates more than 0.25 voxel at any step, is asleep by step 240, and does not move over 120 more steps;
  - **topples:** a 1 x 4 x 1 column added standing, rotated 10° about z, lies down: at step 300 its long axis has an up component under 0.1 and it is asleep;
  - **friction:** a crate on the ground pushed to 5 m/s along x stops after between 1.7 and 2.6 m (friction 0.6 gives 2.1);
  - **small bounce:** dropped from 5 m, it never rises more than 0.25 m after its first contact;
  - **no tunnelling:** a 1-voxel cube at 30 m/s toward a wall 2 voxels thick stops on the near side, at scale 1.0 and at 0.1;
  - **scale does not change the motion (Review Focus 3):** a 2 x 2 x 2 crate at scale 1.0 and a 20 x 20 x 20 crate at scale 0.1 (both 2 m), dropped from 5 m with the same 10° tilt: centre positions in metres within 2% of each other at steps 30, 60 and 120;
  - **an edit under a sleeping body wakes it (Review Focus 1):** the crate asleep on the ground; remove the 4 x 1 x 4 ground under it with `Strata.Edit.fill_box`, `wake`, step 60: it has fallen at least 0.5 m;
  - **the loaded edge (Review Focus 5):** a crate straddling the border of an unloaded chunk below sleeps and does not fall; insert that chunk (air), step: it falls;
  - **a body outlives its chunk:** a crate asleep on the ground; `Strata.World.unload` its chunk and step 60: it still `exists`, is asleep, and has not moved; insert the chunk again and step: it is unchanged;
  - **impact event:** the landing step yields an event of kind 0 with `ev_other` -1 and size above 0;
  - **determinism:** a `Check.all` property over random heightmaps (16 x 16, heights 0..6) and one to six random bodies: two runs give equal `hash` at every one of 120 steps.
- [ ] **Step 2:** run `forge test --filter physics`; expect FAIL (module missing).
- [ ] **Step 3:** implement `Strata.Physics` and `Strata.Contact`. Contacts: each surface voxel centre of a body, carried into world voxel coordinates, inside a colliding world cell is a contact; the normal is the direction out of the solid taken from the six neighbours of that cell, the depth along it. Solve: sequential impulses with friction 0.6 and restitution 0.1, contacts in the fixed order body id then voxel index, and push-out of overlap limited to 0.2 voxel a step.
- [ ] **Step 4:** `forge test`; all pass. Time one step of the 2 m crate at 10 cm and record it in the ledger (the design's risk: if it is above 4 ms, stop and bring the owner the C-kernel question before going on).
- [ ] **Step 5:** `scripts/verify.sh` PASS. Commit `feat(physics): bodies falling, landing, sliding and sleeping against the world`.

### Task 3: Bodies against bodies, and the caps

**Files:** `lib/strata/physics.march`, `lib/strata/contact.march`, `test/physics_test.march`

**Interfaces — Produces:**
```
Strata.Physics.with_view(p : Physics, ex : Float, ey : Float, ez : Float, dx : Float, dy : Float, dz : Float, cos_half : Float) : Physics   -- the game's camera; "in view" is inside this cone
Strata.Physics.waiting_count(p) : Int     -- awake bodies the last step left for later
Strata.Physics.over_cap(p) : Int          -- bodies above the total cap that could not be snapped
Strata.Physics.to_blocks(p : Physics, w : World, id : Int) : (Physics, Strata.Edit.Outcome)
```
Broad phase: bounds sorted along x. A body's surface points are tested against the other body's grid, carried into its frame. The awake cap orders bodies by distance to the nearest focus point shrunk by steps waited, as `Strata.Fluid` does. `to_blocks` turns the rotation to the nearest of the 24 axis-aligned orientations and rounds the position to whole voxels; it writes the body's blocks into cells that are air or do not collide and drops the rest (counted in the ledger as a known loss); the body is removed. Past the total cap, the step snaps the sleeping bodies farthest from every focus point that are outside the view, until the count is at the cap; awake or in-view bodies are never snapped.

- [ ] **Step 1: failing tests:**
  - **a stack stands:** three 2 x 2 x 2 crates stacked: after 600 steps each moved less than 0.05 voxel from where it settled and all are asleep;
  - **a pile settles:** ten crates dropped in a heap: all asleep within 180 steps (3 s) of the last impact event, and no crate moves in the 120 steps after;
  - **overlap separates gently:** two crates added overlapping by half a voxel: they separate within 10 steps and neither exceeds 2 m/s;
  - **a body on a body that leaves wakes (Review Focus 2):** a crate asleep on a crate; `remove` the lower one; step 60: the upper has fallen at least 0.5 m; the same when the lower is snapped by `to_blocks` out from under it (it lands on the new blocks instead);
  - **the awake cap:** 300 crates in the air with the cap at 256: no step advances more than 256, `waiting_count` is 44 after the first step, and every crate has landed by step 600;
  - **the total cap:** cap 10, twelve sleeping crates, view toward six of them: the two farthest outside the view become blocks (the outcome sets their cells) and `count` is 10; with all twelve in view none is snapped and `over_cap` is 2;
  - **to_blocks:** a crate rotated 40° about y snaps to the 90° orientation nearest; one rotated 44° about x likewise; its block ids appear in the world;
  - the determinism property of Task 2 extended to bodies that touch each other.
- [ ] **Step 2:** run; expect FAIL. Implement.
- [ ] **Step 3:** `forge test`; all pass. `scripts/verify.sh` PASS. Commit `feat(physics): body contacts, stacking, the awake and total caps`.

### Task 4: Off the main thread, the clock, driven bodies

**Files:** `lib/strata/physics.march`, `test/physics_test.march`

**Interfaces — Produces:**
```
type Strata.Physics.Job
Strata.Physics.begin(p : Physics, w : World, focus : List((Float, Float, Float))) : (Physics, Job)   -- the edits, pushes and driven positions handed so far belong to this step
Strata.Physics.finish(j : Job, p : Physics, s : Strata.Stream.Stream) : (Physics, Strata.Stream.Stream)
Strata.Physics.finish_world(j : Job, p : Physics, w : World) : (Physics, Strata.Edit.Outcome)
Strata.Physics.step(p : Physics, s : Strata.Stream.Stream, focus : List((Float, Float, Float))) : (Physics, Strata.Stream.Stream)
Strata.Physics.due(p : Physics, now : Float) : Int          -- steps the clock says are owed, at most 2 (the catch-up limit)
Strata.Physics.behind(p : Physics, now : Float) : Float     -- seconds physics trails the clock
Strata.Physics.pose(p : Physics, id : Int, t : Float) : (Float, Float, Float, Strata.Math.Quat.Quat)   -- the grid's low corner and rotation, between the last two finished steps
Strata.Physics.drive(p : Physics, w : World, id : Int, vx : Float, vy : Float, vz : Float, dt : Float) : (Physics, Strata.Query.Swept)   -- moves a driven body now, against the world and the bodies at their latest poses
Strata.Physics.with_driven_mass(p : Physics, id : Int, kg : Float) : Physics
type Strata.Physics.Contact
Strata.Physics.contacts(p, id) : List(Contact);  ct_other(c) : Int;  ct_x/ct_y/ct_z(c) : Float;  ct_impulse(c) : Float
Strata.Physics.raycast(p : Physics, w : World, ox, oy, oz, dx, dy, dz, max : Float, stop : Int -> Bool) : (Strata.Query.Hit, Int)   -- the nearer of the world hit and any body hit; the body id or -1
Strata.Physics.sweep(p : Physics, w : World, x, y, z, hx, hy, hz, dx, dy, dz : Float) : Strata.Query.Swept   -- against the world and bodies at their latest poses
```
Ruling for the plan: the body-aware raycast and sweep live in `Strata.Physics`, not `Strata.Query`, because `Strata.Physics` already depends on `Strata.Query` and the reverse would be a cycle; record it in the ledger. The job owns the state arrays while it runs; the `Physics` the main thread keeps holds the last two finished poses (for `pose`, `drive`, `sweep`, `raycast`) and the inputs queued for the next `begin`. A step is never dropped. Results made stale by edits handed meanwhile are a Task 7 concern; in this task `finish` applies what the step produced.

- [ ] **Step 1: failing tests:**
  - **the job equals the step:** for the same inputs, `begin` then `finish_world` gives the same `hash` as `step_world`, for 60 steps of the Task 3 pile;
  - **edits belong to the next step:** an edit handed with `wake` after `begin` N does not affect step N's result, and does affect N+1, whether `finish_world` is called at once or after a further `begin` of other work (hashes equal across both orders);
  - **the clock:** `due` at t = 0.05 s after the last step is 2 (three owed, at most 2); `behind` is then 1/60 s;
  - **pose interpolates:** a falling crate's `pose` at the midpoint between two finished steps is the midpoint of their positions (within 1e-9), and its rotation the normalised slerp midpoint;
  - **drive stands on a body:** a driven 0.6 x 1.8 x 0.6 m box driven down onto a sleeping crate rests on its top (within 0.01 m) and is not blocked sideways above it;
  - **drive pushes by mass:** driven mass 80 kg walking at 2 m/s into a 10 kg crate moves it at least 0.5 m in 60 steps; into a 16000 kg crate is blocked and moves it less than 0.01 m; `contacts` of the driven body names the crate;
  - **a driven body rides a moving body (Review Focus 4):** a driven box standing on a crate that is pushed to 2 m/s along x: after 30 steps the driven box is still on top (lowest point within 0.05 m of the crate's top) and moved at least 0.8 m along x without input; a driven box on a crate that starts falling falls with it;
  - **raycast sees a rotated body:** a 4 x 1 x 1 bar rotated 45° about y: a ray along x through where the unrotated bar's far end would be misses, a ray through the rotated bar's middle hits it with the body's id.
- [ ] **Step 2:** run; expect FAIL. Implement.
- [ ] **Step 3:** `forge test`; all pass. `scripts/verify.sh` PASS. Commit `feat(physics): worker steps on a clock, interpolated poses, driven bodies, body-aware queries`.

### Task 5: Drawing bodies, true normals, the 10 cm viewer world

**Files:**
- Modify: `native/strata_vk.c`, `native/shaders/quad.vert`, `native/shaders/cross.vert`, `native/shaders/gbuffer.frag`, `native/shaders/gbuf.glsl`, `native/shaders/light.frag`, `native/shaders/shade.glsl`, `lib/strata/ffi/vk.march`, `lib/strata/render.march`, `lib/strata/physics.march`, `viewer/lib/strata_viewer.march`, `viewer/lib/walker.march`, `scripts/golden.sh`
- Create: `viewer/lib/fine.march` (the 10 cm terrain), `viewer/test/fine_test.march`
- Test: `test/physics_test.march`, `viewer/test/walker_test.march`

**Interfaces — Produces:**
```
Strata.Ffi.Vk.body_begin(slot : Int) : Bool               -- like chunk_begin; a body slot never writes the light volume
Strata.Ffi.Vk.slot_pose(slot : Int, px : Float, py : Float, pz : Float, qx : Float, qy : Float, qz : Float, qw : Float) : Unit   -- the slot's low corner in metres and its rotation; world slots keep the identity
Strata.Physics.take_meshes(p : Physics) : (Physics, List((Int, Int, Strata.Mesh.Meshed)))   -- (body id, part index, mesh) for parts made or changed since the last take
Strata.Physics.part_offset(p : Physics, id : Int, part : Int) : (Int, Int, Int)   -- the part's low corner in the body grid, voxels
Strata.Render.bodies(r : Render, p : Physics, t : Float) : (Render, Physics)    -- uploads new parts, frees parts of removed bodies, sets every body slot's pose at clock t
viewer: STRATA_SCALE=0.1 selects the 10 cm world (stream radius 6 x 3); C drops a crate in front of the camera; scenes 21 crate-drop, 22 tilted-slab, 23 fine-overview
```
A body's grid is cut into 32-cubed parts; each part is meshed by `Strata.Mesh` with an `Around` whose neighbours are the body's other parts (air beyond the grid), on the worker that made or changed the grid (`add` marks a new body for meshing at the next step). The G-buffer's normal channel carries an octahedral-encoded normal in place of the face index; the light pass reads it; light history matches by position and normal (dot above 0.9) instead of face. Culling uses each slot's bounding sphere. The 10 cm terrain has, at fixed places near its spawn, a stone tower (3 x 3 m, 8 m tall, hollow), a free-standing wall (8 x 3 x 0.3 m), a cliff (at least 12 m high with a 10 x 8 x 2 m face), and a stream 1 m wide in a channel; the 1 m world is unchanged. The walker becomes a driven body moved by `Strata.Physics.drive`; its gravity and jump stay in the walker.

- [ ] **Step 1: failing tests:** `test/physics_test.march`: a 40 x 10 x 10 body yields two parts with offsets (0, 0, 0) and (32, 0, 0) and `take_meshes` returns both once and then nothing; `viewer/test/fine_test.march`: the fine terrain is a pure function (equal chunks for equal inputs), the tower, wall and cliff are where the module says; `viewer/test/walker_test.march`: every existing walker test passes with the walker driven.
- [ ] **Step 2:** run; expect FAIL. Implement the March side.
- [ ] **Step 3:** implement the shim and shaders. Run `native/build_shaders.sh`.
- [ ] **Step 4:** `scripts/golden.sh`: every existing golden passes unchanged (the normal change must not alter axis-aligned lighting). If one differs, find why before going on; do not re-store it.
- [ ] **Step 5: scenes**, each run hidden and offscreen: 21 crate-drop (a crate falls onto grass at 1 m, shots at frames 40 and 120: in the air, at rest on the ground); 22 tilted-slab (a 4 x 1 x 2 slab resting on a crate at about 30°, low sun, `STRATA_LIGHT=1`: its lit faces shade by their true tilt); 23 fine-overview (`STRATA_SCALE=0.1`, the tower, wall and cliff in view). Look at each, then store.
- [ ] **Step 6:** `scripts/verify.sh` PASS. Commit `feat(physics): bodies drawn at their pose, true normals in the G-buffer, a 10 cm viewer world`.

### Task 6: Rays that see bodies

**Files:** `native/strata_vk.c`, `native/shaders/trace.glsl`, `native/shaders/light.frag`, `native/shaders/reflect.glsl`, `lib/strata/ffi/vk.march`, `lib/strata/render.march`, `scripts/golden.sh`

**Interfaces — Produces:**
```
Strata.Ffi.Vk.body_shape(handle : Int, classes : NativeU8Arr, sx : Int, sy : Int, sz : Int) : Bool   -- one light class byte per voxel
Strata.Ffi.Vk.body_place(handle : Int, px : Float, py : Float, pz : Float, qx : Float, qy : Float, qz : Float, qw : Float) : Unit
Strata.Ffi.Vk.body_drop(handle : Int) : Unit
viewer: STRATA_CRATES=n drops n crates in view at start (for the GPU timing)
```
`Strata.Render.bodies` also keeps each body's shape and placement current. The shim builds, each frame, a coarse world-aligned grid (one cell per chunk) listing the bodies whose bounds overlap each cell; shadow, sky and reflection rays that enter a listed cell step through each listed body's grid in the body's frame and take the nearer hit. Light history is reset inside each moving body's screen-space bounds, per body. A body whose chunk is not loaded gives up its slots and its shape (`Strata.Render.bodies` frees them, and uploads them again when the chunk returns).

- [ ] **Step 1:** implement; `native/build_shaders.sh`.
- [ ] **Step 2:** `scripts/golden.sh`: every existing golden passes unchanged.
- [ ] **Step 3: scenes:** 22 again at `STRATA_LIGHT=4` (tilted-slab-lit: the slab's shadow on the ground has the slanted outline of the slab, and the slab does not shadow its own lit faces); 24 body-by-lake (a crate on the shore of the scene-2 lake, its reflection in the water). Look, then store.
- [ ] **Step 4: measure:** scene 1 offscreen with `STRATA_CRATES=0` and `STRATA_CRATES=100`, light mode 4; GPU time difference against the 1 ms target. Record it in the ledger, met or missed.
- [ ] **Step 5:** `scripts/verify.sh` PASS. Commit `feat(physics): shadows, sky and reflection rays see bodies`.

### Task 7: Support — what comes loose

**Files:**
- Create: `lib/strata/support.march`
- Modify: `lib/strata/physics.march`
- Test: `test/support_test.march` (new), `test/physics_test.march`

**Interfaces — Produces:**
```
type Strata.Support.Loose
Strata.Support.loose(r : Registry, w : World, changes : List(Strata.Edit.Change), anchor_y : Int, scale : Float) : Loose
Strata.Support.pieces(l : Loose) : List(List((Int, Int, Int)))   -- each loose piece's cells, in a fixed order
Strata.Support.capped(l : Loose) : Int                            -- fills stopped at 200 000 voxels and counted as held
Strata.Physics.capped_fills(p : Physics) : Int                    -- running total
```
The window, the connection fill and the span values are as design §3. Within a step, support runs first, over every edit handed to that step; loose pieces leave the world by one `Strata.Edit.set_many` to air (plants on them leave with them), and each becomes a dynamic body at rest, split along 128-voxel lines if larger. At `finish`, a piece any of whose cells, or whose six neighbours, were changed by an edit handed after `begin` is not applied; its edit is handed to the next step again and an event of kind 4 records it.

- [ ] **Step 1: failing tests** in `test/support_test.march`, anchor depth y = 0 (stone below is anchored):
  - **support off leaves it:** a floating 3 x 3 x 3 stone cube with support off: no piece after any edit;
  - **a cut pillar's top falls:** a 1 x 6 x 1 pillar on the ground carrying a 3 x 1 x 3 roof; carve its third cell: one piece, the pillar above the cut and the roof (3 + 9 = 12 cells);
  - **a joined bridge stays:** a 7 m bridge of stone (span 4 m) between two pillars: its middle is 3.5 m from support: no piece;
  - **an overhang past its span breaks off:** a stone ledge 6 m out from a cliff wall: the cells 5 and 6 m out are one piece; with span 6 m (`with_physics`), no piece;
  - **span at 10 cm:** the same ledge at scale 0.1 (60 voxels long, span 4 m = 40 voxels): the outer 20 voxels are the piece;
  - **an undercut ledge on a large cliff comes loose:** a cliff joined to a 64-wide slab of ground; carve under a 6 m ledge so nothing is below it: the outer 2 m comes loose although the cliff is joined to the whole world;
  - **an anchor block holds:** the floating cube of the first test, support on, joined to one block registered with `anchor` true: no piece; remove that block: the cube is one piece;
  - **unloaded is anchored:** a structure whose only link to the ground runs into an unloaded chunk: no piece;
  - **the fill cap:** a 64 x 64 x 64 floating cube with the cap lowered to 1 000 for the test: no piece, `capped` 1.
- [ ] **Step 2: failing tests** in `test/physics_test.march`, support on:
  - **loose terrain falls:** cut the pillar: within 3 steps the piece is a body, its cells are air in the world, and by step 120 it has hit the ground;
  - **a large piece is split:** a loose slab 150 x 2 x 2 voxels becomes two bodies (128 and 22 long) that start at rest side by side;
  - **a stale piece is redone, not applied:** `begin` with the pillar cut; before `finish`, a block is placed under the roof and handed with `wake`: `finish` applies nothing for that piece, the placed block is still there, an event of kind 4 is recorded, and at the next step the piece is worked out afresh (and now stays);
  - determinism property: random edits on random terrain with support on, two runs, equal hashes every step.
- [ ] **Step 3:** run; expect FAIL. Implement.
- [ ] **Step 4:** `forge test`; all pass. `scripts/verify.sh` PASS. Commit `feat(physics): support by connection and span; loose terrain becomes bodies`.

### Task 8: Breaking

**Files:**
- Create: `lib/strata/fracture.march`
- Modify: `lib/strata/physics.march`
- Test: `test/fracture_test.march` (new), `test/physics_test.march`

**Interfaces — Produces:**
```
Strata.Fracture.pieces(r : Registry, g : Grid, cx : Int, cy : Int, cz : Int, excess : Float, scale : Float, seed : Int) : List((Grid, Int, Int, Int))
    -- cuts the grid around (cx, cy, cz) into cells grown from seed points about piece_cm apart; excess (impulse over strength, at least 1.0) sets how far from the contact the cutting reaches; returns every voxel exactly once, as pieces with offsets
```
In the step, after integration (design §4 item 7): a contact whose impulse over its contact area exceeds the struck block's strength breaks the body by `pieces`, seeded from the body id and the step number. World blocks a body strikes past their strength break the same way, through the step's edit. Then fragments (design §4 item 8): fewer than 8 voxels; one voxel turns to spray at once (event kind 5); a fragment that sleeps with world ground under its nearest aligned spot and that spot empty is written there, otherwise removed with an event of kind 5.

- [ ] **Step 1: failing tests** in `test/fracture_test.march`: `pieces` returns every voxel of the grid exactly once; the same inputs give equal pieces; at scale 0.1 a 40 x 40 x 4 stone slab (piece 0.5 m) broken with a large excess gives pieces averaging between 60 and 250 voxels; a larger excess breaks more of the slab than a smaller one.
- [ ] **Step 2: failing tests** in `test/physics_test.march`:
  - **material decides:** a glass crate (strength 2 000) dropped 3 m onto stone breaks into at least two bodies; a stone crate (default) dropped the same way stays one body;
  - **voxels are kept:** in both, the voxels in all bodies plus those in spray events plus those written to the world equal the crate's;
  - **a fragment snaps onto ground:** a 5-voxel fragment comes to rest on the ground: it is written into the grid at its nearest aligned spot and is no longer a body;
  - **a fragment on a body does not snap:** a 5-voxel fragment at rest on a sleeping crate is removed with a spray event; nothing is written to the world;
  - **one voxel turns to spray:** a 1-voxel body is removed at the step it is made, with an event of kind 5;
  - **world blocks break:** a 1000 kg crate at 10 m/s into a glass pane of world blocks: some pane cells become air in the step's outcome and as many voxels exist as bodies;
  - determinism property extended with glass crates.
- [ ] **Step 3:** run; expect FAIL. Implement.
- [ ] **Step 4:** `forge test`; all pass. `scripts/verify.sh` PASS. Commit `feat(physics): bodies and blocks break by impact and material; fragments snap or spray`.

### Task 8b: Fine bodies and pinned bodies

Added 2026-11-12 at the owner's approval of design §11, ahead of Task 9 because the demo game's trees wait on it.

**Files:** `lib/strata/physics.march`, `lib/strata/contact.march`, `lib/strata/render.march`, `lib/strata/ffi/vk.march` (or wherever `Strata.Ffi.Vk` lives), `native/strata_vk.c`, `native/shaders/trace.glsl`, `native/shaders/quad.vert`, `native/shaders/loose.vert`, `test/physics_test.march`

**Interfaces — Produces:**
```
Strata.Physics.add_fine(p : Physics, g : Grid, k : Int, x : Float, y : Float, z : Float, q : Quat, kind : Int) : (Physics, Int)
    -- voxels of the world's size / k; k must be 1, 2, 4 or 8, otherwise nothing is added and the id is -1
Strata.Physics.pinned() : Int                       -- the third kind
Strata.Physics.kind_of(p : Physics, id : Int) : Int -- -1 for none
Strata.Physics.voxel_size(p : Physics, id : Int) : Float
Strata.Physics.cut(p : Physics, x : Float, y : Float, z : Float, r : Float) : Physics   -- at the next step
Strata.Ffi.Vk.body_voxel(handle, metres) / slot_voxel(slot, metres)   -- 0 = the world's
```
Event kind 6: voxels cut (where, how many).

- [ ] **Step 1: failing tests** in `test/physics_test.march` ("Physics fine bodies"):
  - **mass:** a 4 x 4 x 4 crate added with k = 4 at scale 1 weighs what a 1 x 1 x 1 crate weighs;
  - **rests on the world:** that fine crate dropped 2 m onto flat ground at scale 1 sleeps with its bottom within 0.025 m of y = 0 (the contact slop is 0.02 of a world voxel);
  - **fine on coarse and coarse on fine:** a 1 m crate dropped onto a resting 8 x 8 x 8 fine crate (k = 8) rests on top of it (its bottom within 0.025 m of the fine crate's top), and the fine crate dropped onto a resting coarse crate rests on it;
  - **never blocks:** `to_blocks` on a fine body writes nothing and leaves the body;
  - **k checked:** k = 3 adds nothing and gives -1;
  - **rays:** `raycast` straight down onto a fine crate at rest hits it at its top face.
- [ ] **Step 2: failing tests** ("Physics pinned bodies"):
  - **stands:** a pinned fine tree (k = 4: a 2 x 6 x 2 trunk under a 4 x 4 x 4 crown, 1 m x 2.5 m x 1 m) on the ground stays put for 120 steps; a 0.5 m fine crate dropped inside its crown's top comes to rest on it, and the column has not moved;
  - **pushes and drives do not move it:** `push` and a driven walker pressing against it leave its position unchanged;
  - **cut through, the top falls:** `cut` at the column's base (a sphere through its lowest fine voxels, above ground level) leaves a held stump pinned under the original id and the crown a new dynamic body that falls and comes to rest on the ground; one event of kind 6 with the voxels cut;
  - **ground dug out, it is freed:** removing the world cells under a pinned body and handing the edit to `wake` makes it dynamic, and it falls;
  - **the job equals the step** with a cut handed between steps.
- [ ] **Step 3:** run; expect FAIL. Implement: a voxel size on each body, threaded through mass, corners, contacts (two sizes: the corners' body and the target grid), sub-steps, reach and bounds, fracture, snapping, meshing, `raycast`/`drive`/`sweep`; the pinned kind; cuts handed like pushes and applied at the start of the step; renumbered with the step's new bodies in `finish_world`.
- [ ] **Step 4:** renderer: body rows carry their voxel size (`trace.glsl` converts distances for bodies as it does for figure parts); body slots are posed at their voxel size in `quad.vert` and `loose.vert`; `Render.bodies` passes it. Viewer suite and goldens unchanged (no fine bodies in them).
- [ ] **Step 5:** `forge test`; all pass. `scripts/verify.sh` PASS. Commit `feat(physics): fine bodies, pinned bodies, cutting`.

### Task 9: Water

**Files:** `lib/strata/physics.march`, `lib/strata/fluid.march`, `viewer/lib/strata_viewer.march`, `test/physics_test.march`, `test/fluid_test.march`

**Interfaces — Produces:**
```
Strata.Physics.marks(p : Physics) : Strata.Store.Store(NativeU8Arr)   -- per chunk key, one bit per voxel (Strata.Occupancy layout): cells whose centre lies inside a body's colliding voxel
Strata.Fluid.set_marks(f : Fluid, w : World, m : Strata.Store.Store(NativeU8Arr)) : (Fluid, Strata.Edit.Outcome)
    -- cells newly marked lose their water to the nearest open cells above and beside them (the amount is kept exactly); cells no longer marked wake their fluid neighbours; from then on the fluid treats marked cells as walls
```
Bodies feel drag in fluid cells in proportion to the eighths they hold (a body is never lifted: no buoyancy). A body whose bounds first enter fluid cells makes an event of kind 3 (where, speed). The viewer calls `set_marks` before each fluid `begin` and turns splash events into spray as it does falling water.

- [ ] **Step 1: failing tests** in `test/fluid_test.march`: a marked cell holding 8 eighths beside open cells: after `set_marks`, it holds none and the box's amount is unchanged; a marked cell is never filled by a fluid step; unmarking a cell beside resting water wakes it and water enters it.
- [ ] **Step 2: failing tests** in `test/physics_test.march`:
  - **conserved:** a crate dropped into a 6 x 4 x 6 pool, marks set and the fluid stepped every 6 physics steps for 600 physics steps: the pool's amount is equal at every fluid step;
  - **a dam holds:** a stream in a 1-wide channel; a 1 x 2 x 3 body resting across it: over 100 fluid steps the amount downstream of the body does not grow while upstream rises; remove the body, set marks: within 20 fluid steps water passes;
  - **sinks:** a crate dropped 2 m into water 4 m deep reaches the bed and sleeps there; its speed in the water stays below its speed at the same distance fallen in air;
  - **splash:** exactly one event of kind 3 on entry.
- [ ] **Step 3:** run; expect FAIL. Implement.
- [ ] **Step 4:** viewer: marks, splash spray. `forge test`; all pass. `scripts/verify.sh` PASS. Commit `feat(physics): bodies dam water and sink in it; water is pushed aside, never lost`.

### Task 10: Scenes, soak, measurements

**Files:** `viewer/lib/strata_viewer.march`, `scripts/golden.sh`, `scripts/soak.sh`, `demo/lib/strata_demo.march`, `RESULTS.md`, `README.md`, `GAPS.md` (only for gaps met), `docs/physics-ledger.md`

**Interfaces — Produces:** viewer scenes 25 to 30, each with a scripted edit at a fixed frame: `STRATA_TOPPLE`, `STRATA_CRUMBLE`, `STRATA_COLLAPSE` (frame numbers); demo runs printing `physics:` lines; the soak's `phys_worst_ms` and `phys_behind_s` fields.

- [ ] **Step 1: scenes**, hidden and offscreen, looked at and then stored:
  - 25 wall-topple (10 cm): the wall's base carved at frame 30; shots at frames 45, 75, 150: leaning, falling, lying broken on the ground;
  - 26 tower-crumble (10 cm): the tower's base carved; shots at 60 and 300: falling, a pile;
  - 27 cliff-collapse (1 m): the 20 x 15 x 4 m face undercut; shots at 60 and 300;
  - 28 rubble-dam (1 m): a wall pushed into the scene-20 stream; at frame 600 the water above the rubble is higher than below;
  - 29 sink-lake (1 m): a crate dropped into the test lake; shots at the splash and once it has settled on the bed (seen from the shore, through the water);
  - 30 glass-shatter (1 m): a glass crate dropped onto stone; shot after it has broken.
- [ ] **Step 2: headless measurements** in `demo/`, release build, each printed as a `physics:` line: worker time per step (mean and worst over the scene) for the 10 cm wall, the 10 cm tower once 100 bodies are awake, the 1 m cliff; the support pass after a small edit at 10 cm; the 10 cm cliff face of 10 x 8 x 2 m; two runs of each with equal final hashes.
- [ ] **Step 3: soak:** `scripts/soak.sh` gains a scripted collapse (the cliff of scene 27 under the circuit at frame 600); it reports and asserts worst frame at most 20 ms (the target is 16.7), and reports physics' main-thread worst (`phys_worst_ms`, target 1 ms, including the walker's `drive`) and the most time behind (`phys_behind_s`).
- [ ] **Step 4:** write every design target into `RESULTS.md` with its measured value and met or missed, plainly. README gains a Physics section (what it does, the calls, the screenshots) and its known limits: nothing floats, no joints or vehicles, no structural load, bodies do not hold up terrain, voxels lost when `to_blocks` meets occupied cells, the fluid's own timing-dependent drops, and every target the measurements show missed.
- [ ] **Step 5:** `scripts/verify.sh` PASS; `scripts/soak.sh` PASS. Commit `docs(physics): scenes, soak, results`.

# Strata physics: design

Status: design, awaiting approval. Nothing here is built.
Date: 2026-11-11
Follows: `2026-11-10-strata-design.md`, and the fluids built from
`2026-11-10-fluid-design.md` (whose worker-step pattern this reuses).
Branch: `physics`, cut from `fluids`.

## Intent

Nothing in a Strata world moves except water and the viewer's walking box.
This project makes the world physical in the way Teardown is: things made
of voxels fall, tumble, collide and come to rest at whatever angle they
land; destroying part of something breaks it; and a structure cut loose
from what holds it up falls. A wall knocked out at its base topples whole.
A tower crumbles into pieces that pile up. A cliff face undercut by digging
collapses. Rubble that falls into a stream dams it, and a piece that falls
into a lake sinks.

## Decisions the owner made

1. Objects fall and collide; destroyed objects break; attached structures
   fall when they lose support. Teardown is the reference.
2. Loose pieces tip over and tumble: bodies rotate freely.
3. A fallen piece stays a tilted body where it lands. Something broken
   into small pieces may snap to the nearest grid-aligned spot.
4. Both voxel scales, 1 m and 10 cm. The scenes that must work: a whole
   wall falling, a tower crumbling, a cliff face collapsing.
5. A piece that falls into water sinks.
6. Water reacts to being dammed: a body resting in a stream blocks it.
7. Players and creatures may be rigid bodies, but the game decides; the
   library offers the options.
8. Assume future multiplayer.
9. Whether an impact breaks something depends on force and material.
10. Support is level 2: connection to an anchor, plus a span limit per
    material (an overhang longer than its material allows breaks off).
    Real load (weight flowing through a structure) is not built.
11. Past a limit on how many bodies exist, far sleeping bodies out of view
    snap into the grid where nobody sees the jump.
12. From the first design: frame rate is the priority under heavy
    simulation; the library names no game concept; deterministic where it
    can be.

Everything else below is the designer's call and may be overturned. The
calls made after the builder review are listed at the end.

## Goals

- **Bodies.** A body is a rigid voxel object with its own grid of blocks
  from the game's registry, a position, a rotation, and linear and angular
  velocity. Its mass and inertia come from its blocks.
- **Bodies collide** with the world grid and with each other, slide with
  friction, tumble, stack, and come to rest without jitter.
- **Terrain comes loose.** After any edit, a pass finds what no longer has
  support and turns it into bodies, through an ordinary edit, so meshing,
  lighting and fluids follow with no path of their own.
- **Impacts break things** into pieces, by force against material
  strength.
- **Small fragments** stop being bodies: once at rest they snap into the
  grid, or become spray.
- **Water** treats bodies as walls and slows a body that enters it, which
  then sinks; a body never destroys or creates water.
- **Two kinds of body for the game**: dynamic (physics moves it) and
  driven (the game moves it, at once; it collides, stands on bodies,
  pushes them, and is told what hit it).
- **Bodies are lit as the world is**: shadowed, casting shadows, seen in
  reflections, with their true tilted normals.
- **Events** a game reads for sound and particles: impacts, breaks,
  pieces coming loose, splashes.
- **Deterministic**: the same world, the same edits and the same inputs at
  the same steps give the same bodies, step for step, on the same build.
- **Stated budgets**, measured, misses reported.

## Non-goals

- Real structural load: a thin bridge does not snap for being thin, only
  for spanning too far for its material.
- Floating. Nothing floats; buoyancy is not built. Bodies are not carried
  by flowing water.
- Joints, hinges, wheels, vehicles, ragdolls. One body is one rigid grid.
- Lockstep determinism across different CPUs or compilers, and any
  networking. State is kept small and plain so a server could send it.
- Explosions or tools as features. A game makes them from edits and
  `push` (an impulse at a point).
- Fire, smoke, soft bodies, erosion, pressure.
- Simulating bodies in chunks that are not loaded (section 4).
- A save format. Bodies are plain data a save could hold; nothing is
  written.
- Bodies holding up terrain. Support runs through the world grid only
  (section 3).

## Anti-goals

- A pile of rubble that jitters, creeps or never sleeps.
- A body that sinks into the ground, passes through a wall, or explodes
  apart when two bodies overlap.
- A collapse that stalls the frame. Work per step is bounded; when it
  cannot keep up, physics runs slower than the clock rather than holding
  up a frame.
- A player whose own movement lags behind the input.
- Water that vanishes or multiplies where a body lands in it.
- A floating island in a Minecraft-style world falling because physics
  was switched on. Support is off unless a game turns it on.
- A body drawn without shadow, or whose lighting jumps as it turns.

## Constraints

- March `origin/main` through `scripts/env.sh`; the C shim; no Vulkan or C
  type crosses into March. `scripts/verify.sh` stays green. Scripted
  viewer runs use `STRATA_HIDDEN=1 STRATA_OFFSCREEN=1 STRATA_PIXELS=1`.
- Per-voxel or per-contact work per step runs in native arrays owned by
  one task, on a worker. Persistent-map work per cell cost about
  5 microseconds in the fluid and is not affordable here.
- What the hot loops need from the registry (solidity, density, strength)
  is read once into arrays the task owns (GAPS S15).
- A task cannot be polled (GAPS S12): work begun on one frame is awaited
  later, when it is expected to be finished.
- Physics depends on nothing on the list after it: interface and text,
  audio, the far field, the save format, the cost pass. It changes three
  things that exist: the renderer (bodies drawn at an angle, a G-buffer
  normal, rays that see bodies), the fluid (bodies as walls), and the
  viewer (a 10 cm world).

## The design

### 1. What a body is

A body holds:

- a grid of block ids, up to 128 voxels a side (12.8 m at 10 cm, 128 m at
  1 m). A loose piece larger than that is split along 128-voxel lines into
  several bodies that start with the same velocity;
- position (its centre of mass, in metres) and rotation (a quaternion);
- linear and angular velocity;
- mass, centre of mass and inertia, worked out from block densities when
  the grid changes;
- its kind: dynamic or driven;
- whether it is asleep.

A body's voxels are the world's size unless it is added fine: a body in a
10 cm world has 10 cm voxels, and a fine body has voxels of the world's
size divided by 2, 4 or 8 (§11). Its blocks are ordinary registry blocks,
so it is textured and lit as they are.

Bodies live in `Strata.Physics`, a value the game keeps beside its stream
and fluid, holding every body and the work in flight. A body is named by an
integer id, never reused, so ids order the work deterministically.

### 2. Materials

The registry gains material values per block, set as roughness and
emission are set today (`Strata.Blocks.with_physics`):

| field | meaning | default |
|---|---|---|
| density | kg per cubic metre | 2000 |
| strength | the impact a block survives, as normal impulse per square metre of contact (N s/m²) | 50 000 |
| span | how far, in metres, the block can reach sideways from support | 4.0 |
| piece size | the typical size, in metres, of the pieces it breaks into | 0.5 |
| anchor | the block holds up anything joined to it (bedrock) | no |

Span and piece size are in metres so the same material behaves alike at
either scale. Blocks that do not collide (plants, fluid) carry nothing and
are never part of a body: a body made from terrain leaves them behind, and
a plant whose ground is taken is removed by the same edit.

### 3. Support: what comes loose

Support is off by default. A game turns it on for its world and sets the
anchor depth: every block at or below it is anchored. The anchor flag also
anchors any block. A chunk that is not loaded counts as anchored, so the
edge of the loaded world never falls away. Bodies never hold up the world:
support runs through world blocks only.

The pass runs inside the physics step after an edit that removes
collision from any cell (or places a block, which may itself be
unsupported), over a **window** around the change: the change's bounding
box grown by the largest span any registered material has, plus two
voxels. At 10 cm with spans up to 4 m that is about 90 voxels a side
around a small edit.

1. **Connection.** From the solid neighbours of each changed cell, fill
   through face-connected solid blocks. The fill stops as soon as it
   reaches an anchor or leaves the window: either means that part is held
   up. A fill that does neither is a loose piece. A fill also stops at
   200 000 voxels and counts as held; the count of such stops is reported.
2. **Span.** Inside the window, each solid block takes a support value:
   full (its span) if the block below it is held, else the best of its
   side neighbours' values less one voxel's width. A block on the window's
   boundary counts as held at full value. Blocks below zero, joined to
   each other, form a loose piece, which breaks off where it meets held
   blocks. So an undercut overhang on a cliff comes away even though the
   cliff is joined to the whole world.
3. **Loose pieces** are removed from the world by one `Strata.Edit` and
   become dynamic bodies at the same place, unrotated, at rest. A piece is
   not fractured as it comes loose; it breaks when it lands (section 4).

### 4. The step

Physics advances in fixed steps of 1/60 s. Each step, in order:

1. **Support** for edits handed to this step (section 3).
2. **Forces.** Gravity (9.8 m/s² by default, set per world), water drag on
   the part of a body in fluid cells.
3. **Contacts.** Broad phase: bodies' bounding boxes, sorted along one
   axis. Narrow phase: each body keeps the centres of its surface voxels;
   a surface point of one body inside a solid cell of the world or of
   another body's grid is a contact, with a depth and a normal taken from
   the grid around the cell.
4. **Solve.** Sequential impulses with friction and a little restitution,
   a fixed number of iterations (8), contacts visited in a fixed order
   (by body id, then by voxel index), and a bounded push-out for overlap
   so two overlapping bodies separate over a few steps instead of flying
   apart.
5. **Integrate.** Positions and rotations advance. A body moving more than
   half a voxel in a step is advanced in sub-steps, so nothing tunnels.
6. **Sleep.** A body slower than a threshold for half a second sleeps. A
   sleeping body costs nothing. It wakes when touched by an awake body,
   when an edit changes a cell inside its bounding box, or when the game
   pushes it.
7. **Break.** A contact whose impulse, spread over its contact area,
   exceeds the strength of the body's weakest block breaks the body there
   (the area is what the impacting contact points span: the smaller of
   their box across the normal and twice their furthest distance from
   their mean squared, so a face's corners give its area however it is
   turned; never less than one voxel face; a step takes the hardest hit
   of its sub-steps): the
   grid near the contact is cut into pieces of about the material's piece
   size (cells grown from seed points placed by a hash of body id and
   step, so it is deterministic), more of the body breaking for a greater
   excess. Each piece becomes a body carrying the velocity of where it
   was. World blocks a body strikes break the same way, if they are no
   stronger than the body's weakest block (glass does not break stone): the broken cells
   leave the world by an edit and become bodies. So a falling tower
   arrives as one or a few bodies and crumbles into many when it hits.
8. **Fragments.** A piece of a break with fewer than 8 voxels is a
   fragment, as is a body the game adds with `add_fragment`; other small
   bodies a game adds stay bodies. A fragment
   that falls asleep with world ground (not a body) under its nearest
   aligned spot (or another of its own voxels), and that spot empty, is written into the grid there and
   stops being a body. Any other fragment that falls asleep is removed and
   a burst of spray marks it. A fragment of one voxel turns to spray at
   once.

At most 256 bodies are awake and handled in a step, nearest the focus
points first, with steps waited counted as the fluid's are. The rest wait;
nothing is lost.

A body whose chunk unloads is kept as data, asleep, and gives up its
renderer slots; it comes back when the chunk loads. A body at the edge of
the loaded world treats the unloaded side as a wall and sleeps there.

**The total limit** (owner's ruling). Bodies that exist at once are
capped, 4 096 by default (renderer slots are shared with world chunks:
16 384 in all, about 3 100 used by the viewer's world). Past the cap, the
sleeping bodies farthest from the focus points and outside the view are
written into the grid at their nearest aligned spot, as fragments are,
until the count is back under the cap. The jump happens where nobody is
looking. A body that is awake or in view is never snapped; if every body
is, the cap is exceeded and the excess is reported.

### 5. Where the step runs

As the fluid does: `begin` hands a worker the world as it stands (a
persistent value, so free to share), the body state, and the edits and
inputs for that step; `finish` takes the result. Unlike the fluid, **a
physics step is never dropped.**

- **Edits belong to steps.** An edit the game makes is handed to the next
  `begin`. Step N's support pass sees the world as it stood at `begin` N.
- **Stale results.** At `finish`, a loose piece or a broken world block
  whose cells changed since `begin` is not applied; its edit is handed to
  step N+1 again, which works it out afresh. This never deletes a block
  placed meanwhile or drops a piece the player has just propped up. What
  counts as "changed since `begin`" is the set of edits the game made in
  between, which belong to step N+1, so the result depends only on the
  order of edits and steps, not on timing.
- **When a step runs late.** A step is begun on one frame and awaited no
  sooner than the next frame on which it is expected to be done (two
  frames at 120 frames a second). If a worker step takes longer than a
  step's worth of time, physics falls behind the clock: bodies move in
  slow motion while it does, frames are never held, and the time behind
  is reported. Only one step is in flight at a time. When work eases,
  physics catches up by at most two steps per frame.
- **Drawing.** The game draws bodies between their last two finished
  states, interpolated by the clock, so motion is smooth at any frame rate
  and bodies are drawn one step (17 ms) behind.

Determinism is a property of the inputs: step N sees the world with every
edit handed to it, the pushes and driven-body positions given for step N,
and nothing that depends on how long a worker took.

### 6. Driven bodies

A driven body is moved by the game on the main thread, at once, every
frame: `Strata.Physics.drive` sweeps it against the world and against the
other bodies at their latest drawn poses, as `Strata.Query.sweep` moves the
walker now. So a player's movement never waits for a worker. At each
`begin` the driven body's position is handed to the step as an input, where
it pushes dynamic bodies with a mass the game sets (a creature can shove a
crate but not a wall) and is reported its contacts: which body, where, how
hard. It never rotates unless the game rotates it, and it is not knocked
over.

A game that wants its player knocked flat by a falling wall makes the
player a dynamic body instead, or switches between the two.

`Strata.Query.raycast` and `sweep` gain forms that also see bodies, so
aiming at and walking on rubble work with the calls a game already uses.

### 7. Water

- **Walls.** Before each fluid step, every body marks the grid cells whose
  centre lies inside one of its colliding voxels (`Strata.Physics.marks`),
  and the game hands the marks to `Strata.Fluid.set_marks`; from then on
  the fluid treats a marked cell as a wall. A cell that holds water when it
  becomes marked has its water moved to the nearest cells with room above
  and beside it (searched through air, the same water and other marked
  cells), in one edit, so the amount is kept exactly; water that finds no
  room within 512 cells stays where it was. A cell that is unmarked wakes
  its fluid neighbours. Water edits wake no body: water does not collide.
- **Sinking.** A body under water feels drag: velocity and spin lose a
  share 4 x wet x dt a step (taken implicitly), where wet (0 to 1) is, over
  the layers its voxels fill, the share of eighths in the free cells of
  that layer of its box grown a cell sideways, its own cells left out. It
  slows and settles on the bed. Nothing lifts it.
- **Splash.** A body entering water makes an event: where, how fast, how
  big. The viewer turns it into spray, as it does falling water.
- **Physics edits and fluid steps.** A fluid step is dropped only if a
  cell it writes was changed under it (`still_true` checks each cell), so
  physics edits elsewhere do not stall the water. A fluid step dropped
  because a body broke the bank it was writing is retried next step, as
  now.

### 8. Drawing and lighting

- **Drawing.** A body's grid is meshed by the existing mesher and uploaded
  through a body path into slots. A body slot carries a transform (a
  position in metres and a rotation) and never writes the world's light
  volume. Culling uses each slot's bounding sphere. A body is remeshed
  only when its grid changes (a break); moving it rewrites its transform.
- **Normals.** The G-buffer gains a true normal per pixel in place of the
  face index, so a turning body's lighting turns smoothly with it. Light
  history matches by position and normal instead of face. World chunks,
  which are axis-aligned, look as they do now (checked by the existing
  goldens).
- **Rays see bodies directly.** Each body keeps a small bit grid of its
  solid voxels on the GPU (a 7 200-voxel wall is 1 KB), its transform and
  its bounds. A coarse world-aligned grid, one cell per chunk, lists the
  bodies overlapping each cell. Shadow, sky and reflection rays that cross
  a listed cell step into each listed body's grid in the body's own frame.
  So bodies cast exact tilted shadows, shadow themselves correctly, are
  seen in reflections, and the world's light volume is never touched by a
  body. A hit body voxel is coloured from its block's light class.
- **History.** Light history is reset where a moving body's screen-space
  bounds were, per body, not through the single edit box the world uses.

### 9. The viewer

- A 10 cm world: its own terrain (cliffs, a stone tower, a wall, a
  stream), at a stream radius of 6 x 3 chunks (about 19 m around the
  camera), switched by `STRATA_SCALE=0.1`. The 1 m world stays the
  default.
- Support on in both, with an anchor depth below the terrain's lowest
  valley.
- A key that drops a crate; carve and build as now, so undercutting a
  cliff or knocking out a wall's base brings it down.
- The walker becomes a driven body.

### 10. What a game sees

    Strata.Physics.new(registry, config)          -- gravity, step rate, support on or off, anchor depth, caps
    Strata.Physics.add(p, grid, x, y, z, rot, kind) -- a body from blocks the game supplies; returns (p, id)
    Strata.Physics.add_fine(p, grid, k, x, y, z, rot, kind) -- a body of voxels 1/k of the world's (k 2, 4 or 8)
    Strata.Physics.pinned()                       -- the kind of a body fixed until it is no longer held
    Strata.Physics.cut(p, x, y, z, r)             -- take body voxels within r metres, at the next step
    Strata.Physics.remove(p, id)
    Strata.Physics.to_blocks(p, stream, id)       -- write a body back into the grid at its nearest aligned spot
    Strata.Physics.wake(p, outcome)               -- hand an edit the game made to the next step
    Strata.Physics.push(p, id, x, y, z, ix, iy, iz) -- an impulse at a point, at the next step
    Strata.Physics.drive(p, world, id, vx, vy, vz, dt) -- move a driven body now
    Strata.Physics.begin(p, world, focus) / finish(job, p, stream)
    Strata.Physics.step(p, stream)                -- one step at once, for tools and tests
    Strata.Physics.pose(p, id, t)                 -- position and rotation, interpolated to clock t
    Strata.Physics.events(p)                      -- impacts, breaks, loose pieces, splashes, deferred edits
    Strata.Physics.contacts(p, id)                -- a driven body's contacts last step
    Strata.Physics.marks(p)                       -- the cells bodies fill, for Strata.Fluid.set_marks
    Strata.Fluid.set_marks(f, w, marks)           -- marked cells are walls; their water pushed aside, never lost: (Fluid, Outcome)
    Strata.Render.bodies(r, p, t)                 -- keep the renderer in step with the bodies

### 11. Fine bodies and pinned bodies

Added 2026-11-12, approved by the owner ("Yes, as recommended") for the
demo game's trees: generator trees built at 0.25 m standing in the 1 m
island as bodies, colliding with the walker and shots, coming down when
burned or cut, and breaking.

**Fine bodies.** A body may have voxels of the world's size divided by 2, 4
or 8 (`add_fine`). Nothing else divides: the cells of a fine grid line up
with the world's. Everything that reads a body's grid reads it at the
body's own size: its mass and inertia (density × its voxel volume), its
surface corners, how many sub-steps it needs (half of its own voxel a
sub-step), its pieces when it breaks, its mesh and its rows for the rays.
Contacts already test each body's corners against the other's grid, the
world's or another body's; a corner is placed in that grid at that grid's
voxel size. Body against body is tested both ways, so a coarse face whose
corners straddle a thin fine trunk is still caught by the trunk's own
corners. A fine body never becomes blocks: `to_blocks` leaves it, the cap
passes it over, and a fine fragment that comes to rest turns to spray
rather than being written. A fine body at rest stays a sleeping body.
(Resampling a fine body into world cells is left as a possible later
option for debris.)

**Pinned bodies.** A third kind beside dynamic and driven: a pinned body is
fixed and asleep. It is a wall to every other body and to `drive`; pushes
and touches do not move it. It is freed (becomes dynamic and wakes) when
it is no longer held: a pinned body is held while any of its colliding
voxels lies on a colliding world cell (the cell under the voxel's bottom
face). This is checked when an edit handed by `wake` touches its bounds,
and when it is cut.

**Cutting.** `cut(p, x, y, z, r)` takes, at the next step, every voxel of
every non-driven body whose centre lies within r metres of (x, y, z), as
a fire or a blade would. A body left empty is removed. A body left in more
than one face-connected part keeps its id for one part and the rest become
new bodies: for a pinned body, the part kept is a held one if any part is
held, and the others become dynamic; otherwise the largest part is kept.
Each part moves with the velocity of where it was. Cut voxels are reported
as one event of kind 6 (where, voxels).

Pinned bodies do not break from being struck (only moving bodies are
judged), and do not count toward the awake cap. A field of 30 to 60 pinned
trees costs the step nothing while nothing moves near them; what they cost
the rays is measured (Task 10 and the trees scene), not assumed.

## Budget and targets

Designer's targets, stated so a miss is reported. M3 Max, release build,
offscreen timing, 2560 x 1600.

| | target |
|---|---|
| worst frame during any scene below | 16.7 ms or less |
| main-thread cost of physics in a frame, including driven bodies | 1 ms or less |
| worker time per step, 10 cm wall falling whole (8 x 3 x 0.3 m, about 7 200 voxels) | 8 ms or less |
| worker time per step, 10 cm tower crumbling (100 awake bodies) | 16 ms or less, so physics keeps pace with the clock |
| worker time per step, 1 m cliff face (20 x 15 x 4 m, landing and breaking into about 50 bodies) | 8 ms or less |
| support pass after a small edit, 10 cm, spans up to 4 m | 10 ms or less, inside the step |
| loose terrain starts to fall | within 3 steps of the edit |
| a pile at rest | asleep within 3 s of the last impact, then no visible motion |
| sleeping bodies | no cost per step |
| no tunnelling | a body at 30 m/s does not pass a wall 2 voxels thick |
| GPU time added by 100 bodies in view, everything lit | 1 ms or less |
| water amount with bodies in it | conserved exactly |
| determinism | two runs give the same body state hash at every step |
| 10 cm cliff face, 10 x 8 x 2 m (160 000 voxels) | measured and reported; not promised |

## Testing

- **Pure logic in March tests:** mass and inertia of known grids; a body
  falling onto flat ground lands, slides to rest and sleeps; a stack of
  boxes stands; a box on a slope steeper than friction allows slides;
  overlap separates without flinging; no tunnelling at speed; connection
  (a cut ring falls, a joined bridge stays); span (an overhang past its
  span breaks where it meets support, one inside its span stays, an
  undercut ledge on a large cliff comes loose); support off leaves
  floating terrain alone; a result made stale by an edit is redone, not
  applied; fracture by material (glass breaks, stone survives the same
  fall; the same impact gives the same pieces); fragments snap only onto
  world ground; water amount conserved as bodies mark and unmark cells;
  determinism as a property (random worlds and edits, two runs, equal
  hashes each step); the awake cap (work waits and is not lost); the
  total cap (the farthest sleeping body out of view is snapped, never an
  awake or visible one); a driven
  body stands on a body and is blocked by one.
- **Golden images**, looked at before they are stored: a 10 cm wall
  toppling (three frames); a tower crumbling to a pile; a 1 m cliff face
  collapsing; rubble damming a stream; a piece sinking in a lake; glass
  shattering on stone; a tilted body under a low sun (its shadow and its
  lit faces); the existing goldens unchanged by the normal change.
- **Soak:** the flight circuit with a scripted collapse under it, with
  the worst frame, worker step times and time fallen behind reported.

## Order of work

One plan. Each step leaves the viewer working.

1. Bodies headless: grids, mass, the step against the world grid, sleep,
   determinism, the begin/finish job and its late-step rule.
2. Body against body: contacts, stacking, piles at rest; the awake cap;
   driven bodies.
3. Drawing: the body upload path and transform, interpolation, the
   G-buffer normal, the crate key; the 10 cm viewer world.
4. Rays that see bodies.
5. Support: material fields, the window, connection, span, loose terrain
   becoming bodies, stale results.
6. Breaking: impact fracture into pieces, fragments.
7. Water: marks, displaced water, sinking, splashes.
8. Scenes, soak, measurements against the targets.

## Left to the builder

- Contact normals from voxel grids, the restitution and friction values,
  sleep thresholds.
- The fracture pattern, as long as the golden images look right to the
  owner.
- How a body's grid is stored (chunk forms, or one dense array per body).
- How the G-buffer carries a normal, and how the body list grid is laid
  out on the GPU.
- How the chunk table carries a transform without slowing world chunks.

## Risks

- **March may be too slow for the solver.** Contacts at 10 cm number in
  the thousands per step. The plan is native arrays on a worker first,
  measured in step 1. If a step misses its target by a wide margin, the
  narrow phase and solver move into a C kernel called on whole arrays;
  that is a change of language for one piece of the engine and the owner
  rules on it before it happens.
- **Rays into bodies cost GPU time.** Many bodies in one coarse cell make
  every ray crossing it test each one. Measured against the 1 ms target;
  if it misses, bodies far from the camera fall back to their bounds.
- **Piles that will not sleep.** Voxel contacts on voxel ground are
  noisy. Rest is a tested property and a golden image, not a hope.
- **The normal change touches every pixel** of the existing renderer. The
  existing goldens are the check that the world looks as it did.
- **Slow motion.** A heavy collapse that misses the worker target is seen
  in slow motion. That is reported per scene, not hidden.
- **Light and mesh churn.** A break remeshes a body and an edit remeshes
  chunks; a crumbling tower does many of both. The soak measures it.

## Calls the designer made after the builder review

A reviewer read this as a builder would. These answers are mine; any may
be overturned.

1. Span is worked out in a window around each edit (the largest span plus
   two voxels); what lies beyond the window counts as held. Without it the
   fill reaches the whole terrain and span never runs.
2. A step that runs late slows physics rather than holding a frame, since
   frame rate is the priority. Steps are never dropped or batched.
3. Driven bodies move on the main thread at once, so the player never
   lags; their positions are inputs to the step.
4. Support results and world breaks whose cells changed since `begin` are
   redone in the next step, not applied.
5. Bodies are lit with true normals, so a turning body's light turns with
   it. This changes the G-buffer.
6. Bodies break into pieces of a material's piece size when they land
   hard; they do not come loose already in pieces. That is where the
   tower's and cliff's many bodies come from.
7. Rays step into bodies' own grids, rather than bodies being stamped into
   the world's light volume, which can only be written in whole chunks
   and has no copy to restore from.
8. A fragment snaps into the grid only onto world ground. Bodies never
   hold up terrain, so a block built on rubble is unsupported.
9. The viewer gains a 10 cm world at radius 6 x 3. The 10 cm cliff scene
   is 10 x 8 x 2 m, not 20 x 15 x 4, to fit it.
10. Bodies in unloaded chunks are kept asleep and give up their slots.

# Strata entities: design

Status: design, awaiting approval. Nothing here is built.
Date: 2026-11-11
Follows: `2026-11-10-strata-design.md`. A new project beside physics,
animation, audio, generation and interface text. It sits on top of
`Strata.Physics` (`2026-11-11-physics-design.md`), `Strata.Anim`
(`2026-11-11-animation-design.md`), `Strata.Sound`
(`2026-11-11-audio-design.md`) and `Strata.Query`. It changes none of them
except where "What this asks of other modules" says so.

## Intent

A game that wants one creature today takes a body id from
`Strata.Physics.add`, a figure id from `Strata.Anim.spawn_at` and voice ids
from `Strata.Sound.play`, and keeps the three in step by hand every frame.
Nothing ties them to a place in the world, and nothing decides what happens
to them when the camera leaves.

This project adds the entity: one thing in the world with one id, a
position, and whichever of those parts it needs. Strata spawns it, moves
it, keeps its parts in step, shows it when a camera is near, goes on
simulating it when no camera is, and removes it. The game decides what an
entity is and what it does.

## Decisions the owner made

1. Entities are players, NPCs and enemies; what they do varies by game. The
   layer must serve many game types and names none of them.
2. The scale to design for is any of: a large arena game, a bullet storm,
   a city simulation. That is thousands of entities, and tens of thousands
   for bullets.
3. Simulation and drawing are separate. An entity that is off screen is
   still simulated and is not drawn. When the camera jumps to it, it is
   drawn in the right place.
4. An unseen entity does not have to collide with the real blocks. It may
   follow a route without touching the terrain, and need only stand
   correctly on the true ground once it can be seen.
5. Some things must be held in memory although the terrain around them is
   not loaded. Paths are the named example.
6. A game's own data on an entity is plain numbers in numbered slots.
   Anything richer lives in the game's own table, keyed by entity id.
   (The designer recommended this; the owner accepted it.)
7. From the first design: high frame rate under load is the priority, and
   the library names no game concept.

Everything else below is the designer's call.

## Goals

- One id for one thing in the world. The id of a removed entity is never
  mistaken for a live one.
- A game defines **kinds** in a table, as it defines blocks. A kind states
  which parts its entities have, how they move, what happens to them unseen,
  and how many number slots they carry.
- An entity can be as small as a position and a velocity. A body, a figure
  and sounds are each optional.
- Strata moves entities, in one of four ways chosen per kind (section 3).
- Strata keeps an entity's body, figure and voices at the entity's position
  without the game doing anything.
- Entities near a camera are **seen**: they have their figure and sounds and
  collide with real blocks. The rest are **unseen**: simulated, with none of
  those. Strata switches an entity between the two and the game's code does
  not change.
- A kind chooses what unseen means for it: go on living (at a slower rate
  if it likes), freeze, or be removed.
- A route is held by the entity, not by the terrain, so it outlives the
  chunks it crosses.
- "Which entities are within N metres of this point" and "which entities
  does this ray pass" are answered fast.
- The game is told what happened each tick: touches between kinds it asked
  about, block hits, entities coming into and leaving view, routes finished,
  entities removed.
- All entity state is flat arrays of numbers, so a later save format or
  network layer can copy it without knowing any game.
- Everything except drawing and sound works headless.
- The same world, kinds and calls give the same entity state at every tick
  on the same machine.

## Non-goals

- Behaviour, AI helpers, state machines. The game loops over its entities
  and decides.
- Pathfinding. This design fixes what a route is and how an entity follows
  one. Producing routes is the navigation project.
- The save file and the network protocol. This design makes entity state
  copyable (section 8) and stops there.
- A general component system in which a game invents component types.
- Drawing an entity that has no figure. A figure is the only thing this
  layer shows, and `Strata.Anim` holds 4,096. A bullet storm of 20,000 is
  simulated and reports its touches, but drawing that many needs a sprite
  path in the renderer, which is its own project.
- One entity attached to another (a rider on a cart). A held item is
  already served by `Strata.Anim`, where one figure hangs from another's
  socket; the game does that with the figure ids this layer hands it.
- Inventories, names, teams, health. Slots hold numbers; meaning is the
  game's.
- Unseen entities that collide with real terrain, or that know about edits
  made where no chunk is loaded.
- Lockstep agreement across different machines.

## Anti-goals

- A callback from Strata into game code per entity. The game calls Strata,
  never the reverse.
- Per-entity storage in March lists or records. 20,000 bullets do not fit a
  frame that way (GAPS S15, and `Strata.Physics` keeping bodies in a list
  is fine at 256 and not at 20,000).
- An entity kind built into the library (a "player", a "projectile").
- A visible pop or a fall through the floor when an entity becomes seen.
- A tick whose cost depends on the number of entities that could exist
  rather than the number that do.
- A library call that can block the frame on a worker.

## Constraints

- March `origin/main` through `scripts/env.sh`. `scripts/verify.sh` stays
  green after every task. Gaps go in `GAPS.md`; March is not changed.
- No panic reachable from library code. Every lookup is total: a dead or
  unknown id reads as documented defaults and a write to it is counted as
  refused, as `Strata.Anim` counts refusals.
- No C type crosses into March: integers, floats and native arrays only.
- `Strata.Anim` holds at most 4,096 figures and `Strata.Physics` at most
  256 awake bodies and 4,096 in all. This layer lives within those caps and
  reports when it could not give an entity a part (section 4).

## The design

### 1. What an entity is

A world of entities is opened with a capacity, 65,536 by default, fixed for
its life like the renderer's quad capacity. Each entity is a row in a set
of packed arrays:

| Column | Holds |
|---|---|
| kind | index into the kind table |
| position, velocity | metres and metres per second, three floats each |
| facing | a turn about the vertical axis, radians |
| state | alive, seen or unseen, frozen, on the ground |
| body, figure | the `Strata.Physics` and `Strata.Anim` ids, or none |
| voices | up to two looping `Strata.Sound` voice ids |
| route | the route it follows and how far along it is, or none |
| age | seconds since it was spawned |
| slots | the game's numbers: floats and ints, as many as its kind asked for |

An id is a row number and a generation packed into one Int. Removing an
entity raises its row's generation, so an old id no longer matches. Rows
are reused.

A full turn is not stored. Facing is one angle because nearly every entity
only turns about the vertical. An entity that tumbles is a `body` mover and
its turn is read from the physics body.

### 2. Kinds

A kind is registered before entities are spawned, in the style of
`Strata.Blocks`: a default value and `with_` functions. A kind states:

- **Parts.** A rig id for its figure, or none. Up to two looping sound
  sources, or none. A body, only with the Body mover (section 3).
- **Mover.** One of the four in section 3, with its numbers: a box size for
  collision, whether gravity applies, a step height.
- **Unseen rule.** Live, freeze or remove; and, for live, a tick divisor
  (2 means its unseen entities are stepped every second tick with twice the
  time).
- **Lifetime.** Seconds after which an entity of this kind is removed, or
  none. A bullet sets this.
- **Slots.** How many float slots and how many int slots, at most 8 of
  each.
- **Seen distance.** Metres from a focus within which this kind is seen.
  The default, and the most allowed, is the stream's horizontal radius less
  two chunks, so that an entity is unseen before it reaches the edge of the
  loaded blocks (section 4). `open` is told the stream's radius and the
  chunk's side in metres.

### 3. Movers

**Placed.** Strata does not move it. The game sets its position.

**Flying.** Position advances by velocity; gravity is added if the kind
asks. While seen, each step is a ray along the motion against the kernel's
copy of the blocks (section 9), stopping at what `Strata.Query.raycast`
with the solid predicate would stop at; at a block hit the entity stops there and a block-hit event is reported
with the block and face. While unseen there are no blocks to hit and it
flies on until its lifetime ends or its unseen rule removes it. This is the
bullet, and it is the path that must be fastest.

**Walking.** While seen, the entity is a box swept against the kernel's
copy of the blocks, one axis at a time as `Strata.Query.sweep` does it,
with gravity and a step height, as the viewer's walker moves now. The game sets the velocity it wants along the ground, or
gives a route and a speed and Strata steers along it. While unseen, the
entity moves along its route at its speed, in a straight line between
waypoints, touching no blocks; with no route it stands still.

**Body.** The entity has a `Strata.Physics` body and goes where the body
goes. The kind gives either a box (half extents), which becomes a driven
body through `Strata.Physics.add_driven` and is moved each tick with
`Strata.Physics.drive` from the entity's wanted velocity, so physics does
its collision and it shoves dynamic bodies; or a `Strata.Body.Grid`, which
becomes a dynamic body through `Strata.Physics.add` and is read back each
tick. A body exists only while the entity is seen (section 4).

A body is legal only on a kind whose mover is Body; registering any other
combination is refused. A creature that must push crates is a Body mover
with a box. One that only walks and is hit is a Walking mover with no
body.

### 4. Seen and unseen

Each tick the game passes the focus points it already gives
`Strata.Stream`. An entity is seen when it is within its kind's seen
distance of any focus, measured horizontally, and becomes unseen once it is
more than one chunk's side beyond that, so one pacing at the edge does not
flicker between the two. With the cap on seen distance this leaves a whole
loaded chunk beyond the farthest seen entity. A seen entity that would
still step into a column the kernel has no blocks for (a chunk not yet
generated) is held where it is for that tick. The test is distance, not the view cone, so turning the
camera changes nothing.

**Becoming seen.** In order: the entity is set on the ground (below); it is
given its figure, its body and its voices; an "entered view" event is
reported so the game can start the right clip.

**Setting on the ground.** A walking entity arriving in view is at its
route's height, which may be inside a hill or above a pit a player dug. The
chunk under it may also not be loaded yet. So it waits, unseen, until the
column of chunks at its position is loaded; then Strata searches up and
down from its height for the nearest place its box fits with ground under
it, and puts it there. If none is found within 16 m it stays unseen and is
tried again next tick, and a counter reports how many are waiting. Flying
and placed entities are not moved.

**Becoming unseen.** Its figure, body and voices are removed and a "left
view" event is reported. Then its kind's unseen rule applies. A body-mover
kind whose rule is "live" keeps its last position and velocity and is
treated as placed until seen again.

**When a cap is hit.** `Strata.Anim.spawn_at` answers -1 when its world is
full. The entity is then seen without a figure, the refusal is counted per
kind, and Strata asks again each tick, nearest a focus first.

`Strata.Physics.add` never refuses; past its caps it leaves bodies waiting
or writes far sleeping ones into the world as blocks. So the entity world
keeps its own budget of bodies, set at `open` (default 128), and gives
bodies to the nearest entities that want one. An entity over the budget is
seen without a body, is counted, and is treated as placed until one is
free. If physics turns an entity's dynamic body into blocks
(`Strata.Physics.exists` becomes false), the entity stays, without a body,
and a "body lost" event is reported; the game decides whether to remove
it. The game's own bodies are outside this budget, and the game sizes the
two to fit the physics caps.

### 5. Routes

A route is a list of points in metres, held in the entity world and shared
by any number of entities. A game adds a route and gets an id; it sets an
entity's route, speed, and whether the route loops. Strata reports "route
finished" when an entity reaches the end. A route is freed by the game, or
automatically when asked to be and no entity uses it.

Routes hold at most 256 points each. The entity world holds a fixed number
of route points in all (262,144 by default), in one packed array.

This is the whole of what "holding paths" means here: the points live with
the entities, not the terrain, and are saved and copied with them
(section 8). How a route is found, and what a route should do when the
terrain along it changes, belong to the navigation project.

### 6. Finding and touching

Each tick, after movement, every live entity is put in a uniform grid of
4 m cells keyed by hash, built fresh. From it:

- `near(point, radius, kind mask)` gives the ids within a radius.
- `raycast(origin, direction, max, kind mask)` gives the first entity whose
  box the ray meets. A game wanting the exact voxel then asks
  `Strata.Anim` with that entity's figure.
- **Touches.** A game declares pairs of kinds that should report touching
  ("kind 3 against kind 5"). After movement, Strata tests boxes for those
  pairs only and reports each touching pair once per tick. A flying entity
  is tested along the whole of its step, so a fast bullet does not skip
  through a thin target.

Kinds are limited to 62 so a kind mask is one Int.

Touching reports; it does not push. Entities do not block each other unless
they have physics bodies, which do.

### 7. What a game sees

    Strata.Entities.kind() / with_figure / with_driven_body / with_dynamic_body / with_sound / with_mover /
        with_unseen / with_lifetime / with_slots / with_seen_distance
    Strata.Entities.open(kinds, capacity, bodies, radius_h, chunk_metres, voxel_metres) -- the entity world
    Strata.Entities.blocks(e, stream)           -- once a frame: the kernel's copy of the blocks
    Strata.Entities.touching(e, kind_a, kind_b) -- ask for touch events for this pair

    Strata.Entities.add(e, kind, x, y, z, facing)    -- (e, id); id -1 if full (`spawn` is not a legal name: GAPS S22)
    Strata.Entities.remove(e, id)
    Strata.Entities.alive(e, id) / seen(e, id) / kind_of(e, id)
    Strata.Entities.position / velocity / facing / age       -- reads
    Strata.Entities.set_position / set_velocity / set_facing -- writes
    Strata.Entities.float(e, id, n) / set_float / int / set_int  -- slots
    Strata.Entities.figure(e, id) / body(e, id) / voice(e, id, n) -- part ids, -1 if none now

    Strata.Entities.route(e, points) / free_route
    Strata.Entities.follow(e, id, route, speed, loops)

    Strata.Entities.each(e, kind)               -- the ids of a kind, as a native array
    Strata.Entities.near / raycast

    Strata.Entities.tick(e, world, physics, anim, sound, focus, dt)
    Strata.Entities.events(e)                   -- what the last tick reported
    Strata.Entities.stats(e)                    -- live, seen, waiting for ground, refused parts
    Strata.Entities.hash(e)                     -- equal runs give equal hashes

For bulk work a game can also take a kind's columns as native arrays
(positions, velocities, a slot) and write a whole column back in one call,
so a bullet pattern is one loop over arrays and not 20,000 calls.

`tick` does, in order: lifetimes; movement of seen entities; movement of
unseen entities due this tick; the seen and unseen switch; the grid;
touches; then pushes positions to figures (`Strata.Anim.place`) and voices
(`Strata.Sound.move`). The game calls it once per simulation step, after
its own behaviour code and before `Strata.Anim.update`.

### 8. Copying the state

`Strata.Entities.snapshot(e)` gives the whole entity world as a small set
of native arrays (the columns, the routes, the free rows and generations),
and `restore` rebuilds an entity world from them with every entity unseen;
the next tick gives the near ones their parts. Seen state is not kept: a
restored walker that was mid-fall is set on the ground again, and a
dynamic body's turn and spin are lost. `hash` covers exactly the arrays a
snapshot holds. Two things are promised and tested: a snapshot restored
and snapshotted again gives equal arrays, and with no focus (every entity
unseen) a restored world ticks to the same hash as the original. The kind table is not in the
snapshot: the game registers the same kinds before restoring, and a
snapshot records the kind count and slot counts so a mismatch is refused.

That is all this design provides toward saves and networking. The file
layout and the wire format belong to those projects.

### 9. Where the code lives

`Strata.Entities` in `lib/strata/entities.march` is the API and the kind
table. The columns and the hot loops (flying and unseen-route movement, the
grid, the touch tests, the seen test) live in a C kernel,
`native/strata_ent.c`, as figures live in `native/strata_anim.c`, reached
through `lib/strata/ffi/ent.march`. The C kernel knows nothing of Vulkan,
GLFW, physics or audio and has its own C tests under `native/tests`.

**The kernel's copy of the blocks.** `Strata.Query` walks March chunk
values C cannot read, and 20,000 March ray casts do not fit 2 ms. So the
kernel holds one collision mask per loaded chunk: 4,096 bytes, one bit a
voxel, in `Strata.Occupancy`'s layout, set where the block is solid (not
"solid and opaque", which is what `Strata.Occupancy` holds; glass must
stop a bullet). `Strata.Entities.blocks(e, stream)` is called once a frame
after `Strata.Stream.tick` and hands over the mask of each chunk loaded or
edited since the last call and drops each chunk unloaded. Seen flying and
seen walking both run in C against these masks. There is no per-entity
March loop. One bit a voxel cannot hold a model block's boxes, so the
kernel differs from `Strata.Query` in one place: a model block that
collides stops entities as its whole cell. Cube blocks and cross blocks
are as `Strata.Query` treats them, and on worlds without model blocks the
kernel's ray and sweep are tested to agree with `Strata.Query.raycast` and
`sweep`.

### 10. What this asks of other modules

- `Strata.Query.sweep` and `raycast` are not called; they are the
  reference the kernel's own ray and sweep are tested against.
- `Strata.Physics`, `Strata.Anim` and `Strata.Sound` are used through
  their existing calls: `add`, `add_driven`, `remove`, `drive`, `pose`,
  `exists`; `spawn_at`, `remove`, `place`; `play`, `move`, `stop`.
- `Strata.Stream` gains one read, `take_changed`: the chunks loaded or
  edited, and the chunks unloaded, since last asked, for the kernel's
  masks. (`take_meshes` and `take_unloads` are emptied by the renderer, so
  they cannot be shared.) Setting on the ground reads the kernel's masks.
- `Strata.Anim` gains one call, `place_many`, so 2,000 figures are placed
  in one crossing into C and not 2,000.
- Drawing figures is the animation project's job (its plan's later tasks).
  This layer hands figures their positions and draws nothing itself.

## Budget and targets

Measured on the machine `RESULTS.md` uses, in a release build, in
`RESULTS.md` when done. A miss is recorded, not hidden.

| Scene | Target for one `tick` |
|---|---|
| 20,000 flying entities, all seen, one touch pair against 200 targets | 2 ms |
| 2,000 walking entities, all seen, each with a figure | 4 ms, not counting `Strata.Anim.update` |
| 10,000 walking entities on routes, all unseen, divisor 4 | 0.5 ms |
| Spawn and remove 5,000 entities in one tick | 1 ms |
| Camera jump: 500 entities become seen at once | no tick over 8 ms; all on the ground within 30 ticks of their chunks loading |

## Testing

- **C tests** for the kernel: ids and generations, movement, the ray and
  the sweep against masks, the grid
  against a brute-force search, swept touches against a brute-force test,
  snapshot and restore.
- **March tests** (`test/entities_test.march`), headless: kinds and
  refusals, every mover against a small built world, the seen and unseen
  switch with hysteresis, setting on the ground (in a hill, over a pit, no
  ground), routes, lifetimes, events.
- **Properties** with `Check.all`: two runs of random spawns, moves and
  removes give equal hashes at every tick; `near` equals a brute-force
  filter; with no focus, a restored snapshot ticks to the same hash as the
  original.
- **Three scenes**, run headless by a small app, `entcheck`, as
  `animcheck` runs the animation scenes, each checking its result and
  printing its tick cost: a bullet storm (20,000 flying entities fired in
  patterns at targets), an arena (2,000 walkers with figures), and a city
  (10,000 unseen walkers on looping routes, then a camera jump to a far
  district, checking the workers there are standing on the ground).
- **In the viewer**, once the animation project draws figures: the arena
  and the city as scenes to walk through, and the storm with as many
  bullets given a figure as `Strata.Anim` holds. Until then nothing draws
  an entity and these wait.

## Order of work

1. The C kernel: rows, ids, columns, spawn and remove, with C tests.
2. `Strata.Entities`: kinds, open, spawn, reads and writes, slots, `each`,
   bulk columns, hash.
3. The kernel's block masks and `blocks`; the C ray, checked against
   `Strata.Query.raycast`. Flying and placed movers, lifetimes, block-hit
   events.
4. The grid, `near`, `raycast`, touch pairs. The bullet storm scene.
5. The C sweep, checked against `Strata.Query.sweep`. Walking while seen.
   Routes and unseen movement.
6. The seen and unseen switch, setting on the ground.
7. Parts: figures, bodies, voices, cap handling. The arena scene.
8. The body mover.
9. Snapshot and restore. The city scene. Budgets into `RESULTS.md`.
10. The viewer scenes, when the animation project draws figures.

## Left to the builder

- The exact packing of an id, the grid's hash and cell size, and the layout
  of the columns, provided the targets are met.
- The form of the event list (a native array of fixed-size records is
  suggested, read through accessor functions as `Strata.Physics` events
  are).
- How steering along a route while seen handles a blocked step (suggested:
  keep pushing toward the next waypoint and report "blocked" after one
  second without progress; the game decides what to do).
- Names of functions, where they do not collide with a March keyword
  (`spawn` cannot be a function name: GAPS S22).

## Risks

- **A second copy of collision.** The kernel's ray and sweep repeat what
  `Strata.Query` does in March, and the two can drift. The comparison
  tests are the guard. The masks also cost 4 KB a loaded chunk (about
  13 MB at the reference 3,087 chunks).
- **Physics caps.** 256 awake bodies means an arena of 2,000 cannot give
  every entity a body. Kinds that only need to be hit and to walk should
  not ask for one; the touch tests and the Walking mover cover them.
  The design says so in the kind documentation.
- **Routes through changed terrain.** An unseen walker follows its route
  through a wall built after the route was made, and is then set on the
  ground on the wrong side of it. This is accepted for now (decision 4) and
  is the navigation project's to improve.
- **Tick order with physics.** Physics steps on a worker and reports a step
  late. A body-mover entity's position therefore lags its body by up to one
  physics step; drawing uses the body's interpolated pose, so it does not
  show, but game logic reading the entity's position sees the lag.

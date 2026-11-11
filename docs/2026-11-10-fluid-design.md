# Strata fluids: design

Status: design, awaiting approval. Nothing here is built.
Date: 2026-11-10
Follows: `2026-11-10-strata-design.md` (the first piece of project 4) and
the lighting built from `2026-11-10-lighting-design.md`.

## Intent

Water in a Strata world sits still. This project makes it move: dig out the
bank of a lake and the water runs downhill, pools in any hollow it reaches,
and otherwise carries on to the sea, while the lake's level drops until it
is empty. Where water falls it throws spray, more of it and faster for a
bigger, harder fall.

## Decisions the owner made

1. Released water flows downhill, collects in a lower area, or otherwise
   flows out to sea.
2. A breached lake empties. Water is a finite amount that moves; nothing
   refills it.
3. Falling water has particle effects that depend on the force and the
   volume of the water falling.
4. From the first design: frame rate stays the priority under heavy
   simulation; the library names no game concept.

Everything else below is the designer's call.

## Goals

- Water conserves its amount: what leaves one place arrives in another,
  apart from what the sea takes.
- A voxel can be partly full, so a draining lake's surface goes down
  smoothly and a thin stream is thin.
- Water at rest costs nothing. Only water that is moving, or whose
  surroundings just changed, is looked at.
- Any edit can set water moving: digging a bank, opening a floor, placing
  a block that displaces nothing but blocks a channel.
- Moving water is drawn, shadowed, reflected and lit by the renderer that
  exists, because it is ordinary blocks changed by ordinary edits.
- A game can ask what the water did this step (how much fell, where, how
  far) to place particles and sound.
- Spray particles for falls, drawn in large numbers at little cost.
- A stated CPU budget for the simulation, measured.

## Non-goals

- Pressure: water does not rise in a pipe to find its own level, and does
  not push uphill behind a dam.
- Smooth surfaces, waves, or currents that carry objects.
- More than one fluid interacting (lava meeting water).
- Rain, springs, evaporation. A game adds or removes water with an edit.
- Erosion. Water does not move terrain.
- Particles that collide with the world or carry water. They are a
  picture only.
- Simulating water in chunks that are not loaded.

## Anti-goals

- Water that multiplies or vanishes: an empty bucket that fills a valley,
  or a stream that dies in mid-air.
- A lake that shimmers forever because the simulation never decides it is
  at rest.
- A hitch when a large lake is breached. The work per step is bounded and
  the rest waits.
- A second copy of the world's water that can disagree with the chunks.

## Constraints

- March and the existing C shim. `scripts/verify.sh` stays green.
- Fluid steps run through `Strata.Edit` and `Strata.Stream`, so meshing,
  the light volume, collision and change records need no fluid-specific
  path.
- Deterministic: the same world and the same edits give the same water,
  step for step.
- No Vulkan type crosses into March.

## The design

### 1. What water is

A fluid is a family of block states: one block id per fill level, eight
levels to a voxel, level 8 a full voxel. The registry already has state
families; a game registers its water as one and tells the fluid module
which family it is and how it behaves:

- **flowing**: finite water, the kind this project moves;
- **a sink**: a block that swallows flowing water that reaches it and is
  never used up or moved. A game makes its sea a sink.

The viewer's sea becomes a sink, and its terrain gains lakes above sea
level that are flowing water, so there is something to drain.

### 2. How it moves

Time advances in fluid steps, ten a second by default, independent of the
frame rate. Each step looks only at **active** cells and applies, in a
fixed order:

1. **Fall.** Water moves into the cell below as far as that cell has
   room.
2. **Spread.** What could not fall shares itself with the four side
   neighbours that are lower, levelling toward them. Water that has a way
   down nearby prefers it, so a stream follows a slope instead of
   widening forever.
3. **Sink.** Water beside or above a sink is taken by it.
4. **Rest.** A cell whose amount did not change and whose neighbours did
   not change leaves the active set.

A step's changes are gathered and applied as one edit. A cell becomes
active when it, or a cell next to it, is changed by any edit, the fluid's
own included. Loading a chunk does not wake its water.

The last eighth is the hard part: a thin film must not creep across flat
ground forever, and a drained lake must not keep a wet bed. Designer's
calls:

- Water that cannot fall and has no lower neighbour looks for a way down
  within a short distance (the builder picks it; about eight cells) and
  moves toward the nearest one. So a bed drains to its breach.
- With no way down in reach, a single eighth stays where it is, at rest.
  Water in a true hollow stays as a puddle: that is collecting in a lower
  area.
- "Empty" means no cell of the lake above the breach's floor holds more
  than one eighth, and what remains is under 2% of the lake.

Rules read the state as it was at the start of the step and resolve
competing moves into one cell by a fixed order of directions, so the
result does not depend on the order cells are visited in.

Falling water may drop up to four cells in a step, so a waterfall keeps
pace with the lake feeding it. Sideways, water moves one cell a step.

Each moving cell carries how far its water has fallen without landing.
That number lives in the active set, not in the chunks, and is lost if
the cell comes to rest.

### 3. Bounds

- A step handles at most a set number of active cells, nearest the focus
  points first. The rest wait, in order, and nothing is lost.
- Water at the edge of the loaded world stops there, at rest, until the
  neighbouring chunk loads; the load wakes the edge.
- When a chunk unloads, its active cells are set aside, not dropped, and
  resume when it loads again. A waterfall the player walks away from is
  still falling when they come back; it does not hang in the air.
- The active set, with the cells set aside, is the only fluid state
  outside the chunks. A save format, when there is one, stores it.
- Determinism holds for the same world, the same edits and the same
  focus points over time: the cap chooses which cells wait by distance
  from the focus, so where the player stood is an input.

### 4. What a game sees

    Strata.Fluid.new(registry, families)           -- which block families are fluids, and of which kind
    Strata.Fluid.step(fluid, stream)               -- one fluid step; returns the stream with the edit applied
    Strata.Fluid.wake(fluid, outcome)              -- tell the fluid about an edit the game made
    Strata.Fluid.events(fluid)                     -- what moved this step
    Strata.Fluid.active_count(fluid)

An event says where water fell, how many eighths, and how far it has
fallen without landing. That is the force and volume the owner asked for.
A game calls `step` on its own clock; the viewer calls it ten times a
second.

### 5. Drawing partial water

A partly full voxel draws with its top lowered to its level, and its
sides only as tall as the water. Still water at level 8 draws exactly as
it does today. Falling water (a column with water above it) draws full
height so a waterfall is a sheet, not a staircase.

### 6. Spray

Particles are small lit squares that fly on a fixed path: each is given a
starting point, a starting velocity, a birth time and a lifetime, and the
GPU works out where it is from the clock. Nothing is simulated per
particle per frame and nothing is read back.

    Strata.Render.spray(x, y, z, vx, vy, vz, count, spread, life, tex)

The viewer turns fluid events into spray: where water lands after a
fall, a burst whose count follows the eighths that fell and whose speed
follows the distance fallen; along a fall, a thin mist. Particles fade
out over their life, are lit by the sun and sky without rays, and are
drawn after the water.

### 7. The viewer

- Lakes above sea level, in basins the terrain already has.
- A scripted scene that breaches a lake and one that opens a floor under
  it, for golden images at fixed steps.
- The existing carve and build tools are enough to play with it by hand.

## Budget and targets

Designer's targets, stated so a miss is reported.

| | target |
|---|---|
| CPU per fluid step at the default cap | 2 ms or less |
| active cells handled per step (default cap) | 4096 |
| worst frame while a lake of 20 000 voxels drains | 16.7 ms or less |
| that lake, breached at its foot by the viewer's carve sphere (radius 4), empty within | 2 minutes |
| water at rest after the last change | within 5 seconds, then no active cells |
| amount conserved (no sinks in play) | exactly |
| live particles without dropping under 120 fps | 50 000 |

## Testing

- **Pure logic in March tests:** each rule on small hand-built worlds;
  conservation as a property test over random terrain and random edits
  (total eighths before and after any number of steps are equal when
  there is no sink); every world reaches rest; determinism (two runs give
  the same hashes); the cap (work waits and is not lost); the loaded
  edge; the film on flat ground stays put.
- **Golden images:** a lake before, during and after a breach; a
  waterfall with spray; partial levels beside full ones.
- **Soak:** the existing flight with a lake draining under it, with the
  same assertions plus the fluid's step time.

## Order of work

One plan. Each step leaves the viewer working.

1. Fluid families and the rules, headless, with the property tests.
2. The active set, the cap, the loaded edge, rest.
3. Stepping through the stream in the viewer; highland lakes; the sea as
   a sink. Water moves, drawn as full blocks.
4. Partial levels drawn at their height.
5. Spray: the particle draw, the event feed, the viewer's waterfalls.
6. Scenes, soak, measurements against the targets.

## Left to the builder

- How far water looks for a way down, and the order that settles competing moves.

- The exact spreading rule and how far it looks for a way down.
- How the active set is stored and ordered.
- How a lowered water top reaches the vertex shader (the light class
  table has spare words and already rides with each quad).
- The particle buffer's layout, its capacity, and what happens when it
  is full.
- Where highland lakes go in the viewer's terrain.

## Risks

- **Remeshing cost.** Every step that moves water dirties chunks, and a
  draining lake touches many. The cap bounds it; the soak measures it.
  If it is too slow, fluid steps drop to five a second before anything
  else changes.
- **Never resting.** Levelling rules can oscillate. Rest is a tested
  property, not a hope.
- **Looks.** Eight levels may read as steps on a slow drain. If so, the
  draw can lean a surface between neighbouring levels; the simulation
  does not change.
- **Lighting cost.** Moving water resets light history near it each
  step, and water is already the renderer's most expensive surface.
  Measured in step 6.

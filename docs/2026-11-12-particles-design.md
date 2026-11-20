# Particles: design

Status: awaiting approval. Branch `claude/particle-effect-system-599ddd`,
on main after the combat merge (887e2ee), built with March main
(`scripts/env.sh` sets `MARCH_HOME` so forge uses the compiler on PATH).

## Intent

Strata gets a particle effect system. A game describes an effect as data
(how its particles look and move over their life) and emits it from
places, once or continuously, without simulating anything per particle
on the CPU. Strata names no game concept: there is no "fire" in the
library, only effects a game defines.

The first user is the viewer's match (`STRATA_COMBAT=1`):

- a fireball trails flame and smoke as it flies and bursts in sparks and
  smoke where it lands;
- what a fireball burns now burns for a few seconds, in flames and smoke,
  before it is gone.

Why: it is a Strata feature. Games built on Strata (the match, city
builders, small urban FPS maps) need smoke, sparks, dust, fire and the
like; the match is the proving ground.

## Goals

- `Strata.Particles`: effect definitions, emitters and their spawning, in
  March.
- Particles drawn on the GPU from a closed form, as spray is today: the
  CPU writes a particle once, at birth, and never again.
- Two looks: **glowing** (added to what is behind, not shaded, as flame
  and sparks) and **shaded** (blended over what is behind, lit by the sun
  and sky as spray is, as smoke, dust, steam).
- Over its life a particle's colour and opacity, and its size, follow a
  ramp of up to four keys.
- Motion: start position and velocity with jitter, a constant
  acceleration (gravity down, or buoyancy up), and linear drag. A small
  per-particle wobble so a column of smoke is not a set of straight lines.
- Emitters: a point, a sphere or a box; a burst of N now, or a rate per
  second that the game moves each frame and stops when it likes.
- The viewer's match: fireball trail, fireball burst, burning that takes
  seconds.
- `spray` keeps working, its look unchanged (it becomes one effect).

## Non-goals

- Particles that collide with blocks or bodies, or affect anything.
  (Sparks bouncing off the ground would need a compute pass; the effect
  format is meant to survive that change.)
- Sorting particles by depth. Glowing particles add and need none; shaded
  ones are drawn in birth order, which is acceptable for soft smoke.
- Particles that cast light. The light comes from blocks (below) and from
  traced figures, as today.
- Fire that spreads from cell to cell, or hurts what stands in it. The
  fireball's damage stays as it is, dealt at impact.
- An editor or file format for effects. Effects are March values.

## Anti-goals

- Per-particle work on the CPU each frame, or a per-frame upload of the
  particle buffer.
- Moving March work into C to make it fast. The shim writes particle
  records into the mapped buffer (as `sv_spray` does); deciding what to
  emit, where and how many, lives in March. If emission is slow in March,
  that is a March gap to record (see GAPS), not a reason to move it.
- A match that looks different in its stored scenes for reasons other
  than particles and the burning delay.

## Constraints

- Vulkan renderer in `native/strata_vk.c`; particles are drawn in the
  final pass after the translucent quads, as spray is (kind 6), depth
  tested, not written. The final pass is display-referred (spray tonemaps
  in its own shader), so glow is an additive blend in that space.
- The ring of 65 536 particles, appended in birth order, live span from
  the oldest still alive to the newest (`native/strata_spray.h`), stays.
- Texture layers come from the game's tile array (the viewer's
  `StrataViewer.Textures`, 16 x 16 tiles painted in code). Layer 13 is a
  soft white dot today.
- Build with March main; `forge test` for March tests; `scripts/verify.sh`
  is the gate; `scripts/combat_check.sh` checks the match.

## Approaches

Considered:

- **A. Closed-form particles on the GPU, extending spray (chosen).** Each
  particle is written once with its birth, life, start, velocity and
  effect; the vertex shader computes where it is and how it looks from the
  clock and the effect's table. No CPU work per particle per frame, the
  same 65k ring. Costs: no collisions, no forces that depend on the world.
- **B. Particles simulated in March on the CPU each frame.** Collisions
  and arbitrary forces, at a per-frame CPU and upload cost, and the work
  would mostly be fighting March's known hot-loop gaps rather than
  effects.
- **C. Particles simulated by a GPU compute pass with state.** Most
  capable (collisions against the light volume), about twice A's work;
  fire does not need it. A's effect format can be carried into C later.

The owner chose A.

## The design

### The particle record (GPU)

Sixteen floats (64 bytes; 4 MB for the ring):

| floats | meaning |
|---|---|
| 0-2, 3 | start position (metres), birth time (seconds) |
| 4-6, 7 | start velocity (m/s), life (seconds) |
| 8 | effect index |
| 9 | size scale (multiplies the effect's size ramp; per-particle jitter) |
| 10 | seed (0..1: picks wobble phase, colour jitter) |
| 11 | rotation of the sprite at birth (radians) |
| 12 | texture layer override (-1: the effect's own layer) |
| 13-15 | spare (zero) |

### The effect table (GPU)

Up to 64 effects, held in a uniform buffer the particle shaders read
(a new binding beside the frame's uniforms). An effect is:

- `look`: glowing or shaded;
- `layer`: the texture layer;
- `colour`: up to four keys (age 0..1, linear RGB, opacity); between keys
  linear; one key means constant;
- `size`: up to four keys (age 0..1, metres across);
- `accel`: (ax, ay, az) m/s² (gravity is (0, -9.8, 0); smoke rises with a
  positive ay);
- `drag`: k per second; velocity tends to accel / k;
- `wobble`: metres of sideways sway and its rate in Hz;
- `spin`: radians per second the sprite turns.

Position at age t, with drag k > 0:
`p = p0 + (v0 - a/k)(1 - e^{-kt})/k + (a/k) t`, and with k = 0 the
ballistic `p0 + v0 t + a t²/2` (what spray does today, a = gravity).
Wobble adds a horizontal offset `w * sin(2π f t + seed·2π)` on two axes
with different phases.

Shaded particles keep spray's lighting (sun where the sun reaches, sky
under a roof, fog, tonemap), with one difference for cost: an effect is
lit **per pixel** (two rays a fragment, as spray is; effect 0 only, by
default) or **per particle** (the two rays traced once in the vertex
shader, the default for game effects). Large, long-lived smoke traced per
pixel could cost more than the rest of the frame. (Builder's call; no
frame budget was set beyond the existing 16.7 ms; the cost of a match
with a burning tree is measured and recorded in RESULTS.) Glowing particles are `colour * opacity`
added, fogged toward nothing with distance.

Drawing: the ring is drawn twice, shaded pipeline then glowing pipeline,
each one discarding (degenerate vertex) the other kind. Both are in the
live span; the per-frame cost is the vertex shader on the live span twice.

### `Strata.Particles` (March)

```march
-- an effect, built from a default and setters, as Strata.Blocks builds a Def
let flame = Pt.effect()
  |> Pt.glowing()
  |> Pt.layer(13)
  |> Pt.colour([(0.0, 1.0, 0.9, 0.5, 0.0), (0.15, 1.0, 0.6, 0.15, 0.9), (1.0, 0.6, 0.1, 0.0, 0.0)])
  |> Pt.size([(0.0, 0.25), (1.0, 0.05)])
  |> Pt.accel(0.0, 3.0, 0.0) |> Pt.drag(2.0) |> Pt.wobble(0.1, 1.5)

-- registered with the renderer once; the answer names it
let (fx, flame_id) = Pt.define(fx, flame)            -- refused past 64

-- a burst: count particles from a shape, a mean velocity and its spread, a life and its spread
let fx2 = Pt.burst(fx, flame_id, Pt.sphere(x, y, z, 0.3), (0.0, 1.0, 0.0), 1.5, 30, 0.8, 0.3)

-- an emitter: a rate per second, moved and stepped by the game each frame
let (fx3, e) = Pt.emitter(fx2, flame_id, Pt.point(x, y, z), (0.0, 0.5, 0.0), 0.5, 120.0, 0.6, 0.2)
let fx4 = Pt.move(fx3, e, Pt.point(x2, y2, z2))
let fx5 = Pt.step(fx4, dt)                           -- spawns what each emitter owes this frame
let fx6 = Pt.stop(fx5, e)                            -- particles already out live their life
```

- `Particles` is a value the game threads, as `Strata.Sound` is. It holds
  the defined effects and the live emitters. `step` gives each emitter
  `rate * dt` plus its carried fraction, and spawns the whole part.
- A moving emitter spawns along the segment from its last position to
  this one, so a fast fireball leaves a continuous trail, not clumps.
- Randomness is a hash of a counter in the `Particles` value: two runs
  with the same calls draw the same particles (the stored scenes depend on
  it).
- Spawning goes through one FFI call per burst or per emitter step,
  carrying the shape, mean, spreads and a seed; the shim writes the
  records (as `sv_spray` writes spray's). This keeps per-particle FFI
  calls out. (Whether March fills a native array of records and hands it
  over in one call instead is the builder's to measure; see "Left to the
  builder".)
- `Strata.Render.spray` stays, its signature and its pictures unchanged
  (builder's call, below): it keeps its own record writer in the shim, with
  its own jitter (the 0.35 x 0.15 x 0.35 m start box, upward-only vertical
  spread, life 0.7 to 1.0 of the given one) and its own hash sequence, and
  writes records of a built-in effect 0 (shaded, ballistic under gravity,
  no drag, opacity fading over the last third, lit per pixel as today),
  with the call's texture layer in the record's override. The goldens with
  spray in them (`lake-breached`, `lake-draining`) must pass unchanged.
- Statistics: live particles, and how many were overwritten while still
  alive (a full ring), through `Strata.Ffi.Vk.stat`.

### The viewer's match

Effects, defined at the start of a match (`StrataViewer.Match` or a
small `StrataViewer.Fx` beside it):

- **flame**: glowing, yellow-white to orange to dark red, rising,
  shrinking, 0.4 to 0.8 s;
- **ember**: glowing, small, orange, ballistic with light drag, 0.6 to
  1.5 s, a few at a time;
- **smoke**: shaded, grey, rising slowly, growing from 0.3 to 1.2 m,
  fading in and out, 2 to 4 s;
- **spark**: glowing, small, bright, fast and falling, 0.3 to 0.6 s;
- **steam**: shaded, white, rising, for ice that melts.

A flame texture layer is added to `StrataViewer.Textures` (a soft
teardrop) beside the soft dot (13); smoke uses the dot.

**Fireball in flight**: an emitter on each fireball, moved to its entity
each frame: flame about 200 a second, smoke about 30 a second, embers
about 20. Stopped when the fireball is gone.

**Fireball burst**: sparks (about 80), flame (about 60), smoke (about
20), in a sphere of 0.5 m about the point. Over water: steam and no fire
(as today it burns nothing there).

**Burning that takes seconds** (game rules, in the viewer):

- `Combat.fire_changes` still says which cells burn and what they leave.
  A burst now schedules them instead of applying them at once: each cell
  catches after a delay that grows with its distance from the point (0 at
  the centre, about 0.5 s at 2 m) and burns for a time set by its block,
  with a jitter from a hash of the cell:
  - tall grass: 0.5 to 1 s;
  - grass: 1 to 2 s (then dirt);
  - leaves: 2 to 3 s;
  - wood: 4 to 6 s;
  - ice: 0.5 to 1 s, melting (steam, no flame).
- A burning cell (wood, leaves, grass) is set to an **embers** block the
  viewer registers, with emission so the ray-traced light lights the
  ground about it. Two such blocks (builder's call): **burning wood**,
  opaque, a charred bark texture with glowing cracks, for wood and for
  grass (ground is opaque too); and **burning leaves**, cutout like leaves,
  the leaves texture's holes kept, its leaf pixels glowing, so a burning
  canopy still lets light and sight through. Tall grass is not swapped (a cube where a blade was would look
  wrong); it burns as it stands and is removed at its end.
- While burning, a cell has a flame emitter (rate by block: wood and
  leaves more than grass) and a smoke emitter at its top.
- At its end the cell becomes what fire leaves (air, or dirt for grass),
  its emitters stop, a last puff of smoke rises. Support (on in a match)
  then drops a tree whose trunk burned through, as it does today, but
  seconds later.
- The cells burning at once are capped (256); past that a burst's cells
  are applied as today, at once, with a burst of smoke. Emitters cap
  likewise (the particle ring overwrites its oldest when full).
- A cell already burning or burned is not scheduled again. A cell edited
  by something else while it burns (dug, built over) is dropped from the
  schedule: at its end, if it no longer holds embers (or, for tall grass,
  tall grass), it is left alone.
- The fire report (`combat: fire burned ...`) counts cells when they
  catch, so the fire scene still reports what burned; its run may need
  more frames for the trunk to finish.

## What changes where

- `native/strata_spray.h`, `native/strata_vk.c`: the record, the effect
  table buffer and its binding, the two pipelines, spawn calls by shape.
- `native/shaders/spray.vert`, `spray.frag` (or `particle.*`): the closed
  form, ramps, the two looks.
- `lib/strata/particles.march` (new), `lib/strata/ffi/vk.march`,
  `lib/strata/render.march` (spray as an effect), `lib/strata.march`.
- `viewer/lib/textures.march` (flame layer, embers texture),
  `viewer/lib/strata_viewer.march` (embers block, `Particles` threaded
  through the frame), `viewer/lib/match.march`, `viewer/lib/combat.march`
  (the burn schedule, pure).
- Tests: `test/particles_test.march` (emission counts, carried fractions,
  segment spawning, determinism, refusals); `viewer/test/combat_test.march`
  (the burn schedule: delays, durations, what is left, caps, cells edited
  meanwhile); `native/tests/` for the shim's record writer; a golden or
  two (`scripts/golden.sh`): a fireball in flight and a trunk burning;
  `scripts/combat_check.sh`'s fire scene updated.
- Docs: README (a Particles section), RESULTS (cost: live particles, CPU
  of `step`, GPU of the two draws), a ledger.

## Calls made in writing this up

Settled without the owner, after a reviewer's read; each can be overruled:

- Spray keeps its own writer and its stored pictures (above).
- Game effects are lit once per particle, spray per pixel (above).
- Two embers blocks, wood-and-grass and leaves (above).
- Effects are defined once per run, not per match (matches repeat, and
  there are 64 slots); a match's restart drops its burn schedule and
  stops its emitters.
- Particles spawned along a moving emitter's segment are born at the
  frame's clock, their start spread along the segment; their birth times
  are not staggered.

## Left to the builder

- The exact uniform layout of the effect table, and whether a texture
  buffer suits better than a uniform buffer.
- Whether spawning writes records in the shim from a description (as
  above) or March fills a native array and hands it over; measure both if
  the first is not clearly enough.
- The colours, rates and timings above are starting points; tune them by
  looking.
- Whether glowing particles also feed the light pass's emission (they do
  not in this design).

## Addendum, 19 November: particles that meet the world

The non-goal above ("particles that collide with blocks") is lifted for
blocks, as an option an effect asks for. Bodies and figures stay out.

### The problem

A particle is a closed form of the clock; it has no state to stop. A
collision must therefore be a fact about the whole flight, known once, that
the closed form can branch on.

### Considered

- **In the vertex stage, every frame.** Walk the flight from birth to now
  against the light volume for each of a particle's 36 vertices. Sees edits
  made mid-flight, but pays tens of rays a vertex a frame for as long as the
  particle lives: the opposite of "written once".
- **At birth, on the CPU.** The shim would need the voxels (it keeps no copy
  of the light volume's pages) and a second ray walker in C beside the
  shader's; work moved into C that the GPU already knows how to do.
- **At birth, on the GPU (chosen).** A compute pass, run only in a frame
  that bore particles of an effect that asks, walks each new particle's
  whole flight once against the light volume (`trace.glsl`, the walker the
  lighting uses) and writes what it finds into the record's three spare
  floats. From then on the particle is a closed form again, in two or three
  pieces.

### The design

- **The effect** says what its particles do on meeting a solid voxel
  (table floats 41 to 43, unused until now): 0 nothing (the default, and
  spray), 1 **die**, 2 **rest** (stop there, keep ageing through its ramps,
  its tumble frozen), 3 **bounce** once (velocity mirrored about the face
  met, the normal part times a restitution, the rest times what friction
  leaves) and rest at the next meeting. `Pt.dies_on_solid`,
  `Pt.rests_on_solid`, `Pt.bounces(restitution, friction)`,
  `Pt.meets_nothing`.
- **The record's spares**: [13] the age in seconds of the first meeting, or
  a number past any life for none; [14] the faces met, first + 8 x second
  (0 +y, 1 -y, 2 +x, 3 -x, 4 +z, 5 -z); [15] the age of the second meeting
  (bounce only). The shim writes [13] = -1, "not yet looked for", into a
  particle of an effect that asks; the pass replaces it in the same frame,
  before the particle is first drawn.
- **The pass** (`collide.comp`): one invocation a particle born since the
  last frame. The flight (drag, acceleration and sway included) is cut into
  32 chords of equal time over the particle's life and each chord traced;
  the meeting's age is the chord's start plus its share of the chord. On a
  bounce the second leg starts from the meeting and takes what chords are
  left. A particle born inside a solid voxel (a burst at the point of an
  impact) flies free until a chord's end is in the open. Solid voxels stop
  a particle, and cutout ones where their texture is opaque; water, glass
  and thin blocks do not. Bodies and figures are not looked at (a
  preprocessor switch leaves them out of the walker): a particle resting
  where a body was would hang in the air when it moved.
- **The vertex shader** branches once on the effect's mode. Mode 0 runs the
  code it ran before. Otherwise, past the first meeting: die sends the
  particle off screen; rest evaluates the flight at the meeting's age;
  bounce flies a second closed form from there (no sway) until the second
  meeting. A resting particle is lifted half its size along the face it
  met, eased in over a tenth of a second, so a cube sits on the ground
  rather than in it.
- **Cost for effects that do not ask**: no dispatch (the shim counts
  colliding births; a frame with none binds nothing), and one comparison in
  the vertex shader on a table row it already reads.

### Limits, by construction

- The world is looked at once, at birth. A block placed in a flight's way
  afterwards is flown through; one removed still stops the particle, and a
  particle resting on it stays in the air until its life ends.
- A flight that leaves the loaded chunks meets nothing from there on, and
  chunks loaded after the birth are not seen.
- A particle is a point: its middle meets the voxel, and a large one
  overlaps a wall by half its size before it stops.
- One bounce. A chord can step over a corner thinner than its sag (a
  centimetre or so for a one-second life).
- Water, glass, thin blocks, bodies and figures stop nothing.
- Spray (effect 0) is unchanged and meets nothing.

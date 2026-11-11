# Strata animation: design

Status: design, awaiting approval. Nothing here is built.
Date: 2026-11-11
Follows: `2026-11-10-strata-design.md`. A new project beside lighting, fluids,
audio and interface text. Draws through the renderer of
`2026-11-10-renderer-design.md`, is lit by `2026-11-10-lighting-design.md`,
and asks the world questions through `Strata.Query`. Its drawing builds on
the moving-object path of `2026-11-11-physics-design.md` (physics plan
Tasks 5 and 6), which is not built yet; everything else depends on nothing
that is not built.

## Intent

Everything Strata draws today is part of the voxel grid. This project adds
things that move independently of the grid: people, NPCs, enemies, animals,
birds, fish and vehicles. Strata provides the machinery to build them,
animate them, and let their motion respond to the world around them, in
the spirit of Teardown. The game decides what each one is and what it does.

## Decisions the owner made

1. Characters walk, run, jump, crouch, swim and swing weapons. Other moving
   things include vehicles, birds and fish, and the engine assumes no body plan.
2. Figures are built from rigid voxel parts joined at pivots, so the look
   is blocky and jointed. Nothing bends smoothly.
3. Teardown is the reference point, and what the owner takes from it is
   motion that reacts to the world rather than replaying a fixed loop.
4. Hit reactions, ragdolls and similar responses are left to the game.
   Strata provides the means (setting any joint, blending over playing
   motion, asking the world questions) but no physics simulation.
5. A basic code API builds figures and motions. There is no editor and no
   file importer.
6. Scale: potentially a lot of figures at once, like a busy arena shooter.
7. Poses need only repeat exactly on the same machine, not across
   machines.
8. Figures can optionally take part in the traced lighting, per figure: cast
   traced shadows, appear in reflections, darken sky light, and light the
   world with their glowing voxels. Figures that do not opt in use a cheaper
   path.
9. From the first design: high frame rate under load is the priority, and
   the library names no game concept.

Everything else below is the designer's call, and is marked where it is
not obvious.

## What most engines do (for reference)

- **Rigid-part animation** is how Minecraft entities, Blockbench models,
  MagicaVoxel-based games and Teardown's vehicles and robots work. Each
  part is a mesh with a transform. It is cheaper than skinned meshes (no
  per-vertex blending) and suits the voxel look.
- **Layered playback with masks and crossfades** (Unity's Animator layers,
  Unreal's layered blend per bone) is the usual way to run while swinging.
- **Procedural adjustment after playback** is the usual way motion is made
  to react to the world: two-bone IK for feet and hands, look-at chains,
  and spring "jiggle" joints. Teardown's robots go further and drive their
  legs entirely from IK and physics. Strata provides the pieces without
  the physics.

## Goals

- A game builds a **rig** in code: a tree of voxel parts with a palette,
  pivots and named attachment points.
- A game builds **clips** in code: keyframed rotation and movement per part,
  looping or not, with marked moments.
- A **figure** is a rig placed in the world. Many figures share one rig.
- Several clips play on one figure at once, each on its own **layer** with
  its own weight and its own set of parts, and switching clips crossfades.
- Motion adapts to the world: feet find the ground, a chain of parts aims
  at a point, a limb reaches for a point, loose parts lag and sway, and the
  game can set any joint at any moment over what is playing.
- The game is told when a clip crosses a marked moment, like a foot landing
  or a blade arriving.
- A figure can opt in to traced light, joining the world in the rays: its
  shadows are traced, it shows in reflections, it shades the sky light
  near it, and its glowing voxels light what is around it.
- A figure can hang from another figure's attachment point: a sword in a hand,
  a rider on a mount, a turret on a vehicle.
- Figures can be hit-tested at the voxel level: a ray answers which figure,
  which part and which voxel. A box answers which figures it overlaps.
- Figures are lit, shadowed and fogged like the world, and cast sun shadows.
- Hundreds of figures animate and draw at once within a stated budget.
- Everything except drawing works headless, so a game server or a test
  can pose figures and hit-test them.

## Non-goals

- Physics inside animation: no ragdolls, collision response or forces on
  parts. The game moves a figure itself, with `Strata.Query.sweep` or, once
  physics lands, a driven `Strata.Physics` body standing in for it.
- Smooth deformation, skinning or squash and stretch. Parts are rigid.
  Part scale is not animated.
- Importing MagicaVoxel, Blockbench or any other file. The API is built so
  an importer can sit on top of it later.
- An animation editor or state-machine language. Which clip plays when is
  game logic.
- Root motion extracted from clips. A clip can declare how far one loop
  carries the figure, so a game can match playback speed to ground speed.
- Changing a figure's voxels after its rig is finished, such as carving
  damage into a character. Room is left (see Limits).
- Traced light for every figure at once in a crowd. Traced figures are
  capped (section 9) and the rest use the shadow map.
- Behaviour, pathfinding and AI.
- Networking, lag compensation, and poses that agree across machines.

## Anti-goals

- A body plan built into the library (a "humanoid" type, a "leg" that only
  works with two legs). Foot placement is a helper over generic parts.
- Per-frame work in March proportional to the number of joints. The pose
  path for hundreds of figures runs in C.
- A pose that differs between two runs given the same commands and the
  same `dt`.
- Moving figures that ghost or smear because the lighting's frame history
  assumes the world is still.
- A library call that can block the frame on a worker, a file or the GPU.

## Constraints

- March `origin/main` through `scripts/env.sh`. `scripts/verify.sh` stays
  green after every task.
- No C or Vulkan type crosses into March: small integers, floats and native
  arrays only.
- The animation C code knows nothing of Vulkan or GLFW and builds and tests
  headless, as `strata_mix.h` does for audio. The renderer reads poses from
  it in C, not through March.
- forge does not build a dependency's C sources (GAPS S11). A consumer
  lists `native/strata_anim.c` in its own `[ffi]` sources, and a consumer
  that draws lists it beside `strata_vk.c`. Header-only changes need the
  stamp line (S14).
- MoltenVK on an M3 Max, Vulkan 1.2 plus the features already required
  (renderer design). Multi-draw replay cost on MoltenVK is a known risk.
- A value shared between worker tasks is not passed through a hot loop
  (S15).
- macOS on an M3 Max is the only measured platform.

## The design

### 1. The split

March describes and decides. C does the per-frame pose arithmetic. This is
the same split the audio project uses.

```
 March (main thread)                         C: strata_anim (no Vulkan)            C: strata_vk
 -------------------                         --------------------------            -----------
 Strata.Rig / Strata.Clip   --upload once--> rigs, part meshes, clips
 Strata.Anim calls          --extern-->      per-figure commands (play, place,
                                             override, aim, reach, attach)
 Strata.Anim.ground (Query)  --targets-->    IK targets
 Strata.Anim.update(dt)     --extern-->      sample, blend, procedural, hierarchy ->
                                             one 3x4 matrix per part per figure    --> read by the figure pass
                            <--extern--      events, sockets, hit results, stats
```

- **March** owns rig and clip building, meshing a rig's parts once at build
  time, everything that reads the voxel world (foot placement), and the
  game-facing API.
- **C** (`native/strata_anim.h` holds the logic and is tested on its own;
  `native/strata_anim.c` wraps it for March) owns the figure pool and, every
  update, poses every figure: samples clips, blends layers, applies
  procedural adjustments, walks the hierarchy and writes world matrices.
  It also answers hit tests against the posed parts.
- **The renderer** (`strata_vk.c`) reads the matrices straight from
  `strata_anim` through a C function, so poses never travel through March.
- **Determinism** (designer's call): the C update is single-threaded and
  uses the same order of operations every time. The same commands with
  the same `dt` give bit-identical poses, events and hit results on the
  same build and machine (owner's decision: that is enough). Tests and
  replays depend on it. Agreement across machines or compilers is not
  promised, so ordinary C, libm and SIMD are allowed; a networked game
  lets one machine decide hits.
- **Time** is the `dt` the game passes, never the wall clock.
- **Update rate apart from frame rate** (designer's call). A game may call
  `update` once a frame, or at a fixed tick (say 60 Hz) under a 120 Hz
  display. C keeps the last two poses of every part, and the renderer
  draws between them by a blend factor the game passes to `draw` (1.0 when
  updating once a frame).
- **The camera never changes an answer** (designer's call). Culling
  (section 8) only decides what is posed for drawing. Marks, sockets and
  hit tests give the same results whatever the camera sees, because a
  stale figure is posed on demand before any query. Springs are visual only:
  sockets and hit tests use the pose before springs, so a frozen spring
  on a distant figure cannot change a hit.

### 2. Rigs: `Strata.Rig`

A rig is a description built once and shared by every figure made from it.

- **Voxel size** in metres, per rig, independent of the world's voxel
  scale. A 1.8 m figure at 1/16 m voxels is 29 voxels tall.
- **Palette** (designer's call): 255 entries, each a colour, a roughness
  and an emission, plus index 0 for empty. Figure voxels are palette entries,
  not world blocks, because a character wants per-voxel colour rather than
  tiled textures. This matches MagicaVoxel's model, so an importer later
  maps one to one. Roughness and emission mean what they mean on blocks,
  so a visor shines and eyes glow on the figure itself.
- **Parts**: up to 64 per rig. Each part has a voxel grid of up to 128 on
  each axis, a parent (or none for the root), a **pivot** in the parent's
  space, and a rest rotation. The part rotates about its pivot. A part
  with no voxels is allowed and is a pure joint, which is useful for a
  hip or a turret ring.
- **Sockets**: named attachment points on a part, each with a position
  and rotation. Hands, muzzles, saddles and mount points are sockets.
- **Masks**: a named set of parts, given as the roots of subtrees ("this
  part and everything below it"). Layers use masks to choose which parts
  they move.
- **Legs** (for foot placement, section 5): a chain of three parts (upper,
  lower, foot) and the height of the sole below the foot's pivot. A rig
  can declare any number, so a spider gets eight and a fish gets none.
- **Finishing** a rig meshes each part once with a greedy mesher over its
  grid, the way `Strata.Mesh` meshes a chunk but with palette colours, and
  uploads the meshes and the hierarchy to C. Corner shading is computed
  as the world mesher does. The part mesher is pure March and tested
  against a brute-force face count. Meshing cost is paid once per rig, not
  per figure or per frame.

A rig is a value until finished. Afterwards it has an id and cannot change
(see Limits).

### 3. Clips: `Strata.Clip`

A clip is a description built once against one rig.

- A **duration** and whether it **loops**.
- Per part, a track of **rotation keys** and, optionally, **movement keys**
  (an offset from the pivot, for a figure bobbing or a piston). Each key has
  a time and an interpolation to the next key: step, linear, or smooth
  (eased). Rotation interpolates by slerp.
- A part with no track holds its rest pose in that clip. Under a layer's
  mask this means the clip leaves that part to the layers below.
- **Marks**: a time and a number the game chooses (designer's call: an
  integer tag, so the library names nothing). "Left foot down" or "blade
  arrives" are the game's meanings for its tags.
- **Stride**: optionally, the distance one loop carries the figure, so a
  game can set playback rate from ground speed and feet do not slide.
- **Helpers** (designer's call, because the owner asked for a basic API):
  `swing` adds a sine oscillation of a part about an axis with an
  amplitude, period and phase, which makes wings, fins, tails and a simple
  walk a few lines each. `spin` adds continuous rotation, which suits
  wheels and propellers. Both write ordinary keys, so the runtime has only
  one kind of clip.

### 4. Figures and layers: `Strata.Anim`

A figure is a rig placed in the world: a position, a rotation and its
animation state. Up to 4096 figures exist at once (designer's call).

**Layers.** Each figure has up to 8 layers, evaluated bottom to top. A layer
holds:

- a clip, its current time, its **rate** and whether it is paused;
- a **weight** from 0 to 1;
- a **mask** (all parts by default);
- a **mode**: override, where the layer replaces what is below it in
  proportion to its weight, or additive, where it adds its rotation on top
  so a breathing or recoil clip layers over anything.

**Crossfade.** `play` on a layer that already has a clip fades the old one
out and the new one in over a time the game gives. A layer fades in or out
when its weight changes, over a time the game gives. Neither snaps.

**Blends** (designer's call): a layer can hold up to four clips placed
along one number, such as idle, walk, run and sprint along speed. The two
neighbours of the current value blend, and their times are kept in step
by phase, so a walk-to-run blend keeps the feet in step.

**Marks.** Each update records every mark a playing clip crossed, in time
order, including marks crossed on a loop's wrap and marks skipped past by
a large `dt`. A mark fires once per crossing. A layer at zero weight still
fires marks (designer's call: a footstep sound should not depend on a
fade), and the event says the layer and its weight so the game can ignore
faint ones.

### 5. Reacting to the world: the procedural stage

After the layers are blended, each figure's pose passes through the
following, in this order. Every step has a weight, so the game fades them
in and out.

1. **Overrides.** The game sets any part's rotation (and movement) with a
   weight. This is the means for a game's own hit reactions, a head
   snapped by a blow, or a wheel turned by the steering.
2. **Aim.** A chain of up to four parts turns so that a chosen axis of its
   last part points at a world point, with the turn shared along the chain
   and limited per part to a maximum angle. A head watches the player, a
   spine and arms aim a weapon, a turret tracks.
3. **Reach.** Two-bone IK: a chain of three parts places its end at a
   world point, bending toward a pole point (the knee or elbow direction).
   Out of reach, the chain straightens toward the point. A hand grabs a
   ledge, a foot plants, a claw strikes a spot.
4. **Springs.** A part can be marked springy with a stiffness and damping:
   it follows its animated rotation with lag and overshoot, driven by how
   its parent actually moved. A tail sways when the figure turns, ears flop,
   a fish's rear segments follow the front, antennae wobble. Springs are
   the only state carried between updates, and they are deterministic.

**Foot placement** (`Strata.Anim.ground`, in March) is a helper built from
reach. For each leg of a figure, it casts a ray down through the voxel world
with `Strata.Query.raycast` from above the foot's animated position, sets
the leg's reach target to where the sole meets the ground, and lowers the
figure's root by the largest drop so that the longer leg reaches. On stairs
one foot lands on each step. Mid-air (no ground within reach) it fades the
legs back to the clip. The game chooses which figures get it (typically
those near the camera) and calls it each frame before `update`. The rays
use the previous update's foot positions, so `ground` does not wait on a
pose.

Because the voxel world is made of axis-aligned faces, feet are not tilted
to a surface normal (designer's call: the ground is level or a step).

### 6. Attachments and sockets

- `attach(child, parent, socket)` hangs a figure from another figure's
  socket, with an offset. The child is posed after its parent in the same
  update. Chains are allowed up to four deep (a gun on a turret on a
  vehicle carrying a rider). A figure attached to a figure that is removed is
  detached and left where it was.
- `socket(figure, socket)` answers that socket's world position and rotation
  as of the last update, for spawning a projectile at a muzzle or a sound
  at a foot.

### 7. Hit tests

Answered by C against the posed parts as of the last update:

- **Ray**: origin, direction and maximum distance. The answer is the
  nearest hit with the figure, part, the voxel within the part, the distance,
  the face normal in world space and the palette entry. Each part is first
  tested as a box in its own space, then the ray steps voxel by voxel
  through that part's grid. Figures are found through a coarse grid of
  their bounds rebuilt at each update.
- **Box**: a rotated box in the world. The answer is every figure and part
  whose box it overlaps (designer's call: part boxes, not voxels, since a
  sword sweep needs no more).
- An optional figure to ignore (the shooter itself and what it carries).

A game combines these with `Strata.Query.raycast` against the world to
learn which came first.

### 8. Cost control

- **Culling.** At each update, figures whose bounds are outside the camera's
  view (with a margin) and are not marked "always pose" advance their
  clip times and fire their marks, but skip posing for up to three of
  every four updates. A socket query or hit test on a stale figure poses
  it first, so answers are never stale.
- **Distance** (designer's call): figures past a distance the game sets
  pose every other update, and springs on them are frozen.
- **Shared rigs.** Part meshes exist once on the GPU per rig, and figures
  only add a matrix per part.

### 9. Drawing and light

**Built on the physics path** (owner's decision). The physics project
(`docs/2026-11-11-physics-design.md`, its plan's Tasks 5 and 6) builds
what any moving voxel grid needs: renderer slots that carry a position
and rotation, a true normal in the G-buffer in place of the face index,
light history matched by position and normal and reset inside each moving
object's screen bounds, and shadow, sky and reflection rays that step into
a moving object's own voxel grid through a coarse per-chunk list. A
figure's part is that same kind of thing, so this project extends that
path and builds no second one. Its drawing work starts only when physics
Tasks 5 and 6 are on `main`. Everything before that (posing, rigs, clips,
the procedural stage, hit tests) is headless and does not wait.

What figures add to that path:

- **Shared meshes, many poses.** A physics body owns its meshes. A rig's
  part meshes are uploaded once and drawn for every figure made from that
  rig, so a slot gains instances: one mesh, a list of poses. The poses come
  straight from `strata_anim` in C, as a 3x4 matrix per part per figure,
  never through March. The renderer reads them through a C function that
  `strata_anim.c` provides, declared weak in `strata_vk.c` so a game that
  draws but does not animate still links. How draws are issued (instanced
  per part mesh, or one multi-draw) is the builder's call, measured.
- **Palette colours.** Physics bodies are made of registry blocks and are
  coloured by texture and light class. A rig's voxels are palette entries.
  A figure quad carries a palette index in place of a texture, and the
  G-buffer pass takes the colour, roughness and emission from the rig's
  palette table on the GPU, so a visor shines and eyes glow. The world's
  255 light classes are not spent on palettes.
- **Two ways a figure takes part in light** (owner's decision that it is
  optional; the mechanism is the designer's call). Every figure is lit by
  the world: world sun shadows fall on it, sky light and glowing blocks
  light it, fog covers it, and its own glowing voxels glow. What differs is
  what the figure does to the light around it:
  - **Plain** (the default): the figure casts sun shadows through a
    shadow map containing only plain figures, rendered from the sun around
    the camera and combined with the traced shadow. It does not appear in
    reflections, does not darken sky light and does not light the world.
  - **Traced** (`Strata.Anim.traced(a, figure, on)`): the figure's parts
    join the physics path's ray list, so every ray the light pass casts can
    hit them. Its shadow is traced like the world's, with the same soft
    edge, it appears in water and ice, it darkens the sky light of a floor
    under it or a wall beside it, and a sky or bounce ray that reaches one
    of its glowing voxels collects that light, so a torch-bearer or a
    glowing creature lights the ground and walls around it. A traced
    figure is left out of the shadow map.
  - For traced figures the ray shapes are shared the same way meshes are:
    a rig part's voxel grid is uploaded once with its palette, and each
    traced figure adds only a placement per part. A ray that hits a
    figure voxel takes colour, roughness and emission from the palette.
  - Traced figures are capped at 64 at once (designer's call). Past the
    cap, extra figures are drawn plain and counted in `stats`, never
    silently dropped. The game picks which figures matter (the player, a
    boss, the nearest few).
- **With lighting off** (the lighting design's fallback switch), every
  figure is plain.
- **Lighting history.** The physics path resets history inside each moving
  object's screen bounds. Figures use the same reset, per figure. A crowd
  of moving figures therefore has fresh, noisier sky light on the figures
  themselves; that is accepted and checked by eye in the arena scene (see
  Risks).
- **Translucency.** Palette entries are opaque (designer's call). Glass
  visors and ghosts are out.

### 10. The API a game sees

```
-- building (once)
Strata.Rig.new(voxel_size) -> rig
Strata.Rig.colour(rig, index, r, g, b, roughness, emission) -> rig
Strata.Rig.part(rig, parent, px, py, pz, sx, sy, sz) -> (rig, part)    -- pivot, grid size
Strata.Rig.voxel / box (rig, part, ..., colour) -> rig                  -- fill the grid
Strata.Rig.socket(rig, part, x, y, z, rotation) -> (rig, socket)
Strata.Rig.mask(rig, roots) -> (rig, mask)
Strata.Rig.leg(rig, upper, lower, foot, sole) -> rig
Strata.Rig.finish(rig) -> rig id                                        -- meshes and uploads
Strata.Clip.new(rig, duration, loops) -> clip
Strata.Clip.turn / move (clip, part, time, ..., interp) -> clip         -- a key
Strata.Clip.swing / spin (clip, part, axis, ...) -> clip                -- helpers
Strata.Clip.mark(clip, time, tag) / stride(clip, metres) -> clip
Strata.Clip.finish(clip) -> clip id

-- running (every frame)
Strata.Anim.open(capacity) -> a
Strata.Anim.spawn(a, rig, x, y, z, rotation) -> (a, figure) / remove(a, figure)
Strata.Anim.place(a, figure, x, y, z, rotation) -> a
Strata.Anim.play(a, figure, layer, clip, fade, rate, weight, mask, additive) -> a
Strata.Anim.blend(a, figure, layer, clips, positions, value) -> a
Strata.Anim.set_rate / set_weight / set_time (a, figure, layer, ...) -> a
Strata.Anim.override(a, figure, part, rotation, weight) -> a
Strata.Anim.aim(a, figure, chain, axis, target, limit, weight) -> a
Strata.Anim.reach(a, figure, chain, target, pole, weight) -> a
Strata.Anim.spring(a, figure, part, stiffness, damping) -> a
Strata.Anim.traced(a, figure, on) -> a                                   -- join the traced light
Strata.Anim.attach(a, child, parent, socket, offset) / detach -> a
Strata.Anim.ground(a, figure, world, weight) -> a                         -- foot placement
Strata.Anim.update(a, camera, dt) -> a                                  -- poses everything
Strata.Anim.marks(a) -> list of (figure, layer, tag, weight)
Strata.Anim.socket(a, figure, socket) -> position and rotation
Strata.Anim.hit_ray(a, origin, direction, max, ignore) -> hit
Strata.Anim.hit_box(a, centre, half, rotation, ignore) -> list of (figure, part)
Strata.Anim.stats(a) -> figures, posed, stale, parts drawn, update time
```

Names beyond these are the builder's call. `Strata.Model` already names
block model geometry, so rig building does not reuse it. `Strata.Anim` is
a value, as `Strata.Sound` and `Strata.Fluid` are. Its externs live in
`Strata.Ffi.Anim`, in a capability of their own, and each is wrapped in an
ordinary function. A game that draws calls nothing extra: the renderer
picks up the posed figures at `draw`.

### 11. The viewer

Rigs and clips built in code in the viewer (designer's call: these are
example content, not library):

- a humanoid of about 15 parts with idle, walk, run, jump, crouch, swim
  and swing clips, a sword on its hand socket, foot placement, and its
  head aiming at the camera;
- a bird with two-segment wings flapping by `swing`, circling;
- a fish with a three-segment springy tail, in the lake;
- a cart with four wheels turned by `spin` and a steering override.

Keys spawn a crowd of wandering humanoids, switch foot placement and
springs on and off, switch the nearest figures between plain and traced
light, hand the humanoid a glowing torch (a traced attachment), and fire a hit-test ray from the crosshair that
outlines the part it hits. With `STRATA_ANIM_METER=1`, a line once a second
prints figures, posed, stale, parts drawn and update time.

Scripted scenes for goldens and timing: a humanoid climbing stairs, a sword
swing mid-arc, the creatures in the lake scene, a figure walking across the
lit forest floor (ghosting check), and the **arena**: 512 humanoids
running, swinging and aiming on flat ground, with foot placement for the
nearest 128.

## Budget and targets

Designer's targets, stated so a miss is reported. M3 Max, release build,
2560 x 1600, lighting on.

| | target |
|---|---|
| `Strata.Anim.update`, arena (512 figures x 15 parts, 3 layers, aim, 2 legs each), C, main thread | 1.0 ms or less |
| `Strata.Anim.ground` for 128 figures, 2 legs each, March | 0.5 ms or less |
| figure pass, arena, GPU | 1.5 ms or less |
| figure shadow map, arena, GPU | 1.0 ms or less |
| traced figures, 64 humanoids in view, added to the light pass, GPU | 1.5 ms or less |
| traced figures, none opted in, added to the light pass, GPU | under 0.1 ms |
| hit ray, arena, average | 10 µs or less |
| frame rate, arena scene, everything on | 120 fps (8.3 ms) |
| figures that exist at once / layers per figure / parts per rig | 4096 / 8 / 64 |
| C memory for 4096 figures, two poses kept, excluding rigs | 48 MB or less |
| GPU memory for figure quads | 64 MB or less |
| added to the worst frame of the existing soak with no figures, CPU and GPU | under 0.1 ms |

If the arena misses 120 fps, what gives way is, in order: the stale-pose
rate for distant figures, then foot placement for fewer figures, then the
target, which is reported as missed with its measured cause.

## Testing

- **C tests** (`native/tests/`, run by the gate like the mixer's): key
  sampling and each interpolation; slerp at the antipode; layer blending
  and masks; additive layers; crossfades continuous (no jump in any part's
  matrix between consecutive updates); blends keep phase; marks fire once
  per crossing, across a loop wrap, under a `dt` longer than the clip, and
  at zero weight; aim respects limits; reach hits reachable targets
  exactly and straightens toward unreachable ones; springs settle and are
  deterministic; attachment order and removal; hit ray against a brute-
  force test of every voxel of every part; determinism of a long scripted
  run (hash of all matrices).
- **March tests**: rig building and its limits (refused past 64 parts or
  128 voxels an axis, and reported); the part mesher against a brute-force
  face count on random grids; clip building (keys out of order, a key past
  the duration, a clip for the wrong rig are refused); `swing` and `spin`
  produce the keys they claim; foot placement on flat ground, a step, a
  slope of steps and over a drop.
- **Golden images**: a posed humanoid in each of the main clips at a fixed
  time, the stairs scene, the cast shadow (plain and traced, side by
  side), a traced figure reflected in the valley lake, a traced glowing
  figure lighting a cave wall at dusk, the creatures, and the moving
  figure over the lit floor, where the check is that the image settles with
  no trail.
- **Timing**: the arena scene measured against every target above, in
  `RESULTS.md`.
- **Seeing a step**: each task ends with the viewer command or scene that
  shows it.

## Order of work

One plan. Each step leaves the gate green and the viewer working.

1. The pose kernel, headless: `strata_anim.h` with figures, clips, layers,
   blends, crossfades and marks; C tests.
2. March: `Strata.Rig` with the part mesher, `Strata.Clip` with its
   helpers, `Strata.Anim` bindings; determinism test. Seen as a printed
   pose table.
3. The procedural stage: overrides, aim, reach, springs; `ground`.
4. Attachments, sockets, hit ray, hit box.
5. Cost, headless: culling, stale poses, two poses kept, the arena
   measured without drawing.
6. Drawing, once physics Tasks 5 and 6 are on `main`: shared part meshes
   with instanced poses, palette colours in the G-buffer, per-figure history
   reset; the viewer shows the humanoid. Goldens begin.
7. Shadows: the figure shadow map for plain figures.
8. Traced figures: shared palette shapes, per-figure placements, the cap;
   the reflection, shadow and glowing-figure scenes.
9. The viewer's creatures and scenes, the arena drawn, measurements
   against the targets, README and RESULTS.

## Left to the builder

- The figure quad packing and the GPU palette table layout.
- How figure draws are issued, chosen by measurement.
- The shadow map's resolution, extent and filtering.
- How the physics path's ray list is extended to shared shapes with many
  placements, and how palette shapes sit beside light-class shapes.
- The aim solver's way of sharing a turn along a chain, and the spring
  integrator.
- The coarse grid used by hit tests and its cell size.
- C data layout, command format, and how March hands lists (blend clips,
  masks, chains) across as native arrays.
- Names of functions and modules beyond those listed above, file layout
  and test organisation.

## Limits, stated now

- Parts are rigid and voxels of a finished rig never change. Carving
  damage into a figure later means giving that figure its own copy of a
  part's grid and remeshing it, as an edited chunk does. The hit test
  already answers which voxel was hit, so a game can record it.
- Only 64 figures are traced at once. Plain figures do not appear in
  reflections, darken sky light, or light the world.
- A traced figure's reflection is coloured from its palette, lit by the sun,
  when it is off screen, as world hits are: a coarse reflection, not a
  mirror image of its shading.
- Feet are not tilted to the ground.
- No physics: a figure never moves unless the game moves it.
- Measured on one machine only.

## Risks

- **Draw submission for many parts on MoltenVK.** 512 figures of 15 parts
  is 7 680 part instances. If neither instancing nor multi-draw holds the
  figure pass target, parts of one rig are merged into fewer draws by
  writing a part index into each quad, so one instanced draw per rig
  covers all its figures.
- **Lighting history on moving figures.** History reset per figure leaves
  moving figures' own sky light noisier than the world's. If it reads as
  crawling in the arena, figures take a cheaper, steadier sky term (sky
  light sampled at the figure's centre once per figure).
- **Physics path not landed, or landed different.** If physics Tasks 5
  and 6 change shape before they reach `main`, Tasks 6 to 8 of this plan
  follow what landed, not what the physics plan said.
- **Traced figures' cost.** Every light ray now steps a second grid. If 64
  traced figures miss 1.5 ms, the cap falls (32, then 16), then reflection
  rays skip figures, and the miss is reported.
- **Ground rays in March.** 256 rays a frame through `Strata.Query` may
  cost more than 0.5 ms. Fallbacks: one ray per leg every other frame,
  then fewer figures.
- **Quad count of detailed rigs.** A 1/32 m rig has many more faces than a
  1/16 m one. The GPU memory target assumes the viewer's rigs, and a game
  with finer rigs pays for them.

# How mechs are built: strata_kit and Steel Rain's mechs

Status: design, for approval. Nothing is built.
Date: 2026-11-20
Follows: `2026-11-20-plugins-tactics-design.md` (the plugin shape).
Mockups shown to the owner: `docs/steel-rain-mockups/` (HTML fragments
from the brainstorming companion; open in a browser).

## Intent

Steel Rain ("FFT meets XCOM meets MechWarrior") lets the player build a mech
from parts found and bought during a run. The owner wants the mech on
screen to change as items are swapped, with the chassis core staying the
same: guns, mount points, arms, movement pieces (legs, treads, hover), back
mounted items, hand-held shields, and so on. Damage shows on the mech.

This design covers how a mech is put together, how it looks, how it
breaks, and the screen where the player builds it. The generic half goes
into a shareable plugin, `strata_kit`; the mech rules and the hangar
belong to Steel Rain.

## Decisions the owner made

- **The core stays, everything else swaps**, and the whole model changes
  as items are swapped.
- **Part kinds**: guns, mount points, arms, movement pieces, back-mounted
  items, hand-held shields (and more of the same kind later).
- **Parts are universal by mount size**: every mount is Light, Medium or
  Heavy, and any part of the right size fits any chassis.
- **Chassis have weight and power limits.**
- **Damage shows both ways**: hits carve holes in the armour, and a
  destroyed part breaks off and tumbles away.
- **Hangar layout B, "slots first"** (mockup `hangar-layouts.html`,
  second card): a tree of every mount on the left, indented under the part
  that brings it; the mech in the middle; weight, power, speed and the
  parts that fit the selected mount on the right.
- **Movement pieces at launch: two legs, treads, and jet/hover.**
- **Approach: start with authored parts assembled at runtime (A), aiming
  for generated parts (C).**

## Designer's calls in this document (overturn any)

- A part fits a mount **of its size or larger** (a Light gun fits a Heavy
  shoulder). Weight and power stop "biggest of everything".
- Parts **bring mounts of their own** (an arm brings a hand and a forearm
  hardpoint), so a build is a tree.
- A destroyed part **takes its children with it** (an arm falls with the
  gun in its hand).
- Mechs are drawn at **0.1 m per voxel**; a Medium mech stands about 5 m
  and fits one tactical tile's footprint (Steel Rain sets its tile size
  to suit). Within Strata's rig limits (64 parts, 128 voxels a side).
- Parts reserve **trim palette slots** so one part can wear any mech's
  colours (team colours, paint).
- Placeholder parts for tests and the example are **built in code**;
  real art comes later as MagicaVoxel files under the convention below.
- **A part may hold several jointed pieces** (legs, arms, treads with
  wheels): one file, one part, several rig parts (see Authoring).
- **Mounts have a slot type and a size**; parts list the slot types they
  go in and their size (see Fitting).
- **One shared palette** for every part, supplied by the game, with trim
  and interior colours at fixed indices. A part file's own palette is
  checked against it, not merged.
- **Left and right**: a part is drawn for one side and **mirrored** when
  mounted on a mount marked as the other side. **Mounts carry an up
  direction** as well as a facing, so roll is fixed.
- **Walking is procedural**: a gait from the legs' lengths, with
  `Strata.Anim`'s foot planting, so any leg part walks without authored
  clips. This leans toward C, and authored clips cannot follow parts whose
  rig numbering changes with every build.
- **Up to 12 mechs on the field at once**, which sizes the rig budget.
- **Strata core gains replacing and freeing a rig, and re-rigging a
  figure** without losing its animation state. Today `Strata.Anim` can only
  add rigs (256 per world) and a figure cannot change rig; swaps and carves
  would exhaust it in minutes. Added to the public API under the plugin
  rule, in its own tasks.
- **Steel Rain's world is 0.4 m per voxel** (mech voxels 0.1 m, a quarter,
  which `Strata.Physics` bodies require). A tactical tile of 8 world voxels
  is 3.2 m, enough for one mech's footprint.
- **Loose parts look approximately right**: their palette colours map to a
  few debris blocks (dark metal, light metal, trim, scorch) in the game's
  registry; bodies are made of blocks, not palette colours.
- **Parts are authored solid**; a carved hole shows the interior colour
  (a fixed palette index, scorched dark) on its new faces.

## Goals, non-goals, anti-goals

Goals:
- `strata_kit`: modular voxel figures from parts with typed mount points,
  assembled into a `Strata.Rig`, with carving damage and parts breaking
  off as physics bodies. Generic: it never says "mech".
- A MagicaVoxel authoring convention for parts, and a loader.
- The part format does not care where a part came from (file or code), so
  a generator (C) can produce parts later without changing the kit.
- An example: a turntable viewer that assembles a placeholder mech, swaps
  parts live, carves it and knocks a part off.
- Steel Rain's mech model, written down here so the game can build it:
  chassis, sizes, weight, power, speed, movement types, the hangar.

Non-goals (this project):
- The part generator (C). Designed for, not built.
- Steel Rain's hangar screen, stats and rules in code: they are built
  with the game. This document fixes what they are.
- Real art. Weapons' firing effects. Hover's rules on the grid (crossing
  water and gaps), which are Steel Rain gameplay.
- Four- or six-legged walkers (later unlocks; the hip mount is standard).

Anti-goals:
- A part that only works on one chassis.
- Swapping a part needing anything but a rebuild from the build tree.
- Mech concepts (weight, power, chassis) in `strata_kit` or Strata core.

## Approaches considered

- **A. Separate part files, assembled at runtime.** Chosen to start. Fits
  universal parts directly; swapping is a rebuild of a value.
- **B. One file per chassis with every variant shown or hidden**
  (`Strata.Character`'s extras). Rejected: every part drawn into every
  chassis contradicts universal parts.
- **C. Parts generated from parameters (kitbashing).** The aim. Endless
  variety with less art, at the cost of a generator and a risk of generic
  looks. Reached by keeping A's part format source-agnostic, so generated
  parts drop in.

## Anatomy of a mech (Steel Rain)

Mockup: `mech-anatomy.html`.

```
                [back: missile pod]
    [shoulder L]      CORE      [shoulder R: autocannon]
   [arm L]                         [arm R]
   [hand L: shield]                [hand R: rifle]
                  [hip]
          [legs | treads | hover]
```

- **Chassis** = a core part (cockpit and torso), its mounts with sizes, a
  weight limit and a power budget. Unlocked between runs. Example: a light
  core with a Medium back, two Light shoulders, two Medium arm mounts, a
  Medium hip.
- **Parts** have a size, a weight and a power draw, and may bring mounts:
  - **Arms** bring a hand mount and may bring a forearm hardpoint. A
    **gun-arm** is an arm whose forearm is the weapon (no hand).
  - **Hand-held items**: guns, shields.
  - **Shoulder and forearm items**: guns, launchers.
  - **Back items**: missile pods, jump jets, and the like.
  - **Movement pieces** on the hip: two legs, treads, hover. They may bring
    hip-side mounts.
- **Speed** for the FFT clock comes from the chassis and the movement
  piece, less a penalty for weight. The formula is Steel Rain's, set with
  its balance; the hangar shows the result.

## strata_kit

Lives in `plugins/strata_kit/`, a forge package depending on Strata by
path, under the same rules as `strata_tactics`: public Strata only, nothing
privileged. Pure functions mostly; where it has per-frame state it follows
the plugin shape.

### Parts

A part is: a voxel grid with its palette, an **attach point** (position and
facing, in the part's voxels) where it joins its parent, a list of **mounts**
it offers (name, kind, position, facing), optional **effect markers**
(name, position, facing: muzzles, thrusters, sparks), **numeric tags** the
game defines (`weight`, `power`, `armour`, …; the kit only stores and sums
them), a **kind** string (the game's: `M`, `arm`, whatever it fits by), and
which palette slots are **trim**.

Made two ways, giving the same value:
- `from_vox(file)`: the authoring convention below.
- `from_grid(...)`: from a grid and the same fields in code. Placeholders,
  tests, and later the generator, use this.

### Authoring convention (MagicaVoxel)

One part per `.vox` file, drawn with the game's shared palette. The part's
root piece is the model named `part`. Further pieces are models named
`piece:<name>:<parent>` (parent is `part` or an earlier piece), each with
a marker `joint:<name>` whose centre is the point it turns about in its
parent. Leg pieces are named so the assembler can register the legs:
`piece:thigh_l`, `piece:shin_l`, `piece:foot_l` (and `_r`); tread wheels
are `piece:wheel<n>`; an arm's chain is `piece:upper`, `piece:fore`.
Marker models are tiny models (any voxels) whose centre is the point:
- `attach` (exactly one, in `part`): where the part joins its parent.
- `mount:<slot>:<size>:<name>`: a mount the part offers, e.g.
  `mount:hand:L:hand` or `mount:shoulder:M:shoulder_l`. A mount whose name
  ends `_l` is on the left: a part drawn for the right is mirrored there.
- `fx:<name>`: an effect marker, e.g. `fx:muzzle`, `fx:thrust`.

A marker's facing and up are written in its name as a suffix, `@+y^+z`
(facing, then up; the file's axes, converted as `Strata.Character`
converts them); default `@+y^+z` (forward, up). They go in the name
because `Strata.Vox` reads a model's position but not its full rotation.
Tags go in a sidecar text file beside the `.vox` (`<name>.part`: `slots =
hand shoulder forearm`, `size = M`, `side = r`, `weight = 9`, …). Mount
layouts are standard per slot and size, so all Medium arms put their
`attach` the same way relative to the shoulder they plug into.

### Builds and assembly

- A **build** is a tree: a root part, and for each of its mounts, either
  nothing or a child build.
- **Fitting**: a mount has a slot type and a size; a part has the slot
  types it goes in and a size. `fits` is the game's, given both: Steel
  Rain's says the mount's slot is among the part's slots and the part's
  size is the mount's or smaller. The kit treats slots and sizes as plain
  strings.
- `assemble(parts, build, fits, voxel_size, trim colours) -> (rig, map,
  problems)`: walks the tree; each part becomes a rig part whose anchor is
  its `attach` point, pivoted at its parent mount's position and turned to
  that mount's facing. `map` tells the game which rig part is which node
  (to aim an arm, find a muzzle). `problems` lists what did not go in: a
  part that does not fit its mount, a missing mount name, past 64 parts.
- `sum(parts, build, tag) -> Float`: the total of a numeric tag over the
  build (weight, power).
- `markers(parts, build, map, name) -> list`: where every effect marker of
  a name is, by rig part, for particles and muzzle flashes.
- Swapping a part = changing the build and assembling again. Rigs are
  values until `Strata.Anim.finish_rig` uploads them; the plan measures
  the cost of a rebuild and upload (target: well inside a frame for a
  hangar hover preview).

### Movement pieces and posing

The kit knows movement only as part shapes; it supplies helpers the game
uses:
- **Legs**: a part declares its leg chains by piece names; the assembler
  registers them as `Strata.Rig` legs, so `Strata.Anim` plants the feet as
  it does for humanoids. **A procedural gait** (stride and lift from the
  legs' lengths, body bob, idle and turn-in-place) walks any leg part
  without authored clips.
- **Treads**: no legs; wheel sub-parts named `wheel*` are listed so the game
  can spin them, plus a small body sway.
- **Hover**: no legs; a bob, and `fx:thrust` markers for thruster
  particles.
- **Aiming**: a two-bone aim for an arm chain toward a point, so held guns
  and gun-arms point at their target. Builder's call how it plugs into
  `Strata.Anim` poses.

### Damage

- **Carving**: `carve(rig part, point, radius)` removes voxels from that
  part's grid around a hit (in the part's own frame) and gives the game the
  changed rig to re-upload, plus the count removed (for armour loss, if the
  game wants it). Carving never removes a part's `attach` region, so a part
  stays attached until it is destroyed.
- **Breaking off**: `detach(build, node) -> (build, loose)`: removes the
  node and its children from the build; `loose` is the removed parts'
  grids in world pose, for `Strata.Physics` as bodies, optionally split by
  `Strata.Fracture` round the final hit. The game decides when a part is
  destroyed.

### Example

`plugins/strata_kit/example/`: a lit turntable with a placeholder mech
built in code from a light core, two arms, a rifle, a shield, a back pod
and each of the three movement pieces. Keys cycle parts on each mount (the
model changes), click carves at the cursor, a key knocks off the right arm
and it falls and breaks. Scripted mode writes a screenshot for
`docs/kit-example.png`.

## Steel Rain's side (built with the game)

- **Chassis and parts data**: sizes, weight, power draw, armour, the
  `fits` rule (size or larger), weight and power limits, speed formula.
- **Hangar, layout B**: mount tree on the left, indented by the part that
  brings each mount, each row showing size, mount name and what is in it;
  the mech in the middle, turnable by drag; on the right, weight and power
  bars against the chassis's limits, speed, and the list of parts that fit
  the selected mount (too-big parts greyed out). Hovering a part in the
  list previews it on the mech before committing. Over-limit builds are
  shown in red and cannot be taken into a mission.
- **Damage rules**: which hits carve where, when a part counts as
  destroyed, what losing a part does (its weapons and mounts go with it).

## Left to the builder

- Exact names and types in `strata_kit`.
- How aiming plugs into `Strata.Anim`.
- The tread sway and hover bob.
- How trim slots map to colours.
- Whether carving re-meshes the whole part or a region, as long as a hit
  stays inside a frame.

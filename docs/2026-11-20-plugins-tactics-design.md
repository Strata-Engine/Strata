# Plugins, and the first one: strata_tactics

Status: design, for approval. Nothing is built.
Date: 2026-11-20

## Intent

The owner is starting a small game on Strata, working title **Steel Rain**:
a mech roguelike, "FFT meets XCOM meets MechWarrior". Runs across a
branching overworld, turn-based fights on themed procedural maps with
destructible terrain, parts and loadouts under weight and power limits,
chassis unlocked between runs. Steel Rain is designed later, separately.

Before the game, the owner wants a way to **package basic game elements and
share them**: a tactical grid with turns and a camera is one such element;
city-builder tools (drawing roads, zoning, placing buildings) are another.
Today Strata is a library and each game hand-wires everything in its own
loop (the viewer is one 2,400-line program); forge can share packages, but
nothing gives a package a standard way to bring state, input, per-frame
behaviour, 3D drawing and UI into a game.

This project adds:

1. **A plugin shape**: a convention, with shared types in Strata core, for a
   forge package that brings a game element.
2. **The engine pieces plugins need** that a package cannot provide itself:
   a 3D overlay pass, a cutaway, combining input bindings from several
   sources, and turning a screen point into a world ray.
3. **The first plugin, `strata_tactics`**: a tactical grid, FFT turn order,
   and an XCOM-style camera. It proves the plugin shape.

Success: a game adds `strata_tactics` to its `forge.toml`, wires it in about
a dozen lines, and has a working tactical layer over any Strata world; and
`strata_tactics` got there using nothing an outside package could not use.

## Decisions the owner made

- **Approach A, with C later.** A plugin is a forge package with a standard
  shape; the game keeps its own loop, holds each plugin's state and calls
  it. A small optional runner that drives a list of plugins (C) comes later
  and is not in this project. Strata owning the loop (B, Bevy-style) was
  rejected.
- **`strata_tactics` ships with Strata but is not privileged.** It lives in
  this repository and uses only the public plugin API and public Strata
  modules. Anything it lacks is added to the public API for everyone.
- **Grid, turns and camera are one element**, packaged together.
- **Turns work exactly like Final Fantasy Tactics**: charge time (CT)
  raised by speed each clock tick, active turn (AT) at 100, charged actions
  with their own speed (details below). **The reference is the 2025 remake,
  Final Fantasy Tactics: The Ivalice Chronicles**, in its Enhanced mode
  (the mode is the designer's call; the remake also ships a Classic mode
  following the older War of the Lions rules). Where sources disagree,
  the remake's behaviour wins, and the clock's tests are worked from it.
- **The camera is versatile, like the recent XCOM games**, not FFT's fixed
  isometric view.
- Engine rule carried over: Strata core names no game concept.

## Goals, non-goals, anti-goals

Goals:
- A documented plugin shape any package can follow, with the shared types in
  `Strata.Plugin`.
- The four engine pieces above, generic and tested.
- `strata_tactics`: grid, turns, camera, overlay helpers, each tested
  headless; and a small example game that shows them working in a window.
- A short guide, `docs/plugins.md`: how to write and how to use a plugin.

Non-goals (this project):
- The runner (approach C).
- Steel Rain: units' stats, abilities, damage, AI, missions, overworld,
  loadouts. The plugin knows a unit only as an id, a tile, a speed and the
  few movement numbers below.
- Forge building a dependency's C sources (GAPS S11). Needed before plugins
  with native code can be shared; `strata_tactics` needs none because the
  native pieces go into Strata core. A follow-up in the March repository.
- Several standing levels per tile (see Grid).
- Units larger than one tile.
- A plugin registry, versioning scheme, or hot loading of plugins.

Anti-goals (a failure even if it works):
- `strata_tactics` reaching into Strata internals or getting hooks nobody
  else can use.
- Game concepts (units, teams, turns, mechs) leaking into Strata core.
- A plugin changing the world behind the game's back.

## Approaches considered

- **A. Standard shape, the game keeps its loop.** Chosen. Fits Strata's
  "games own the loop" design and the toolkit-not-framework stance; fits
  March's typed, value-passing style; plugins stay testable as pure
  functions. Cost: about a dozen lines of wiring per plugin, and plugins
  talk to each other only through the game.
- **B. Strata owns the loop; plugins register systems.** Rejected for now:
  turns Strata into a framework, forces a port of the viewer, and needs
  either a type-erased resource store or actors to hold plugins' differing
  state (actors cannot be called under `forge test`, GAPS S12).
- **C. A, plus an optional runner.** Planned after this project; A must not
  rule it out (see The plugin shape).

## The plugin shape

A plugin is a forge package exposing one main module with four parts. The
names below are the convention; the types live in `Strata.Plugin`.

- **setup(config, registry-and-settings…) → (state, declarations)**. Runs
  once at start-up. Declares what the plugin adds: input bindings it
  proposes (action names prefixed with the plugin's name, e.g.
  `tactics.rotate_left`), blocks or assets if any. Returns its initial
  state.
- **update(state, frame) → (state, requests)**. Once per frame. `frame`
  gives read access to what a plugin may look at: the world (for queries),
  the frame's `Strata.Input.Input`, the pointer's position and its world
  ray, the time step, the screen size. `requests` is a list of things the
  plugin asks the game to do: voxel edits, sounds, and the
  plugin's own events for the game (typed by the plugin). **The plugin never
  changes the world itself**; the game applies the requests it accepts. The
  game may also call the plugin's own functions directly (move a unit, end
  a turn); `update` is for what happens every frame.
- **overlay(state) → Strata.Plugin.Overlay**. What to draw in 3D this frame
  (quads, lines, the cutaway). The game merges the overlays of its plugins
  and hands them to the renderer.
- **gui(state) → Strata.Gui nodes**. UI for the game to place in its screen.

**The camera belongs to the game** (designer's call). A plugin that has a
camera exposes its view-projection matrix; each frame the game picks which
matrix drives the renderer and passes that same matrix back in the next
`frame`, so every plugin's pointer ray comes from the camera actually on
screen. No plugin sets the renderer's camera itself.

Requests that are Strata's (edit, sound) are a type in
`Strata.Plugin`; a plugin's own events are its own type, carried alongside.

For C later: the runner needs to hold plugins of different state types in
one list. **Builder's call** how (a record of closures over the plugin's
state is the likely form); the constraint on this project is only that the
four parts are plain functions of the shape above, so a runner can wrap
them without changing any plugin.

**Input bindings across plugins.** Each plugin proposes bindings; the game
combines them with its own. `Strata.Input` gains a way to merge binding
lists and report clashes (one source bound to actions of two owners), so
the game can see and rebind them. The game always has the last word.

## Engine pieces in Strata core

All generic; none mentions tiles, units or turns.

1. **Overlay pass.** The renderer draws a per-frame list of translucent
   world-space quads and lines with colours, after the world. Each item says
   whether it is hidden by geometry or drawn through it (a path behind a
   wall should still show, faintly). Sized for a few thousand items a frame.
   Quads may sit just above a surface; the pass must avoid z-fighting with
   the ground.
2. **Cutaway.** The renderer hides world geometry by rule for this frame:
   everything above a height inside a region, and everything inside a
   shape between the camera and a point (a cylinder or capsule, so walls in
   front of a unit vanish). Figures, bodies and shapes obey it too. Hidden
   geometry should still cast shadows and block light so the scene does
   not change look when the cut moves (**builder's call** if the renderer
   makes that costly; say so in the plan). A soft edge or dither at the cut
   is a nice-to-have.
3. **Input merging and clash reporting**, as above, in `Strata.Input`.
4. **Screen point to world ray**: from a view-projection matrix and a
   pointer position, the ray in metres. In `Strata.Math`.

The camera itself stays in the plugin: the renderer already takes a
view-projection matrix (`Strata.Ffi.Vk.set_camera`) and `Strata.Math.Mat4`
has `look_at` and `perspective`.

## strata_tactics

Lives in `plugins/strata_tactics/`, its own forge package depending on
`strata = { path = "../.." }`, with its own tests. Wired by the game like
any plugin.

### Grid

- **A tile is a square of N × N voxel columns** over a rectangular region of
  the world the game names. N is a setting (Steel Rain's mechs set it);
  **builder's call** on the default.
- **One standing height per tile** (designer's call, offered to the owner
  and not overturned): the floor a unit on that tile stands on. Height is
  in voxels; the game also sets a "height step" in voxels, the unit
  FFT-style climb and jump numbers are counted in. **Where a column has
  more than one place a unit fits (a roof over a ground floor, a bridge
  over a street), the tile's height is the highest of them** (designer's
  call): rooftops and bridges are standable, interiors under them are not,
  in keeping with mechs that rarely fit indoors. The floor-level control
  (Camera) serves to see under roofs and bridges, not to stand there. A
  tile with no place a unit fits is impassable.
- **Clearance per tile**: the free height above the floor, stored with it.
  Each unit brings its own height (designer's call: chassis may differ),
  and a tile is open to a unit only if it fits. Reach, path and picking
  take the unit's height.
- Where a tile's columns disagree (a slope, a rubble pile),
  **builder's call** on the rule, which must be stated in the plan and be
  the same everywhere.
- Per tile also: whether it is standable, fluid depth, and a terrain kind
  the game assigns from the blocks under it (the game passes a function from
  block id to kind at setup; the plugin attaches no meaning to kinds beyond
  passing them to the game's cost function).
- **Rebuilt in part after destruction.** The game passes what an edit
  changed (the voxel box from the edit outcome) and only the tiles it
  touches are re-read. Rebuilding the whole grid is also available.
- Built on `Strata.World` reads and `Strata.Nav` standing tests; uses no
  internals.
- **Budget** (designer's call): maps up to 64 × 64 tiles. Reach for one
  unit under 2 ms, a full forecast under 1 ms, a partial rebuild after a
  blast within one frame, a full build under a second. If plain March data
  misses these, the fix goes into March with a reproducer, per the
  project's standing rule.

Queries (all pure, all tested):
- **Reach**: the tiles a unit can get to this turn, given its move (tiles),
  jump (height steps up), drop (height steps down), a cost per terrain kind
  from the game, and which tiles are blocked or may be passed through
  (a function the game supplies, so teams and allies stay the game's).
  FFT-style: four-way moves, each costing the destination's terrain cost.
- **Path** to a reachable tile, for drawing and for moving a figure along.
- **Line of sight** between two tiles from given eye heights, by voxel ray
  (`Strata.Query.raycast`), so a wall shot away opens a line. Returns
  clear, blocked, or partly blocked.
- **Cover** for a tile against a direction (XCOM-style): none, half, or full,
  from the height of solid voxels along that tile edge relative to a unit's
  height. Thresholds are settings. The game decides what cover does.
- **Areas** for targeting: a radius, a line, a cone, from a tile, optionally
  limited by height difference (FFT's vertical tolerance).
- **Picking**: the tile under the pointer, from the world ray (engine piece
  4) against the world and then the grid.
- **Tile to world and back**: a tile's centre and floor point in metres.

### Turns (FFT)

The plugin keeps a timeline of units (id, speed, CT) and charged actions.
The game adds and removes units and changes their speed.

- **The clock.** One clock tick runs these steps, in FFT's order:
  1. **Status timers** count down by one; those that reach zero end and are
     reported to the game. The plugin stores timers the game starts (a
     name, a unit, a length in ticks) and knows only the speed modifiers
     among them (below); every other status means nothing to it.
  2. **Charged actions** whose counter is due resolve: reported to the game
     in a fixed order, to apply. Each was queued with its own speed; its
     counter falls by that speed each tick, FFT-style.
  3. **CT rises** by each unit's effective speed (base speed with any speed
     modifiers such as Haste and Slow; a modifier that starts this tick
     applies this tick).
  4. **Active turn**: the first unit with CT ≥ 100 gets its turn. Ties break
     by the order units were added (**proposed**; FFT breaks ties by unit
     order). Ticking stops while a unit has its turn.
- **A turn's cost.** The game tells the plugin whether the unit moved and
  whether it acted; the unit's CT falls by the base cost plus the move cost
  and the act cost: by default 60, +20, +20, so 100 for both. FFT then caps
  the unit's remaining CT; whether and where is version-dependent in the
  sources and **must be checked against The Ivalice Chronicles (Enhanced)
  before it is built**, as must the tie-break and the order charged actions
  resolve in. Every number here is a setting with FFT's
  value as the default.
- **Charging units** are flagged, so the game can apply its own rules
  (FFT makes them easier to hit); a charged action can be cancelled.
- **Forecast**: the order of the next K turns and charged-action
  resolutions, computed without changing anything. Recomputed on demand
  (after a speed change, a new charged action, a "what if I wait" query for
  the turn-order UI).
- **Deterministic**: same inputs, same sequence. Tested against sequences
  worked out by hand from FFT's rules.

### Camera (XCOM-style)

A camera state the plugin steps each frame from its input actions and the
game's requests, producing the view-projection matrix for the renderer.

- **Orbit** around a focus point on the ground: rotate in steps (default
  45°, a setting) with a smooth turn, or freely by dragging.
- **Pan** by keys, stick, and screen-edge scroll; the focus stays over the
  ground (follows the grid's floor height under it).
- **Zoom** smoothly between limits; **tilt** follows zoom on a curve
  (closer is lower), as XCOM does.
- **Follow** the active unit when its turn starts, and track a moving unit.
- **Floor level**: the player steps the cutaway height up and down (XCOM's
  level selection). The plugin always also cuts what stands between the
  camera and the focused unit or the tile under the pointer.
- **Shots**: the game asks for a short camera move and gets told when it
  ends: go and look at a tile or event (an enemy revealed, an explosion),
  then return; an action view framing two points (shooter and target) from
  over the shoulder or from the side. Shots avoid putting the camera inside
  geometry (by ray) and fall back to the orbit view if they cannot.
- Every speed, limit and step is a setting; free rotation and no tilt make
  it suitable for a city builder too.

### Overlay helpers

Functions from grid queries to `Strata.Plugin.Overlay` items: a reach region
(filled tiles with an outline), a path with its end marked, a target area,
a line of sight, the tile under the pointer, cover markers on tile edges.
Colours are settings. The game chooses which to show.

### What the plugin does not do

It does not move figures, play animations, decide damage, choose AI
moves, or hold unit stats beyond speed, move, jump and drop. It answers
questions and keeps the turn clock; the game acts.

## Testing and the example

- Strata core pieces: unit tests (input merging, screen ray); the overlay
  and cutaway checked by picture in the viewer, as other render features
  are (`docs/*.png`), using the devtools MCP to screenshot.
- `strata_tactics`: headless tests that build small worlds from blocks and
  check grid, reach, line of sight, cover, picking and partial rebuilds;
  turn-clock tests against hand-worked FFT sequences; camera math tests
  (orbit, steps, zoom-tilt curve, shot framing).
- **Example game** `plugins/strata_tactics/example/` (an app package):
  a small map on the island, three or four placeholder figures on two
  sides taking FFT turns, the reach and path overlays, picking, the camera
  in full, and a blast that knocks down a wall and updates the grid and
  line of sight. It is also the demonstration that wiring takes about a
  dozen lines.

## Left to the builder

- The default tile size N and height step.
- The tile-height rule where columns disagree.
- Exact names and field layout of `Strata.Plugin` types.
- How the later runner will hold plugins (only the constraint above binds).
- Whether hidden geometry keeps casting shadows, if costly.
- Overlay capacity and how the pass is drawn.
- Order of work, as long as engine pieces land before the plugin uses them.

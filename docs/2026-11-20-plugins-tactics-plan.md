# Plugins and strata_tactics Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** a plugin shape for Strata, the engine pieces plugins need (3D
overlay, cutaway, input merging, screen rays), and the first plugin,
`strata_tactics`: a tactical grid, FFT turn clock and XCOM-style camera.

**Architecture:** Strata core gains generic modules (`Strata.Overlay`,
`Strata.Plugin`, `Strata.Math.Ray`, input merging) and two renderer
features. `strata_tactics` is a separate forge package under
`plugins/strata_tactics/` that depends on Strata by path and uses only its
public modules. A small example app under it wires everything in a window.

**Tech Stack:** March (forge, `forge test`), Vulkan renderer in
`native/strata_vk.c` with GLSL in `native/shaders/` (rebuilt into
`native/strata_shaders.h` by `native/build_shaders.sh`).

**Spec:** `docs/2026-11-20-plugins-tactics-design.md`. Rulings made on the
way go in `docs/tactics-ledger.md` (create it in Task 1).

## Global Constraints

- `. scripts/env.sh` before any forge command (March main; GAPS S8:
  windowed runs need `MARCH_PIN_MAIN=1`).
- Commit after each task; merge the work branch into main after each
  reviewed task (owner's standing rule).
- Strata core names no game concept: no tile, unit, team or turn in
  `lib/strata/`.
- `strata_tactics` uses only public Strata modules. If it needs something
  missing, add it to Strata core in its own commit, with a test, and note
  it in the ledger.
- A plugin never changes the world itself; it returns requests.
- The game owns the camera: plugins expose a view-projection matrix and
  never call `Strata.Ffi.Vk.set_camera`.
- Every FFT number is a setting whose default is FFT's (The Ivalice
  Chronicles, Enhanced mode).
- Budget: maps up to 64 × 64 tiles; reach < 2 ms per unit; full forecast
  < 1 ms; partial rebuild after a blast within one frame (16 ms); full
  build < 1 s. Slow March code is fixed in March with a reproducer; C only
  as a marked stopgap.
- March pitfalls already met (GAPS.md): no `opaque`, `by`, `on`, `loop` as
  names; constructors do not resolve through aliases (give sum types an
  `_index` reader); aliases unique across the test suite; bind extern
  results, never `let _ =`.
- No stored golden image changes unless a task says so; new scenes add
  new goldens.
- `scripts/verify.sh` stays green; it gains `plugins/strata_tactics` and
  `plugins/strata_tactics/example` in its consumer loop (Task 7).

## Review Focus

1. **A unit standing on the grid's edge or a tile outside the region**:
   every query answers "nothing" (empty list, `None`, impassable), never
   reads outside its arrays. Pinned in Tasks 7, 9, 10 and 11.
2. **A blast that crosses the grid boundary or removes a unit's floor**:
   partial rebuild touches only in-region tiles and a tile with no floor
   left reads as not standable. Pinned in Task 8.
3. **Units with equal CT and equal speed, or a speed of 0**: the turn
   order is the tie-break order every time, and a unit at speed 0 never
   gets a turn while `advance` still terminates. Pinned in Task 12.
4. **The pointer over the sky or outside the grid**: picking answers
   `None`, the camera does not jump. Pinned in Task 11.
5. **The camera asked for a shot whose framing is inside a wall**: it
   falls back to the orbit view and still reports the shot done. Pinned in
   Task 14.

---

## Part 1: Strata core

### Task 1: Matrix inverse and screen rays

**Files:**
- Modify: `lib/strata/math/mat4.march` (add `invert`)
- Create: `lib/strata/math/ray.march`
- Create: `docs/tactics-ledger.md` (empty heading: "Rulings made while
  building plugins and strata_tactics")
- Test: `test/math_test.march`

**Interfaces:**
- Produces:
  - `Strata.Math.Mat4.invert(m : NativeFloatArr) : Option(NativeFloatArr)`
    (`None` when the determinant's magnitude is below 1e-12).
  - `Strata.Math.Ray.from_screen(view_proj : NativeFloatArr, px : Float, py
    : Float, width : Float, height : Float) : Option(((Float, Float,
    Float), (Float, Float, Float)))`: origin on the near plane and unit
    direction, in metres. Pixel (0, 0) is top-left, as GLFW reports the
    pointer; clip space is GL's (z in [-1, 1]) as `Mat4.perspective` makes.

- [ ] **Step 1: Write the failing tests** in `test/math_test.march`:
  - `"invert of a look_at times perspective, times the matrix, is the identity"`
    — each of 16 entries within 1e-5.
  - `"invert of a singular matrix is None"` — all-zero matrix.
  - `"the centre pixel's ray runs along the camera's view"` — eye
    (0, 10, 10) looking at the origin, fov 60°, 800 × 600: direction within
    1e-4 of normalise(0, -10, -10); origin within 0.2 m of the eye.
  - `"the top-left pixel's ray leans up and left"` — same camera: the
    ray's direction has x < 0 and is above the centre ray's y.
- [ ] **Step 2: Run** `forge test` at the repo root. Expected: the four fail
  (unknown function).
- [ ] **Step 3: Implement** `invert` (cofactor expansion) and
  `from_screen` (unproject NDC (x, y, -1) and (x, y, 1), subtract,
  normalise).
- [ ] **Step 4: Run** `forge test`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(math): matrix inverse and screen rays`.

### Task 2: Merging input bindings from several owners

**Files:**
- Modify: `lib/strata/input.march`
- Test: `test/input_test.march`

**Interfaces:**
- Produces:
  - `type Clash = Clash(Source, String, String, String, String)` — the
    source, the losing owner, its action, the winning owner, its action —
    with readers `clash_source(c) : Source`, `clash_loser`,
    `clash_loser_action`, `clash_winner`, `clash_winner_action` (Strings).
  - `Strata.Input.merge(owners : List((String, List((String, Source,
    Float))))) : (List((String, Source, Float)), List(Clash))`. Owners are
    in priority order (the game lists itself first). A binding whose source
    an earlier owner already bound to a *different* action is dropped and
    reported; the same owner binding one source to two actions is kept (as
    today). Bindings to the same action name from two owners are both kept.
- Ruling to ledger: "earlier owner wins a clash; the loser's binding is
  dropped".

- [ ] **Step 1: Write the failing tests**:
  - `"merge with no clash keeps every binding in order"`.
  - `"a later owner's binding on a taken key is dropped and reported"` —
    game binds Key(81) to "game.menu", tactics binds Key(81) to
    "tactics.rotate_left": result has one Key(81) binding (game's) and one
    Clash with loser "tactics", loser action "tactics.rotate_left".
  - `"one owner may bind a key to two actions"` — both kept, no clash.
  - `"pad axes and mouse buttons clash like keys"`.
- [ ] **Step 2: Run** `forge test`; expected FAIL.
- [ ] **Step 3: Implement** `merge`.
- [ ] **Step 4: Run** `forge test`; expected PASS.
- [ ] **Step 5: Commit** `feat(input): merge bindings from several owners and report clashes`.

### Task 3: `Strata.Overlay` — what to draw over the world, as data

**Files:**
- Create: `lib/strata/overlay.march`
- Test: `test/overlay_test.march`

**Interfaces:**
- Produces:
  - `type Item = OvQuad(Float × 12 corners, Float r, g, b, a, Bool
    through) | OvLine(Float × 6 ends, Float width_m, Float r, g, b, a, Bool
    through)` — corners in strip order as `Strata.Model` uses (0, 1 along
    one edge, 2, 3 along the other); `through` draws the item faintly where
    geometry hides it. Constructors through functions:
    `quad(corners : List((Float, Float, Float)), rgba : (Float, Float,
    Float, Float), through : Bool) : Item` and `line(a, b, width, rgba,
    through) : Item`; `kind_index(i) : Int` (0 quad, 1 line).
  - `type Cut = Cut(Option(CutHeight), List(CutCapsule))` with
    `CutHeight(Float y, Float x0, Float z0, Float x1, Float z1)` (hide
    everything above y inside the box, metres) and `CutCapsule(Float ax, ay,
    az, bx, by, bz, Float radius)`. Functions: `no_cut() : Cut`,
    `cut_above(y, x0, z0, x1, z1) : Cut`, `with_capsule(c, a, b, radius) :
    Cut`, `merge_cuts(a : Cut, b : Cut) : Cut` (lowest height wins, its box
    is the union; capsules concatenated, at most 4 kept, first first).
  - `type Overlay = Overlay(List(Item), Cut)`; `empty() : Overlay`,
    `add(o, item) : Overlay`, `merge(a, b) : Overlay` (items of a then b;
    cuts merged).
  - `pack(o : Overlay) : (NativeF32Arr, Int)` — the items and their count,
    **20 floats per item**: `[kind, through, r, g, b, a, width, 0, g0 …
    g11]`; a quad's g0..g11 are its four corners (x, y, z), a line's g0..g5
    its two ends and the rest 0; `width` is 0 for quads. The renderer task
    reads this layout.
  - `cut_floats(c : Cut) : NativeF32Arr` — 36 floats: `[has_height, y,
    x0, z0, x1, z1, n_capsules, pad]` then 4 × 7 capsule floats.

- [ ] **Step 1: Write the failing tests**:
  - `"pack writes 20 floats per item in the stated layout"` — one quad and
    one line; check kind, through, colour, width and the corner floats.
  - `"merge_cuts keeps the lower height and the union box"`.
  - `"merge_cuts keeps at most four capsules, first first"`.
  - `"an empty overlay packs to zero items"`.
- [ ] **Step 2: Run** `forge test`; FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `forge test`; PASS.
- [ ] **Step 5: Commit** `feat(overlay): overlay items and cutaway rules as data`.

### Task 4: The overlay pass in the renderer

**Files:**
- Create: `native/shaders/overlay.vert`, `native/shaders/overlay.frag`
- Modify: `native/strata_vk.c` (a pipeline kind, a host-visible buffer
  sized for 4096 items, `sv_overlay_set(const float*, int)`), 
  `native/build_shaders.sh`, `native/strata_shaders.h` (regenerated)
- Modify: `lib/strata/ffi/vk.march` (`x_overlay_set` / `overlay_set`),
  `lib/strata/render.march` (`overlay(o : Strata.Overlay.Overlay) : Bool`)
- Modify: `viewer/lib/strata_viewer.march` (`STRATA_OVERLAY_SCENE=1`: a
  10 × 10 checker of quads 2 cm above flat ground in front of the camera,
  a line passing behind a wall with `through = true`)
- Modify: `scripts/golden.sh` (scene `overlay`), add
  `test/golden/overlay.ppm.gz`

**Interfaces:**
- Consumes: `Strata.Overlay.pack` layout (Task 3).
- Produces: `Strata.Render.overlay(o : Strata.Overlay.Overlay) : Bool`
  — call once per frame before `Strata.Render.draw`; false when items were
  dropped (more than 4096). The count dropped is `Strata.Ffi.Vk.stat(n)`
  with the next free stat index (ledger it).

Approach: draw after lighting/composite and before the UI, alpha-blended,
depth-tested against the scene depth with a small bias toward the camera
(no z-fighting at 2 cm); items with `through` draw a second time with depth
test inverted (greater) at 35% alpha. Lines are camera-facing quads of the
given width in metres, built in the vertex shader from the pulled item
(no vertex buffer, as the figure pass does).

- [ ] **Step 1: Add the viewer scene and golden entry**; run
  `scripts/golden.sh` and confirm the `overlay` scene FAILS for lack of a
  stored image (and every other scene still passes).
- [ ] **Step 2: Implement** the pass, the FFI and `Render.overlay`.
- [ ] **Step 3: Run** the scene with `STRATA_FRAMES=60 STRATA_SHOT=…`
  and look at the image: checker visible, no flicker or z-fighting, the
  line shows faintly through the wall. Then `scripts/golden.sh --update`
  for this scene only, inspect, and copy it to `docs/overlay.png`.
- [ ] **Step 4: Run** `scripts/verify.sh`; expected PASS.
- [ ] **Step 5: Commit** `feat(render): a world-space overlay pass`.

### Task 5: The cutaway in the renderer

**Files:**
- Modify: `native/shaders/ubo.glsl` (cut fields), `gbuffer.frag`,
  `figure.vert`/its fragment path, `loose.vert` path (bodies and shapes),
  `cross.vert` path — discard fragments inside the cut;
  `native/strata_vk.c` (`sv_set_cut(const float*)` into the UBO),
  `native/strata_shaders.h`
- Modify: `lib/strata/ffi/vk.march`, `lib/strata/render.march`
  (`cut(c : Strata.Overlay.Cut) : Unit`; `Render.overlay` also calls it
  with the overlay's cut)
- Modify: `viewer/lib/strata_viewer.march` (`STRATA_CUT_SCENE=1`: a hollow
  stone house with a figure inside; cut above 2 m over the house's box and a
  capsule from the eye to the figure), `scripts/golden.sh` (scene `cut`),
  `test/golden/cut.ppm.gz`, `docs/cut.png`

**Interfaces:**
- Consumes: `Strata.Overlay.cut_floats` (Task 3).
- Produces: `Strata.Render.cut(c : Strata.Overlay.Cut) : Unit`.

Approach: the cut is a raster discard only. The traced light and shadow
read the voxel data, not the G-buffer, so cut geometry keeps casting
shadows and blocking light (check this holds; if a traced pass reads the
G-buffer for occlusion, ledger what changes and why). A 0.1 m dithered edge
at the height cut is the nice-to-have; do it if it is a few lines.

- [ ] **Step 1: Add the scene and golden entry**; confirm it fails for lack
  of an image and the rest pass.
- [ ] **Step 2: Implement.**
- [ ] **Step 3: Look at the image**: roof and upper walls gone, the figure
  visible through the capsule, the floor still in the house's shadow. Store
  the golden and `docs/cut.png`.
- [ ] **Step 4: Run** `scripts/verify.sh`; PASS (no other golden changes:
  with no cut set the UBO's `has_height = 0` and `n_capsules = 0`).
- [ ] **Step 5: Commit** `feat(render): cutaway by height and capsule`.

### Task 6: `Strata.Plugin` and the plugin guide

**Files:**
- Create: `lib/strata/plugin.march`, `docs/plugins.md`
- Test: `test/plugin_test.march`

**Interfaces:**
- Consumes: Tasks 1–3.
- Produces (`Strata.Plugin`):
  - `type Frame = { pf_world : Strata.World.World, pf_input :
    Strata.Input.Input, pf_pointer : Option((Float, Float)), pf_ray :
    Option(((Float, Float, Float), (Float, Float, Float))), pf_wheel :
    Float, pf_dt : Float, pf_width : Float, pf_height : Float,
    pf_view_proj : NativeFloatArr }` and `frame(world, input, pointer,
    wheel, dt, width, height, view_proj) : Frame` which fills `pf_ray` from
    `Strata.Math.Ray.from_screen`.
  - `type Request = ReqEdit(Strata.World.Op) | ReqBlast(Int, Int, Int,
    Int, Int, Int) | ReqSound(Int, Float, Float, Float, Float)` — blast is
    (x, y, z, radius, seed, id) as `Strata.Edit.blast` takes; sound is
    (patch, x, y, z, gain); `request_index(r) : Int`; constructors
    `edit(op)`, `blast(…)`, `sound(…)`.
  - `apply(w : Strata.World.World, reqs : List(Request)) :
    (Strata.World.World, List(Strata.Edit.Outcome), List(Request))` —
    applies edits and blasts in order (each outcome's world feeds the
    next), returns the outcomes in order and the requests it does not
    handle (sounds) for the game.
  - The convention, documented in the module header and `docs/plugins.md`:
    a plugin module provides `setup(config, registry) : (state,
    Strata.Blocks.Registry, List((String, Strata.Input.Source, Float)))`,
    `update(state, frame : Frame) : (state, List(Request), List(event))`,
    `overlay(state, …) : Strata.Overlay.Overlay`, `gui(state, …) :
    Strata.Gui.GNode` (the `…` are plugin-specific arguments the game
    supplies, such as a label function); action names prefixed `<plugin>.`; never call
    `Vk.set_camera` or edit the world; a camera plugin exposes
    `view_proj(state, aspect) : NativeFloatArr`.
- `docs/plugins.md`: writing a plugin (the four parts, requests, naming,
  no privileged access); using one (forge dependency by path or git; merge
  bindings with `Strata.Input.merge`; call order each frame: build
  `Frame` with last frame's view-projection, `update`, `Plugin.apply`,
  pass outcomes to plugins that track the world, merge overlays,
  `Render.overlay`, gui layers); native code caveat (GAPS S11) and the
  later runner. Leave a "Wiring example" heading that Task 16 fills.

- [ ] **Step 1: Write the failing tests**:
  - `"frame fills the ray from the pointer and view-projection"` — matches
    `Ray.from_screen` for the same inputs; no pointer gives `None`.
  - `"apply runs edits in order and the second sees the first"` — box
    fill then set_block inside it; final block is the second's id; two
    outcomes.
  - `"apply hands back sounds untouched"`.
- [ ] **Step 2: Run** `forge test`; FAIL.
- [ ] **Step 3: Implement** the module and write `docs/plugins.md`.
- [ ] **Step 4: Run** `forge test`; PASS.
- [ ] **Step 5: Commit** `feat(plugin): the plugin shape, requests, and a guide`.

---

## Part 2: strata_tactics

Package: `plugins/strata_tactics/forge.toml` (`type = "lib"`, `strata = {
path = "../.." }`, `[ffi]` listing `../../native/strata_noise.c` and
`../../native/strata_vox.cpp` as `demo/forge.toml` does). Modules are flat
files in `plugins/strata_tactics/lib/`, named `StrataTactics.*`; tests in
`plugins/strata_tactics/test/`. Tiles are `(Int, Int)` = (tx, tz), tx in
[0, width), tz in [0, depth).

Test worlds: copy the `flat()` / `reg()` / `load_box` pattern from
`test/nav_test.march` into `plugins/strata_tactics/test/worlds.march`
(module `StrataTactics.Test.Worlds`): stone below y = 0, air above, ids
1 stone, 2..9 water, 10 stone; voxels −64..63 across, −32..31 up; scale
1.0. Helper `box(w, x0, y0, z0, x1, y1, z1, id)`.

### Task 7: Package and grid build

**Files:**
- Create: `plugins/strata_tactics/forge.toml`, `lib/strata_tactics.march`
  (root module, `version()`), `lib/grid.march`, `test/worlds.march`,
  `test/grid_test.march`
- Modify: `scripts/verify.sh` (add `plugins/strata_tactics` and
  `plugins/strata_tactics/example` to the consumer loop)

**Interfaces:**
- Produces (`StrataTactics.Grid`):
  - `type Settings = { gs_x0 : Int, gs_z0 : Int, gs_width : Int, gs_depth :
    Int, gs_tile : Int, gs_step : Int, gs_ylo : Int, gs_yhi : Int,
    gs_min_height : Int, gs_kind : Int -> Int }` — voxel origin of tile
    (0, 0), size in tiles, tile edge N in voxels, height step in voxels,
    the y range scanned, the smallest unit height in voxels, block id to
    terrain kind. `settings(x0, z0, width, depth, tile, step, ylo, yhi,
    min_height, kind) : Settings`. Defaults ledgered: tile 4, step 1.
  - `type Grid` over flat `NativeIntArr`s indexed `tz * width + tx`:
    floor (voxel y a unit's feet are in), clearance (voxels of air above
    floor, capped at `yhi - floor`), fluid (voxels of fluid above floor),
    kind, standable (0/1).
  - `build(w : Strata.World.World, s : Settings) : Grid`.
  - Readers: `width(g)`, `depth(g)`, `inside(g, t) : Bool`, `floor(g, t)
    : Int`, `clearance(g, t) : Int`, `fluid(g, t) : Int`, `kind(g, t) :
    Int`, `standable(g, t) : Bool`, `fits(g, t, height : Int) : Bool`
    (standable and clearance ≥ height), `centre(g, t) : (Float, Float,
    Float)` (metres, on the floor), `tile_of(g, x : Float, z : Float) :
    Option((Int, Int))` (metres), `settings_of(g) : Settings`. Every reader
    outside the grid answers 0 / false / `None`.

**The column rule** (designer's call, ledger it): each voxel column of a
tile is scanned from `yhi` down for the highest cell whose block below
collides (`Strata.Blocks.collides`) and which has at least `min_height`
non-colliding cells above it (fluid counts as non-colliding); that cell's
y is the column's floor, or none. The tile's floor is the median of its
columns' floors (lower median for even counts); the tile is standable when
at least half its columns have a floor within `step` of the tile's floor.
Clearance is the minimum, over those columns, of free cells above the
tile's floor. Kind is `gs_kind` of the block under the centre column's
floor. Fluid is the count of fluid cells from the floor up in the centre
column.

- [ ] **Step 1: Create the package and test worlds; write failing tests**:
  - `"flat ground: every tile stands at y 0 with full clearance"` —
    grid 8 × 8, tile 4, at (−16, −16).
  - `"a 3-voxel block raises one tile to y 3"`.
  - `"a roof over a room: the tile stands on the roof"` — hollow box,
    walls 4 high, roof at y 4: interior tiles' floor is 5.
  - `"a bridge high enough to walk under still gives the bridge"` — the
    highest rule.
  - `"a tile half rubble: median floor, standable"` and `"a tile with a
    pillar in one column: floor unchanged"`.
  - `"a column with no room for the smallest unit has no floor"` —
    ceiling 1 voxel above ground and min_height 2: tile not standable.
  - `"water one deep reads fluid 1"`.
  - `"outside the grid every reader answers nothing"` — (−1, 0), (8, 3):
    `inside` false, `standable` false, `floor` 0, `tile_of` far away
    `None`.
- [ ] **Step 2: Run** `cd plugins/strata_tactics && forge test`; FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `forge test`, then `scripts/verify.sh` (the example
  directory does not exist yet; the loop skips it); PASS.
- [ ] **Step 5: Commit** `feat(tactics): package and grid build`.

### Task 8: Partial rebuild and the grid budget

**Files:** Modify `plugins/strata_tactics/lib/grid.march`; test
`test/grid_test.march`; create `test/budget_test.march`.

**Interfaces:**
- Produces: `refresh(g : Grid, w : Strata.World.World, changes :
  List(Strata.Edit.Change)) : (Grid, List((Int, Int)))` — re-reads only
  tiles whose columns intersect a change's box (`Strata.Edit.low`/`high`,
  widened by one tile so a removed floor's neighbours' clearance updates),
  clipped to the grid; returns the tiles re-read.

- [ ] **Step 1: Failing tests**:
  - `"refresh after a blast equals a full build"` — blast radius 5 at a
    tile centre; compare all five arrays with `build`.
  - `"refresh re-reads only the touched tiles"` — a one-cell change
    reports at most 9 tiles.
  - `"a blast across the grid edge refreshes only in-region tiles"`.
  - `"a blast that removes a tile's floor down to the scan floor leaves it
    unstandable"` — `ylo` = −4, blast digs below it.
  - Budget (`budget_test.march`, timed with `System.now_ms`, reported, and
    asserted at 4 × the budget to stay robust under load): full build of 64
    × 64 tiles, tile 4, under 1 s; refresh after a radius-6 blast under
    16 ms.
- [ ] **Step 2: Run**; FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run**; PASS. Record the timings and load average in the
  ledger.
- [ ] **Step 5: Commit** `feat(tactics): partial grid rebuild`.

### Task 9: Reach and path

**Files:** Create `plugins/strata_tactics/lib/reach.march`, test
`test/reach_test.march`.

**Interfaces:**
- Produces (`StrataTactics.Reach`):
  - `type Mover = { mv_move : Int, mv_jump : Int, mv_drop : Int, mv_height
    : Int }` (tiles; height steps up; height steps down; voxels) and
    `mover(move, jump, drop, height) : Mover`.
  - `pass_open() : Int` 0, `pass_through() : Int` 1 (may cross, may not
    stop), `pass_blocked() : Int` 2.
  - `find(g : Grid, from : (Int, Int), m : Mover, cost : Int -> Int,
    pass : (Int, Int) -> Int) : Reach` — Dijkstra on 4-neighbours; entering
    a tile costs `cost(kind)` (≤ 0 means impassable); a step is allowed
    when the target `fits` the mover's height, its floor rise ≤ `jump ×
    step` and fall ≤ `drop × step`, and `pass` is not blocked; total cost ≤
    `move`. Ties broken by neighbour order +x, −x, +z, −z.
  - `tiles(r : Reach) : List((Int, Int))` — where the mover may stop
    (excludes the start and pass-through tiles).
  - `cost_to(r, t) : Option(Int)`; `path(r, t) : List((Int, Int))` — start
    to t inclusive, empty if unreachable.

- [ ] **Step 1: Failing tests**:
  - `"move 3 on flat ground reaches a diamond of 24 tiles"`.
  - `"a wall 3 steps high stops a mover with jump 2, not one with jump 3"`.
  - `"a drop of 4 steps needs drop 4"`.
  - `"terrain cost 2 halves the reach across it"`; `"cost 0 is
    impassable"`.
  - `"an ally's tile may be crossed, not stopped on; an enemy's blocks"`.
  - `"a mover too tall for the clearance cannot enter"`.
  - `"path ends at the target and each step is a neighbour"`.
  - `"from an edge tile nothing outside the grid is reached"` — start
    (0, 0), every result `inside`.
  - Budget: 64 × 64 flat grid, move 12, under 2 ms (asserted at 8 ms).
- [ ] **Step 2: Run**; FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run**; PASS.
- [ ] **Step 5: Commit** `feat(tactics): reach and path`.

### Task 10: Line of sight and cover

**Files:** Create `plugins/strata_tactics/lib/sight.march`, test
`test/sight_test.march`.

**Interfaces:**
- Produces (`StrataTactics.Sight`):
  - `clear() : Int` 0, `partial() : Int` 1, `blocked() : Int` 2.
  - `line(w : Strata.World.World, g : Grid, a : (Int, Int), eye_a : Int, b
    : (Int, Int), eye_b : Int) : Int` — five voxel rays
    (`Strata.Query.raycast` with `Strata.Query.targetable`) between points
    `eye` voxels above each floor: the centres, and the centres offset ±¼
    tile in x and in z together; clear if all five clear, blocked if none,
    partial otherwise. Outside the grid: `blocked`.
  - `none() : Int` 0, `half() : Int` 1, `full() : Int` 2.
  - `type CoverSettings = { cs_half : Float, cs_full : Float }` default
    0.4 / 0.8 (`cover_settings()`).
  - `edge(w, g, t, dir : Int, height : Int, s : CoverSettings) : Int` —
    dir 0 +x, 1 −x, 2 +z, 3 −z: over the N columns just across that edge,
    the tallest run of colliding cells starting at t's floor; full if ≥
    `cs_full × height`, half if ≥ `cs_half × height`, else none.
  - `against(w, g, t, from : (Int, Int), height, s) : Int` — the best
    `edge` among the one or two edges facing `from` (both when it is
    diagonal by more than 22.5°; none when `from == t`).

- [ ] **Step 1: Failing tests**:
  - `"open ground is clear"`; `"a wall taller than both eyes blocks"`;
    `"a wall covering half the tile's width is partial"`.
  - `"blasting the wall opens the line"` — blocked, then `Edit.blast`,
    then clear.
  - `"a low wall is half cover, a tall one full, nothing is none"` —
    unit height 8: 4-high wall half, 7-high full.
  - `"cover counts only the edge facing the attacker"`; `"a diagonal
    attacker gets the better of two edges"`.
  - `"a line to a tile outside the grid is blocked"`.
- [ ] **Step 2: Run**; FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run**; PASS.
- [ ] **Step 5: Commit** `feat(tactics): line of sight and cover`.

### Task 11: Target areas and picking

**Files:** Create `plugins/strata_tactics/lib/area.march`, test
`test/area_test.march`.

**Interfaces:**
- Produces (`StrataTactics.Area`):
  - `radius(g, centre, r : Int, vertical : Option(Int)) : List((Int,
    Int))` — Manhattan distance ≤ r; with `Some(v)`, only tiles whose floor
    differs from the centre's by ≤ v voxels (FFT's vertical tolerance).
  - `line(g, from, dir : Int, length : Int, vertical) : List((Int, Int))`
    — dir as in Sight; excludes `from`.
  - `cone(g, from, dir, length, vertical)` — step k (1..length) covers
    2k − 1 tiles across.
  - `pick(w : Strata.World.World, g : Grid, ray : ((Float, Float, Float),
    (Float, Float, Float)), cut_y : Option(Float), max : Float) :
    Option((Int, Int))` — when `cut_y` is set and the origin is above it,
    the ray starts where it crosses y = cut_y (cut geometry cannot be
    picked; capsule cuts are ignored, ledger it); `Strata.Query.raycast`
    to the first targetable voxel; the tile under the hit's x, z if
    `inside`. A miss, a ray pointing up from above the cut, or a hit off
    the grid gives `None`.

- [ ] **Step 1: Failing tests**:
  - `"radius 2 is 13 tiles on flat ground"`; `"vertical tolerance drops a
    raised tile"`; `"areas clip at the grid edge"`.
  - `"a line of 3 heading +x"`; `"a cone of 3 is 1 + 3 + 5 tiles"`.
  - `"a ray straight down at a tile centre picks that tile"`.
  - `"a ray at a roof with a cut below it picks the floor inside"`.
  - `"a ray into the sky picks nothing"`; `"a ray hitting ground off the
    grid picks nothing"`.
- [ ] **Step 2: Run**; FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run**; PASS.
- [ ] **Step 5: Commit** `feat(tactics): target areas and picking`.

### Task 12: The turn clock (FFT)

**Files:** Create `plugins/strata_tactics/lib/clock.march`, test
`test/clock_test.march`; modify `docs/tactics-ledger.md`.

**Interfaces:**
- Produces (`StrataTactics.Clock`):
  - `type Settings = { ck_turn_at : Int, ck_base : Int, ck_move : Int,
    ck_act : Int, ck_cap : Option(Int), ck_charge_full : Int,
    ck_max_ticks : Int }` — defaults from the ledger (Step 1); FFT's are
    expected to be 100, 60, 20, 20, a cap to be confirmed, 100, and
    `max_ticks` 10000 (safety for `advance`).
  - `start(s : Settings) : Clock`.
  - Units: `add(c, id : Int, speed : Int, ct : Int) : Clock` (added order
    is the tie-break), `remove(c, id)`, `set_speed(c, id, speed)`,
    `ct(c, id) : Int`, `speed(c, id) : Int` (effective).
  - Timers: `start_timer(c, id : Int, unit : Int, name : String, ticks :
    Int, speed_num : Int, speed_den : Int) : Clock` (a plain status uses
    1/1; Haste and Slow are the game's numbers), `stop_timer(c, id)`.
    Effective speed = floor(base × Π num / Π den) over the unit's live
    timers.
  - Charged actions: `charge(c, id : Int, unit : Int, speed : Int) :
    Clock` (counter starts at `ck_charge_full`, falls by `speed` each
    tick, resolves at ≤ 0), `cancel(c, id)`, `charging(c, unit) : Bool`.
  - `type Event = TimerEnded(Int, Int) | ChargeResolved(Int, Int) |
    TurnStarted(Int)` (timer id, unit / charge id, unit / unit);
    `event_index(e) : Int`.
  - `advance(c) : (Clock, List(Event))` — runs ticks (each: timers −1 and
    end at 0; due charges resolve in queued order; CT += effective speed;
    then the first unit in added order with CT ≥ `turn_at` gets the turn)
    until a turn starts or `max_ticks` pass. If a unit already has CT ≥
    `turn_at` when called (another unit's turn just ended), it gets its
    turn with no tick.
  - `active(c) : Option(Int)`.
  - `end_turn(c, moved : Bool, acted : Bool) : Clock` — CT −= base (+ move)
    (+ act), then the cap if set, floor 0.
  - `forecast(c, k : Int) : List(Event)` — the next k `TurnStarted` and
    `ChargeResolved` events, assuming every unit both moves and acts,
    without changing `c`. `forecast_if(c, moved, acted, k)` — the same
    after the active unit ends its turn so.

- [ ] **Step 1: Research and ledger** the three open details against
  *Final Fantasy Tactics: The Ivalice Chronicles*, Enhanced mode: the CT
  cap after a turn (value or none), the tie-break among units at
  CT ≥ 100, and the order due charged actions resolve in. Cite sources in
  the ledger. Where Enhanced and Classic differ, write both. Set the
  defaults from it.
- [ ] **Step 2: Failing tests** (each sequence worked out by hand in a
  comment beside it):
  - `"speed 10 and speed 8 from 0: turn order for the first 6 turns"` —
    both move and act each turn; cap off.
  - `"waiting costs 60, moving or acting 80, both 100"`.
  - `"the cap, when set, limits CT after a turn"`.
  - `"equal speed and CT: the first added goes first, every time"`.
  - `"a unit at speed 0 never acts and advance stops at max_ticks"` — and
    with only speed-0 units, `advance` returns no `TurnStarted`.
  - `"a charge of speed 25 resolves on the 4th tick, before CT rises"`.
  - `"timers end before charges resolve on the same tick"`.
  - `"haste 3/2 on speed 8 gives 12, starting on the tick it was set"`.
  - `"forecast does not change the clock and matches advancing"`.
  - `"defaults are the ledger's"` — the settings equal the Step 1 values.
- [ ] **Step 3: Run**; FAIL.
- [ ] **Step 4: Implement.**
- [ ] **Step 5: Run**; PASS. Budget test: forecast of 20 turns with 16
  units under 1 ms (asserted at 4 ms).
- [ ] **Step 6: Commit** `feat(tactics): the FFT turn clock`.

### Task 13: The camera: orbit, pan, zoom, levels, cut

**Files:** Create `plugins/strata_tactics/lib/camera.march`, test
`test/camera_test.march`.

**Interfaces:**
- Produces (`StrataTactics.Camera`):
  - `type Settings = { cm_step_deg : Float, cm_turn_s : Float, cm_near :
    Float, cm_far : Float, cm_pitch_near_deg : Float, cm_pitch_far_deg :
    Float, cm_fov_deg : Float, cm_pan_speed : Float, cm_edge_px : Float,
    cm_level_m : Float, cm_cut_radius : Float, cm_free : Bool, cm_tilt :
    Bool }` — defaults 45°, 0.25 s, 10 m, 40 m, 35°, 60°, 40°, 15 m/s at
    near (scaled by distance / near), 12 px, 3 m, 2.5 m, false, true.
    `settings() : Settings`.
  - `bindings() : List((String, Strata.Input.Source, Float))` — actions
    `tactics.pan_x` (A/D, pad axis 0), `tactics.pan_z` (W/S, pad axis 1),
    `tactics.rotate_left` (Q), `tactics.rotate_right` (E),
    `tactics.zoom` (R/F, pad axes 2/3 as mapped), `tactics.level_up`
    (Page Up), `tactics.level_down` (Page Down), `tactics.drag` (mouse
    button 1), `tactics.select` (mouse button 0). Scroll wheel zoom is
    read from the frame's wheel when the game passes it (`step` takes
    it).
  - `start(s, focus : (Float, Float, Float), yaw_deg : Float) : Camera`.
  - `step(c, i : Strata.Input.Input, pointer : Option((Float, Float)),
    wheel : Float, width : Float, height : Float, dt : Float, ground :
    Float -> Float -> Float) : Camera` — rotation steps turn smoothly over
    `turn_s` (eased); drag rotates freely when `cm_free` or the drag
    action is held; pan is screen-relative to yaw and keeps focus y on
    `ground(x, z)`; zoom eases between near and far; pitch follows
    distance linearly from near to far when `cm_tilt`.
  - `follow(c, target : (Float, Float, Float)) : Camera` — eases the focus
    there over `turn_s`.
  - `level(c) : Int`, `set_level(c, n)` — cut height = focus floor + n ×
    `level_m` + `level_m`; n is clamped to [0, 8].
  - `eye(c) : (Float, Float, Float)`; `view_proj(c, aspect : Float) :
    NativeFloatArr`.
  - `cut(c, subject : Option((Float, Float, Float)), box : (Float, Float,
    Float, Float)) : Strata.Overlay.Cut` — the level's height cut over the
    given box (the grid's extent), plus a capsule from the eye to the
    subject (or the focus) with `cut_radius`.

- [ ] **Step 1: Failing tests**:
  - `"rotate_right turns 45 degrees over a quarter second, not at once"`.
  - `"two presses queue to 90 degrees"`.
  - `"pan +x after a 90 degree turn moves the focus along the turned axis"`.
  - `"the focus rides the ground height"`.
  - `"zoom stays within near and far; pitch at near is 35, at far 60"`.
  - `"with tilt off the pitch never changes"`; `"with free on, drag turns
    by pointer motion"`.
  - `"view_proj puts the focus at the screen centre"` — via
    `Mat4.transform_point`, NDC within 1e-4 of (0, 0).
  - `"cut at level 0 is level_m above the focus floor; the capsule runs eye
    to subject"`.
  - `"edge scroll pans when the pointer is within edge_px of a side"`.
- [ ] **Step 2: Run**; FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run**; PASS.
- [ ] **Step 5: Commit** `feat(tactics): the camera orbit, pan, zoom and cutaway`.

### Task 14: Camera shots

**Files:** Modify `plugins/strata_tactics/lib/camera.march`; test
`test/camera_test.march`.

**Interfaces:**
- Produces:
  - `look(c, id : Int, at : (Float, Float, Float), hold_s : Float) :
    Camera` — travel to frame `at` (over `turn_s × 2`), hold, return to
    the orbit view as it was.
  - `frame_two(c, id : Int, a : (Float, Float, Float), b : (Float, Float,
    Float), side : Bool, hold_s : Float, w : Strata.World.World) : Camera`
    — over the shoulder of a toward b (side = false: 1.5 m behind and
    0.8 m right of a, 1.8 m up) or from the side (perpendicular to a–b at
    the midpoint, back far enough to frame both with the fov). If a ray
    from the shot's eye to a or to b hits geometry, or the eye is inside a
    colliding voxel, the shot is replaced by a `look` at the midpoint.
  - `shot_done(c) : List(Int)` — ids whose shot finished on the last
    `step`; `in_shot(c) : Bool`. While in a shot, `step` ignores pan,
    rotate and zoom input.

- [ ] **Step 1: Failing tests**:
  - `"a look shot travels, holds, returns and reports done once"`.
  - `"input is ignored during a shot"`.
  - `"over the shoulder puts a in front-left of centre and b ahead"` —
    project both with `view_proj`; a's NDC x < 0, b near centre.
  - `"a shot whose eye is inside a wall falls back and still reports
    done"`.
- [ ] **Step 2: Run**; FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run**; PASS.
- [ ] **Step 5: Commit** `feat(tactics): camera shots`.

### Task 15: Overlay helpers and the plugin module

**Files:** Create `plugins/strata_tactics/lib/marks.march`; modify
`lib/strata_tactics.march`; tests `test/marks_test.march`,
`test/plugin_test.march`.

**Interfaces:**
- Consumes: Grid, Reach, Sight, Area, Clock, Camera; `Strata.Plugin`,
  `Strata.Overlay`.
- Produces (`StrataTactics.Marks`, each returning
  `List(Strata.Overlay.Item)`, colours as `(Float, Float, Float, Float)`
  parameters with defaults in `colours()`):
  `tiles(g, ts, rgba)` (filled quads 0.03 m above each floor, inset 5%),
  `outline(g, ts, rgba)` (lines along the region's outer edges only),
  `path(g, ps, rgba)` (a line through centres + a marker quad at the end,
  `through = true`), `sight(g, a, b, result)` (green / amber / red line),
  `cursor(g, t, rgba)`, `cover(w, g, t, height, s)` (a short upright
  quad on each edge with half or full cover, two heights).
- Produces (`StrataTactics`, the plugin):
  - `type Config = { tc_grid : StrataTactics.Grid.Settings, tc_camera :
    StrataTactics.Camera.Settings, tc_clock : StrataTactics.Clock.Settings
    }`, `config(grid_settings) : Config` (camera and clock defaults).
  - `type Event = Hovered((Int, Int)) | Unhovered | Clicked((Int, Int)) |
    ShotDone(Int)`, `event_index(e)`.
  - `setup(cfg, r : Strata.Blocks.Registry, w : Strata.World.World) :
    (State, Strata.Blocks.Registry, List((String, Strata.Input.Source,
    Float)))` — builds the grid, starts the clock and camera (focus at the
    grid's centre), returns the camera's bindings.
  - `update(s, f : Strata.Plugin.Frame) : (State,
    List(Strata.Plugin.Request), List(Event))` — steps the camera with the
    grid's floor as ground; picks with `f.pf_ray` and the camera's cut
    height; emits Hovered/Unhovered on change, Clicked on
    `tactics.select` going down over a tile, ShotDone per finished shot.
    No requests in this version (the list is there for the shape).
  - `world_changed(s, w, outcomes : List(Strata.Edit.Outcome)) : State`
    — refreshes the grid from the outcomes' changes.
  - Readers and pass-throughs the game uses: `grid(s)`, `clock(s)`,
    `camera(s)`, `with_clock(s, c)`, `with_camera(s, c)`, `hovered(s) :
    Option((Int, Int))`, `show(s, items : List(Strata.Overlay.Item)) :
    State` (the game's chosen marks for this frame, cleared by `update`).
  - `overlay(s, subject : Option((Float, Float, Float))) :
    Strata.Overlay.Overlay` — shown marks + cursor + the camera's cut over
    the grid's box.
  - `view_proj(s, aspect) : NativeFloatArr`.
  - `gui(s, label : Int -> String, k : Int) : Strata.Gui.GNode` — a
    column titled "Turn order" listing the next k forecast entries
    (units by label, charges as "<label> (charge)"), the active unit
    first.

- [ ] **Step 1: Failing tests**:
  - Marks: `"tiles makes one quad per tile above its floor"`; `"outline
    has no inner edges"` (a 2 × 2 block gives 8 edge lines);
    `"path marks its end and draws through"`.
  - Plugin: `"setup returns the camera bindings with tactics. names"`;
    `"a frame with the pointer over a tile hovers it once"`; `"select over
    a tile emits Clicked"`; `"the pointer over the sky unhovers and leaves
    the camera's focus where it was"`; `"world_changed after a blast updates
    standable"`; `"overlay carries the camera's cut"`; `"gui lists k turns
    with the active unit first"`.
- [ ] **Step 2: Run**; FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run**; PASS.
- [ ] **Step 5: Commit** `feat(tactics): overlay helpers and the plugin module`.

### Task 16: The example game

**Files:**
- Create: `plugins/strata_tactics/example/forge.toml` (app; deps
  `strata = { path = "../../.." }`, `strata_tactics = { path = ".." }`;
  `[ffi]` as `viewer/forge.toml`, paths one level deeper),
  `example/lib/tactics_example.march`, `example/run.sh` (sets
  `MARCH_PIN_MAIN=1`, builds, runs)
- Modify: `docs/plugins.md` ("Wiring example": the plugin-related lines
  of the example, counted), `docs/tactics-ledger.md`
- Create: `docs/tactics-example.png`

**What it does:** a flat stone world (the test-world generator) with a
few walls, a low wall and a hollow building; grid 24 × 24, tile 4.
Four units on two sides drawn as overlay boxes (coloured upright quads,
team colours), each with speed 6–10, move 4, jump 2, drop 3, height 8.
The clock decides whose turn it is; the active unit's reach is shown;
hovering a reachable tile shows the path; clicking moves the unit there
(teleport; no animation); `T` then hovering shows line of sight and cover
to the hovered tile; `B` blasts the hovered tile (`Strata.Plugin.blast`
request → `Plugin.apply` → `world_changed`); `Space` ends the turn (moved
and acted as done); the turn order panel shows on the right. Camera fully
live; the camera follows the new active unit with `follow` and a `look`
shot at each blast.

Scripted mode for checks: `TACTICS_FRAMES=n TACTICS_SHOT=path` runs a
fixed sequence (first unit's reach shown, a path hovered, one blast, the
camera turned 45°) and writes the frame as a PPM, as the viewer's
`STRATA_FRAMES` / `STRATA_SHOT` do.

- [ ] **Step 1: Build** `cd plugins/strata_tactics/example && forge build
  --release`; expected success.
- [ ] **Step 2: Run** scripted, convert to PNG, look: grid marks on the
  ground, units visible, building cut away around the camera's view, the
  blast's hole in a wall, the turn order panel. Store
  `docs/tactics-example.png`.
- [ ] **Step 3: Play it** windowed for a minute: rotate, zoom, pan, level
  up and down, move units, blast a wall and see line of sight change. Note
  anything off in the ledger and fix it.
- [ ] **Step 4: Count the wiring** (lines that exist only because of the
  plugin: dependency, merge bindings, setup, update, apply/world_changed,
  overlay, gui, view_proj) and write it into `docs/plugins.md`. If it is
  well over a dozen, ledger why.
- [ ] **Step 5: Run** `scripts/verify.sh`; PASS.
- [ ] **Step 6: Commit** `feat(tactics): an example game`.

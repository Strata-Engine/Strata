# strata_kit (how mechs are built) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** modular voxel figures from parts with typed mounts, assembled
into a `Strata.Rig`, walking, aiming, carved by hits, losing parts that
fall and settle as rubble; plus the Strata core changes that make
swapping and carving sustainable.

**Architecture:** Strata core gains rig grid replacement, and replacing
and freeing rigs in `Strata.Anim` and the renderer. `strata_kit` is a
forge package under `plugins/strata_kit/` that uses only public Strata,
holding parts as values, a build as a tree, and pure functions from a
build to a rig. A turntable example shows it all.

**Tech Stack:** March (forge), the animation kernel `native/strata_anim.c`
/ `strata_anim.h` (headless scenes in `animcheck/`, run by
`scripts/anim_check.sh`), the renderer `native/strata_vk.c`, Python 3 for
test fixtures (the `.vox` writer in `scripts/characters.py`).

**Spec:** `docs/2026-11-20-mech-construction-design.md`. Rulings made on
the way go in `docs/kit-ledger.md` (created in Task 1).

**Depends on:** Part 1 (Tasks 1–6) of
`docs/2026-11-20-plugins-tactics-plan.md` (`Strata.Plugin` requests are
used in Task 11). Tasks 1–10 here do not need it and may run before it.

## Global Constraints

- `. scripts/env.sh` before any forge command; windowed runs need
  `MARCH_PIN_MAIN=1` (GAPS S8).
- Commit after each task; merge the work branch into main after each
  reviewed task.
- Strata core names no game concept: no mech, chassis, weight or power in
  `lib/strata/`. `strata_kit` never says "mech" either: parts, mounts,
  slots, sizes, tags.
- `strata_kit` uses only public Strata. Anything missing goes into Strata
  core in its own commit, with a test, noted in the ledger.
- Mech voxels 0.1 m; the example world 0.4 m per voxel (loose parts are
  fine bodies at k = 4).
- Rig limits: 64 parts, 128 voxels a side per part, 32 sockets per rig,
  palette indices 1..255. A build that exceeds them is refused with a
  reason, never truncated silently.
- Budget: up to 12 assembled figures alive at once; a hangar preview swap
  (assemble + replace + re-upload) and a carve each under 16 ms for a
  25-part build; no pool exhaustion after 1000 replace/free cycles.
- Shared palette: the game supplies one; trim indices and the interior
  index are fixed by the game and passed to the kit.
- March pitfalls (GAPS.md): no `opaque`, `by`, `on`, `loop` as names;
  constructors do not resolve through aliases (give sum types an
  `_index` reader); aliases unique across the test suite; bind extern
  results; `spawn` is a keyword (`spawn_at`).
- `scripts/verify.sh` stays green and gains `plugins/strata_kit` and
  `plugins/strata_kit/example` in its consumer loop (Task 4).

## Review Focus

1. **A build that does not fit** (part on the wrong slot, too big, a mount
   name the parent lacks, past 64 parts or 32 markers): `assemble` still
   returns a rig of everything that did fit, and lists each problem with
   the node it concerns. Pinned in Task 7.
2. **Carving that would cut a part off its joint, or empties a part**: the
   attach region survives, and a part with no voxels left stays as a pure
   joint (its children keep their place). Pinned in Task 9.
3. **Mirrored parts**: markers, facing and up follow the mirror, so a
   left gun's muzzle points forward, not backward, and the hand still
   meets the grip. Pinned in Tasks 4 and 7.
4. **Replacing a rig that figures are using mid-animation**: they keep
   their placement and draw the new rig next frame; no stale part index
   (a removed arm's override) touches the new rig. Pinned in Task 2.
5. **A loose piece that comes to rest overlapping terrain, a figure, or
   off the map**: settling writes only free cells inside the world and
   drops the rest; nothing is stamped into a figure. Pinned in Task 11.

---

## Part 1: Strata core

### Task 1: Replacing a rig part's grid

**Files:**
- Modify: `lib/strata/rig.march`
- Test: `test/rig_test.march`
- Create: `docs/kit-ledger.md` (heading "Rulings made while building
  strata_kit")

**Interfaces:**
- Produces: `Strata.Rig.with_grid(r : Rig, p : Int, g : NativeU8Arr) :
  Rig` — part p's voxels become `g` (same size as the part, index
  `x + sx * (y + sy * z)`, palette indices, 0 empty), replacing its edit
  history; refused (counted, reason set) for a bad part or a wrong length.
  `Strata.Rig.grid` then answers `g`, and `meshes` meshes it.

- [ ] **Step 1: Failing tests**:
  - `"with_grid replaces a part's voxels and meshes them"` — 4×4×4 part,
    grid with one voxel at (1, 2, 3) colour 5: `get` answers 5 there and 0
    elsewhere; `meshes` has 6 faces for that part.
  - `"with_grid of the wrong length is refused and changes nothing"`.
  - `"with_grid then box edits apply on top"`.
- [ ] **Step 2: Run** `forge test` at the repo root; FAIL.
- [ ] **Step 3: Implement** (store the grid as the part's base; edits
  apply over it).
- [ ] **Step 4: Run** `forge test`; PASS.
- [ ] **Step 5: Commit** `feat(rig): replace a part's grid`.

### Task 2: Replacing and freeing rigs in the animation kernel

**Files:**
- Modify: `native/strata_anim.c`, `native/strata_anim.h` (rig slots,
  shape and brick pools reusable), `native/stamp_anim.sh` stamp,
  `lib/strata/ffi/anim.march`, `lib/strata/anim.march`
- Modify: `animcheck/lib/animcheck.march` (scenes `replace`, `free`,
  `churn`), `scripts/anim_check.sh` (add them)

**Interfaces:**
- Produces:
  - `Strata.Anim.replace_rig(a : Anim, id : Int, r : Strata.Rig.Rig) :
    (Anim, Bool)` — rig `id` becomes `r` (any part count). Every figure of
    that rig keeps its placement, drop and attachment as a child; its
    layers, overrides, aim and reach slots, springs and per-part state are
    cleared, because part numbers may differ. False (and nothing changed)
    when `r` is refused or `id` is not a live rig. `rig(a, id)` answers `r`.
  - `Strata.Anim.free_rig(a : Anim, id : Int) : (Anim, Bool)` — frees the
    rig's slot, shapes and bricks; refused (false) while any figure uses
    it. A freed id may be reused by a later `finish_rig`.
  - `Strata.Anim.rig_users(a, id) : Int` — figures of that rig.
  - Pool usage readable through `Strata.Anim.stat` (ledger the indices):
    live rigs, shapes used, bricks used.
- Ruling to ledger: "replace keeps the id so figures need no re-rig; per
  part animation state is cleared (the design's 're-rig a figure without
  losing its animation state' is met for placement and attachment; layer
  state cannot survive renumbered parts)".

- [ ] **Step 1: Write the scenes** (each prints `ok <scene>` or fails):
  - `replace` — rig A (3 parts), figure f with a layer playing and an
    override on part 2, placed at (5, 0, 5); replace with rig B (5
    parts): f's origin unchanged, `hit_ray` at B's new top part hits f
    with part index 4, no override applied (part 2's pose equals rest),
    `rig_users` 1.
  - `free` — freeing a used rig fails; after removing its figures it
    succeeds; the next `finish_rig` reuses the id; shapes and bricks
    return to their earlier counts.
  - `churn` — 12 figures with their own 25-part rigs; 1000 rounds of
    replacing a random one with a rebuilt rig: never refused, bricks used
    at the end within 5% of after round 1.
- [ ] **Step 2: Run** `scripts/anim_check.sh`; the three new scenes FAIL.
- [ ] **Step 3: Implement** the kernel changes (free lists for rig slots,
  shapes and bricks; clearing figure state on replace) and the March
  calls.
- [ ] **Step 4: Run** `scripts/anim_check.sh` and `forge test`; PASS, and
  the existing scenes' hashes unchanged.
- [ ] **Step 5: Commit** `feat(anim): replace and free rigs`.

### Task 3: The renderer follows replaced and freed rigs

**Files:**
- Modify: `lib/strata/render.march`, `native/strata_vk.c` if
  `rig_clear` does not already free mesh space for reuse
- Modify: `viewer/lib/strata_viewer.march` (`STRATA_RIG_SWAP=1`: a figure
  of a 2-part rig, replaced at frame 20 by a 4-part rig of another
  colour), `scripts/golden.sh` (scene `rig-swap`, shot at frame 40),
  `test/golden/rig-swap.ppm.gz`

**Interfaces:**
- Consumes: Task 2.
- Produces:
  - `Strata.Render.refresh_rig(r : Render, a : Strata.Anim.Anim, id :
    Int) : Render` — uploads rig `id` anew (meshes, ray shapes,
    palette) under the same id, whether or not it was held.
  - `Strata.Render.forget_rig(r : Render, id : Int) : Render` — drops it
    from the held list and frees its mesh space.

- [ ] **Step 1: Add the scene and golden entry**; confirm it fails for
  lack of an image and every other scene passes.
- [ ] **Step 2: Implement.**
- [ ] **Step 3: Look at the image**: only the 4-part rig, new colour, no
  leftover faces of the old one. Store the golden.
- [ ] **Step 4: Run** `scripts/verify.sh`; PASS.
- [ ] **Step 5: Commit** `feat(render): refresh and forget rigs`.

---

## Part 2: strata_kit

Package `plugins/strata_kit/forge.toml` (`type = "lib"`, `strata = { path
= "../.." }`, `[ffi]` as `demo/forge.toml` plus `../../native/strata_anim.c`).
Flat modules in `plugins/strata_kit/lib/`, named `StrataKit.*`; tests in
`plugins/strata_kit/test/`. Coordinates inside a part are its own voxels,
y up, facing +z forward (the rig's convention after `Strata.Character`'s
axis change).

### Task 4: Package, parts and mirroring

**Files:**
- Create: `plugins/strata_kit/forge.toml`, `lib/strata_kit.march`,
  `lib/part.march`, `test/part_test.march`
- Modify: `scripts/verify.sh` (consumer loop gains `plugins/strata_kit`
  and `plugins/strata_kit/example`)

**Interfaces:**
- Produces (`StrataKit.Part`):
  - `type Marker = Marker(String, Float, Float, Float, (Float, Float,
    Float), (Float, Float, Float))` — name, position (part voxels),
    facing (unit), up (unit). Readers `marker_name`, `marker_at`,
    `marker_facing`, `marker_up`.
  - `type Mount = Mount(String, String, String, Marker)` — slot, size,
    name, where. Readers `mount_slot`, `mount_size`, `mount_name`,
    `mount_marker`; `mount_left(m) : Bool` (name ends `_l`).
  - `type Piece = Piece(String, String, Int, Int, Int, NativeU8Arr,
    (Float, Float, Float))` — name, parent piece name (`""` for the root),
    size, grid, joint (in the parent's voxels; the root's is its attach).
  - `type Part` — pieces (root first), attach marker, mounts, effect
    markers, slots (`List(String)`), size (`String`), side (`"r"`, `"l"`
    or `""` for symmetric), tags (`List((String, Float))`).
  - `from_grid(sx, sy, sz, grid, attach : Marker, mounts, fx, slots, size,
    side, tags) : Part` (one piece named `part`), and `with_piece(p, name,
    parent, sx, sy, sz, grid, joint) : Part`.
  - Readers: `pieces`, `attach`, `mounts`, `fx`, `slots`, `size`, `side`,
    `tag(p, key) : Float` (0 when absent), `mount(p, name) :
    Option(Mount)`.
  - `mirror(p : Part) : Part` — every piece's grid flipped in x (x →
    sx − 1 − x), every marker's position x → sx − x and facing/up x
    negated, joints mirrored within their parents, mount names `_l` ↔
    `_r`, side `r` ↔ `l`.

- [ ] **Step 1: Failing tests**:
  - `"from_grid keeps the grid, markers and tags"`.
  - `"tag of an absent key is 0"`.
  - `"mirror flips voxels in x"` — a voxel at (0, 1, 2) in a 4-wide part
    lands at (3, 1, 2).
  - `"mirror flips a marker's position and facing, keeps up"` — muzzle
    at (3.5, 2, 6) facing (0.6, 0, 0.8): mirrored at (0.5, 2, 6) facing
    (−0.6, 0, 0.8), up unchanged.
  - `"mirror renames mounts and side"` — `hand_r` → `hand_l`, side r → l.
  - `"mirror twice is the identity"`.
- [ ] **Step 2: Run** `cd plugins/strata_kit && forge test`; FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `forge test`, then `scripts/verify.sh`; PASS.
- [ ] **Step 5: Commit** `feat(kit): package, parts and mirroring`.

### Task 5: Parts from MagicaVoxel files

**Files:**
- Create: `plugins/strata_kit/lib/part_file.march`,
  `plugins/strata_kit/tools/make_fixtures.py` (reusing the writer
  functions of `scripts/characters.py`: copy them, do not import),
  `plugins/strata_kit/test/fixtures/*.vox` and `*.part` (generated,
  committed), `test/part_file_test.march`

**Interfaces:**
- Consumes: `Strata.Vox.read`, Task 4.
- Produces (`StrataKit.PartFile`):
  - `parse_sidecar(text : String) : Result(List((String, String)),
    String)` — `key = value` lines, `#` comments, blank lines; an error
    names the line.
  - `from_vox(v : Strata.Vox.Vox, sidecar : List((String, String)),
    palette_size : Int) : Result(Part, String)` — the convention of the
    design: model `part` (root, exactly one), `piece:<name>:<parent>`
    with `joint:<name>`, `attach` (exactly one), `mount:<slot>:<size>:<name>`,
    `fx:<name>`; facing and up suffix `@<axis>^<axis>` (default `@+y^+z`
    in file axes); axis change as `Strata.Character` (rig x = file −x,
    rig y = file z, rig z = file y); a model's low corner is its
    translation less floor(size / 2). Sidecar keys `slots`, `size`,
    `side`, every other key a numeric tag. Errors: no `part`, two
    `attach`, a piece whose parent is unknown or later, a marker name that
    does not parse, a voxel colour index ≥ `palette_size`, a non-numeric
    tag.
  - `load(path : String, palette_size : Int) : Result(Part, String)` —
    reads `<path>` and `<path minus .vox>.part`.
- Fixtures from `make_fixtures.py`: `arm_m.vox` (pieces `part` =
  shoulder, `piece:upper:part`, `piece:fore:upper`, mounts
  `mount:hand:L:hand_r@+z^+y`, `mount:forearm:L:forearm_r`), `gun_l.vox`
  (`fx:muzzle@+y^+z`), `legs_m.vox` (thigh/shin/foot pieces, both sides),
  `bad_two_attach.vox`, `bad_parent.vox`, and their `.part` files.

- [ ] **Step 1: Write the fixture script and generate the fixtures**;
  `python3 plugins/strata_kit/tools/make_fixtures.py` writes them.
- [ ] **Step 2: Failing tests**:
  - `"an arm loads with its pieces in order and its two mounts"`.
  - `"a marker's position is its model's centre in rig axes"` — the
    fixture's attach at file (2, 3, 0) size 1 → rig (−2.5, 0.5, 3.5) per
    the convention (compute the expected value in the test comment).
  - `"facing suffix @+z^+y converts to rig axes"` — file +z (up) is rig
    +y; file +y is rig +z.
  - `"sidecar slots, size, side and tags"` — `slots = hand shoulder`,
    `weight = 9`.
  - `"two attach markers are an error naming them"`; `"a piece with an
    unknown parent is an error"`; `"a colour past the palette is an
    error"`.
  - `"parse_sidecar reports the bad line"`.
- [ ] **Step 3: Run**; FAIL.
- [ ] **Step 4: Implement.**
- [ ] **Step 5: Run**; PASS.
- [ ] **Step 6: Commit** `feat(kit): parts from MagicaVoxel files`.

### Task 6: Builds: trees, fitting, totals, detaching

**Files:** Create `plugins/strata_kit/lib/build.march`, test
`test/build_test.march`.

**Interfaces:**
- Produces (`StrataKit.Build`):
  - `type Build = Node(Int, List((String, Build)))` — a part index (into
    the game's part list) and children by the parent's mount name.
    `node(part, children)`, `leaf(part)`, `part_of(b)`, `children(b)`.
  - `type Path = List(String)` — mount names from the root.
    `at(b, path) : Option(Build)`, `put(b, path, child : Option(Build)) :
    Build` (None empties the mount; a missing path changes nothing).
  - `fits(parts : List(Part), b : Build, ok : (String, String, List(String),
    String) -> Bool) : List((Path, String))` — every node checked against
    its parent's mount (slot, size) and its own (slots, size) through the
    game's `ok`; answers each misfit with a reason ("no mount hand_r on
    arm_m", "gun_h does not fit shoulder:L").
  - `sum(parts, b, key) : Float` — the tag summed over every node.
  - `count(b) : Int`; `paths(b) : List(Path)` (depth-first, mount order).
  - `detach(b, path) : (Build, Option(Build))` — the subtree removed and
    returned; the root cannot be detached (answers `(b, None)`).
- Steel Rain's `ok` is used in tests as the example: slot in the part's
  slots, and size rank (L 0, M 1, H 2) of the part ≤ the mount's.

- [ ] **Step 1: Failing tests**:
  - `"a correct build has no misfits"`.
  - `"a heavy gun on a light shoulder is a misfit with a reason"`;
    `"a light gun on a heavy shoulder fits"`; `"a leg part on the back
    mount is a misfit"`.
  - `"a child on a mount the parent lacks is a misfit"`.
  - `"sum adds weight over every node"` — 30 + 9 + 9 + 4 = 52.
  - `"detach takes the subtree"` — detaching the right arm returns it with
    its rifle; the build no longer sums them.
  - `"detaching the root changes nothing"`.
- [ ] **Step 2: Run**; FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run**; PASS.
- [ ] **Step 5: Commit** `feat(kit): builds, fitting, totals and detaching`.

### Task 7: Assembly into a rig

**Files:** Create `plugins/strata_kit/lib/assemble.march`, test
`test/assemble_test.march`.

**Interfaces:**
- Consumes: Tasks 4, 6; `Strata.Rig`.
- Produces (`StrataKit.Assemble`):
  - `type Settings = { as_voxel : Float, as_palette : List((Int, Float,
    Float, Float, Float, Float)), as_trim : List((Int, (Float, Float,
    Float))) }` — voxel size, the shared palette (index, r, g, b,
    roughness, emission), trim index → this figure's colour.
  - `type Map` — for each rig part: the build path, the piece name;
    for each path: the root rig part of that node. `rig_part(m, path,
    piece : String) : Option(Int)`, `path_of(m, rig_part) :
    Option(Path)`, `pieces_of(m, path) : List(Int)`.
  - `assemble(parts, b, ok, s) : (Strata.Rig.Rig, Map, List((Path,
    String)))` — depth-first: a node is skipped (with its subtree) and
    reported if it misfits; a part whose side differs from its mount's
    side (mount `_l`, part `r`) is mirrored first; each piece becomes a
    rig part (`Strata.Rig.part` then `with_grid`), its anchor at its
    joint (the root piece's anchor at the part's attach), pivoted at the
    parent mount's position and turned so the part's attach facing/up
    align with the mount's facing/up (a quaternion from the two frames;
    `Strata.Rig.rest`); `fx` markers become rig sockets (refused past
    32, reported); legs registered with `Strata.Rig.leg` for each
    `thigh_<s>`, `shin_<s>`, `foot_<s>` triple (sole = the foot piece's
    lowest voxel); palette and trim written into the rig. Past 64 rig
    parts, the remaining nodes are reported and skipped.
  - `markers(m, parts, b, name) : List((Path, Int))` — socket ids of fx
    markers named `name` (for `Strata.Anim.socket`).

- [ ] **Step 1: Failing tests** (placeholder parts built with
  `from_grid` in a test helper module `test/kit_parts.march`, module
  `StrataKit.Test.Parts`: `core()`, `arm_r()`, `rifle()`, `shield()`,
  `legs()`, `treads()`, `hover()`, `pod()`):
  - `"a core alone is one rig part"`.
  - `"the arm's attach lands on the core's shoulder mount"` — the rig
    part's pivot equals the mount's position; anchor equals the attach.
  - `"a part facing +z on a mount facing +x is turned a quarter about
    y"` — rest quaternion within 1e-5.
  - `"a right arm on the left mount is mirrored"` — its grid equals
    `mirror(arm_r)`'s.
  - `"legs register two rig legs"`.
  - `"muzzle markers become sockets and markers finds them"`.
  - `"a misfit node and its subtree are skipped and reported, the rest
    assembled"`.
  - `"past 64 parts the rest is reported, not silently dropped"`.
  - `"trim colours replace the palette entries at trim indices"`.
  - Budget: assembling the 25-part test build under 5 ms (asserted at
    20 ms), reported.
- [ ] **Step 2: Run**; FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run**; PASS.
- [ ] **Step 5: Commit** `feat(kit): assemble a build into a rig`.

### Task 8: Walking, treads and hover

**Files:** Create `plugins/strata_kit/lib/motion.march`, test
`test/motion_test.march`.

**Interfaces:**
- Consumes: Task 7's `Map`, `Strata.Rig.legs`, `Strata.Rig.reach_length`,
  `Strata.Anim.reach`, `Strata.Anim.override`, `Strata.Anim.ground`.
- Produces (`StrataKit.Motion`):
  - `type Gait` — per leg: phase, planted point (metres), swinging or
    not; body bob. `gait(rig : Strata.Rig.Rig) : Gait` reads leg lengths.
  - `gait_step(g, dt, pos : (Float, Float, Float), heading : Float, speed
    : Float, turn : Float) : (Gait, List((Float, Float, Float)), Float)` —
    the feet targets in metres and the root's height offset. Stride =
    0.8 × leg length, lift = 0.25 × leg length, cadence from speed
    (stride per step), legs opposite in phase; at speed 0 the feet settle
    under the hips within 0.5 s and stay planted; turning in place steps
    feet round the hips.
  - `gait_apply(a, f, rig, g_targets, bob) : Strata.Anim.Anim` — a
    `reach` per leg (pole ahead of the knee), the root override for bob;
    then the game calls `Strata.Anim.ground`.
  - `wheels(m : Map) : List(Int)` — rig parts of pieces named `wheel*`;
    `treads_apply(a, f, m, distance : Float, radius : Float, turn :
    Float) : Strata.Anim.Anim` — wheels turned about x by distance /
    radius, root rolled by `0.02 × turn` radians.
  - `hover_apply(a, f, m, t : Float) : Strata.Anim.Anim` — root offset
    0.15 m × sin(2π t / 2 s), pitch 0.02 rad × sin(…); thruster positions
    come from `Assemble.markers(…, "thrust")`.
  - `aim_arm(a, f, m, path, target : (Float, Float, Float), muzzle_axis :
    (Float, Float, Float), slot : Int) : Strata.Anim.Anim` — the arm's
    pieces from shoulder to the held part as the chain of
    `Strata.Anim.aim`, limit 1.2 rad.

- [ ] **Step 1: Failing tests** (pure `gait_step`):
  - `"standing still keeps both feet planted"`.
  - `"walking forward, feet alternate and each step is one stride"`.
  - `"no foot target is more than leg length from its hip"`.
  - `"speed 0 after walking settles the feet under the hips within half
    a second"`.
  - `"turning in place moves the feet round the hips"`.
  - `"wheels finds the wheel pieces"`; `"hover's offset is within 0.15
    m and periodic in 2 s"` (compute from the pure helper the apply
    functions use: `hover_offset(t) : (Float, Float)`).
- [ ] **Step 2: Run**; FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run**; PASS. (The applied motion is checked by eye in Task
  12.)
- [ ] **Step 5: Commit** `feat(kit): walking, treads, hover and aiming`.

### Task 9: Carving

**Files:** Create `plugins/strata_kit/lib/damage.march`, test
`test/damage_test.march`.

**Interfaces:**
- Consumes: `Strata.Rig.grid`, `with_grid`, `Strata.Anim.FigureHit`.
- Produces (`StrataKit.Damage`):
  - `type Carve = { cv_interior : Int, cv_keep : Float }` — the interior
    palette index, and the radius (voxels) around a piece's anchor that is
    never removed (default 2.0). `carve_settings(interior) : Carve`.
  - `carve(rig, part : Int, x : Int, y : Int, z : Int, radius : Float,
    s : Carve) : (Strata.Rig.Rig, Int)` — voxels within `radius` of the
    hit voxel's centre removed, except within `cv_keep` of the part's
    anchor; every remaining voxel face-adjacent to a removed one takes
    `cv_interior`; answers the rig and the count removed.
  - `carve_hit(rig, h : Strata.Anim.FigureHit, radius, s)` — the same from
    a ray hit.

- [ ] **Step 1: Failing tests**:
  - `"a carve removes a ball of voxels and counts them"` — solid 9³ part,
    radius 2 at the centre of a face: count equals the hand count in a
    comment.
  - `"new faces take the interior colour"`.
  - `"the anchor region is never removed"`.
  - `"carving everything leaves a pure joint"` — `count` voxels removed
    all but the keep ball; carving the keep ball too still leaves it.
  - Budget: carve + `meshes` of a 40×60×30 part under 16 ms (asserted at
    64 ms), reported.
- [ ] **Step 2: Run**; FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run**; PASS.
- [ ] **Step 5: Commit** `feat(kit): carving damage`.

### Task 10: Breaking off and loose pieces

**Files:** Modify `plugins/strata_kit/lib/damage.march`; test
`test/damage_test.march`.

**Interfaces:**
- Consumes: Tasks 6, 7, 9; `Strata.Body`, `Strata.Fracture`,
  `Strata.Anim.matrices` or `origin` for world poses.
- Produces:
  - `type Loose = Loose(Strata.Body.Grid, (Float, Float, Float),
    (Float, Float, Float, Float))` — a grid of block ids, its low corner
    in metres, its rotation.
  - `debris(index_to_block : Int -> Int)` is passed by the game: palette
    index → block id (0 for empty).
  - `break_off(parts, b, m, rig, pose : Int -> (Float, Float, Float,
    (Float, Float, Float, Float)), path, debris, shatter : Option((Int,
    Int, Int, Float, Strata.Blocks.Registry))) : (Build, List(Loose))` —
    detaches `path` (Task 6), turns each rig part of the removed subtree
    (current carved grids, at `pose(rig part)`, which the game reads from
    `Strata.Anim`) into a `Loose`; with `shatter = Some((x, y, z, excess,
    registry))` the struck piece is cut with `Strata.Fracture.pieces`
    round (x, y, z) first. The game re-assembles the build and replaces
    the rig.
  - Loose grids are at the part's voxel size; the game adds them with
    `Strata.Physics.add_fine(p, g, 4, …)` (0.1 m in a 0.4 m world).

- [ ] **Step 1: Failing tests**:
  - `"breaking off the right arm returns it and its rifle as loose
    grids"` — two `Loose` (or more pieces), block ids from `debris`.
  - `"the remaining build no longer has the arm"`.
  - `"a carved arm breaks off carved"` — removed voxels absent in the
    loose grid.
  - `"shatter splits the struck piece into several grids whose voxels sum
    to the original"`.
  - `"breaking off the root changes nothing"`.
- [ ] **Step 2: Run**; FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run**; PASS.
- [ ] **Step 5: Commit** `feat(kit): parts break off as loose pieces`.

### Task 11: Settling into rubble

**Files:** Modify `plugins/strata_kit/lib/damage.march`; test
`test/settle_test.march`.

**Interfaces:**
- Consumes: `Strata.Plugin.Request` (plugins plan Task 6),
  `Strata.Physics.is_asleep`, `pose`, `remove`.
- Produces:
  - `settle(w : Strata.World.World, g : Strata.Body.Grid, corner : (Float,
    Float, Float), q : (Float, Float, Float, Float), k : Int, occupied :
    (Int, Int, Int) -> Bool) : List(Strata.Plugin.Request)` — the fine grid resampled to the world's
    voxels (k³ fine voxels per world voxel; a world cell is filled when
    at least `k³ / 2` of its fine voxels are solid, taking the most common
    block id among them), and one `ReqEdit(op_cell(…))` per filled cell
    that is inside a loaded chunk and currently air (or fluid). Cells
    that are solid, unloaded, or overlap a figure's box (the game passes
    `occupied`) are dropped.
  - The game, each frame, settles every fine kit body that `is_asleep`,
    applies the requests through `Strata.Plugin.apply`, removes the body,
    and passes the outcomes to the tactics grid (`world_changed`).

- [ ] **Step 1: Failing tests**:
  - `"a resting 8x8x8 fine cube becomes 2x2x2 world blocks"` (k = 4).
  - `"a thin plate under half a cell fills nothing there"`.
  - `"cells already solid are not overwritten"`.
  - `"cells a figure occupies are dropped"`.
  - `"cells outside loaded chunks are dropped"`.
  - `"the most common block wins a cell"`.
- [ ] **Step 2: Run**; FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run**; PASS.
- [ ] **Step 5: Commit** `feat(kit): loose pieces settle into rubble`.

### Task 12: The turntable example

**Files:**
- Create: `plugins/strata_kit/example/forge.toml` (app; deps `strata`
  `../../..`, `strata_kit` `..`; `[ffi]` as `viewer/forge.toml` with
  paths one deeper), `example/lib/kit_example.march`, `example/run.sh`
- Create: `docs/kit-example.png`; modify `docs/kit-ledger.md`

**What it does:** a flat 0.4 m world, a lit stage. A placeholder mech
from `StrataKit.Test.Parts`-like parts (copied into the example) on a
light core: two arms, rifle in the right hand, shield in the left, a back
pod, and legs. Keys: `1`–`6` select a mount, `[` `]` cycle the parts that
fit it (assemble, `replace_rig`, `refresh_rig` each time; the model
changes at once); `M` cycles the movement piece (legs, treads, hover)
and the mech walks / rolls / hovers in a circle; the mouse turns the
camera round the stage; left click carves where the ray hits the mech
(`hit_ray` → `carve_hit`); `X` breaks off the right arm with a shatter;
loose pieces fall, sleep and settle into world blocks. The weight and
power totals (`sum`) and any misfits show in a corner label.

Scripted mode `KIT_FRAMES=n KIT_SHOT=path`: a fixed sequence (assemble,
swap the right hand to a second gun, two carves, break off the arm, wait
for it to settle) then the frame as a PPM.

- [ ] **Step 1: Build** `cd plugins/strata_kit/example && forge build
  --release`; success.
- [ ] **Step 2: Run scripted**, convert to PNG, look: the swapped gun,
  carved holes with the interior colour, the arm gone from the mech and
  lying as blocks on the ground. Store `docs/kit-example.png`.
- [ ] **Step 3: Play it** for a minute: swap every mount, all three
  movement pieces, carve until a part is bare, break things off. Record
  timings of swap and carve against the 16 ms budget in the ledger and
  fix anything off.
- [ ] **Step 4: Run** `scripts/verify.sh`; PASS.
- [ ] **Step 5: Commit** `feat(kit): a turntable example`.

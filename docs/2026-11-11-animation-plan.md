# Strata Animation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Jointed voxel figures (people, creatures, vehicles) built and animated in code, whose motion reacts to the world: layered clips with crossfades, overrides, aim, reach, springs, foot placement, attachments, marks and voxel-level hit tests, for hundreds of figures at once, drawn and optionally traced on the physics project's moving-object path.

**Architecture:** March builds rigs and clips (`Strata.Rig`, `Strata.Clip`), meshes part grids once, does foot placement against the world, and gives the game `Strata.Anim`. C (`native/strata_anim.h`, logic; `native/strata_anim.c`, the extern wrapper) owns the figure pool and poses every figure each update: sample, blend, procedural stage, hierarchy, world matrices; it also answers hit tests. The renderer reads part matrices from `strata_anim` in C, through a weak symbol, and draws them on the slot-with-pose path the physics plan builds.

**Tech Stack:** March (origin/main via `scripts/env.sh`), C (no Vulkan in the animation files), the Vulkan shim `native/strata_vk.c`, GLSL built by `native/build_shaders.sh`.

**Spec:** `docs/2026-11-11-animation-design.md`

## Global Constraints

- Branch `claude/strata-animation-support-310347`. Before Task 1, merge `main` into it (it is behind `main`). Commit after each task. After each task's review is clean and the gate passes, merge the branch into `main` through a temporary worktree (`git worktree add <scratch> main`, `git merge --no-ff`, gate, `git worktree remove`). Never push.
- **Tasks 6 to 9 start only when physics plan Tasks 5 and 6 are merged into `main`** (`git log main --oneline | grep "feat(physics): shadows, sky and reflection rays see bodies"`). Until then stop after Task 5 and report. Merge `main` in before Task 6 and build on what landed; where it differs from the physics plan's interfaces quoted below, follow what landed and record it in the ledger.
- `scripts/verify.sh` prints PASS at the end of every task. Every scripted viewer run uses `STRATA_HIDDEN=1 STRATA_OFFSCREEN=1 STRATA_PIXELS=1` and `STRATA_AUDIO=0`.
- GAPS S1 to S20 apply (read `GAPS.md` on `main`): no `opaque`, `by`, `on`, `loop` as names; `needs IO.Clock` and `needs IO.Process` in every test module; aliases unique across the whole suite (use `Rg`, `Cl`, `An`, `Pm`, `Fan` for the new modules); externs wrapped in ordinary functions; extern March names unique across modules (S17: prefix them `xn_`); full module paths for Unit calls; no extern result bound to `_`; stamp line for any header `strata_vk.c` or `strata_anim.c` includes that changes (S14).
- C prefix `an_` for everything in `strata_anim.h`/`strata_anim.c` (audio owns `sa_`/`sm_`). The animation C files include nothing from Vulkan or GLFW.
- Fixed values: figures 4 096; layers 8 per figure; parts 64 per rig; part grid at most 128 voxels a side; blend 4 clips per layer; aim chain at most 4 parts; attachment depth 4; traced figures 64; palette 255 entries plus 0 empty. Stale culled figures pose at most 1 update in 4; far figures every other update with springs frozen.
- Determinism: the same calls with the same `dt` give an identical `an_hash` on the same build and machine. Not promised across machines. The camera never changes marks, sockets or hit results; sockets and hits use the pose before springs.
- Time is the `dt` passed, never the wall clock.
- Rulings made while building go in `docs/animation-ledger.md`, in the form `docs/fluid-ledger.md` uses.
- Timings are trusted only when `uptime` shows a load average under 8; otherwise recorded as not judged.
- Golden images are looked at before they are stored. Existing goldens pass unchanged unless a task says otherwise.

## Review Focus

1. **Degenerate rigs and clips:** a rig of one empty part, a clip with no keys, a key at exactly the duration, two keys at the same time, a zero-length clip. Poses stay finite (no NaN in any matrix) and the clip holds its rest pose. Tests: Task 1 ("degenerate clips"), Task 2 ("degenerate rigs").
2. **Large and odd `dt`:** `dt` of 0, negative, or several clip lengths at once. Zero changes nothing, negative is treated as 0 and counted, a long `dt` fires each mark crossed once per crossing in order. Test: Task 1 ("dt edge cases").
3. **Removed and reused figures:** commands for a removed figure, a figure attached to one being removed, a socket or hit query on a removed figure. Ignored or answered as a miss, never a crash or a stale pose. Tests: Task 1 ("removed figure"), Task 4 ("attachment to a removed figure").
4. **Unreachable or degenerate targets:** a reach target at the root or beyond reach, an aim target at the aiming part's own pivot, a pole on the chain's line. The chain straightens or holds, with no NaN and no flip between consecutive updates. Test: Task 3 ("degenerate targets").
5. **Foot placement at the edge of the world:** ground in an unloaded chunk, no ground within reach, a step taller than the leg. Legs fade back to the clip; the root never drops more than the leg can reach. Test: Task 3 ("ground at the edges").

---

### Task 1: The pose kernel, headless

**Files:**
- Create: `native/strata_anim.h`, `native/tests/anim_test.c`

**Interfaces — Produces** (C, all in `strata_anim.h`, static functions; state only inside `AnWorld`):
```c
#define AN_FIGURES 4096
#define AN_LAYERS 8
#define AN_PARTS 64
#define AN_BLEND 4
typedef struct AnWorld AnWorld;           /* every array allocated in an_open */
AnWorld *an_open(int capacity);  void an_close(AnWorld *);
/* rigs: parts in parent-before-child order; parent -1 for the root */
int  an_rig(AnWorld *, int nparts, const int32_t *parent, const float *pivot /*3n, metres in parent space*/,
            const float *rest_q /*4n xyzw*/, const float *box /*6n: min,max in part space, metres*/);   /* id, or -1 (stats 5 says why) */
int  an_mask(AnWorld *, int rig, int nroots, const int32_t *roots);   /* subtree roots; id or -1 */
/* clips: key ints are (part, channel 0 turn / 1 move, interp 0 step / 1 linear / 2 smooth); key floats (time, x, y, z, w) */
int  an_clip(AnWorld *, int rig, float duration, int loops, float stride,
             int nkeys, const int32_t *key_ints, const float *key_floats,
             int nmarks, const float *mark_t, const int32_t *mark_tag);   /* id or -1 */
int  an_spawn(AnWorld *, int rig);  void an_remove(AnWorld *, int fig);   /* fig ids reused only after 2^20 spawns */
void an_place(AnWorld *, int fig, const float pos[3], const float q[4]);
void an_play(AnWorld *, int fig, int layer, int clip, float fade, float rate, float weight, int mask, int additive);
void an_blend(AnWorld *, int fig, int layer, int n, const int32_t *clips, const float *positions,
              float value, float fade, float weight, int mask);
void an_set(AnWorld *, int fig, int layer, int what /*0 rate,1 weight,2 time,3 blend value,4 paused*/, float v, float fade);
void an_update(AnWorld *, float dt);
const float *an_matrices(const AnWorld *, int fig);   /* 12 floats a part (3x4, column-major), world space; NULL if no such figure */
int  an_marks(const AnWorld *, int max, int32_t *ints /*fig,layer,tag*/, float *weights);   /* since the last update, in time order */
uint64_t an_hash(const AnWorld *);                     /* FNV-1a over every live figure's matrices and spring state */
double an_stat(const AnWorld *, int which);            /* 0 figures, 1 posed, 2 stale, 3 refused calls, 4 negative dt, 5 last refusal reason */
```
A layer's pose is per part (rotation quaternion, movement offset). Override layers lerp/slerp toward their pose by weight; additive layers multiply their rotation relative to the clip's first key on top. A crossfade keeps the outgoing clip running and ramps it out over `fade` while the new one ramps in. A blend layer picks the two neighbours of `value` along `positions` and keeps their phase equal (normalised time). Marks fire when the clip time crosses them, including on wrap, at zero weight too.

- [ ] **Step 1: failing tests** in `anim_test.c` (`CHECK` style of `spray_test.c`, prints `anim_test: ok`), on a 3-part chain rig (root, arm on the root at (0,1,0), hand on the arm at (0,1,0)):
  - sampling: a linear turn key 0 deg at t=0 and 90 deg about z at t=1; at 0.5 the arm's matrix is a 45-degree turn within 1e-5; step interp holds 0 until 1; smooth at 0.25 is below the linear value;
  - slerp at antipodal keys (q and -q) takes the short way and stays unit length;
  - hierarchy: turning the arm 90 deg moves the hand's origin to (-1, 1, 0) from the root;
  - masks: layer 1 with mask {hand} moves only the hand; additive layer adds its turn on top of layer 0;
  - crossfade continuity: switching clips with fade 0.2 at 60 updates/s, no matrix element changes by more than 0.1 between consecutive updates;
  - blend: walk (stride 1) and run (stride 2) at value 0.5 stay in phase: both clips' normalised times equal after 100 updates;
  - **dt edge cases** (Review Focus 2): `dt` 0 changes no matrix; `dt` -1 changes nothing and stat 4 is 1; a looping clip of 1 s with marks at 0.25 and 0.75 updated by `dt` 2.6 reports the marks 0.25, 0.75, 0.25, 0.75, 0.25 in that order; a mark on a weight-0 layer is reported with weight 0;
  - **degenerate clips** (Review Focus 1): no keys, a key at exactly the duration, two keys at the same time, duration 0: every matrix finite, rest pose held where there are no keys;
  - **removed figure** (Review Focus 3): every call on a removed figure is ignored and counted in stat 3; `an_matrices` answers NULL;
  - refusals: a 65-part rig, a clip for a rig that does not exist, layer 8: refused, stat 5 names the reason;
  - determinism: a scripted 600-update run with 50 figures twice in fresh worlds gives the same `an_hash`.
- [ ] **Step 2:** run `STRATA_NO_GPU=1 scripts/verify.sh`; expect FAIL (`strata_anim.h` missing).
- [ ] **Step 3:** implement `strata_anim.h`. Parts are stored per rig in parent-first order so the hierarchy is one forward pass.
- [ ] **Step 4:** run; expect PASS.
- [ ] **Step 5:** create `docs/animation-ledger.md` with the header and the representation choices made. Commit `feat(anim): the pose kernel, headless — rigs, clips, layers, blends, crossfades, marks`.

### Task 2: Rigs, clips and figures from March

**Files:**
- Create: `native/strata_anim.c`, `lib/strata/ffi/anim.march`, `lib/strata/rig.march`, `lib/strata/clip.march`, `lib/strata/anim.march`, `test/rig_test.march`, `test/clip_test.march`, `animcheck/forge.toml`, `animcheck/lib/animcheck.march`, `scripts/anim_check.sh`
- Modify: `scripts/verify.sh` (add `animcheck` to the app loop; run `scripts/anim_check.sh` after the C tests), `README.md` (an "Animation" section; the consumer `[ffi]` block gains `native/strata_anim.c`)

**Interfaces — Produces:**
```
C (strata_anim.c): one global AnWorld; every an_ call above wrapped as int64_t/double/void with native arrays,
  plus int64_t an_x_matrices(int64_t fig, void *out_f32) (copies, answers parts) for March.

Strata.Ffi.Anim (needs Anim; extern "strata_anim" : Cap(Anim)) — one wrapper per C call; names xn_*.

Strata.Rig (pure):
  type Rig;  new(voxel_size : Float) : Rig
  colour(r, index : Int, red, green, blue, roughness, emission : Float) : Rig      -- index 1..255
  part(r, parent : Int, px, py, pz : Float, sx, sy, sz : Int) : (Rig, Int)          -- parent -1 for the root; pivot in the parent's voxels
  voxel(r, part, x, y, z, colour : Int) : Rig;  box(r, part, x0, y0, z0, x1, y1, z1, colour : Int) : Rig
  rest(r, part, q : Strata.Math.Quat.Quat) : Rig
  socket(r, part, x, y, z : Float, q : Strata.Math.Quat.Quat) : (Rig, Int)
  mask(r, roots : List(Int)) : (Rig, Int)
  leg(r, upper, lower, foot : Int, sole : Float) : Rig
  meshes(r) : List((Int, Int, Int, Int, NativeI32Arr))   -- (part, ox, oy, oz, quads): each part cut into 32-cubed pieces, quads in Strata.Quad layout with the palette index in the texture field
  refused(r) : Int;  why(r) : String
Strata.Clip (pure):
  type Clip;  new(rig : Strata.Rig.Rig, duration : Float, loops : Bool) : Clip
  turn(c, part, time, q : Strata.Math.Quat.Quat, interp : Int) : Clip;  move(c, part, time, x, y, z, interp) : Clip
  swing(c, part, ax, ay, az, degrees, period, phase : Float) : Clip     -- writes keys every 1/16 of the period, smooth
  spin(c, part, ax, ay, az, turns_per_second : Float) : Clip            -- writes keys every quarter turn, linear
  mark(c, time, tag : Int) : Clip;  stride(c, metres : Float) : Clip;  refused(c) : Int
Strata.Anim (needs Anim):
  type Anim;  open(capacity : Int) : Anim
  finish_rig(a, r : Strata.Rig.Rig) : (Anim, Int)  -- id; keeps the rig's legs and sockets March-side
  finish_clip(a, c : Strata.Clip.Clip) : (Anim, Int)
  spawn(a, rig, x, y, z, q) : (Anim, Int);  remove(a, f) : Anim;  place(a, f, x, y, z, q) : Anim
  play(a, f, layer, clip, fade, rate, weight, mask : Int, additive : Bool) : Anim
  blend(a, f, layer, clips : List(Int), positions : List(Float), value, fade, weight, mask) : Anim
  set_rate / set_weight / set_time / set_value (a, f, layer, v, fade) : Anim
  update(a, dt : Float) : Anim;  marks(a) : List((Int, Int, Int, Float));  matrices(a, f) : NativeF32Arr
  hash(a) : Int;  stats(a) : (Int, Int, Int, Int)     -- figures, posed, stale, refused
animcheck: ANIMCHECK_SCENE=name ./animcheck   prints "scene name: ok" or the failure and exits 1
```
The part mesher is greedy, per face direction, merging equal palette indices, with corner shading computed as `Strata.Mesh` does; a piece's neighbours are the part's other pieces, air beyond the part. `Strata.Model` already names block models; do not reuse it.

- [ ] **Step 1: probe** whether `forge test` in `animcheck/` links its `[ffi]` sources; record the answer in the ledger. Either way, C-backed checks run as `animcheck` scenes through `scripts/anim_check.sh`, and `test/*_test.march` test only pure modules.
- [ ] **Step 2: failing tests** `test/rig_test.march`: a single filled voxel gives 6 quads; a 2x1x1 bar of one colour gives 6 quads, of two colours 10; random grids (Check.all, 8 cubed, 3 colours): quad area summed per face equals the brute-force count of exposed faces; a 40x10x10 part gives pieces at offsets (0,0,0) and (32,0,0) and no face on the shared side; **degenerate rigs** (Review Focus 1): a rig of one empty part meshes to nothing and is accepted; a 65th part, a 129-voxel side, colour 0 or 256 in `colour` are refused and counted. `test/clip_test.march`: keys out of order are sorted; a key past the duration is refused; `swing` of 30 degrees over 1 s writes 17 keys, the one at 0.25 s a 30-degree turn; `spin` at 1 turn/s for a 2 s clip writes 9 keys.
- [ ] **Step 3:** run `forge test`; expect FAIL. Implement `Strata.Rig` and `Strata.Clip`; expect PASS.
- [ ] **Step 4: animcheck scenes**, each failing first: `chain` (the Task 1 chain rig built through `Strata.Rig`, the arm's matrix after a quarter of a turn clip matches the expected values within 1e-4); `determinism` (200 figures, 4 clips, 1 200 updates at 1/60, run twice in one process through `open`/`close`, equal hash); `marks` (a walk with marks 1 and 2 reports them alternately over 3 loops).
- [ ] **Step 5:** implement `strata_anim.c`, `Strata.Ffi.Anim`, `Strata.Anim`, `animcheck`, `scripts/anim_check.sh` (builds animcheck, runs every scene, exits non-zero on the first failure). Run `scripts/verify.sh`; expect PASS.
- [ ] **Step 6:** **See it:** `ANIMCHECK_SCENE=table ./animcheck` prints the chain's three matrices at t = 0, 0.25, 0.5. Commit `feat(anim): Strata.Rig, Strata.Clip, Strata.Anim, the part mesher, animcheck`.

### Task 3: The procedural stage and foot placement

**Files:** `native/strata_anim.h`, `native/strata_anim.c`, `native/tests/anim_test.c`, `lib/strata/ffi/anim.march`, `lib/strata/anim.march`, `animcheck/lib/animcheck.march`, `test/anim_ground_test.march` (create)

**Interfaces — Produces:**
```c
void an_override(AnWorld *, int fig, int part, const float q[4], const float move[3], float weight);   /* weight 0 clears */
void an_aim(AnWorld *, int fig, int slot /*0..3*/, int n, const int32_t *chain, const float axis[3], const float target[3], float limit_deg, float weight);
void an_reach(AnWorld *, int fig, int slot /*0..7*/, const int32_t chain[3], const float target[3], const float pole[3], float weight);
void an_spring(AnWorld *, int fig, int part, float stiffness, float damping);   /* stiffness 0 clears */
void an_drop(AnWorld *, int fig, float metres);   /* lowers the root for foot placement, after layers */
int  an_part_origin(const AnWorld *, int fig, int part, float out[3]);   /* last update's, pre-spring */
```
```
Strata.Anim: override(a, f, part, q, x, y, z, weight) / aim(a, f, slot, chain : List(Int), ax, ay, az, tx, ty, tz, limit, weight)
             / reach(a, f, slot, upper, lower, end, tx, ty, tz, px, py, pz, weight) / spring(a, f, part, stiffness, damping) : Anim
             ground(a, f, w : Strata.World.World, weight : Float) : Anim
```
Order inside `an_update`: layers, overrides, aim, reach (feet), springs, hierarchy. Aim shares the turn along the chain in equal parts, each clamped to `limit_deg`. Reach is analytic two-bone IK in the plane of root, target and pole. Springs are a damped spring on the part's rotation in its parent's frame, integrated semi-implicitly at the update's `dt`, sub-stepped at 1/120 s. `ground` (March): for each of the rig's legs, cast `Strata.Query.raycast` straight down from 0.5 m above the foot's last origin to the leg's full length below it; on a hit, reach target = hit top plus sole, pole = 1 m ahead of the knee in the figure's facing; drop = the largest (animated foot height - target), at most the leg's length; with no hit the leg's reach weight fades to 0 over 0.15 s.

- [ ] **Step 1: failing C tests:** an override at weight 1 replaces the clip's rotation exactly and at 0.5 halves it; aim at a target 45 degrees off with limit 30 per part over a 2-part chain turns 22.5 each, with limit 10 stops at 20; reach places the end within 1e-4 of a reachable target with the knee on the pole's side; **degenerate targets** (Review Focus 4): a target beyond reach straightens the chain toward it, a target at the root and a pole on the chain line give finite matrices and no flip (the knee side does not change between 100 consecutive updates with a target moving 1 mm a step); a spring part on a figure turned 90 degrees in one update overshoots and settles within 2 s at stiffness 60 damping 8; springs are in the hash and repeat.
- [ ] **Step 2:** implement in `strata_anim.h`; run `verify.sh` C tests; PASS.
- [ ] **Step 3: failing March tests** in `test/anim_ground_test.march`, against the pure part of `ground` (`Strata.Anim.ground_targets(r : Strata.Rig.Rig, feet : List((Float, Float, Float)), w : Strata.World.World) : (List(Option((Float, Float, Float))), Float)`: per leg, in the rig's leg order, its foot's last origin in; per leg a target or none, and the drop, out): flat ground gives both feet at ground plus sole and drop 0; a 1-voxel step under one foot raises that target by one voxel and the drop follows the lower foot; **ground at the edges** (Review Focus 5): ground in an unloaded chunk and no ground within reach give none; a step taller than the leg gives a drop of at most the leg's length. At voxel scale 0.1 and 1.0 the drop in metres for a 0.3 m step is the same.
- [ ] **Step 4:** implement `ground_targets` and `ground`; PASS. animcheck scene `stairs`: a 2-leg rig walking in place on stairs 0.25 m high, both soles within 0.02 m of their steps after 30 updates.
- [ ] **Step 5:** `scripts/verify.sh` PASS. Commit `feat(anim): overrides, aim, reach, springs, foot placement`.

### Task 4: Attachments, sockets, hit tests

**Files:** `native/strata_anim.h`, `native/strata_anim.c`, `native/tests/anim_test.c`, `lib/strata/ffi/anim.march`, `lib/strata/anim.march`, `animcheck/lib/animcheck.march`

**Interfaces — Produces:**
```c
int  an_socket_def(AnWorld *, int rig, int part, const float pos[3], const float q[4]);   /* socket id within the rig */
int  an_attach(AnWorld *, int child, int parent, int socket, const float pos[3], const float q[4]);  /* 0 refused: depth > 4 or a cycle */
void an_detach(AnWorld *, int child);
int  an_socket(AnWorld *, int fig, int socket, float out_pos[3], float out_q[4]);   /* poses a stale figure first; 0 if no such figure */
int  an_shape(AnWorld *, int rig, int part, int sx, int sy, int sz, float voxel, const uint8_t *palette_idx);  /* the part's grid, for hits */
typedef struct { int32_t fig, part, x, y, z, colour; float dist, nx, ny, nz; } AnHit;
int  an_hit_ray(AnWorld *, const float o[3], const float d[3], float max, int ignore, AnHit *out);   /* 1 hit, 0 miss */
int  an_hit_box(AnWorld *, const float c[3], const float half[3], const float q[4], int ignore, int max, int32_t *figs_parts);  /* pairs */
```
```
Strata.Anim: attach(a, child, parent, socket, x, y, z, q) / detach(a, child) : Anim
             socket(a, f, s) : Option((Float, Float, Float, Strata.Math.Quat.Quat))
             hit_ray(a, ox, oy, oz, dx, dy, dz, max, ignore) : Option(Hit)   -- Hit has fig, part, x, y, z, colour, dist, normal
             hit_box(a, cx, cy, cz, hx, hy, hz, q, ignore) : List((Int, Int))
```
`finish_rig` uploads every part's grid with `an_shape`. Children are posed after parents in the same update (sort attachments by depth). `ignore` skips that figure and everything attached to it. Hits use a coarse uniform grid of figure bounds (cell 4 m) rebuilt in `an_update`; a ray tests part boxes in part space then steps the part's grid with a 3D DDA.

- [ ] **Step 1: failing C tests:** a child on a hand socket follows the hand through a 90-degree turn; depth 5 and a cycle are refused; **attachment to a removed figure** (Review Focus 3): removing the parent detaches the child, which keeps its last world placement; socket on a removed figure answers 0; hit ray against 50 randomly posed figures equals a brute-force test of every voxel of every part (figure, part, voxel, distance within 1e-4) for 1 000 random rays; `ignore` skips the shooter and what it carries; hit box returns exactly the pairs whose part boxes overlap the query (brute force on 200 random boxes); a stale culled figure (Task 5 adds culling; here, an un-updated pose) is posed before a socket or hit answers.
- [ ] **Step 2:** implement; PASS. animcheck scene `hits`: a humanoid-shaped rig, a ray through the head reports the head part and the voxel's colour.
- [ ] **Step 3:** `scripts/verify.sh` PASS. Commit `feat(anim): attachments, sockets, ray and box hit tests`.

### Task 5: Cost control and two poses, measured headless

**Files:** `native/strata_anim.h`, `native/strata_anim.c`, `native/tests/anim_test.c`, `lib/strata/ffi/anim.march`, `lib/strata/anim.march`, `animcheck/lib/animcheck.march`, `RESULTS.md`, `docs/animation-ledger.md`

**Interfaces — Produces:**
```c
void an_view(AnWorld *, const float planes[24], const float eye[3], float far_metres);  /* 6 planes ax+by+cz+d >= 0 inside; none set = all visible */
void an_always(AnWorld *, int fig, int on);
void an_traced(AnWorld *, int fig, int on);          /* flag only; drawn from Task 8; past 64 counted in stat 6 */
const float *an_draw_matrices(AnWorld *, int fig, float blend);   /* between the last two updates' poses */
/* the renderer's source, read from C by strata_vk.c (declared weak there): */
int an_draw_list(AnWorld *, float blend, const int32_t **rig_part, const float **xf, const int32_t **flags);  /* visible parts this frame */
```
```
Strata.Anim: view(a, planes : NativeF32Arr, ex, ey, ez, far) / always(a, f, on) / traced(a, f, on) : Anim
```
A figure outside the view planes (with a 2 m margin) and not `always` advances clip time and marks every update but poses at most 1 update in 4; beyond `far_metres` it poses every other update with springs frozen. Each figure keeps its last two posed matrix sets; `an_draw_matrices` interpolates (slerp of the decomposed rotation, lerp of the translation) at `blend`.

- [ ] **Step 1: failing C tests:** marks of a culled figure fire on the same updates as a visible one's; socket and hit results for a culled figure equal those of the same figure with no view set (the camera never changes an answer); a far figure's springs do not advance; `an_draw_matrices` at blend 0 and 1 equal the two poses, at 0.5 lies between; the 65th traced figure is counted and drawn plain.
- [ ] **Step 2:** implement; PASS.
- [ ] **Step 3: measure:** animcheck scene `arena` (512 figures of 15 parts, 3 layers, an aim chain, 2 legs with reach targets moving, 1/60 updates for 10 s, half outside a view frustum) prints the mean and worst `an_update` time; scene `ground128` prints `Strata.Anim.ground` for 128 figures over a hilly world; scene `hits` prints the mean hit ray time over 10 000 rays. Record in `RESULTS.md` against the targets: update 1.0 ms, ground 0.5 ms, hit ray 10 µs; met or missed with the measured cause. A miss is reported, not fixed past the design's fallbacks.
- [ ] **Step 4:** `scripts/verify.sh` PASS. Commit `feat(anim): culling, stale poses, two poses kept, the arena measured headless`.

**Stop here until physics Tasks 5 and 6 are on `main`** (see Global Constraints).

### Task 6: Figures drawn on the moving-object path

**Files:** `native/strata_vk.c`, `native/shaders/` (the figure vertex shader, `gbuffer.frag`, `gbuf.glsl`), `lib/strata/ffi/vk.march`, `lib/strata/render.march`, `lib/strata/anim.march`, `viewer/forge.toml`, `viewer/lib/strata_viewer.march`, `viewer/lib/figures.march` (create: the example rigs and clips), `scripts/golden.sh`

**Interfaces — Consumes** (from the physics plan, as landed): slots that carry a pose (`body_begin`, `slot_pose`), the octahedral normal in the G-buffer, history matched by position and normal and reset per moving object's screen bounds, culling by slot bounding sphere.

**Interfaces — Produces:**
```
Strata.Ffi.Vk.rig_part_begin(rig : Int, part : Int, ox, oy, oz : Int) : Bool   -- a shared mesh slot for one 32-cubed piece; never writes the light volume
Strata.Ffi.Vk.rig_palette(rig : Int, colours : NativeF32Arr) : Bool           -- 256 x (r, g, b, roughness, emission)
Strata.Render.figures(r : Render, a : Strata.Anim.Anim, rigs : List(Strata.Rig.Rig)) : Render   -- uploads new rigs' pieces and palettes once
Strata.Render.draw(...) gains blend : Float   -- passed to an_draw_list
viewer: STRATA_FIGURES=n spawns n walking humanoids; scenes 31 humanoid-poses, 32 figure-walk-lit
```
Each frame `strata_vk.c` calls `an_draw_list` (weak; absent means no figures) and draws every visible part's pieces instanced with their matrices. A figure quad's texture field is a palette index; a flag bit in word 1 marks figure quads, and the G-buffer pass reads colour, roughness and emission from the rig's palette for them. History is reset inside each moving figure's screen bounds, as the physics path does per body.

- [ ] **Step 1:** merge `main`; read what physics Tasks 5 and 6 landed; record any difference from the interfaces above in the ledger.
- [ ] **Step 2:** `viewer/lib/figures.march`: a humanoid (1/16 m voxels, 15 parts: pelvis root, spine, head, two-part arms with hands, two-part legs with feet; hand sockets; legs declared) with idle, walk, run, jump, crouch, swim and swing clips; marks 1 and 2 for left and right foot down, 3 for the blade's arrival.
- [ ] **Step 3:** implement the shim, shaders and March side; `native/build_shaders.sh`.
- [ ] **Step 4:** `scripts/golden.sh`: every existing golden passes unchanged.
- [ ] **Step 5: scenes:** 31 humanoid-poses (seven humanoids, one per clip, frozen at the clip's midpoint, low sun); 32 figure-walk-lit (a humanoid walking across the lit forest floor, shots 1 s apart once settled; the check is no trail behind it and the floor's lighting settles). Look, then store.
- [ ] **Step 6:** measure the figure pass GPU time with `STRATA_FIGURES=512` against 1.5 ms; record it. If missed, try the design's fallback (one instanced draw per rig with a part index in each quad) and record both.
- [ ] **Step 7:** `scripts/verify.sh` PASS. Commit `feat(anim): figures drawn — shared part meshes, instanced poses, palette colours`.

### Task 7: Plain figures' shadows

**Files:** `native/strata_vk.c`, `native/shaders/` (a depth-only figure shadow vertex shader, `light.frag`, `shade.glsl`), `scripts/golden.sh`

A sun shadow map of plain figures only, covering the view out to 64 m (extent, resolution and filtering are the builder's; record them), sampled in the light pass and combined with the traced shadow by minimum. Off when lighting is off or when no plain figure is drawn (no pass, no cost).

- [ ] **Step 1:** implement; `native/build_shaders.sh`; existing goldens pass unchanged.
- [ ] **Step 2: scene** 33 figure-shadow (a humanoid on grass, low sun: its shadow has two legs and an arm). Look, then store.
- [ ] **Step 3:** measure with 512 figures against 1.0 ms and with 0 figures against 0.1 ms; record.
- [ ] **Step 4:** `scripts/verify.sh` PASS. Commit `feat(anim): plain figures cast sun shadows through a shadow map`.

### Task 8: Traced figures

**Files:** `native/strata_vk.c`, `native/shaders/trace.glsl`, `native/shaders/light.frag`, `native/shaders/reflect.glsl`, `lib/strata/ffi/vk.march`, `lib/strata/render.march`, `scripts/golden.sh`

**Interfaces — Consumes:** the physics path's ray list (`body_shape`, `body_place`, `body_drop`, the per-chunk list rays step through).

**Interfaces — Produces:**
```
Strata.Ffi.Vk.rig_shape(rig : Int, part : Int, idx : NativeU8Arr, sx, sy, sz : Int) : Bool   -- palette indices, shared by every figure of the rig
```
Shapes become shareable: a placement names a shape, and a palette shape's voxel hit takes colour, roughness and emission from its rig's palette (not a light class). Each frame `strata_vk.c` places the parts of figures flagged traced (from `an_draw_list`'s flags, at most 64 figures) in the ray list; those figures are left out of the shadow map. With lighting off (`set_lighting` fallback), every figure is plain.

- [ ] **Step 1:** implement; `native/build_shaders.sh`; existing goldens and physics goldens pass unchanged.
- [ ] **Step 2: scenes:** 34 traced-shadow (scene 33 with the humanoid traced, beside the plain one: the traced shadow is soft-edged like the trees'); 35 figure-reflection (a traced humanoid on the valley lake's shore, seen in the water); 36 glowing-figure (a traced humanoid carrying a glowing torch rig on its hand socket into a cave at dusk: the wall beside it is lit in the torch's colour). Look, then store.
- [ ] **Step 3:** measure 64 traced humanoids in view against 1.5 ms added to the light pass and none traced against 0.1 ms; record. On a miss, apply the design's fallbacks in order (cap 32, then 16, then reflections skip figures) and record each.
- [ ] **Step 4:** `scripts/verify.sh` PASS. Commit `feat(anim): traced figures — shadows, reflections, sky and glow`.

### Task 9: Creatures, the arena, measurements

**Files:** `viewer/lib/figures.march`, `viewer/lib/strata_viewer.march`, `scripts/golden.sh`, `scripts/soak.sh`, `README.md`, `RESULTS.md`, `docs/animation-ledger.md`

- [ ] **Step 1:** add to `figures.march`: a bird (two-segment wings by `swing`, circling), a fish (three-segment springy tail, in the lake), a cart (four wheels by `spin`, steering by `override`).
- [ ] **Step 2:** viewer keys: `G` spawns 64 wandering humanoids; `K` switches foot placement and springs; `T` switches the 8 nearest figures between plain and traced; `V` gives the humanoid nearest the camera a torch. The crosshair's hit ray outlines the part it hits. `STRATA_ANIM_METER=1` prints once a second `anim: figures f posed p stale s parts d traced t update u ms`.
- [ ] **Step 3: scenes:** 37 stairs (a humanoid climbing stairs with foot placement), 38 swing (the sword mid-arc), 39 creatures (bird, fish, cart in the lake scene), 40 arena (512 humanoids running, swinging and aiming on flat ground, foot placement for the nearest 128, 32 traced). Look, then store 37 to 39; 40 is for timing and a look, not a golden.
- [ ] **Step 4: measure** the arena against every target in the design's table (update, ground, figure pass, shadow map, traced, hit ray, 120 fps, memory, no-figure overhead in the existing soak); record each in `RESULTS.md`, met or missed with its cause. Apply the design's order of what gives way only as far as it states.
- [ ] **Step 5:** README "Animation" section complete (building a rig and clip in a dozen lines, the frame loop, the consumer `[ffi]` block). `scripts/verify.sh` PASS. Commit `feat(anim): creatures, the arena, measurements`.

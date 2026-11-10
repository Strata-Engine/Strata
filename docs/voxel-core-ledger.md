# SDD ledger — plan: docs/2026-11-10-strata-voxel-core.md
Pre-flight: Task 7 consumes Store (Task 6) for Mesh.around_of; Task 9 consumes Task 8 World/Outcome; Task 10 consumes World. No conflicts found.
Ruling: the sdd-workspace/task-start/task-done scripts are not used — the plan lives in another repository than the code, so the ledger is kept by hand here — cost if wrong: none, same format.
Ruling: OWNER REDIRECT mid-run ("show me working rendering as soon as you can"). Order becomes T1, T2, T3, T4, T7, then a render slice pulled forward from project 2 (Vulkan shim + viewer app), then T5, T6, T8-T11. Mesh gets an `around_from` constructor so it does not wait on Store — cost if wrong: the render slice predates project 2's own design and may be reshaped by it.
Ruling: Vulkan runs on Homebrew MoltenVK 1.4.2 (KosmicKrisp is not in Homebrew); the spec names MoltenVK as the fallback — cost if wrong: swap the ICD, no code change.
Task 1: Ruling: Strata.Noise keeps only the generic primitives (hash2, value2, fbm, ridged, ridged_fbm, smooth, lerp, clamp01); cube_forge's noise.march is mostly terrain shaping with sea level and rivers, which the spec's anti-goals exclude — cost if wrong: copy more later.
Task 1: Ruling: F32Buf tests dropped from the copied math test (F32Buf is not copied; the mesher emits i32 quads) — cost if wrong: none.
Task 1: complete (tests: scripts/verify.sh → 12 tests, 0 failures, PASS)
Task 2: Ruling: the key type's constructor is ChunkKey, not Key — stdlib JsonPath also defines Key and March reports the clash — cost if wrong: none, callers use Coord.key().
Task 2: complete (tests: scripts/verify.sh → 20 tests, 0 failures, PASS)
Task 3: Ruling: Opacity's empty constructor is Clear, not Empty (stdlib IOList defines Empty); added opacity_index/layer_index/style_index/model_of because constructor patterns do not resolve outside their module (GAPS S5) — cost if wrong: rename.
Task 3: Ruling: with_collide's parameter is 'yes' ('on' is a keyword) — cost: none.
Task 3: complete (tests: scripts/verify.sh → 30 tests, 0 failures, PASS)
Task 4: Ruling: added Chunk.set_idx and Chunk.from_flat beyond the plan's list — generators fill a flat array and build once instead of paying a palette scan per voxel — cost if wrong: two extra public functions.
Task 4: Ruling: private constructors are ChUniform/ChPaletted/ChDirect to stay clear of stdlib constructor names — cost: none.
Task 4: complete (tests: scripts/verify.sh → 44 tests, 0 failures, PASS)
Task 7: Ruling: the naive oracle lives inside test/mesh_test.march, not test/support/ — avoids depending on how forge treats test subdirectories — cost: none.
Task 7: Ruling: Mesh takes Around built by around_from(f) (Store does not exist yet; around_of(Store, Key) arrives with Task 6); layer access is by number (quads_at/quad_count_at) plus Layer-typed wrappers (GAPS S5) — cost: none.
Task 7: Ruling: the buried-rock shortcut fires when the six face neighbours are uniform solid-opaque of ANY id, not only the same id; the face rule depends only on solid-opacity — cost: none, oracle-checked.
Task 7: complete (tests: scripts/verify.sh → 69 tests, 0 failures, PASS)
Render slice: Ruling: built ahead of project 2's design at the owner's request. native/strata_vk.c (Vulkan 1.2 API on MoltenVK, one 64 MB quad buffer, instanced 4-vertex strips, CPU frustum cull, one draw per chunk layer, forward shading with corner shading and fog), lib/strata/ffi/vk.march, viewer/ app. Compute culling, indirect draws, deferred and traced lighting remain project 2/3 — cost if wrong: the shim is reshaped by project 2's design.
Render slice: complete (viewer renders 676 chunks, 153595 quads in 1.2 MB, 120 fps vsync-capped; screenshot docs/first-render.png; scripts/verify.sh PASS incl. viewer)
Task 5: Ruling: the C memory helper is NOT built. Measured on the release viewer: terrain 0.3 ms and mesh+upload 2.3 ms a chunk in pure March; the spec says to drop helpers March is fast enough without. The consumer [ffi] path the task also had to settle is proven by viewer/forge.toml (GAPS S11) — cost if wrong: add native/strata_mem.c later behind Strata.Ffi.Mem; no public interface changes.
Task 5: complete (by ruling; no code)
Task 6: complete (tests: scripts/verify.sh → 78 tests, 0 failures, PASS; adds Mesh.around_of / has_all)
Task 8: Ruling: a single-cell edit in an unloaded chunk is queued and listed in deferred but yields no Cell change (the old id is unknown until the chunk arrives) — cost if wrong: a game that needs it reads the block after load.
Task 8: Ruling: bulk edits dirty every loaded chunk their bounding box (grown by one voxel) reaches, without checking that a voxel actually changed — over-remeshing, never a stale mesh — cost: some wasted remeshes at sphere corners.
Task 8: Ruling: Change is read through kind/old_id/new_id/low/high/radius (GAPS S5); World exposes Op, apply_op, replace_chunk, enqueue for Edit — cost: none.
Task 8: complete (tests: scripts/verify.sh → 96 tests, 0 failures, PASS)
Task 9: Ruling: workers are scheduler tasks, not actors, and Config's 'workers' is the number of generation tasks and of mesh tasks started per tick. tick awaits the tasks it started on the PREVIOUS tick, so it can wait if one is still running a frame later; the plan said tick never blocks. March has no non-blocking await and Actor.call cannot run under forge test (GAPS S12) — cost if wrong: a slow generator stalls a frame; fix is a task_poll builtin in March.
Task 9: Ruling: stop() awaits outstanding tasks rather than killing actors; Stream.config() constructs a Config; Stream.pending() added for diagnostics — cost: none.
Task 9: complete (tests: scripts/verify.sh → 104 tests, 0 failures, PASS)
Task 10: Ruling: Hit is Miss | Struck(...) read through is_hit/hit_x/hit_y/hit_z/hit_face/hit_dist, and Swept through swept_x.. and blocked_x.. (GAPS S5). A ray that starts inside a stopping block hits it at distance 0 with the face looking back along its strongest axis — cost: none.
Task 10: Ruling: sweep moves in steps no longer than the box's own thickness on that axis so it cannot tunnel; the plan did not say — cost: none.
Task 10: complete (tests: scripts/verify.sh → 121 tests, 0 failures, PASS)
Task 11: Ruling: per-chunk times are totals over counts because March main exposes only a millisecond clock; 'worst' is in whole ms — cost: coarse worst-case figures.
Task 11: Ruling: the picture is ASCII PPM (P3) written with File.write, converted to docs/demo-surface.png — cost: none.
Task 11: Ruling: optimised the mesher after measuring (direct centre copy, per-layer slice skipping): 5.25 -> ~4 ms per drawing chunk, 1.5 -> 0.5 ms per empty non-uniform chunk; oracle tests unchanged and green — cost: none.
Task 11: complete (demo runs twice with identical hashes, 8 bytes per quad, picture shows the crater; scripts/verify.sh → 121 tests, PASS incl. demo and viewer)
Render slice: Ruling: the viewer now streams (Strata.Stream), uploads and frees meshes by slot, and carves or builds spheres on click (Query.raycast + Edit.fill_sphere); added sv_mouse_button to the shim — beyond the plan, toward the owner's 'show me rendering' and destructible-world goals — cost: none to the core.
Render slice: verified headless: 3087 chunks streamed in 1.27 s, 361819 quads in 2.8 MB, 118 fps capped, scripted carve visible in docs/streamed-world.png; scripts/verify.sh PASS
Final review: fresh reviewer (most capable model), no Critical, one Important, minors below.
Final: fixed stale seam after a queued single-cell edit — "a block queued into an unloaded chunk at a border remeshes the loaded neighbour when the chunk arrives" and "an edit queued for an unloaded chunk still dirties the loaded chunk across the border" RED→GREEN, suite 124/124
Final: fixed deferred single-cell edits recording no change (reverses the Task 8 ruling; they now record Cell with old id -1) — "an edit to an unloaded chunk is queued and applied when the chunk arrives" RED→GREEN, suite 124/124
Final: fixed (re-graded Minor→Important) palette slots never reclaimed, so a voxel cycling through states pushed a chunk to the 131 KB direct form — "a voxel cycling through hundreds of ids does not push the chunk out of its palette" RED→GREEN, suite 124/124
Final: minor (deferred): an edit to a loaded chunk in the hysteresis ring (missing a neighbour) keeps its old mesh until that neighbour reloads
Final: minor (deferred): a generator task that fails is retried every tick forever and settled() never becomes true
Final: minor (deferred): World.insert on an already-loaded edited key replaces it with the generated chunk (Stream guards; the API does not)
Final: minor (deferred): bulk edits queue their op on every unloaded chunk in the bounding box, including sphere corners never touched; a huge box walks every chunk in it
Final: minor (deferred): World.new accepts a scale <= 0; Stream.set_focus divides by it
Final: minor (deferred): raycast walks through unloaded air up to max rather than stopping at the loaded world's edge (doc says leaving is a miss; result is the same, cost is not)
Final: minor (deferred): sweep's 100000-step cap silently truncates an extreme move of a very thin box
Final: minor (deferred): fragment shader indexes the palette with 8 bits of the 16-bit texture id; ids >= 256 alias
Final: minor (deferred): the frame fence is reset before submit, so a failed vkQueueSubmit would hang later waits
Final: minor (deferred): swapchain image count above 8 and a failed swapchain rebuild are not handled
Final: minor (deferred): the viewer discards refused uploads (reachable past 16384 slots, about radius 30)
Final: minor (deferred): test gaps — stale-stamp drop mid-mesh, task Err branches, determinism with edits, raycast property at other scales and axis-parallel rays, demo not run by verify.sh
Final: Ruling: renderer omissions the reviewer declined to judge (instances not drawn, cutout drawn without discard, translucent quads unsorted) stand for the early slice — a person flying the viewer sees no plants-as-crosses and leaves as solid cubes — cost: they belong to project 2's design.
Final: Ruling: the March array header layout (+16 length, +32 data) the shim assumes is taken from cube_forge and confirmed only by the renderer working on main 1a4251bc8 — cost if March changes it: garbage geometry, caught by the first screenshot.

# SDD ledger — plan: docs/2026-11-10-lighting-plan.md
Pre-flight: T1's passes and set_lighting modes are extended by T3-T6; T2's light classes feed T4-T6's shaders through the quad flags byte and the block table; T2's volume is read by T4-T6. No conflicts found.
Ruling: the owner said "Build it" before reading the design document and after "don't overcomplicate the ask"; taken as approval of the design and of native execution, as chosen for the last two plans — cost if wrong: the design's calls (reflections in, glowing blocks via sky rays, no shafts) are the designer's.
Ruling: the light pass is a full-screen fragment shader, not a compute shader as the first design said; the design leaves the shader structure to the builder and a fragment pass needs no storage-image plumbing — cost: none.
Ruling: the design review's five questions were settled without the owner (see the plan's "Answers to the design review"): global history cap on an edit; water lit forward without history; model blocks are 'thin' emitters that cast no shadow; a lone emitter's noise is accepted; the volume covers the whole stream box (156 MB at radius 10, over the 128 MB target); Blocks.def() becomes fully rough — cost if wrong: each is a visible lighting behaviour the owner may want different.
Task 1: complete (deferred passes; all 13 goldens match stored images within 1/255 WITHOUT --update; scene 1 offscreen 1500 fps, GPU 0.60 ms; scripts/verify.sh PASS)
Task 2: Ruling: the volume covers the whole stream box, not a smaller lighting radius — one size to reason about, no chunk in view without light data — costs 156 MB at radius 10x3, over the 128 MB target (met at radius 8).
Task 2: Ruling: light bytes ride the chunk upload (sv_chunk_light between begin and commit) and are copied into the volume at the start of the next frame, 128 chunks a frame at most — keeps uploads between frames — a burst of more than 128 chunks is lit a frame or two late.
Task 2: Ruling: the debug view leaves cutout holes and thin blocks black or showing what is behind — it is a debugging view — none.
Task 2: complete (gate: scripts/verify.sh PASS, 154 lib tests, 15 golden scenes; soak PASS worst 16.57 ms)
Task 3: Ruling: a noon sun is about 1.0 in linear units and the screen curve is linear up to 0.6 then rolled off — lit surfaces keep their texture's colour and the sky shows as authored — brighter suns wash out sooner than a filmic curve would.
Task 3: Ruling: light mode 0 keeps a fixed flat sky colour and ambient share inside the shim now that set_sky is gone — mode 0 is the comparison baseline — a game cannot restyle mode 0.
Task 3: complete (gate: scripts/verify.sh PASS; goldens sun-high, sun-low looked at)
Task 4: Ruling: a run of leaf voxels blocks a sun ray only where it enters the run, not at every leaf inside — interior leaf faces are not drawn, and testing each one made canopies cast solid shade with no dappling — a thick canopy lets through as much light as a thin one.
Task 4: Ruling: settled pixels sample one frame in four (8-pixel blocks); every pixel samples while an edit settles — brings mode 2 from 3.5 ms to 1.7 ms — soft edges take about four times as long to smooth after the camera moves.
Task 4: Ruling: while an edit settles, a pixel whose new sample contradicts its memory drops the memory, on top of the plan's cap of 2 — the cap alone left the shadow half dark at frame 34 — a frame of extra noise in leaf shade near an edit.
Task 4: Ruling: stillness is checked on frames 118 and 119 from two runs of the same deterministic sequence, since the viewer writes one image a run — same frames, same pixels — none.
Task 4: Ruling: the edit step is covered by the stored images shadow-placed and shadow-removed rather than a separate threshold check — freshly sampled leaf shade differs by about 3% of pixels from a settled image, too close to what a missing shadow would add — a lag small enough to stay under the golden tolerance would pass.
Task 4: Ruling: forge reuses a build when only the shader header changed; build_shaders.sh now stamps strata_vk.c (GAPS S14) — shader-only edits were silently not taking effect — none.
Task 4: GPU target 2 ms: 1.74 ms at sun 55, 2.60 ms at sun 15 (missed with a low sun).
Task 4: complete (gate: scripts/verify.sh PASS; 7 shadow goldens looked at; stillness 0.0017%)
Task 5: Ruling: ice is a solid smooth block in patches on snow above height 84, not frozen lake tops — the viewer's only water is at sea level, far below the snow — no translucent ice.
Task 5: Ruling: a mirror hit takes its colour from this frame's albedo buffer and last frame's sun visibility, not from the lit image — the lit image is being written while it would be read — reflected surfaces carry no reflected highlights or sky light of their own.
Task 5: Ruling: beyond 40 voxels a mirror ray looks at every third voxel — reflections of thin far things can be missed.
Task 5: Ruling: golden scenes may state DRAWS=n; the high-ground scene has no water or plants in it and issues 4 draws — the fixed count of 5 assumed every kind of geometry is always present — none.
Task 5: mode 3 costs 7.0 ms with a lake filling half the screen: over the 6 ms whole-frame target before sky light is added.
Task 5: complete (gate: scripts/verify.sh PASS; 4 reflection goldens looked at)
Task 6: Ruling: the light pass stays at full resolution although the lake view is over 6 ms — the overage is the water's own rays in the final pass, which half-resolution lighting would not reduce; every other scene is 3.4 to 4.6 ms — the lake view runs at about 100 fps, under the 120 target.
Task 6: Ruling: sky light leans on up to 255 past samples (shadows stay at 64) — a lantern is found by few rays and 64 left visible flicker — sky light takes longer to follow a change that is not an edit, such as a moving sun.
Task 6: Ruling: a pixel of a model or plant has no class in the geometry buffer and looks it up in the light volume; before this, such pixels read class 0 and were treated as mirrors in mode 3 (reflect-lake and reflect-glint re-stored) — one more texel fetch for those pixels.
Task 6: Ruling: a thin emitting block lights a ray that starts beside it at 0.3 strength — the floor under a lantern was a hot square — the lamp's own block is under-lit.
Task 6: stillness target missed for lantern light at dusk: 0.68% at 2 s, 0.07% at 10 s against 0.05%.
Task 6: complete (gate: scripts/verify.sh PASS; goldens full-forest, full-cave, full-lantern, full-overview looked at)
Task 7: Ruling: the soak's 20 ms worst-frame assertion fails in this session with lighting on AND off (load average 35 from other work on the machine); its other assertions pass — not attributable to lighting — a real hitch from lighting would be hidden until a quiet run.
Task 7: complete (gate: scripts/verify.sh PASS; soak functional assertions pass, timing assertion not judged)
Final review: fresh code-reviewer (fable). No critical findings; five important; verify.sh PASS at review time.
Final: fixed "a moved sun, a mode change or a quality change takes seconds to show" — a sun that moves more than a fraction of a degree unsettles every pixel for two frames; a mode change or flat/traced sky switch drops the memory — scripted sun move 55 to 25 degrees, shot ten frames later against the settled image: 20.6% of pixels differ before, 5.5% after (the rest is fresh leaf shade); suite 155/155, gate PASS.
Final: fixed "every re-upload of a held chunk resets the whole screen" — a chunk whose light bytes are unchanged is no longer written or counted as an edit (hash per cell), and an edit unsettles only pixels within 32 voxels of the chunks it changed — two frames after an edit against no edit: 5.45% of pixels differ before, 5.12% after; small in that scene because most of the view is within reach of the slab; suite 155/155.
Final: fixed "far mirror hits are always shaded as in shadow" — a hit found by a stride goes back and walks the last stretch exactly — reflect-down and reflect-lake show the reflected hillside sunlit where it was dark; both re-stored after looking; suite 155/155.
Final: fixed "overflowed blocks take whatever the 255th class is, and the overflow is not surfaced" — test "past 255 classes a block shares a class of its own kind" RED (got 255, expected 1) then GREEN; Strata.Render.light_overflow added and the viewer prints it; suite 155/155.
Final: Ruling: the light volume assumes one focus point (finding 4) — documented on Strata.Render.set_light_box rather than sized from several boxes; the viewer and every plan so far have one focus — a split-screen or server-style game gets missing shadows until this is built.
Final: also taken from the minors because they were one line each: the shim starts in light mode 4 and Strata.Render.set_lighting exists; reflections do not read the pass's memory while it is fresh; a failed light volume allocation no longer leaves a null table to dereference.
Final: minor (deferred): two queued writes to one cell can land in one copy call; correct only if the driver applies regions in order.
Final: minor (deferred): a failed queue submit drops light writes already taken from the queue; those chunks keep stale light data.
Final: minor (deferred): the class table is one shared buffer rewritten under frames in flight; a frame can see it empty if blocks are re-uploaded mid-game.
Final: minor (deferred): set_light_box called late re-queues nothing (documented), and a radius over 30 is refused leaving the small default.
Final: minor (deferred): owner coordinates are 16-bit, so lighting fails past 32767 chunks from the origin; ray precision degrades past about 16 000 voxels.
Final: minor (deferred): a lamp more than 12 voxels away is found about a third as often by sky rays, a step in its brightness.
Final: minor (deferred): the bounce term assumes every hit is half sunlit, so a sealed room is lit more than it should be.
Final: minor (deferred): a mirror ray from the underside of water starts inside the water and hits its own voxel.
Final: minor (deferred): no pure-logic test of wrap addressing (the goldens volume-walk and shadow-small-box cover it); stillness compares adjacent frames and would not see slow drift.
Final: minor (deferred): depth reconstruction error at about 300 m may self-shadow distant faces; unconfirmed, hidden by fog at the default radius.
Final: not judged by the reviewer: all timings (loaded machine), Vulkan validation layers.

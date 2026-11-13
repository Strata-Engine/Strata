# Particles: the ledger

Design: `docs/2026-11-12-particles-design.md`. Plan:
`docs/2026-11-12-particles-plan.md`. Branch
`claude/particle-effect-system-599ddd`, built with March main (0.7.0)
through `scripts/env.sh`'s `MARCH_HOME`. Executed inline (one session), with
one fresh review of the whole branch at the end.

## Before the work

- `combat` merged into main (887e2ee): the frame loop keeps both main's
  run context (`Ctx`, the `R` key, scene 134) and combat's match and river
  drops; combat's GAPS entries renumbered S32 to S34.
- Main did not run under March main: `Strata.Coord.mix` multiplied past
  2^62 and March main traps where 0.5.0 wrapped. The chunk-perf session's
  fix (71f0b65: `int_wrapping_mul` in the hashes, `MARCH_HOME` in
  `env.sh`) is cherry-picked here (8205625), not onto main.
- The library's own `forge test` is not green under March main before
  this work (471 tests, 5 failures in physics and fluid: overflow traps,
  and properties that never ran under 0.5.0, GAPS S31). Each task was
  gated instead on its own tests, the viewer's suite, the C tests and the
  goldens; the same five names fail, and no others.

## Rulings

- **Tasks 1 and 2 in one commit.** Their changes share `strata_vk.c`, the
  header and the C test.
- **`sv_emit` takes its floats as one array** of 18, not twenty extern
  parameters.
- **The glowing pass is drawn while a glowing particle may be alive**
  (`glow_until`), so scenes with spray alone keep their draw counts. (A
  first version drew it once any glowing effect was defined; the viewer
  defines its effects every run, which would have added a draw to every
  spray scene.)
- **The overwritten-particles stat is 25** (19 to 24 were taken by the merge with main): a push that
  forces the ring's tail past a slot still in the live span.
- **The effect table is one storage buffer** shared by every frame in
  flight, written in place when an effect is defined (at start).
- **The ray tracer's bindings are seen by vertex shaders** (11 to 14, 17
  to 21, 24): a shaded effect lit once a particle traces there.
- **Checkpoint merges into main wait for the final review.** Inline
  execution has no reviewer per task, and main should carry reviewed work.
- **The burn schedule sorts a burst's cells nearest first,** so past the
  cap of 256 a burst's middle burns slowly and its edge at once.
- **A sixth effect, blaze,** for burning cells: the fireball's flame,
  started inside an opaque cell, never cleared its faces.
- **`combat-fire` (frame 60) is the in-flight golden** (it already
  catches the fireball mid-arc); the new golden is `combat-burning`
  (frame 150).
- **`blast_test` aliased `Strata.Coord` as `Cco`.** Its `Cb` met combat's
  `Cb` (`StrataViewer.Combat`) in the compiled test build: GAPS S41. Main
  fixed the same clash its own way (`Kb`, `Rules`, `Cmb`), and the merge
  took main's names.

## The owner's change: tiny voxels

Asked after Task 6: particles that look like tiny voxels, spray included.
An effect grew to 44 floats (`[40]` its shape). The particle pipelines draw
36 vertices a particle as a triangle list with back faces culled: a cube
(twelve triangles, counter-clockwise from outside, as `pt_cube_vertex`
holds and the C test checks), or a square drawn as two triangles each way
round. Cube faces are lit by their own direction and tumble at the
effect's spin about an axis the particle's seed picks. Spray tumbles as
cubes in its block's texture. The viewer's six effects are cubes in the
near-white layer 13; the flame tile (14) stays for a square-drawn flame.

## The effects as tuned

| effect | look | size (m) | colour | motion |
|---|---|---|---|---|
| flame | glowing | 0.16 to 0.04 | yellow-white, orange, dark red | rises (3 m/s2), drag 2, sway 8 cm, spin 2 |
| blaze | glowing | 0.4 to 0.1 | as flame | rises (2.5), drag 2, sway 10 cm, spin 1.5 |
| ember | glowing | 0.05 | orange, fading | falls (2), drag 0.5, spin 4 |
| spark | glowing | 0.06 to 0.02 | bright yellow to orange | falls (9.8), drag 0.3, spin 6 |
| smoke | shaded | 0.25 to 0.7 | grey, opacity 0 to 0.3 to 0 | rises (0.6), drag 0.8, sway 20 cm, spin 0.6 |
| steam | shaded | 0.2 to 0.6 | white, opacity 0 to 0.35 to 0 | rises (1), drag 1, spin 0.6 |

The match: a fireball carries flame 200/s, smoke 30/s, embers 20/s; its
burst is 80 sparks, 60 flames, 20 smoke; a burning wood or leaves cell
blazes at 40/s (grass and tall grass 15/s) with smoke 8/s; ice melts in a
burst of 15 steam; a cell going out puffs 6 smoke.

## Goldens

Stored after looking: `fx-showcase` (new), `combat-burning` (new),
`combat-fire` (the fireball's trail; 38 to 40 draws), and with the cubes
`splash-early`, `splash-late`, `splash-shaded` (blocky droplets). The lake
scenes with spray passed unchanged until the cubes and pass with them.

## Landing on main

- Main moved twice while particles waited: combat (e58d71b) and then
  physics Task 8 (8d83288). The burning blocks moved to 25 and 26, after
  main's step blocks (22 to 24); the overwrite stat moved to 25.
- **`combat-burning` re-stored after the physics merge.** Rendered at the
  commit before it (075114e), it matched its golden exactly; after it,
  15.7% of pixels differ, every one lighting noise in whole chunks (light
  mode 4 still converging at frame 150), the scene's blocks, fire and
  figures unchanged. Physics now writes its outcome through
  `Strata.Edit.set_many` each step, which may be resetting those chunks'
  light. Not investigated here: physics is another session's work. Cost if
  wrong: a re-lighting regression in physics hides behind this golden.

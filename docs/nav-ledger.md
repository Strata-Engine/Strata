# Path finding ledger

Design: docs/2026-11-12-nav-design.md. No plan document: the user asked for
"tools for entity AI and path finding" with three conditions (target March
main, merge to Strata main regularly, test with the combat match) and then
"just dive in". Built inline; one reviewer at the end.

## Rulings

Ruling: the match was not on main when this began. `combat` was merged into this branch first (three conflicted files); while the work went on another session merged `combat` into main with the same resolution, and main was merged back here (cabc0fd) with nothing lost on either side.
Ruling: "target March main" is taken as today's `origin/main` (`f6481bbe6`), not the stale checkout at `~/code/march-main` (`1a4251bc8`, 146 commits behind). A private build of it was made in this session's scratch directory. `scripts/env.sh` now sets `MARCH_HOME` so forge really uses the March it names. Strata no longer builds with 0.5.0 (GAPS S37). This is the largest consequence of the work and it reaches every other session: see "Before main" below.
Ruling: path finding is an A* search over the voxels themselves, in March, with no stored navigation data: the world is edited every few seconds and a search that reads it has nothing to keep right (design, "Why A*").
Ruling: `Strata.Nav` works in voxels and takes a walker (footprint, height, step, drop, swims), not a game's entity kind; `walker_of` makes one from metres at the world's scale.
Ruling: a walker wades water up to its chest (its head cell must be out of fluid) and a swimmer floats in the top cell of a fluid; into water costs three times a step, so a ford is taken when one is near.
Ruling: an unfinished search answers the way to the nearest cell it reached, not nothing, so a game always has somewhere to walk; `arrived` tells the two apart.
Ruling: the match plans when a fighter decides (four times a second), only when `straight` says the direct way is shut, and keeps a plan one second. The old sidestep is kept underneath for fighters blocking each other.
Ruling: a rusher remembers a target for 6 s out of sight. Without it path finding had nothing to do behind a wall: a fighter only ever chased what it could see, so a wall ended the chase before a path could be asked for.
Ruling: the first test scene was a dry ditch. It proved nothing: the field's middle is by a river, the ditch filled, and fighters swim. The scene is a stone wall raised after the rusher has seen its target (`fence`).
Ruling: swimmers plan too (to their target, else to the spawn point they made for). It was needed for the fence by the river, and it answers the combat review's deferred "a swimmer can turn back to its own bank".
Ruling: `Strata.Steer` is in the library and tested but the match uses none of it yet. Wiring `apart` in changes every fight; it was left out so that what was measured is what is merged.
Ruling: `STRATA_COMBAT_NAV=0` keeps the old behaviour runnable, so the difference stays measurable.

## Findings

Finding: on March main `Strata.Coord.key_hash` panicked with integer overflow, which stopped 197 library tests and crashed the test binary. One multiplication; fixed with `int_wrapping_mul` (and three more like it).
Finding: on March main the suite's 148 properties run for the first time (GAPS S38). One test's own checker was wrong for a ray grazing a voxel's edge (fixed); one fluid property is very slow.
Finding: a lambda that prints or reads a file fails March main's capability check, charged to no module (GAPS S35).
Finding: a search costs about 5.5 microseconds a cell expanded in the match, 3 ms a plan and 9 ms at worst, on a loaded machine (see RESULTS). Not yet profiled against the March performance goal: how much is `Strata.World.get_block` (a store lookup a cell) and how much the search itself is not known.

## Review (one fresh reviewer, read-only, lib/strata/nav.march, steer.march and the match's use of them)

Sound, by its reading: the heap, the stale-entry test, the heuristic against the costs, the key and index widths, the cells a search reads against the cells it has classified, the move rules, `smooth`, `advance`, `cell_of`, `walker_of`, `Strata.Steer`, the fence.

Review: fixed a walker taller than 16 cells finding no path at all (its head lay outside the window's height: a 1.8 m fighter at 0.1 m a voxel) -- the window is 40 and twice the height tall; test `a walker taller than the window's margin` (18 cells: arrives, and `nearest` finds).
Review: fixed a fighter with no way to its goal searching four times a second (the plan's wait was skipped whenever no corners were left) -- the wait holds with or without corners.
Review: fixed a plan followed after its goal had changed (a new target, a patrol point turned, the idle rule's move to a spawn point) -- a plan keeps the cell it was made for and is dropped when the goal is more than four cells from it; the idle rule clears the way, the plan and the remembered target.
Review: fixed `straight` passing a line through the exact corner of a cell with a block on one side (A* refuses that diagonal; `smooth` and the match's straight test let it back in, and a 0.6 m fighter would rub the corner) -- both ways round the corner must be open and end at one height; tests `through the exact corner of a cell`, four cases.
Review: fixed `straight` and `smooth` cutting a swimmer's path back through a pond the search had routed it round -- a straight line keeps one footing; test `a line into or out of water is not straight`.
Review: fixed a rusher swinging at a remembered target through a wall (a swing lands on whoever is in reach, with no line tested) -- a target out of sight is chased, not swung at.
Review: fixed the heap's size, which a walker with a long drop could outrun (a neighbour was then silently dropped) -- sized from the walker's step and drop.
Review: Ruling: the rusher's memory counts 0.25 s a decision, not elapsed time; decisions are every 15 frames and the match's step is fixed, so they agree.
Review: a test helper of mine was wrong, not the code: two neighbouring cells of a path are one legal move and need not be `straight` under the stricter rule (a diagonal beside water).

## Checks on the final code (feb4463)

scripts/combat_check.sh: PASS, twelve scenes, each twice (23 minutes). Bots-only match: winner at frame 42,475 with path finding, 51,697 without, same build.
forge lint --strict: clean in the library, viewer, demo and the three checkers.
Viewer tests: 92 of 92. Nav 20 of 20, Steer 7 of 7.
Library suite as a whole: NOT passed. With a fixed seed 461 tests passed and none failed, then the test program ended with no summary inside `Fluid properties: the amount is conserved`, 18 minutes in. Open.
Not run on March main: scripts/golden.sh (the stored pictures; the three combat pictures will have moved), audio_golden.sh, anim_check.sh, ent_check.sh, the C tests, the demo's build and test.

## Before main

Merging this to main makes main need March `f6481bbe6` or later. Sessions still building with 0.5.0, and the shared `~/code/march-main` checkout, will fail on `Authority.must` and `int_wrapping_mul` after they take main. That is what the user asked for ("target march main"), and it is not something to do to other sessions unannounced: the merge to main waits on the user's word.

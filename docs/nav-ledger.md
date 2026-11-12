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
Finding: a search costs about 10 microseconds a cell expanded in the match (see RESULTS). Not yet profiled against the March performance goal: how much is `Strata.World.get_block` (a store lookup a cell) and how much the search itself is not known.

## Before main

Merging this to main makes main need March `f6481bbe6` or later. Sessions still building with 0.5.0, and the shared `~/code/march-main` checkout, will fail on `Authority.must` and `int_wrapping_mul` after they take main. That is what the user asked for ("target march main"), and it is not something to do to other sessions unannounced: the merge to main waits on the user's word.

# Column Stage Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A chunk column's shared generation work (heights, thermal relax, tree placements, cave factors, bounds) is done once for the chunks of the column that are generated together, taking an island chunk from 4.05 ms of CPU to about 2.1 ms when a column arrives whole, with no chunk, stored hash or golden image changed.

**Architecture:** Shape A of the design. A generator may carry a batch function beside its chunk function: keys of one chunk column and level in, their chunks out, equal chunk for chunk to the single runs. `Strata.Stream` puts the keys of one column that it would start in the same tick into one task. The viewer's `island_full` splits where it calls `area_lv` into a column half and a chunk half; the single-key path is their composition.

**Tech Stack:** March origin/main (07b58d065 or later) through `scripts/env.sh`; no new C.

**Spec:** `docs/2026-11-12-column-stage-design.md`. Measurements it rests on: `docs/2026-11-12-chunk-performance-findings.md` (addendum).

## Global Constraints

- Work on a branch cut from `main`. Commit after each task. `scripts/verify.sh` prints PASS at the end of every task, with `viewer/.march/atlas`, `demo/.march/atlas` and `.march/atlas` left as they are (the atlas does not change).
- **No chunk changes.** The stored atlas hash in `test/atlas_test.march`, the twelve stored `Chunk.hash` literals in `viewer/test/island_test.march` and every golden image pass unchanged. A task that finds one changed has a bug; do not re-store.
- Purity stays the contract. `Strata.Terrain.run(g, k, seed)` keeps its meaning and every existing caller; nothing is cached between calls, and no value is shared between tasks.
- GAPS apply. In particular: test-module aliases are unique across the suite (S6); a module that spawns tasks declares `needs IO.Spawn`; no `&&` or `||` guarding a read that can trap is relied on for older compilers, write nested `if`; a printed line is `Authority.must(println(..))`, not inside a lambda (S35); a property with `Check.all` runs on a random seed (S38), so prefer a fixed loop over listed cases where the cases are known.
- Integer products stay under 2^62 or use `int_wrapping_mul`.
- Float-valued `if` is written plainly; do not rewrite as arithmetic.
- Timings are CPU time and instructions from `/usr/bin/time -l` differences (as `bench/chunk_stages/run.sh` does), never wall-clock, unless `uptime` shows a load average under 8.
- Other sessions edit `viewer/lib/strata_viewer.march` and merge into `main` during this work. Merge `main` in before each task's gate; Task 3's edit to `island_full` is a move of existing lines, so re-apply it by hand after a conflict and do not resolve by taking one side.

## Review Focus

1. **Equality with the single runs.** `run_column` over any keys of one column must give, key for key, the chunk `run` gives, at every level the generator declares, for a column of early-outs, of full chunks, beyond the atlas, on a site, on a lake. (Tests in Tasks 2 and 3.)
2. **What the stream may batch.** Only keys of one (x, z, level) that pass every test a single key passes this tick (wanted, not loaded or stale, not waiting, not running). A stale coarse key keeps its own stamp and its own `fines`. A batch never exceeds the cap or the tick's room. (Tests in Task 4.)
3. **Results applied one at a time.** A batch's chunks go through `apply_gens` as separate results: budget, staleness and "no longer wanted" are judged per key, so a column unloaded while its batch runs drops each result as a single one is dropped today.
4. **The world is the same.** After settling, the stream's `world_hash` with the cap at 1 equals the hash with the cap at 8, on the test generator and on the island.
5. **Task length.** The longest generation task with the chosen cap, measured in Task 5, against the two-tick window.

---

### Task 1: How many keys of a column start together

**Files:** `bench/column_batches/` (new: `forge.toml`, `lib/column_batches.march`, `run.sh`, `README.md`). No library or viewer file changes.

A small app on the viewer's island generator (the copy-with-`pfn`-made-`fn` trick of `bench/chunk_stages/build.sh` is not needed: it uses only `StrataViewer.island_gen`, `load_atlas`, `decor_of` and `Strata.Stream`'s public API). It drives a stream with the viewer's configuration (the call in the viewer's `main`: `config_levels(r, 3, 8, 4, far_radius(), 1, levels)` with levels, `config(r, 3, 32, 8)` without) through three scripts and, at each tick, reads `Strata.Stream`'s public queue and loaded set to count, for the keys a tick would start, how many share a column and level with the key at the head.

- [ ] Read `Strata.Stream`'s public functions; if the queue is not readable from outside, the count is taken instead from the order chunks arrive in (`take_loads` or the chunk log): group consecutive arrivals within a tick by column. Say in the README which was used.
- [ ] Scripts: (a) start-up at the origin until settled; (b) level flight, 400 m along +x at 8 m a tick; (c) a climb, 96 m up at 2 m a tick.
- [ ] `run.sh` prints, per script, a histogram: batches of 1, 2, 3, 4, 5 or more keys, counting only keys that are not early-outs (`StrataViewer.island_early_of(..) < 0`) and, separately, all keys.
- [ ] README: the histograms, the mean full chunks a batch in each script, and the projected chunk cost from 2.45 / k + 1.6 ms.

**Done:** the three histograms are in the README and the commit; the controller records them in the ledger and sets the starting cap for Task 4 (4 unless the numbers say otherwise).

### Task 2: `Strata.Terrain`: a batch function beside the chunk function

**Files:** `lib/strata/terrain.march`, `test/terrain_test.march`.

**Interfaces — Produces:**
```
Strata.Terrain.Gen                      gains a fourth field: Option(Column)
Strata.Terrain.Column = Column(List(Strata.Coord.Key) -> Int -> List(Strata.Chunk.Chunk))
Strata.Terrain.with_column(g : Gen, c : Column) : Gen
Strata.Terrain.run_column(g : Gen, ks : List(Strata.Coord.Key), seed : Int) : List(Strata.Chunk.Chunk)
Strata.Terrain.same_column(a : Strata.Coord.Key, b : Strata.Coord.Key) : Bool    -- same x, z and level
```
`gen`, `gen_levels`, `with_stamps`, `max_level`, `run` and `stamps` keep their signatures; `with_stamps` and `with_column` each keep the other's field.

`run_column(g, ks, seed)`:
- `Nil` gives `Nil`.
- If the keys are not all `same_column` as the first, it answers `List.map(ks, fn k -> run(g, k, seed))` (correct, only not shared): the stream never sends such a list, and a wrong answer must not be possible.
- A key above the generator's declared level or below 0 is air, as in `run`: such keys are answered without calling the batch function, and the rest are passed to it in order.
- A generator without a batch function answers the map.
- If the batch function answers a list of another length than it was given, `run_column` answers the map instead.

- [ ] Write the failing tests first in `test/terrain_test.march`: the five rules above, with a test generator whose batch function counts its calls through its answer (for example a chunk whose uniform id is the batch's length), so "called once for a batch of three" is observable without state.
- [ ] Module notes: one paragraph stating the contract, `run_column(g, ks, seed)` equals the single runs key for key, and why (a column's shared work done once).
- [ ] `grep -rn "Gen(" lib viewer demo test` and update every pattern match on `Gen` for the fourth field (`Strata.World`, `Strata.Stream`, tests).

**Done:** the new tests pass, the gate passes, no other behaviour changes.

### Task 3: The viewer: `island_full` split into a column half and a chunk half

**Files:** `viewer/lib/strata_viewer.march`, `viewer/test/island_test.march`.

**Interfaces — Produces (all in `StrataViewer`):**
```
type IslandCol = IslandCol(...)   -- what island_full computes before it first uses cy: see below
pfn island_col(a, d, scale, lvl, cx, cz, seed) : IslandCol
pfn island_of(col : IslandCol, a, d, scale, lvl, cx, cy, cz, seed) : Strata.Chunk.Chunk
pfn island_full(a, d, scale, lvl, cx, cy, cz, seed) : Strata.Chunk.Chunk    -- now island_of(island_col(..), ..)
fn island_column_chunks(a, d, scale, ks : List(Strata.Coord.Key), seed) : List(Strata.Chunk.Chunk)
fn island_gen(a, d, scale) : Strata.Terrain.Gen                              -- now also with_column(.., island_column_chunks)
```

`IslandCol` holds exactly the values of `island_full` that do not depend on `cy`. Read the function and move, do not rewrite: today that is the `Area` (padded H, calm, ground, lake and river lines), the lifted H, the five inner 32 x 32 arrays, the entrance field, the cave-factor array, the settings unpacked from the `Gp` (`Cv`, sea and material lines), and the tree list with the lowest H it is gated on. The lattice fields, the fills, the density and everything after stay in `island_of`, which reads the arrays from the `IslandCol` once at its top (one `match`), never through an accessor in a loop (GAPS S15).

`island_column_chunks` walks `ks` once. For each key it applies what `island_level` applies to a single key, in the same order: `beyond` (open sea), the level-0 path with its early-out (`island_decorated`'s test), the coarse path with `island_early_lift`. The `IslandCol` is made at the first key that is not an early-out and reused for the rest; a column of early-outs never makes one. The trees' gate on cy (a chunk wholly under the lowest tree base holds none) stays per key.

- [ ] Write the failing test first in `viewer/test/island_test.march`, as a fixed loop (not `Check.all`): for the 12 keys whose hashes are stored and for 30 more columns listed in the test (include one beyond the atlas, one on the slope site, one holding the test lake, levels 0, 1 and 2), `Strata.Terrain.run_column(gen, keys cy - 3 .. cy + 3, 7)` equals the seven `Strata.Terrain.run` results by `Strata.Chunk.hash`, in order; and the same with the keys given top-down.
- [ ] Make the split. `island_stamps`, `island_terrain`, `island_full_chunk` and every other caller of `island_full` are untouched.
- [ ] The twelve stored hashes and all goldens pass unchanged. `STRATA_CHUNK_TIMES=1` still times single chunks through `Strata.Terrain.run` (it measures the unbatched path; say so in its printed line).

**Done:** the new test passes; the gate passes; `bench/chunk_stages/run.sh 1` still reports about 4.05 ms for `island_full` (the single path costs what it did; a difference over 3% is a regression to explain).

### Task 4: `Strata.Stream`: the keys of a column in one task

**Files:** `lib/strata/stream.march`, `test/stream_test.march`.

**Interfaces — Produces:**
```
Strata.Stream.with_batch(c : Config, cap : Int) : Config      -- at most cap keys a generation task; 1 is today's behaviour; below 1 is 1
Strata.Stream.batch(c : Config) : Int
```
`config` and `config_levels` keep their signatures and give a cap of 1, so every existing caller and test is unchanged until it asks.

Inside: `GenTask` becomes `GenTask(List((Key, Int)), Task(List(Made)))`, each key with its stamp. `send_gens`, on reaching a key it would start, also takes from the rest of the queue the keys that are `Strata.Terrain.same_column` with it and pass the same tests (wanted, not loaded unless stale, not waiting, not running), until the batch holds `cap` keys or `room` is used up; `room` counts keys, not tasks, so a tick starts no more chunks than it does today. A stale key (a stamp in `rd`) whose level is above 0 is always a batch of one, with its `fines`. `make` runs `Strata.Terrain.run_column` and hashes each chunk. `await_gens` turns a finished task into one `GenRes` per key, in the batch's order; from there `apply_gens` is untouched. `running(out, k)` looks inside batches.

- [ ] Write the failing tests first in `test/stream_test.march`, on a test generator with a batch function that marks how it was called (as in Task 2):
  - with the cap at 4, a column of 7 wanted keys is made by 2 tasks (4 and 3) and all 7 chunks load;
  - with the cap at 1, the batch function is called with single keys (today's behaviour);
  - the world hash after settling is the same at caps 1, 4 and 8, with one focus and with two;
  - a stale coarse key is regenerated alone and its edits are coarsened over it (the existing stale tests, run at cap 4);
  - a focus moved away while a batch runs: none of its chunks is inserted, and nothing is left in the waiting list;
  - `regenerate` while a batch runs drops the whole batch.
- [ ] Every existing stream test passes at the default cap of 1 without edits, and again with the suite's helper config changed to cap 4 (run it both ways; keep the default).
- [ ] The viewer and the demo call `with_batch(.., cap)` with the cap from Task 1; `STRATA_BATCH=n` overrides it in the viewer (for Task 5).

**Done:** the new tests pass; the gate passes with the viewer at the chosen cap; goldens unchanged.

### Task 5: Measure, choose the cap, record

**Files:** `bench/chunk_stages/lib/chunk_stages.march`, `bench/chunk_stages/run.sh`, `bench/chunk_stages/README.md`, `docs/2026-11-12-chunk-performance-findings.md`, `RESULTS.md`, `docs/generation-ledger.md` (or this plan's own ledger, as the controller keeps it).

- [ ] `bench/chunk_stages`: two new stages over the 200 chunk columns. For each, the keys cy - 8 .. cy + 8 that are not early-outs: (a) made one by one with `Strata.Terrain.run`; (b) made with `Strata.Terrain.run_column`. Report CPU us and instructions per full chunk for each.
- [ ] The viewer's start-up stream (`STRATA_FRAMES=2`, the "chunks streamed" line) at `STRATA_BATCH` 1, 2, 4 and 8: total CPU time of the run from `/usr/bin/time -l`, three runs each.
- [ ] The soak (`scripts/soak.sh`) at the chosen cap and at 1: worst and mean frame time, only if the load average is under 8; otherwise recorded as not judged.
- [ ] The longest generation task at the chosen cap: the batched stage's worst column, CPU ms.
- [ ] Set the viewer's and demo's cap to the value that minimises start-up CPU without the longest task passing 16 ms; record the choice and the numbers. Update the findings' addendum and `RESULTS.md`; add the measured Task 1 histograms to the design document's "saving" paragraph in place of its estimates.

**Done:** the numbers are in the repository; the gate passes; the design document says whether shape B is still worth doing, from the share of single-key batches in Task 1's climb script and what they cost.

---

## After this plan

Not part of it, in the order the findings suggest: the river damp bucketed by atlas cell (0.57 ms a column), the lake-rim mask (0.20 ms), the detail noise as one `field2` call; then shape B if Task 5 says so.

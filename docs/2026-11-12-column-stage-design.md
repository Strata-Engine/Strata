# The column stage: making a chunk column's shared work once

12 November 2026. A design for the first restructuring in
`docs/2026-11-12-chunk-performance-findings.md`. Not started. The owner
chose shape A on 12 November; the plan is
`docs/2026-11-12-column-stage-plan.md`.

## The problem

An island surface chunk costs 4.05 ms of CPU on March 07b58d065. 2.37 ms of
it is `area_lv`: the atlas window, the column heights over a 66 x 66 patch,
the talus field, the thermal relax, the site's beard. Its result (relaxed
H, calm, ground and water lines) depends on the chunk column (cx, cz) and
the level, not on cy. Every chunk generated in full repeats it, and 4.9
chunks of a column are generated in full (981 over the 200 columns the
timing uses).

Three more pieces depend on the column alone and are repeated the same way:
the early-out bounds (`quick_bounds`, 4 us), the tree placements
(`island_trees`, 0.07 ms), and the entrance field and cave factors (0.06
ms). The stages that do depend on cy (lattice fields and fills, density,
`drop_islands`, scan, `finish_go`, `from_flat`, blits) sum to 1.6 ms.

So a chunk is 2.45 ms of column work and 1.6 ms of its own, and a column of
k full chunks costs k x 4.05 ms where 2.45 + k x 1.6 would do: 10.5 ms
instead of 19.8 at k = 4.9, or 2.1 ms a chunk instead of 4.05.

## What stands in the way

`Strata.Terrain`'s contract is a pure function from one key and a seed to
one chunk, run by `Strata.Stream` as one task a key. A generator has
nowhere to keep a column's result, and nothing tells it that the next key
is the same column. March has no module-level state, and a value shared by
tasks is slow to read through an accessor (GAPS S15: 692 ns a read from 8
tasks), though not when its arrays are unpacked once before the loop (1.3
ns).

## Shape A: a column's chunks made in one task (recommended)

The generator may carry a batch function beside its chunk function:

```
Strata.Terrain.with_column(g : Gen, f : List(Key) -> Int -> List(Chunk)) : Gen
Strata.Terrain.run_column(g : Gen, ks : List(Key), seed : Int) : List(Chunk)
```

`ks` are keys of one chunk column and one level; the answer is their
chunks in the same order. A generator without one gets
`List.map(ks, fn k -> run(g, k, seed))`. The contract gains one sentence:
`run_column(g, ks, seed)` equals that map, chunk for chunk. That is a
property test, and it keeps everything that rests on purity (a chunk
dropped and made again, stamps, coarsening, the determinism tests) exactly
as it is.

`Strata.Stream.send_gens` takes the key at the head of the queue and, with
it, every other key of the same column and level that it would also start
this tick (wanted, not loaded, not waiting, not running), up to a cap; one
task makes them all and answers a list of `Made`. `apply_gens` already
holds results in a waiting list and applies `apply_budget` a tick, so a
batch's chunks go through it one by one. A stale coarse key (an edit's
ancestor) is a batch of one, with its `fines`, as now.

In the viewer, `island_full` splits at the line where it calls `area_lv`:
`island_column(a, d, scale, lvl, cx, cz, seed)` makes the area, the inner
32 x 32 copies, the entrance field, the cave factors, the tree list and the
bounds; `island_chunk(col, cy)` does the rest. `island_level` for one key
is `island_chunk(island_column(..), cy)`, so the single-key path and every
existing test and golden are untouched by construction.

Cost and risk:

- No shared state, no cache, no memory held, no S15 exposure: the column's
  arrays live in one task.
- A task is longer: a column of five full chunks is about 10.5 ms of CPU
  where the longest task today is about 4. `tick` waits for tasks started
  two ticks ago, 33 ms at 60 frames a second, so there is room, but the
  ledger records hard chunks at 14 ms under load. The cap on a batch (4
  keys to start with) bounds it; the cap and the worker count are tuned by
  measurement.
- Fewer, larger tasks spread less evenly over the workers at start-up.
- It saves nothing when a column's chunks are asked for one at a time: a
  focus rising one chunk asks for one new layer, one key a column. Then a
  chunk costs what it does today.

Saving, where a column arrives whole (start-up, and columns entering the
box at its edge, which is all of horizontal flight): 4.05 to about 2.1 ms a
full chunk at k = 4.9; 2.3 ms at k = 3.5.

## Shape B: a column cache in the stream

The generator is two functions, `column : (cx, cz, level) -> seed ->
Column` and `chunk : Column -> Key -> seed -> Chunk`, with
`Strata.Terrain.Column` a bag of native arrays the game packs as it likes.
The stream keeps columns in a store keyed by (cx, cz, level) for as long as
any chunk of the column is wanted, makes a missing one first, and hands it
to each chunk task, which unpacks its arrays once.

It saves in every case, including one new layer a column, and it is the
only shape in which neighbouring columns could share 32 x 32 tiles and the
tree pad stop being relaxed again (2.37 to 1.27 ms a column). It costs
more: a second kind of task and a stage in the pipeline (a new column's
first chunk arrives two ticks later, or the first chunk task makes the
column and the others wait a round); a cache with a lifetime and a memory
bound (about 16 KB a column for the inner arrays and the tree list, 7 MB
for a 21 x 21 box, more with levels); a new public type; and values shared
between tasks, which is where S15 bites if anyone later reads one through
an accessor in a loop.

## Recommendation

**Shape A first.** It is a small change to two library files and a split of
one viewer function, it cannot change any chunk, and it takes the common
case from 4.05 to about 2.1 ms. Measure how often a batch is one key in a
real flight before deciding whether B's extra case is worth its machinery.
A's viewer split (`island_column` / `island_chunk`) is exactly the split B
needs, so nothing done for A is thrown away.

**Decided (owner, 12 November):** shape A now; B only if the plan's
measurements ask for it.

## Independent of the shape

Inside the column stage, on today's figures:

- River damp: 0.57 ms. Every column walks the window's whole river list.
  Bucket the samples by atlas cell so a column far from water walks none.
- Lake-rim search: 0.20 ms. 36 samples a column in any window that holds a
  lake. A per-cell mask of "a lake sample within reach" skips the rest.
- Detail noise: 4356 `sample2` calls across the FFI; one `field2` over the
  patch.

Together about 0.8 ms a column. After shape A that is 0.2 ms a chunk, so
they come second.

## Outline of the work for shape A

Each task ends with `scripts/verify.sh` printing PASS; no golden and no
stored hash may change.

1. **Measure first.** A counter in a scratch stream bench: over the
   viewer's start-up and a scripted flight (level, then climbing), how many
   keys of one column `send_gens` would start together. Record the
   distribution; it sets the batch cap and says how much B would add.
2. **`Strata.Terrain`: `with_column`, `run_column`.** The default built
   from `run`. Tests: a generator without a column function answers the
   map; one with it is called once for a batch; keys of other columns or
   levels in one call are refused (an empty answer), since the stream never
   sends them.
3. **Viewer: split `island_full`** into `island_column` and
   `island_chunk`, with `island_level` as their composition and
   `island_gen` carrying the batch function. Test: for 40 columns, levels 0
   to 2, `run_column` over cy - 3 .. cy + 3 equals the seven single runs by
   `Chunk.hash`. The twelve stored chunk hashes and every golden stay.
4. **`Strata.Stream`: batches in `send_gens`,** a list of `Made` a task,
   the cap in `Config`. Tests: the stream's world hash after settling is
   the same with the cap at 1 and at 8, on the test generator and on the
   island; a stale coarse key is still a batch of one; unloading a column
   mid-batch drops its results as it drops a single result today.
5. **Measure and record.** `bench/chunk_stages` gains a column mode (one
   column's full chunks, batched and not); the viewer's start-up stream
   time and the soak's worst frame, before and after; the findings
   document and RESULTS updated; the cap chosen.

## Not in scope

Shape B; shared 32 x 32 tiles; the river, rim and noise changes above;
anything in `Strata.Fluid`, the renderer or the population core.

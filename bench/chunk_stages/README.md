# chunk_stages

Where the CPU time of one island surface chunk goes, stage by stage.

```sh
bench/chunk_stages/build.sh     # builds the harness
bench/chunk_stages/run.sh       # every stage: CPU us, instructions, cycles a chunk
bench/chunk_stages/run.sh 5 11  # only the thermal relax and the density
```

The viewer must have run once, so that `viewer/.march/atlas` holds the island
atlas. The stages are the pieces of `StrataViewer.island_full`, called in its
order with its arguments, over the 200 surface chunks `STRATA_CHUNK_TIMES`
uses. The generator's functions are private to the viewer's module, so
`build.sh` copies `viewer/lib/*.march` into `lib/` with `pfn` made `fn`
(those copies are generated and not committed); `lib/chunk_stages.march` is
the harness.

A row is a `REPS=10` run minus a `REPS=0` run (which only prepares each
stage's inputs), over 2000 chunk-stages, from `/usr/bin/time -l`: CPU time
(user + system; 5 us resolution a chunk), instructions retired and cycles.
No wall-clock time is used.

Stages 26 to 28 make the columns with the window's river list emptied, its
lake flag cleared, or both, to size the river damp and the lake-rim search.
`STAGE=30` and `STAGE=31` (run the binary directly, `REPS=0`) print the
river samples per window and how many chunks of each chunk column are
generated in full.

`build.sh` builds with the compiler `scripts/env.sh` names. (Until 12
November 2026 forge silently used March 0.5.0 from `~/.march/current`; the
findings' first tables compare the two.)

`STAGE=32` (run the binary directly, `REPS=0`) counts how many keys of a
chunk column would be generated together if the stream batched a column
(`docs/2026-11-12-column-stage-plan.md`, Task 1): for the viewer's boxes
about its overview pose, per column, the keys that are generated in full.
A batch is the keys of a column that become wanted at once: a whole column
of the box at start-up or entering it in level flight, one key a column
when the focus climbs or sinks a chunk. Measured on 12 November 2026:

| level | case | columns | full keys a column (0, 1, 2, ...) | mean where any |
|---|---|---:|---|---:|
| 0 | start-up, 7 keys a column | 441 | 0 0 8 265 153 15 0 0 | 3.40 |
| 0 | flight, 13 chunks along +x | 273 | 0 0 147 126 0 0 0 0 | 2.46 |
| 0 | climb one chunk (1 new key) | 441 | 441 0 | none full |
| 1 | start-up, 3 keys a column | 441 | 2 375 64 0 | 1.15 |
| 2 | start-up | 441 | 117 297 27 0 | 1.08 |
| 3 | start-up | 441 | 341 0 100 0 | 2.00 |
| 4 | start-up | 441 | 405 0 36 0 | 2.00 |

So at level 0 a batch holds 3.4 full chunks at start-up and 2.5 in flight
toward the coast; a cap of 4 keys covers 97% of the start-up columns whole.
The layer a climb adds is all air (early-outs), so the one-key-a-column case
costs nothing there; a descent was not counted. Coarse levels have little to
share (1.1 full chunks a column at levels 1 and 2).

Stages 36 to 39 time start-up rather than a chunk; run the binary
directly, a `REPS=n` run minus a `REPS=0` run under `/usr/bin/time -l`,
divided by n (`QUARTER=1` makes the world 0.25 m a voxel): 36 the island's
population alone (`pop_of`: island.pop's `{name}`s substituted, read,
frozen, fitted, planned), 37 the whole Decor (`decor_of`: the 12 tree
templates grown, the ruins read and fitted, then 36), 38 the tree templates
alone, 39 the `{name}`s worked out and substituted alone. Figures:
`RESULTS.md`, Population core, Task 10.

Results and what they mean: `docs/2026-11-12-chunk-performance-findings.md`.

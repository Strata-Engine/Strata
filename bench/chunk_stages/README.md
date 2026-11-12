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

Results and what they mean: `docs/2026-11-12-chunk-performance-findings.md`.

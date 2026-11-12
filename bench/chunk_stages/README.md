# chunk_stages

Where the CPU time of one island surface chunk goes, stage by stage.

```sh
bench/chunk_stages/build.sh     # builds the harness with both compilers
bench/chunk_stages/run.sh       # every stage: CPU us, instructions, cycles a chunk
bench/chunk_stages/run.sh 5 11  # only the thermal relax and the density
COMPILER=default bench/chunk_stages/run.sh   # the binary `forge build` makes by default
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

`build.sh` makes two binaries because `forge build` does not use the `march`
that `scripts/env.sh` puts on `PATH`: it puts the toolchain `~/.march/current`
names (March 0.5.0 here) ahead of it. `out/chunk_stages_default` is that
build; `out/chunk_stages_main` is built with `MARCH_HOME` pointing at an
empty directory, which makes forge fall back to `PATH` (march-main, 0.7.0).
`run.sh` uses the march-main binary unless `COMPILER=default`.

Results and what they mean: `docs/2026-11-12-chunk-performance-findings.md`.

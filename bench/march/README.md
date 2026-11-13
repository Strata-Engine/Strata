# Reduced March cases

Each directory holds one thing the island generator does in a hot loop, as a
small March program and the same program in C, with a `run.sh` that builds
both and prints CPU time and instructions per unit of work.

```sh
bench/march/float_if/run.sh     # one case
bench/march/run_all.sh          # all of them
```

Numbers are CPU time (user + system) and instructions retired from
`/usr/bin/time -l`, as the difference between a `REPS=n` run and a `REPS=0`
run, so the March runtime's start-up is not counted and a loaded machine does
not matter. `out/<case>.ll` is the LLVM IR March emits for the case.

The compiler is the `march` on `PATH` after `scripts/env.sh` (march-main,
0.7.0 at 1a4251bc8). `MARCH=$HOME/.march/versions/v0.5.0/bin/march
bench/march/run_all.sh` runs the cases with the compiler `forge build`
actually uses on this machine (see the findings); every ratio below holds on
both, except that 0.5.0 has no `int_wrapping_mul` (`hash64` does not build)
and calls a runtime helper for `/` and `%` (`divmod`).

| case | what it shows | March | C | ratio |
|---|---|---|---|---|
| `float_if` | an `if` whose value is a Float allocates | 22.3 ns | 0.55 ns | 40 |
| `voxel_loop` | array reads and writes are runtime calls | 3.70 ns | 0.53 ns | 7 |
| `lattice_fill` | the same, in the trilinear fill | 10.8 ns | 1.43 ns | 7.5 |
| `callee_fill` | a callee cannot fill its caller's array (S18) | result lost | | |
| `shared_tasks` | a record read from 8 tasks (S15) | 778 ns | 0.17 ns | 4600 |
| `hash64` | 32-bit-lane hash for want of wrapping multiply | 27.8 ns | 1.54 ns | 18 |
| `closure` | a closure called per element | 4.65 ns | 1.19 ns | 3.9 |
| `short_circuit` | `&&` runs its right side (S16) | 18.2 ns | 0.28 ns | 65 |
| `list_walk` | a List of Float tuples walked per column | 11.0 ns | 2.2 ns | 5 |
| `divmod` | `/` and `%` by a constant | 2.44 ns | 0.85 ns | 2.9 |
| `stage_thermal` | Strata.Thermal.relax, whole stage | 110 ns | 3.2 ns | 34 |
| `stage_density` | the viewer's per-voxel density, whole stage | 71 to 85 ns | 2.7 ns | 26 to 31 |

`task_shared_list` is different: a crash, not a ratio. A list captured by a
task and still read by its spawner kills the program in 18 of 20 runs.

The findings are in `docs/2026-11-12-chunk-performance-findings.md`.

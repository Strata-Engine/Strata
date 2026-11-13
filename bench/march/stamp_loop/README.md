# stamp_loop

The loop of `Strata.Template.blit` without its probability draws and without
the chunk: a 9 x 12 x 9 template of block ids stamped, turned, into a flat
32 x 32 x 32 array, holes skipped. The March is blit's own loop copied (three
nested tail calls of fourteen arguments, the template a seven-field
constructor read through accessors, the turn undone per voxel by a function
answering a tuple); the C is the three `for` loops one would write.
`stamp_two.march` is the same program with the template type, its accessors
and the loop in a second module, `stamp_lib.march`, as a game reaches
`Strata.Template.blit` in the library.

Instructions a template voxel visited (CPU time moves with the machine's
load, 22 to 50 ns for the March rows; instructions move a little too, since
a loaded machine preempts more):

| | instructions | CPU |
|---|---|---|
| March, one module | 545 to 587 | 29 to 50 ns |
| the same, the turn as two Int-valued functions | 386 to 395 | 22 to 37 ns |
| March, the loop in a second module | 829 to 834 | 46 ns |
| the same, the turn as two Int-valued functions | 661 | 40 ns |
| C | 14.4 | 1.0 to 1.3 ns |

Two things, each about a third of the March cost:

- **The tuple.** `unturn` answers an `(Int, Int)`, which is allocated and
  freed per voxel: 160 to 190 instructions.
- **The module boundary.** In one module the accessors borrow the template:
  `t_sx` is 24 instructions and touches no reference count. Compiled as a
  second module the same accessor takes the template owned: it lowers the
  template's count (the caller raised it), raises the count of each of its
  three arrays and lowers them again, 157 instructions and seven
  reference-count calls (`grep -c rc_ ` on `define i64 @StampLib.t_sx` in
  the IR against `@t_sx` in `out/stamp_loop.ll`). `unturn` and `drawn` call
  such accessors about four times a voxel (`t_sx` or `t_sz` in the turn,
  `t_sx`, `t_sy` and `t_ids` in the read): about 245 instructions more.

In both builds every turn of `blit_x` also calls `unturn` and `drawn`
(neither is inlined), writes through `native_i32_arr_set` (about 45
instructions a voxel visited), checks each add and multiply for overflow
and polls the scheduler. C keeps the template's sizes in registers and the
body is a handful of loads and one store.

In the library this is what `Strata.Template.blit` pays before its draws and
its chunk write. `bench/populate_fields` mode 12 is blit alone there (the
same template, whole): 1590 to 1620 instructions a voxel visited, of which
the loop is 808 (measured with the draws and the write switched off in a
scratch build), against this case's 829 to 834 with its flat write. Mode 8
(a chunk with 27 stamps, 15144 voxels visited) spends 11.8 M of its 24.1 M
instructions in the loop, 781 a voxel visited.

Run `./run.sh`.

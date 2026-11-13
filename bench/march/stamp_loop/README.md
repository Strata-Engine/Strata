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
| March, one module | 545 to 591 | 27 to 50 ns |
| the same, the turn as two Int-valued functions | 386 to 395 | 21 to 37 ns |
| March, the loop in a second module | 815 to 836 | 41 to 46 ns |
| the same, the turn as two Int-valued functions | 661 to 663 | 37 to 42 ns |
| C | 14.4 | 0.8 to 1.3 ns |

Two things:

- **The tuple.** `unturn` answers an `(Int, Int)`, which is allocated and
  freed per voxel: 160 to 190 instructions, about 30% of the one-module
  cost and about 20% of the second-module one.
- **The module boundary: 245 to 290 instructions a voxel**, the gap between
  the second-module rows and the one-module rows above. It is the three
  size accessors. In a scratch build with `t_sx(t)`, `t_sy(t)` and
  `t_sz(t)` written as the constants 9, 12 and 9 (`t_ids` kept), the two
  builds are the same (constants also let the compiler fold arithmetic,
  so both drop; the gap is what goes): 376 to 416 instructions a voxel in either with the
  tuple, 246 to 248 in either with the two Int calls. With the reads kept
  they are 570 to 579 and 386 to 387 in one module, 819 to 836 and 661 in
  the second. `unturn` and `drawn` make three size reads a voxel (on
  average one in the turn, `t_sx` and `t_sy` in the read; the same with the
  two Int calls), so each costs about 85 to 92 instructions more across the
  boundary than within one module, as executed. Across a module boundary
  an accessor takes the template owned and adjusts the reference counts of
  the template and its three arrays (seven `rc_` calls in the IR of
  `StampLib.t_sx`, none in `@t_sx` of `out/stamp_loop.ll`). The per-call
  cost depends on the caller: in a bare loop that keeps the template
  (`acc_one`, `acc_two`, the last rows of `./run.sh`) one `t_sx` a turn adds
  about 5 instructions in one module and 127 in a second, three accessors
  127 each. `t_ids` across the boundary costs nothing measurable here.
  (An earlier version of this file priced the accessor at 157 and 24
  instructions, not from instructions run; four such calls a voxel would
  have been about 530 more, twice the gap. Those figures are withdrawn.)

In both builds every turn of `blit_x` also calls `unturn` and `drawn`
(neither is inlined), writes through `native_i32_arr_set` (about 45
instructions a voxel visited), checks each add and multiply for overflow
and polls the scheduler. C keeps the template's sizes in registers and the
body is a handful of loads and one store.

In the library this is what `Strata.Template.blit` pays before its draws and
its chunk write. `bench/populate_fields` mode 12 is blit alone there (the
same template, whole): 1590 to 1620 instructions a voxel visited, of which
the loop is 808 (measured with the draws and the write switched off in a
scratch build), against this case's 815 to 836 with its flat write. Mode 8
(a chunk with 27 stamps, 15144 voxels visited) spends 11.8 M of its 24.1 M
instructions in the loop, 781 a voxel visited.

Run `./run.sh`.

# divmod

Integer `/` and `%` by a constant, four a voxel to recover (x, y, z) from a
flat index. march-main emits `sdiv` and `srem` behind an inline zero test, so
LLVM turns a constant divisor into shifts: 2.44 ns and 29 instructions a
voxel against C's 0.85 ns and 20, 2.9 times (C's loop also vectorises with
`>>` and `&`: 0.24 ns). March 0.5.0, which `forge build` uses on this
machine, calls `march_checked_div_op` and `march_checked_mod_op` in the
runtime for every `/` and `%`, constant or not (2.72 ns here; 5.8% of the
island chunk's samples against 2.0% for the march-main build). Writing
`int_shr` and `int_and` by hand gives 1.83 ns on march-main and 1.19 ns on
0.5.0. A small gap on march-main; listed because the profile of the build
Strata actually runs shows it.

Run `./run.sh`.

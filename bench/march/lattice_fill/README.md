# lattice_fill

Strata.Lattice's trilinear fill (405 samples to 32768 voxels) written in
March, the loop that was moved to C as `sl_fill` (GAPS S18 on this branch).
With the array made in the function and returned, so every write is in place,
the March fill takes 10.8 ns and 274 instructions a voxel: 353 us a chunk
field, against 1.43 ns (47 us) for the same per-voxel loop in C, 7.5 times,
and 0.25 ns (8 us) for C's separable kernel. There is no Float-valued `if`
here; the cost is the nine out-of-line array calls a voxel
(`native_f32_arr_get` eight times and `native_f32_arr_set`), three
`march_int_to_float` calls, the preemption-flag load and the stack save and
restore every iteration (see `../voxel_loop`). Writing the index arithmetic
with shifts instead of `/` and `%` gains 6% on march-main (which emits `sdiv`
and `srem` inline) and 40% on March 0.5.0 (which calls
`march_checked_div_op`).

Run `./run.sh`.

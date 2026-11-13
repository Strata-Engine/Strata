# callee_fill

GAPS S18, still true on march-main (1a4251bc8) and on 0.5.0: a function that
is handed an array and writes into it does not change the caller's array.
The caller still holds the array, so the first `NativeArray.set_f32` in the
callee finds it held twice and writes to a fresh copy
(`native_f32_arr_set`'s copy-on-write path in `runtime/march_runtime.c`); the
copy is dropped when the callee returns. `fill_into(out) : Int` leaves `out`
all zeros (the run prints 0 where the returning form and C print 300). The
two forms cost the same, 3.8 ns an element (one 128 KB copy aside), against
0.08 ns for C's vectorised store, so the gap is one of meaning, not speed:
an out-parameter API cannot be written in March, only "return the array".
The 47-times cost of the plain store loop is the array-call overhead of
`../voxel_loop`.

Run `./run.sh`.

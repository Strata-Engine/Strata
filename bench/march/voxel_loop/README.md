# voxel_loop

A loop over a chunk's 32768 voxels that reads two arrays and writes one, the
density without its caves and with no Float-valued `if`: 3.70 ns and 77
instructions a voxel in March against 0.53 ns and 16 instructions in C, 7
times. In the compiled loop (`objdump -d --disassemble-symbols=_dens
out/voxel_loop_march`) every iteration makes four calls into the separately
compiled runtime, `native_f32_arr_get` twice, `march_int_to_float`, and
`native_f32_arr_set`; each array call checks its bounds, and the set also
tests that the array is a heap value held once before writing in place. The
iteration also reloads the scheduler's `march_preempt_request` flag (a
volatile load, so the loop cannot be vectorised) and saves and restores the
stack pointer. Integers are shifted left and right by one around each
overflow-checked add (63-bit Ints). C's loop is 12 instructions with the
loads, the convert and the store inline, and vectorises.

Run `./run.sh`.

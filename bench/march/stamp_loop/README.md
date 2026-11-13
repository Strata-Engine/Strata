# stamp_loop

The loop of `Strata.Template.blit` without its probability draws and without
the chunk: a 9 x 12 x 9 template of block ids stamped, turned, into a flat
32 x 32 x 32 array, holes skipped. The March is blit's own loop copied (three
nested tail calls of fourteen arguments, the template a seven-field
constructor read through accessors, the turn undone per voxel by a function
answering a tuple); the C is the three `for` loops one would write.

One template voxel costs 31 to 50 ns (by the machine's load) and 582
instructions in March against 1.3 ns and 14 instructions in C, 41 times by
instructions. With the turn undone by two functions that answer an Int each
(VARIANT=1) it is 395 instructions: the `(Int, Int)` a function answers is
allocated and freed per voxel, about 190 instructions. In the compiled
`blit_x` (`objdump -d --disassemble-symbols=_blit_x out/stamp_loop_march`)
every turn of the loop also calls `unturn` and `drawn` (neither is inlined),
raises and lowers the template's reference count around them
(`march_incrc_local` twice, `march_decrc_local`, a `__drop$Stamp` path),
writes through `native_i32_arr_set`, checks each add and multiply for
overflow, and polls the scheduler. C keeps the template's sizes in registers
and the whole body is a handful of loads and one store.

This is what a scatter layer pays per template voxel in
`Strata.Populate.place_with` before the two hashes of the probability draw
(see ../hash64) and the chunk write (see ../voxel_loop):
bench/populate_fields mode 8 spends 13.7 M of its 23.9 M instructions here.

Run `./run.sh`.

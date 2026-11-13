# list_walk

The island's river damp walks a `List((Float, Float, Float, Float))` of
river samples for every column of the patch. With no Float-valued `if` in
either form, a sample visit costs 11.0 ns and 165 instructions from the list
against 6.1 ns from a flat `NativeF32Arr` holding the same floats, and 2.2 ns
in C from either a linked list or a flat array: 5 times C, and the list
itself is 1.8 times the flat array. Walking a list the caller still holds
costs, per cell (`damp_list` in `out/list_walk.ll`): a decrement of the
cell, increments of its head and tail, a decrement of the tuple, increments
of its four boxed floats, three `march_unbox_float` calls, and the matching
drops. The flat form's remaining 2.8 times is out-of-line helpers:
`native_f32_arr_get` three times, `march_math_sqrt`, `march_float_abs` three
times and `march_checked_fdiv` are all calls into the runtime where C has
`fsqrt`, `fabs` and `fdiv` inline.

Run `./run.sh`.

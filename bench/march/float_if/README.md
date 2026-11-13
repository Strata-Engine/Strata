# float_if

An `if` (or `match`) whose value is a Float allocates. `Llvm_case.emit_case`
(march `lib/tir/llvm_case.ml`) types the result slot by
`predicted_unboxed_join`, which knows Int, Bool and unboxed structs but has
no case for Float, so the slot is a `ptr`: each Float arm is boxed with
`march_alloc_float` (a `march_alloc`, 24 bytes from mimalloc) and the merge
block unboxes it and frees it. `fmax`, `fmin`, `clamp01` and every
`if e > 0.0 do e else 0.0 end` are one allocation and one free each. Here the
largest clamped step along a row of 4096 floats, two such `if`s an element,
costs 22.3 ns and 491 instructions an element in March against 0.55 ns and 11
instructions for C's `?:`: 40 times by time. Written with no Float-valued
`if` (`0.5 * (a + b + abs(a - b))`) the March loop takes 3.66 ns; the same
arithmetic in C takes 2.81 ns. So one Float-valued `if` costs about 10 ns and
220 instructions.

Run `./run.sh`; in `out/float_if.ll` see `with_if`: `call ptr
@march_alloc_float` in both arms, then `@march_unbox_float` and
`@__march_rc_decrc_local` at `case_merge`.

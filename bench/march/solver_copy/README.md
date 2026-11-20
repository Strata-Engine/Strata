# solver_copy: a write that reads the array it writes copies it

Reduced from `Strata.Contact.apply` (met 2026-11-19 in the physics test
"scale does not change the motion"). A contact pass threads a contact array
and a body-state array through `match f(...) do (a, b) -> ...`, as
`Contact.pass` and `solve_one` do. Each contact adds an impulse to one body's
velocity. `SIZE` is the state array's length in floats (32 a body).

The pattern that copies is a chain of writes whose later arguments read the
array as it was before the chain:

```
cset(cset(cset(st, i, cget(st, i) +. a), i + 1, cget(st, i + 1) +. b), i + 2, cget(st, i + 2) +. c)
```

The outer calls' `cget(st, ...)` read the original `st` after the inner
`cset` has written it. Under value semantics the old array must still exist
when it is read, so the compiler holds a second reference across the first
write (`__march_rc_incrc_local(st)` before the first `NativeArray.set_float`,
dropped after the last one; see `out/solver_copy.ll`, `apply_im`).
`native_float_arr_set` then finds the array shared and copies all of it,
every call. Reading first and writing after keeps each write in place:

```
let va = cget(st, i) +. a
let vb = cget(st, i + 1) +. b
let vc = cget(st, i + 2) +. c
cset(cset(cset(st, i, va), i + 1, vb), i + 2, vc)
```

Variants (`VARIANT`):

- 4: `apply` as Strata had it (reads in the chain), its only caller's read
  replaced by a constant
- 5: the same chain written inline in `solve_one`
- 6: the chain with the inverse mass read by the caller
- 7: one write of a value that does not read the array
- 8: `apply_read_first`: every read before the first write

Measured 2026-11-20, March `f04f0dc0c`, `REPS=1`, `SIZE=8192` (4 096
contacts), counting `native_arr_alloc` calls of 8 192 floats under lldb (the
initial `make_float` is one of them):

| variant | state arrays allocated |
|---|---|
| 4 | 5 087 to 8 002 |
| 5 | 9 799 |
| 6 | 6 576 |
| 7 | 1 |
| 8 | 1 |

The counts for 4 to 6 vary from run to run; 7 and 8 never copy. CPU for
`REPS=20` (81 920 contacts): variant 4 takes 0.00 s at `SIZE=64` and 0.11 s
at 8192; variant 8 takes 0.00 s at both.

Not a March regression: Strata's physics paid the same copies on March
0.5.0, `07b58d065`, `6c88ab912` and `f04f0dc0c`. It is what value semantics
asks for. A compiler could evaluate the pure `cget` arguments before the
write they follow, but March does not.

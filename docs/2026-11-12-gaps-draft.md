# Draft GAPS entries, 12 November 2026

For `GAPS.md`; not added there because its numbering differs between this
branch and main. New entries are lettered; give them the next free numbers.
Evidence and measurements: `docs/2026-11-12-chunk-performance-findings.md`.
Checked on march-main (0.7.0, 1a4251bc8) and on March 0.5.0, the compiler
`forge build` uses on this machine (entry A).

## A: `forge build` uses `~/.march/current`, not the `march` on PATH

`scripts/env.sh` puts march-main first on `PATH`, but forge prefixes every
compile with the bin directory of its resolved toolchain: a `.march-version`
pin, else the global `~/.march/current` (`Toolchain.path_prefix`). Here that
is `versions/v0.5.0`, so `forge build`, `forge test` and `forge run` compile
Strata with March 0.5.0 and its stdlib and runtime, while a bare `march`
from the same shell is 0.7.0. Seen in the binaries: the viewer calls
`march_checked_mod_op(x, 32)`, which only 0.5.0 emits. Entries marked
"reproduced from main" were checked with a compiler that does not build
Strata. Workaround: `export MARCH_HOME=<a directory with no current link>`
before forge (it then falls back to `PATH`), or a `.march-version` naming an
installed build of main. Not yet applied to `scripts/env.sh`.

## B: an `if` or `match` whose value is a Float allocates

`if a > b do a else b end` on Floats makes a heap box for its value and
frees it: `march_alloc_float` in each arm, `march_unbox_float` and a
reference-count decrement at the join. About 10 ns and 220 instructions for
each such `if`; a nested chain boxes once per level. `fmax`, `fmin`,
`clamp01` and `if e > 0.0 do e else 0.0 end` are all this. 22.3 ns against
0.55 ns in C for a loop with two of them (`bench/march/float_if`); about
half of an island chunk's CPU time; `Strata.Thermal.relax` runs 5 times
faster with none. Cause: `Llvm_case`'s `predicted_unboxed_join` types the
join slot `i64` for Int and Bool arms but has no case for Float, so the slot
is a `ptr`. An Int-valued `if` costs nothing. Workaround: arithmetic in
place of the choice (`0.5 * (a + b + abs(a - b))` for max), or an Int-valued
`if` giving 0 or 1 and `int_to_float` of it. Both on march-main and 0.5.0.

## C: native array reads and writes are calls into the runtime

`NativeArray.get_f32`, `set_f32` and the other element functions compile to
calls to `native_f32_arr_get` and the like in `march_runtime.c`, which is a
separate compilation unit: no inlining, a bounds check per call, and on a
write a test that the array is held once. A loop also reloads the
scheduler's preemption flag (a volatile load) and saves and restores the
stack pointer every turn, so nothing vectorises. A loop reading two arrays
and writing one: 3.70 ns a voxel against 0.53 ns in C
(`bench/march/voxel_loop`); the lattice fill 10.8 ns against 1.43 ns
(`bench/march/lattice_fill`). 9% of an island chunk is inside these calls.
No workaround in March; a C kernel through the FFI where it matters (as
`sl_fill`). The March session on inlined array access is working on it.

## D: float helpers are calls into the runtime

`int_to_float`, `Math.sqrt`, `Math.abs`, `Math.floor`, `Math.tan` and `/.`
on Floats compile to `march_int_to_float`, `march_math_sqrt`,
`march_float_abs`, `march_checked_fdiv` and so on: a call where C has one
instruction. 2% of an island chunk; the flat-array rows of
`bench/march/list_walk` (6.1 ns against 2.2 ns) are mostly this. No
workaround.

## E: walking a list, or reading a record's field, updates reference counts

A function that walks a `List` its caller still holds pays, per cell, a
decrement of the cell, increments of its head and tail and of every boxed
field of a tuple in it, and the matching drops; Floats in a tuple or list
are boxed. 11.0 ns a cell against 6.1 ns for the same floats in a flat
`NativeF32Arr` and 2.2 ns in C (`bench/march/list_walk`). The same
mechanism makes a field accessor (`match t do T(v, ...) -> v end`) cost four
count updates when the record stays alive, which is S15's cause (see the
revised S15). Workaround: keep per-element data in native arrays; unpack a
record's fields once, outside the loop.

## F: a closure called per element costs about twice a direct call

A rule passed as a function value and called for each of 32768 voxels: 4.65
ns an element against 2.29 ns calling the same function by name
(`bench/march/closure`). The loop is specialised for the lambda, but its
`apply` is still called out of line with erased arguments, reloading its
captured values each call. Small (under 1% of an island chunk). Workaround:
none worth the API; noted for `Strata.Surface.scan_wet`.

## Revisions to existing entries

**S15** (a value shared by worker tasks is slow in a hot loop). Mechanism
found and measured (`bench/march/shared_tasks`): a field read through an
accessor is four reference-count updates, one of them an atomic add on the
field; with 8 tasks reading one record that is 778 ns of CPU a read against
4.84 ns for 8 tasks with a copy each and 4.56 ns for one task. Add to the
workaround: copying is not needed. Unpacking the field once before the loop
and passing the array down costs 1.25 ns a read, shared or not. Reproduces
on march-main and 0.5.0.

**S16** (`&&` and `||` evaluate both sides). Still true on march-main
1a4251bc8 and on 0.5.0, compiled and interpreted
(`bench/march/short_circuit`). The March branch `claude/and-or-short-circuit`
is fixing it.

**S18** (S30 on main: a callee cannot fill its caller's array). Still true
on both (`bench/march/callee_fill`). Add: the returning form costs the same
as the lost one; the reason `Strata.Lattice.fill` is C is speed (10.8 ns a
voxel in March against 0.25 ns for `sl_fill`, entry C), and the
out-parameter shape of the API.

**S12** (no way to poll a task). Still true on march-main: there is no
`task_poll`, and `Task.await_ms` ignores its timeout ("accepted for API
compatibility").

**Task 2's ruling on `Strata.Seed`** (ledger: "integers trap on overflow, so
the hash works in 32-bit lanes"). march-main has `int_wrapping_add`,
`int_wrapping_sub` and `int_wrapping_mul`, wrapping at 63 bits. A one-lane
hash with them costs 3.5 ns against the lanes' 27.8 ns
(`bench/march/hash64`). 0.5.0 does not define them, so this waits on entry
A; moving `Strata.Seed` to one lane changes every derived seed and every
golden.

## Not a gap

Integer `/` and `%`: march-main emits them inline behind a zero test (2.9
times C in `bench/march/divmod`, for want of vectorising); 0.5.0 calls
`march_checked_div_op` for each, which shows in the present builds (entry A)
and goes away with them.

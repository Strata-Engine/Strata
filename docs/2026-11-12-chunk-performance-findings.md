# Where an island surface chunk's 12 ms goes

12 November 2026. Branch `claude/chunk-perf-triage-b31a0a`, cut from the
generation branch at 5960ca3. Nothing existing was changed; the harness is
`bench/chunk_stages/`, the reduced March cases are `bench/march/`, the draft
GAPS entries are `docs/2026-11-12-gaps-draft.md`.

## Summary

An island surface chunk at 1 m costs **11.8 ms of CPU and 225 million
instructions** (mean of the 200 surface chunks `STRATA_CHUNK_TIMES` uses, one
task). The stage breakdown below accounts for 100% of it.

- **About half of it, 6 ms, is one March code-generation gap**: an `if` or
  `match` whose value is a Float allocates a box for the value and frees it
  again. `fmax`, `fmin`, `clamp01` and every `if e > 0.0 do e else 0.0 end`
  are a `malloc` and a `free`. 55% of the chunk's samples are in the
  allocator and the reference-count runtime. Reduced: 22 ns against 0.55 ns in
  C, 40 times (`bench/march/float_if`).
- **62% of it, 7.5 ms, is the column stage** (`area_n`: atlas window, column
  heights, thermal relax) over a patch of **66 x 66 columns** to produce 32 x
  32, and it is a function of the chunk column alone: each of the 4.9 chunks
  generated in full per chunk column repeats it.
- With March as it is, the three largest Strata restructurings bring the
  chunk to about 4.6 ms. With the Float gap closed and nothing else, about
  5.1 ms. **The 1 ms target needs both**: about 0.7 ms with the
  restructurings and March's loops near C.
- **`forge build` does not use march-main.** forge puts the toolchain that
  `~/.march/current` names (March 0.5.0, installed 13 October) ahead of the
  `march` that `scripts/env.sh` puts on `PATH`. Every Strata binary and test
  run on this machine has been compiled by 0.5.0. The chunk costs the same
  under both compilers (11.8 ms on march-main 0.7.0, 11.9 ms on 0.5.0), and
  every gap below reproduces on both, so the conclusions do not depend on
  it; but GAPS entries "reproduced from main" were checked with a different
  compiler from the one that builds Strata. See "The toolchain" below.

## How it was measured

The machine's load average was 20 to 55 throughout, so nothing here is
wall-clock time.

- **Whole chunk, in the viewer.** `/usr/bin/time -l` on a
  `STRATA_CHUNK_TIMES=1` run minus a run without it: 23 s of user time and
  450 x 10^9 instructions more, for 2000 full chunks and 40 000 early-outs:
  11 ms and 213 million instructions a full chunk once the early-outs (27 us
  and 0.59 million instructions each) are taken off.
- **Stages.** The generator's functions are private to the viewer module and
  this session may not edit it, so `bench/chunk_stages/build.sh` copies
  `viewer/lib` with `pfn` made `fn` and builds a harness that calls the
  pieces of `island_full` in its order with its arguments, over the same 200
  chunks. A stage's inputs are prepared once and the stage run 10 times; a
  row is that run minus a run of the preparation alone, in CPU time,
  instructions retired and cycles. CPU time has 5 us of resolution a chunk
  and repeats within 0.1 ms on the whole chunk and 40 us on a small stage;
  instructions repeat to 0.1%.
- **Profile.** `sample <pid> 30 1` on the harness running whole chunks. March
  builds keep their symbols (`nm` shows `_dens_loop`,
  `_Strata.Thermal.moves`), so `sample` resolves every March function and
  runtime helper with no extra step; `xctrace` was not needed. March's
  self-recursive loops are tail calls, so a stack is one March frame deep
  under `march_main`: a sample in `mi_malloc` is charged to the March
  function that called the runtime, not to that function's callers. Samples
  were grouped by that innermost March function into stages, and by the
  leaf symbol into kinds (allocator and reference counts, array calls,
  arithmetic helpers, C noise, March's own code).
- **Reduced cases.** Each `bench/march/<case>/run.sh` builds a March and a C
  program and prints CPU ns and instructions per unit of work, as a `REPS=n`
  run minus a `REPS=0` run. `out/<case>.ll` is March's LLVM IR
  (`march --emit-llvm`); the compiled loops were read with `objdump -d`.
  Causes were checked against march's source at `~/code/march-main`
  (1a4251bc8), which was read and not changed.

## The patch

An island chunk relaxes **66 x 66 = 4356 columns** to produce its 32 x 32 =
1024: 4.25 times its own area.

    32   the chunk
    + 2 x 9   the tree pad: ceil(reach / scale) + 1 = ceil(7.5 m / 1 m) + 1.
              A tree whose base is in a neighbouring chunk must stand on the
              same relaxed ground in both, so the chunk makes the ground
              under every tree that can reach it (ledger, Task 9).
    + 2 x 8   the thermal halo: twice the 4 passes, so a column relaxes
              alike in every patch that holds it (ledger, Task 7 fix 1).

Around it the atlas window is copied with 64 m more on each side for the
rivers: about 51 x 51 samples, three layers.

## Breakdown

march-main build, mean over 200 surface chunks, one task. "Alloc" is the
share of the stage's samples in the allocator and reference-count runtime.

| stage | CPU us | instr (k) | share | alloc | mostly |
|---|---:|---:|---:|---:|---|
| atlas window: 3 layer copies, river list | 100 | 2 257 | 0.8% | 45% | March |
| column heights, 4356 columns (`cols3_go`) | 5 435 | 103 655 | 46.2% | 61% | both |
| ... of which the river damp (list walk) | 2 285 | 45 259 | 19.4% | | both |
| ... of which the lake-rim search | 1 315 | 23 857 | 11.2% | | both |
| ... of which the rest (bilinear x 2, lake, detail noise) | 1 810 | 34 594 | 15.4% | | March |
| talus field (C) and `talus_go` | 115 | 1 982 | 1.0% | 0% | algorithm |
| thermal relax, 4356 columns x 4 passes | 1 700 | 35 184 | 14.4% | 67% | March |
| rest of `area_n` (span, inner, site beard) | 190 | 3 585 | 1.6% | 68% | March |
| **`area_n`, all of the above** | **7 540** | **146 663** | **64.1%** | | |
| inner 32 x 32 copies | 15 | 372 | 0.1% | | |
| lattice and entrance fields (C: 4 `field3`/`field2`) | 65 | 983 | 0.6% | 0% | C already |
| cave factors | 30 | 603 | 0.3% | | |
| 4 lattice fills (C, `sl_fill`) | 70 | 554 | 0.6% | 0% | C already |
| per-voxel density with caves (`dens_go`) | 2 255 | 46 579 | 19.2% | 72% | March |
| `any_cut` and `drop_islands` | 535 | 7 745 | 4.5% | 0% | March |
| `above_go` | 115 | 2 230 | 1.0% | | March |
| surface scan and its rule closure | 285 | 5 063 | 2.4% | 4% | March |
| lake and river fill, grass and posts (`finish_go`) | 325 | 4 208 | 2.8% | 0% | March |
| `Chunk.from_flat` | 245 | 3 693 | 2.1% | 0% | March |
| tree candidates and their tests | 75 | 1 053 | 0.6% | | |
| tree blits | 220 | 3 607 | 1.9% | 10% | March |
| ruin blit | 0 | 0 | 0.0% | | |
| **sum of stages** | **11 775** | **223 353** | **100.1%** | | |
| **`island_full` measured whole** | **11 765** | **224 577** | | **55%** | |

The same harness built as `forge build` builds it (March 0.5.0): 11 910 us
and 217 219 k instructions whole; every stage within 5% except
`drop_islands`, 815 us against 535 (0.5.0 calls a runtime helper for each
`/` and `%`).

By kind of work, from the profile of the whole chunk (20 601 samples):

| kind | share |
|---|---:|
| allocator and reference counts (`mi_malloc`, `mi_free`, `march_alloc`, `march_alloc_float`, `march_rc_last_free_slow`, `march_free_any`, ...) | 55.1% |
| March's own compiled code | 27.7% |
| array calls (`native_f32_arr_get`, `_set`, `native_int_arr_*`, `_make`) | 9.4% |
| C noise (FastNoiseLite, `sl_fill`) | 4.8% |
| arithmetic helpers (`march_int_to_float`, `march_checked_fdiv`, `march_float_abs`, ...) | 2.0% |
| scheduler and signals | 1.0% |

The ledger's Task 7 breakdown (columns 0.6 ms, thermal 0.4, density 0.8,
scan 0.3, fill 0.2 to 0.3, `from_flat` 0.2 to 0.5, at the 3 ms point) was
over a 34 x 34 patch with no caves; the column and thermal stages have since
grown 9 and 4 times with the patch (3.8 times the columns) and the river and
rim work per column; the density 2.8 times with the cave fields.

### The early-outs

26.7 us (air) and 26.6 us (stone) of CPU, 591 k instructions, against the
0.02 ms target; the 0.1 ms seen earlier was wall time under load.
`quick_bounds` copies two 21 x 21 atlas windows and takes their span. 68% of
its samples are in the allocator: `window_go`'s `if ... do none else
NativeArray.get_f32(...) end` and `span_go`'s `fmin` and `fmax` are
Float-valued `if`s, three for each of 882 samples. With that gap
closed it is about 9 us; it is also a function of the chunk column alone
(restructuring 1 below).

## Classification

**Column heights (5.4 ms).** Both. Algorithm: 4.25 times the chunk's
columns; every column walks the window's whole river list (16 samples a
window on average, 70 000 sample visits a chunk, 2.3 ms); every column of a
window that holds any lake sample (74 of the 200) searches 36 samples for a
rim (58 000 visits, 1.3 ms); the detail noise crosses the FFI once a column,
building a `Settings` record and two `fnl_state`s each time. March: 61% of
the stage is allocation. A river-sample visit costs 33 ns of which the
arithmetic is 2 ns in C: `clamp01` (two nested Float `if`s) and the
`if v < best` are three boxes; the list walk itself adds 5 ns of
reference-count traffic (`bench/march/list_walk`); `Math.sqrt`, `/.` and
`Math.abs` are runtime calls. A rim visit costs 23 ns for one comparison and
one array read: `fmax` and the `if ... do -1.0 else get end` are two boxes.

**Thermal relax (1.7 ms).** March. The work is 17 424 column-passes of four
subtractions and a few comparisons; C does one in 3.2 ns (0.056 ms a relax).
`excess`, `fmax` and the four edge tests are up to eleven Float-valued `if`s
a column-pass. Rewritten with none, same heights: 21.6 ns
(`bench/march/stage_thermal`), 5 times less. Algorithm: the patch again
(4356 columns for 1024).

**Per-voxel density (2.3 ms).** March. 32 768 voxels at 69 ns; C does the
same loop in 2.7 ns (0.09 ms). `fmin`, `fmax`, `shaped`'s two `if`s and the
chain that returns `d` or -1 (a Float `if` nested six deep, boxed once per
level it passes through) are five to ten boxes a voxel; rewritten with none,
same densities: 16.3 ns (`bench/march/stage_density`). The rest is nine
array calls a voxel and an 18-argument call. Algorithm: little. In a surface
chunk the band H - A .. H + A where the noise matters is 24 of the 32 rows.

**`drop_islands` (0.5 ms).** March, then algorithm. No allocation; it is
March's own code and array calls: two full passes over 32 768 voxels and a
flood, each step an out-of-line `native_*_arr_get` or `_set` and, on 0.5.0,
four runtime calls for `/` and `%`. Algorithm: it floods the whole chunk
whenever one voxel was cut.

**Scan (0.29 ms), `finish_go` (0.33 ms), `from_flat` (0.25 ms), `above_go`
(0.12 ms).** March: 7 to 10 ns a voxel for loops C does in about 1 ns; array
calls, the rule closure (`bench/march/closure`: 2.4 ns a call over a direct
call), index arithmetic.

**Trees (0.3 ms).** March, small. The blit hashes `Seed.cell` twice a
template voxel in 32-bit lanes (`bench/march/hash64`), even where the
probability is 255.

**Window (0.1 ms).** The price of the S15 workaround: three layer copies so
the columns do not read the shared atlas.

**Talus, lattice fields, fills (0.25 ms).** Already C. The talus field is
4356 samples for the patch; it shrinks with it.

## March gaps, ranked

Ratios are CPU time per unit of work, March over C, on march-main; the 0.5.0
figures are within 10% unless said. "Share" is of the 11.8 ms chunk.

| # | gap | March | C | ratio | share of the chunk | case |
|---|---|---:|---:|---:|---|---|
| 1 | A Float-valued `if`/`match` allocates and frees a box | 22.3 ns | 0.55 ns | 40 | about 50%, 6 ms | `bench/march/float_if`, `stage_thermal`, `stage_density` |
| 2 | Array reads and writes are out-of-line runtime calls; the loop polls a volatile flag and saves the stack each turn | 3.70 ns | 0.53 ns | 7 | 9.4% (1.1 ms) in the calls themselves; with what they block, most of the 3.6 ms that separates "gap 1 closed" from C | `bench/march/voxel_loop`, `lattice_fill` |
| 3 | A value shared by tasks: four reference-count updates per field read (S15) | 778 ns | 0.17 ns | 4600 (160 against an owned copy) | 0 in the one-task figure, by a workaround that costs 0.1 ms and forbids shared caches; an estimated 0.15 to 0.25 ms under 8 workers | `bench/march/shared_tasks` |
| 4 | A List of Float tuples walked per element: counts and unboxing per cell | 11.0 ns | 2.2 ns | 5 | about 3%, 0.35 ms | `bench/march/list_walk` |
| 5 | Float helpers are runtime calls: `int_to_float`, `Math.sqrt`, `Math.abs`, `Math.floor`, `/.` | 6.1 ns | 2.2 ns | 2.8 | 2.0%, 0.24 ms | `bench/march/list_walk` (flat rows) |
| 6 | No wrapping multiply (0.5.0): hashes in 32-bit lanes | 27.8 ns | 1.54 ns | 18 (7 like for like) | about 1%, 0.1 ms | `bench/march/hash64` |
| 7 | A closure called per element | 4.65 ns | 1.19 ns | 3.9 (2.0 against a direct March call) | 0.7%, 0.08 ms | `bench/march/closure` |
| 8 | `/` and `%` (0.5.0: a runtime call each) | 2.44 ns | 0.85 ns | 2.9 | 2% on 0.5.0, 0.3 ms; nothing on march-main | `bench/march/divmod` |
| 9 | `&&` and `||` run the right side (S16) | 18.2 ns | 0.28 ns | 65 | none in this chunk; a hazard | `bench/march/short_circuit` |
| 10 | A callee cannot fill its caller's array (S18) | result lost | | | none now (the fills are C: 0.07 ms); the March fill would be 1.4 ms | `bench/march/callee_fill`, `lattice_fill` |

**1. Float-valued `if`.** Cause found. `Llvm_case.emit_case`
(`lib/tir/llvm_case.ml`, the `slot_ty` choice near lines 492 and 746) types a
case's result slot by `predicted_unboxed_join`, which predicts `i64` when
every arm ends in an Int or Bool (a literal, a variable, arithmetic, a
comparison) and a struct type when every arm builds the same unboxed
aggregate. It has no case for Float, so a Float-valued case falls to the
default, `ptr`. Each Float arm is then coerced `double` to `ptr` by
`Llvm_ctx.coerce`, which calls `march_alloc_float` (a 24-byte
`march_alloc`), and the merge block calls `march_unbox_float` and
`__march_rc_decrc_local`, which frees it. The comments there describe the
free as the fix for a leak; the box is still made. In `float_if.ll`,
`with_if` shows it: both arms `call ptr @march_alloc_float`, then unbox and
decrement at `case_merge`. The same `if` over Ints gets `alloca i64`
(`rep` in `divmod.ll`) and allocates nothing, which is why Strata's integer
loops are not affected. One Float `if` is about 10 ns and 220 instructions;
a nested chain boxes once per level. The fix belongs in March: predict
`double` when every arm is a Float. Until then the workaround in Strata is
ugly but mechanical: arithmetic (`0.5 * (a + b + abs(a - b))`) or an
Int-valued `if` feeding `int_to_float`; `stage_thermal` and `stage_density`
do it and give the same results 5 times faster.

**2. Array access and loop overhead.** Cause found. `NativeArray.get_f32`
compiles to `call double @native_f32_arr_get`, a function in
`runtime/march_runtime.c` that is compiled as a separate unit, so clang
cannot inline it: a call, a bounds check, a float-to-double convert per
read; `native_f32_arr_set` adds a heap-pointer test and an atomic load of
the reference count to prove the array is held once. `int_to_float` is
likewise `march_int_to_float`. Each loop turn also reloads
`@march_preempt_request` with a `volatile` load (`Llvm_ctx.emit_reduction_check`;
the comment there explains that the volatility is deliberate), calls
`llvm.stacksave` and `llvm.stackrestore`, and shifts each Int left and right
by one around overflow-checked arithmetic. Nothing vectorises. `voxel_loop`
is 77 instructions a voxel against C's 16; `lattice_fill` 274 against 25.
The March session "short-circuit || and inline array access" is working on
the inlining; these two cases are for it.

**3. Shared values (S15).** Cause found. An accessor
(`match r do Reg(a, _, _) -> a end`) takes the record owned. The caller
increments the record's count, the accessor decrements it and, the record
being still alive, increments the field's count with an `atomicrmw add`;
the caller decrements that after the read. With eight tasks on one record
those are four writes a read to two cache lines every core is writing: 778
ns of CPU a read. Unpacking the field once before the loop costs 1.25 ns a
read whether the record is shared or not, so a shared value is cheap when it
is taken apart outside the loop. The generator's residual reads of the
shared atlas (`region_at` for 81 cave factors and each tree candidate,
`site_near` three times) are a few hundred a chunk; under 8 workers that is
an estimated 0.15 to 0.25 ms a chunk that the one-task figure does not show.
Not measured.

**4 and 5. Lists, tuples and float helpers.** Cause found. Walking a list
its caller still holds costs, per cell, a decrement of the cell, increments
of head and tail, a decrement of the tuple, increments of its boxed floats,
and `march_unbox_float` calls (`damp_list` in `list_walk.ll`). Floats in a
tuple or list are always boxed. `Math.sqrt`, `Math.abs` and `/.` are
`march_math_sqrt`, `march_float_abs` and `march_checked_fdiv` in the
runtime.

**6. Wrapping multiply.** Closed on march-main: `int_wrapping_mul` (and
`_add`, `_sub`) wrap at 63 bits, and a one-lane hash with it costs 3.5 ns
against the lanes' 27.8. March 0.5.0 does not have them, so `Strata.Seed`
cannot use them until Strata builds with march-main.

**7. Closures.** Cause found in part. March specialises the loop for the
lambda (`scan$hspec$1__lam0_rep_apply_0_rep`) but still calls the lambda's
`apply` out of line with every argument and the result as `ptr`, reloads the
captured values from the closure record each call and touches the closure's
reference count. Why the specialised call is not inlined was not found.

**8, 9, 10.** Cause known. `/` and `%`: march-main emits `sdiv` and `srem`
behind an inline zero test (`Llvm_ctx.emit_checked_divmod`); 0.5.0 calls
`march_checked_div_op`. S16 and S18 reproduce as GAPS describes them.

### The known gaps, checked against both compilers

| gap | march-main 0.7.0 (1a4251bc8) | 0.5.0 (what forge builds with) |
|---|---|---|
| S18 (S30 on main): callee cannot fill the caller's array | reproduces | reproduces |
| S15: shared value slow in a hot loop | reproduces, 160 times an owned copy | reproduces, 120 times |
| S16: `&&`, `||` evaluate the right side | reproduces (compiled and interpreted) | reproduces |
| no wrapping arithmetic | closed: `int_wrapping_add`, `_sub`, `_mul` | reproduces: not defined |
| S12: no way to poll a task | reproduces: no `task_poll`, `await_ms` ignores its timeout | reproduces |
| 630 us March lattice fill against 45 us C | 353 us of CPU against 47 us like for like (8 us separable) | 520 us |

## The toolchain

`scripts/env.sh` puts march-main's `march` and `forge` first on `PATH`. But
forge prefixes every compile command with the bin directory of the resolved
toolchain (`Toolchain.path_prefix` in `forge/lib/toolchain.ml`): a
`.march-version` pin found walking up from the project, else the global
default `~/.march/current`, which on this machine is `versions/v0.5.0`. So
`forge build`, `forge test` and `forge run` in Strata run
`~/.march/versions/v0.5.0/bin/march` with 0.5.0's stdlib and runtime.

Evidence: the viewer binary calls `march_checked_mod_op(x, 32)` in
`isle_flood`; march-main's emitter cannot produce that call (it passes the
helper a zero divisor on a cold path only), 0.5.0's does, and the same
source compiled by `march --compile` from `PATH` has the `srem` inline.
Building with `MARCH_HOME` set to an empty directory makes forge find no
toolchain and fall back to `PATH`; the harness built that way has the
inline form. `bench/chunk_stages/build.sh` builds both.

What follows for Strata: `int_wrapping_mul` and whatever the March session
lands (short-circuit `&&`, inlined array access) will not reach Strata until
its builds use march-main; `scripts/env.sh` needs
`export MARCH_HOME=<a directory with no current link>` or the repository a
`.march-version` naming an installed build of main. Neither was done here
(no existing file may change). It is a one-line follow-up, and the goldens
and the gate should be rerun under it.

## Strata restructurings, ranked

Savings are per full chunk, at March's present speed, measured where it says
so.

| # | restructuring | saving | how known |
|---|---|---:|---|
| 1 | Make the column product once per chunk column, and per 32 x 32 tile | 6.0 to 6.8 ms | measured parts |
| 2 | River damp only for columns a river can reach | about 2.0 ms | measured upper bound 2.3 |
| 3 | Lake-rim search only under a precomputed rim mask | about 1.2 ms | measured upper bound 1.3 |
| 4 | `drop_islands` from the cut voxels, not the whole chunk | about 0.4 ms | estimate from 0.54 |
| 5 | Detail noise as one `field2` over the patch, not 4356 `sample2` calls | about 0.3 ms | estimate |
| 6 | Skip rows that are air or stone for every column before the density and the scan | 0.3 to 0.5 ms in a surface chunk; most of the density in the other chunks of a column | estimate |
| 7 | `from_flat`: build the palette from the scan's known ids | about 0.15 ms | estimate from 0.25 |
| 8 | Template blit: no hash where the probability is 255 | about 0.15 ms | estimate from 0.22 |

**1. The column product once.** `area_n` (the window, the columns, the
talus, the relax, the site's beard: relaxed H, calm, ground and water lines)
depends on (cx, cz) and the level, not on cy. Over the 200 chunk columns,
981 chunks pass the early-outs and are generated in full: 4.9 a column, each
paying the same 7.54 ms. Made once per column it costs 7.54 / 4.9 = 1.54 ms
a chunk: 6.0 ms saved. Further, if the product is kept per 32 x 32 tile with
its own halo (a 48 x 48 patch; measured as `area_n` at pad 0: 3.49 ms) and a
chunk reads its trees' ground from the eight neighbouring tiles, the tree
pad is no longer relaxed again by every chunk: 3.49 / 4.9 = 0.71 ms a chunk,
6.8 ms saved. `quick_bounds` is a function of the column too, so every
early-out test of a column shares one. This needs a generator with a column
stage (`Strata.Terrain`: a column function and a chunk function that takes
its result) and a column cache in the stream; a generator is a pure function
with nowhere to keep anything today. Gap 3 (S15) shapes it: a tile read by
several chunk tasks must be unpacked into arrays once per task, never read
through an accessor in a loop.

**2. River damp.** Every column visits every river sample in the window.
Bucket the samples by atlas cell (most cells hold none), or keep per cell
the distance to the nearest river sample less its reach, so a column far
from water does not walk the list. Measured by emptying the list: 2.29 ms
(stage 26); a chunk beside a river keeps some of it. After restructuring 1
this is worth 0.2 ms a chunk.

**3. Lake rim.** A window with one lake sample anywhere in its 200 m makes
all 4356 columns search 36 samples. Two separable max passes over the lake
layer give, per atlas cell, whether any lake sample is within reach; search
only there. Measured by clearing the flag: 1.32 ms (stage 27). After
restructuring 1: 0.1 ms a chunk.

**4 to 8** are small and independent. 6 matters more than its surface-chunk
figure: of the 4.9 full chunks a column, 3.9 lie above or below the surface
chunk, and with the column product in hand the exact range of H says which
rows of each are air or solid before any voxel is visited. Not measured: the
200 chunks timed are surface chunks.

Not a restructuring: the 8-column halo and the 9-column tree pad are each
right for what they guarantee (ledger, Task 7 fix 1 and Task 9). The cost is
in redoing them per chunk, which 1 removes.

## Projected chunk time

Estimates, built from the measured stages. "C parity" takes each March stage
at the cost of its C equivalent (measured for the thermal relax and the
density, 3.2 and 2.7 ns a unit; for the rest, 1 to 2 ns a voxel and 30 ns a
column plus the river and rim visits at 2 ns), and leaves the C noise as it
is.

| case | chunk | how |
|---|---:|---|
| today | 11.8 ms | measured |
| gap 1 closed (Float `if`), nothing else | about 5.1 ms | thermal 1.70 to 0.33 and density 2.26 to 0.47 by the measured rewrites; columns 5.43 to 2.1 and the rest of `area_n` by their allocation shares; other stages unchanged (2.1 ms) |
| gaps 1 to 3 closed: March loops near C | about 1.5 ms | columns 0.65, C noise and fills 0.25, density 0.09, thermal 0.06, trees 0.1, everything else 0.3 |
| restructuring 1 alone | about 4.9 ms | 0.71 + 4.23 (the stages after `area_n`, unchanged) |
| restructurings 1 to 3 | about 4.6 ms | tile 3.49 less 0.53 x 3.2 = 1.8 ms, over 4.9 chunks: 0.37; plus 4.23 |
| restructurings 2 and 3 without 1 | about 8.6 ms | 11.8 - 3.2 |
| gap 1 closed and restructurings 1 to 3 | about 2.6 ms | tile about 0.95 ms over 4.9; density 0.47; the rest 1.9 |
| gaps 1 to 3 closed and restructurings 1 to 3 | about 0.7 ms | tile about 0.35 ms over 4.9; the rest 0.6 |

The design target of 1 ms is met only in the last row. Either side alone
leaves the chunk at 4 to 5 ms: after the restructurings the density (2.3 ms,
March) dominates; after the Float fix the columns (2.1 ms, mostly
algorithm) do.

Early-outs: 27 us today; about 9 us with gap 1 closed; about 2 us shared per
column (restructuring 1).

## To reproduce

```sh
. scripts/env.sh
(cd viewer && forge build --release)            # and run it once, for the atlas
bench/chunk_stages/build.sh
bench/chunk_stages/run.sh                       # the breakdown, march-main build
COMPILER=default bench/chunk_stages/run.sh      # the build forge makes by default
bench/march/run_all.sh                          # the reduced cases
MARCH=$HOME/.march/versions/v0.5.0/bin/march bench/march/run_all.sh
```

The profile: start `STAGE=1 REPS=60
bench/chunk_stages/out/chunk_stages_main` from the repository root with
`STRATA_ATLAS_DIR=viewer/.march/atlas MARCH_PIN_MAIN=1`, and
`sample <its pid> 30 1`.

## Not done, not verified

- No stage was timed with several generator tasks running; the S15 estimate
  for 8 workers is from the reduced case's per-read cost, not from the
  generator.
- The chunks of a column other than the surface chunk were counted (4.9 a
  column) but not timed; restructuring 1's saving assumes they pay the same
  `area_n`, which the code says they do.
- The C-parity figures for stages other than the thermal relax and the
  density are estimates, not C rewrites.
- Coarser levels (the far field) and 0.25 m scale were not measured.
- Nothing was run on a quiet machine; CPU time and instruction counts were
  used throughout for that reason.

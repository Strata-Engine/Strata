# Gaps

March and forge rough edges met while building Strata, with the workaround
used. Strata does not change March; it works around and records.

## S1 — `opaque` cannot be a variable name

`let opaque = ...` is a syntax error on March 0.7.0 ("I got stuck here" at
the name). `opaque` is reserved. Workaround: pick another name. The
constructor `Opaque` is unaffected.

## S2 — `by` cannot be a variable name

A pattern variable named `by` is a parse error reported only as
"parse error" at the enclosing `match`. Workaround: another name.

## S3 — a test using `Check.all` must declare `needs IO.Clock` and `needs IO.Process`

The property runner seeds from the clock and reads `MARCH_PROP_SEED`, and the
capability ceiling charges both to the calling test module. The charge lands on
EVERY test module in the suite, not only the one with the property (the
suite compiles as one program). Workaround: add the two `needs` lines to
every test module.

## S4 — `on` cannot be a parameter name

`fn f(d : Def, on : Bool)` is a bare "parse error" at the `fn` line; `on` is
the actor handler keyword. Workaround: another name.

## S5 — constructors do not resolve through a module alias

With `alias Strata.Blocks as B`, `B.SeeThrough` is "Unknown module B" both
as an expression and as a pattern. The full path works as an expression
(`Strata.Blocks.SeeThrough`); as a pattern it does not parse (cube_forge
G24) or resolves against the wrong type (G25). Workaround: every sum type a
consumer must inspect has an `_index` reader returning a number.

## S6 — test modules share one alias namespace

`alias Strata.Mesh as M` in one test module and `alias Strata.Math.Mat4 as M`
in another resolve to the same target: `M.around_from` was reported as
`Strata.Math.Mat4.around_from ... is not a function in scope`. Workaround:
every alias across the test suite (and the library) is unique per target.

## S7 — `loop` cannot be a function name

`pfn loop(...)` is a syntax error at the name. Workaround: another name.

## S8 — GLFW needs `MARCH_PIN_MAIN=1`

Without it `main` runs on a scheduler thread and the first window call dies
with SIGTRAP on macOS (cube_forge G15). `viewer/run.sh` sets it.

## S9 — an extern call whose result is bound to `_` is dropped

`let _ = Vk.frame()` drew nothing: the call was optimised away (cube_forge
G16). Workaround: use the result.

## S10 — `forge check` rejects a Unit call made through an alias

`Vk.set_palette(...)` as a statement, with `alias Strata.Ffi.Vk as Vk`, fails
`forge check` with "expected `()` but got `Unit`" while `forge build`
accepts it (cube_forge G23). Workaround: the full module path.

## S11 — forge does not build a dependency's C sources

Still true on main (1a4251bc8). A consumer lists `native/strata_vk.c` in
its own `[ffi]` block; see `viewer/forge.toml`.

## S12 — no way to ask "is this task finished?", and `Actor.call` fails in tests

`task_await` blocks, and there is no poll or timed await that returns
early. The alternative, worker actors with a collector actor polled by
`Actor.call`, returns `Err("actor_call: not in scheduler context")` under
`forge test` (cube_forge G49), so it cannot be tested. Workaround in
`Strata.Stream`: work started on one tick is awaited two ticks later, so it
has two whole frames to finish and the wait is normally zero. A real
`task_poll(t) : Option(a)` would let `tick` never wait at all.

## S13 — float literals have no exponent form

`1.0e-12` is a parse error reported as "I was expecting `do` after the
condition here". Workaround: write the digits out.

## S14: forge reuses a build when only an included C header changed

forge keys its build cache on the text of the `[ffi]` C sources, not on the
headers they include. A change to `native/strata_shaders.h` alone (a shader
edit) produced a binary with the old shaders. Workaround:
`native/build_shaders.sh` rewrites a stamp line in `strata_vk.c` with the
header's hash.

Met again in the population plan's Task 9 (found by its re-review):
`native/strata_lamps.h`, the light volume's lamp lists, is included by
`strata_vk.c`, and a change to it alone is not rebuilt either. No stamp line
covers it; `forge clean` before building does.

## S15: a value shared by worker tasks is slow to pass around a hot loop

`Strata.Mesh` ran seven times slower (1183 chunks in 1.95 s, from 0.27 s)
when the block registry was passed down to the per-face function, although
only fluid faces used it. The registry is one value shared by all eight
meshing tasks; handing it to a function 200 000 times a chunk on each of
them is, it seems, a reference-count update on shared memory each time.
Workaround: read what the faces need from the registry once per chunk into
arrays the task owns, and pass those.

## S16: `&&` and `||` evaluate both sides when the right side is a call

`if false && hb(k) do ... end` calls `hb`, and `false && g(k) == 3` calls
`g`, both in the interpreter and under `--compile` (reproduced from `main`,
no helper involved). So `k >= 0 && NativeArray.get_int(bs, k) != ...`
traps with "index -1 out of bounds" when k is -1 (met in `Strata.Erosion`'s
pass test). Never guard a read, or anything else that can trap or is
costly, with `&&` or `||`; use `if k < 0 do false else ... end`. A
suite-wide audit of existing `&&`/`||` guards is worth doing. Met again in
`Strata.Contact.solid` (physics branch): a window read guarded by `&&` died
with "index -11 out of bounds".

## S17: extern functions with the same March name in two modules collide

`Strata.Ffi.Audio` declared `fn x_open(...) = "sa_open"` in its extern
block, and `Strata.Ffi.Vk` already had `fn x_open(...) = "sv_open"`. A
program that called only the audio wrappers failed to link with
`_sv_open` undefined: the call bound to the other module's extern of the
same name, though each is declared in its own module and called
unqualified. Workaround: extern names are unique across the whole program;
the audio bindings use an `xa_` prefix. (Numbered S17 because another branch
took S16 for `&&` and `||`.)

## S18: a constructor name used in two modules confuses type inference elsewhere

`Strata.Emitters` declared `type Got = Got(...)`, a private helper, and
`Strata.Stream` already had a constructor `Got`. Both modules checked and
tested clean, but the viewer, which compiles both, then failed on an
unrelated line with "expected `Strata.Stream.Stream` but got `Stream`". The
same name in two modules is reported only as a hint ("Constructor `Leaf` is
defined by multiple types") when it is reported at all. Workaround:
constructor names are unique across the library; `Strata.Acoustics`'s job is
`Listening`, since `Strata.Fluid` has a `Job`.
Met again merging physics with the island work: the viewer's lakes test
declared its own `type Box = Box(Int, Int, Int, ...)` beside
`Strata.Blocks.Box(Float, ...)`. On main it compiled; with one more test
module in the program (which changes the order modules are compiled in)
the code generator laid `Strata.Blocks.Box` out with the test's integer
fields, and clang rejected the LLVM ("floating point constant invalid for
type" in `Strata.Query.lim_cells`). Renamed to `LakeBox`.
Met again in `StrataTactics.Shot` (2026-11-22): its `type Box` and a
constructor `Struck` (Strata.Query.Hit's) gave a type mismatch inside
`strata/query.march` when the tactics tests compiled. Renamed to
`ShotBox`, `ShotSpec`, `Strike`, `ShotRound`.

## S19: a record field typed by a short type name another module also defines breaks a consumer

`Strata.Physics` declared `type Event = ...` and a record
`type Physics = { ..., events : List(Event) }`. The library typechecked and
its tests passed, but the viewer, which also uses `Strata.Fluid.Event`,
failed `forge check` with "expected `Strata.Stream.Stream` but got
`Stream`" at fluid calls that have nothing to do with physics. A field
typed `Int` in the same place, or an `Event` type used outside a record,
did not trigger it. Workaround: a type named in a record field gets a name
no other module uses (`PhysEvent`).
The same symptom as S18, from a type name rather than a constructor; the
workaround is the same: names unique across the library. Physics also
renamed `View` to `Sight` (Strata.Fluid has a `View`).

## S20: three parser traps met while writing Strata.Physics

- A record literal, or an expression ending in one, followed by a line
  that opens with `(` is read as a call of the record: `let b = { ... }`
  then `(p2, id)` on the next line failed with "this is not a function".
  Workaround: end the block with a named call (`with_id(p2, id)`).
- `f(x).field` is a parse error. Workaround: an accessor function.
- A `match` inside a one-line lambda, as an argument (`fn c -> match ... end`),
  is a parse error. Workaround: a named function.


## S21: `within` cannot be a parameter name

`fn site_near(a : Atlas, xm : Float, zm : Float, within : Float)` is a bare
"parse error" at the `fn` line; `within` is reserved. Workaround: another
name (`reach` in `Strata.Atlas.site_near`).

## S22: `spawn` cannot be a function name, and `linear` cannot be one either

`fn spawn(rig : Int, ...)` in `Strata.Ffi.Anim` is "I got stuck here" at
the name: `spawn` is the actor keyword. `Cl.linear()` as a call is a parse
error too: `linear` is the linear-types keyword. Workaround: other names.
`Strata.Anim.spawn_at` makes a figure (the design calls it `spawn`), and
`Strata.Clip.straight()` names linear interpolation.

## S23: a single-constructor type of Floats through a generic container fails to compile

`type Qq = Qq(Float, Float, Float, Float)` returned by the function passed
to `List.map` (a `List(Qq)`), or read back through `List.nth_opt`, stops
clang: "'%r' defined with type '%ub.Qq = type { double, double, double,
double }' but expected 'i64'". The four-Float type is unboxed into a
struct and the generic code tags it as an Int. `Strata.Math.Quat.Quat` is
such a type. Reproduced in a 15-line app (a `List(Holder(Int, Qq))` mapped
to its `Qq`s). Workaround: `Strata.Rig` and `Strata.Clip` keep rotations as
`(Float, Float, Float, Float)` tuples inside lists and constructors, and
build a `Quat` only at the edges.

## S24: a call of a function a module lacks reads "Unknown module"

`Strata.Anim.ground_targets(...)` in a test, before `ground_targets` was
written, failed with "Unknown module `Strata.Anim`. [unbound_variable]",
though `Strata.Anim` exists and compiles. The message names the module, not
the missing function. Workaround: read it as "no such function in that
module" and check the name.

Met again through an alias (entities Task 3): with `alias Strata.Entities as
En`, a call `En.blocks_world(...)` before `blocks_world` existed was reported
as "Unknown module `En`" at that line and at every other `En.` call in the
same function, including ones that exist, and a type not yet written
(`Strata.Entities.Event`) as "Unknown module `Strata.Entities`". Same reading:
look for the one name that is new.

## S25: a String reaches a C extern as the runtime's own value

An `extern` parameter of type `String` arrives in C as the runtime's string
value: a pointer to a counted heap string (`march_string`: 16 bytes of
header, an 8-byte length, then the bytes), or, for strings of seven bytes or
fewer, the bytes packed into the pointer word itself (top bit set, low bit
clear). There is no documented C-side accessor outside `march_runtime.h`,
which a consumer's C does not include. Workaround: `native/strata_text.c`
carries its own copy of the decoding (`mstr`). It breaks if the runtime's
string layout changes; `viewer/test/text_test.march` ("Strings reach C")
would catch it, with short, long and built strings.

The text bindings met S17 again (two private externs named `x_stat`, in the
renderer's and the text module's bindings): the text module's glyph counts
came back as the renderer's draw-call count. Its externs are prefixed `xt_`.

## S26: `||` does not short-circuit in compiled code

`if id1 == 0 || NativeArray.get_int(d, 12 * (id1 - 1)) == key do ...` reads
the array at index -12 when `id1` is 0: the compiled code works out both
sides and then combines them. Reproduced in a 12-line app. Workaround:
nested `if`s where the right side must not run (`Strata.Fluid.probe`). Being
fixed in the March compiler on a branch of its own.

## S27: two types with the same constructor name in different modules break codegen

A private `type Box = Box(Int, Int, Int, Int, Int, Int)` in `Strata.Fluid`,
beside `Strata.Blocks.Box` (six Floats), made clang refuse
`Strata.Query.lim_cells`: "floating point constant invalid for type", a
Float stored into an `i64` slot. Workaround: a distinct name
(`Strata.Fluid.ChunkSpan`).

## S28: `RRB.get` walks a list

`RRB.get` on a vector of 75 chunks spent most of its time in
`Array.lst_nth`: the vector's leaves are lists, so a read is linear in the
leaf. Workaround: `Strata.Fluid.ChunkTree`, a balanced tree over the
chunks' numbers.

## S29: `on` and `none` cannot be parameter names

Each is a bare "parse error" at the function. Workaround: other names.

## S30: a March function cannot fill an array the caller passed in

Native arrays are copy-on-write values: `NativeArray.set_f32(out, i, v)`
on an array the caller still holds writes a fresh copy, so a March
`fill(samples, out) : Int` returned 32768 and left the caller's `out` all
zeros (met in `Strata.Lattice`). Only a C write through the FFI lands in the
caller's array, which is how `Strata.Noise.field2` and `field3` work.
Workaround: return the array, or, where an out-parameter API is wanted,
write the loop in C (`sl_fill` in `native/strata_noise.c`).

## S31: a `Check.all` property never fails under `forge test`

Met writing the far field's slot tests (generation Task 12): a property
that is plainly false, `Check.all(Gen.int(0, 10), fn x -> false)`, passes,
as do `Check.all_with` with a fixed seed and a `Check.all` whose result is
bound and followed by an assertion. `MARCH_SKIP_PROPERTIES` is not set
(forge sets it only for `--skip-properties`). So every `Check.all` in the
suite checks nothing: a mutation of `Strata.Slots`' draw rule that drew a
region twice went unnoticed by the property written for it, and was caught
at once by the same walk run as an ordinary loop. Workaround: properties
that matter are written as a fixed set of pseudo-random walks (a small
linear congruential generator in the test) asserted with `Test.assert_*`;
`test/slots_test.march` does this for the which-chunk-draws rule. The
existing properties (chunk, slots under random traffic, and others) are
left as they are, and are worth rewriting the same way.

Met again in the entity layer (entities Task 9): `Check.try_prop(fn n -> n > 100, 3)`
answers `Ok(true)`, so a returned `false` never reaches `Check`. A property
that raises does fail, with its counterexample shrunk:
`fn n -> if n > 5 do panic("over five") else true end` reports 6. So a
second workaround keeps shrinking: end the property in
`if <cond> do true else panic("<what failed>") end`, as
`entcheck/test/snapshot_test.march` does.

## S32: `by` cannot be a parameter name; `1.0e12` is not a number

`pfn hits(fs, victim : Int, by : Int, ...)` is "I got stuck here" at `by`:
it is reserved. A float written with an exponent (`1.0e12`) is a bare
"parse error" on its line. Workaround: another name (`who` in
`StrataViewer.Combat.hit`); the number written out (`1000000000000.0`).

## S33: another module's constructors cannot be matched by their qualified names

`match o do StrataViewer.Combat.CbKilled(c) -> ...` in a test is "I got
stuck here" at the dot, and through an alias (`Cb.CbKilled`) "I don't know
a constructor". A qualified constructor is fine in an expression, not in a
pattern. Workaround: the module exports accessors (`Combat.credit`,
`intent_code`, `kind_code`), as the rest of Strata reads `Strata.Query.Hit`.

## S34: an unquoted shell variable holding several settings is one word in zsh

Not March: `base="A=1 B=2"; env $base cmd` under zsh gives `env` the single
assignment `A="1 B=2"`. Three scripted viewer runs lost STRATA_HIDDEN that
way and opened a window. Workaround: a shell function that lists the
settings and takes the rest as "$@" (`scripts/combat_check.sh`'s `run`).


## S35: on March main a lambda that prints or reads a file fails the capability check

Met moving the viewer to March main (`f6481bbe6`): `List.find(ds, fn p ->
Authority.must(File.exists(p)))` and a `List.fold_left` whose lambda prints,
both inside a module that declares `needs IO` and `IO.FileSystem`, fail the
build with "Module `LibCheck` uses `IO.FileRead` but does not declare `needs
IO.FileRead`" (and the same for `IO.Console`), at no file and line 0:0.
`LibCheck` is the module forge wraps a multi-file build in; the lambda's code
is charged to it, not to the module it was written in. Workaround: a named
private function in place of the lambda (`first_there` and `say_lines` in
the viewer).

## S36: `on` cannot be a parameter name

`fn with_nav(m : MtMatch, on : Bool)` is "I got stuck here" at `on`: it is
the actor keyword. Workaround: another name. (With `by`, S32.)

## S37: what moving from March 0.5.0 to main (`f6481bbe6`) took

Recorded so the next toolchain step is cheaper, and because each is a
change a March user meets with no pointer from the old behaviour.

- **Capability results.** `File.*`, `Dir.*`, `Env.get*`, `println`,
  `Process.pid` now answer `Result(_, CapError)`; every call was wrapped in
  `Authority.must` (about 150 sites, by script). `File.read` answers a
  result inside that result.
- **Integers are 63 bits and overflow panics.** `attempt to multiply with
  overflow` stopped 197 of the library's tests, all through one function:
  `Strata.Coord.key_hash` multiplies a 32-bit value by a 31-bit constant.
  Under 0.5.0 that wrapped silently. Fixed with `int_wrapping_mul` there and
  in `Strata.Stream`'s and `Strata.Physics`'s hashes and one test's; the
  low 32 bits, which are all those hashes keep, are the same as before. A
  panic of this kind names no file or line.
- **A panic in one test can end the run.** With the hash panicking, the test
  binary died with SIGSEGV or SIGBUS part of the way through and printed no
  list of failures; `--verbose` on the binary was the way to see them.
- **One-line matches.** `match o do Some(p) -> a  None -> b end`
  on one line is a parse error; each arm needs its own line.
- **Test aliases share one space.** `alias Strata.Steer as Sr` in a test
  collided with `alias Strata.Store as Sr` in another ("`Strata.Store.seek`
  is not a function in scope"); as before, a test's aliases must be its own.

## S38: on March main the suite's properties really run, from a seed taken off the clock

S31 said a `Check.all` never fails under `forge test`. That was March 0.5.0.
On main (`f6481bbe6`) a false property fails, with the run, the shrunk input
and the seed. Strata has 148 `Check.all` calls that had therefore never
checked anything, and they now run 100 cases each from a seed that differs
every run (`MARCH_PROP_SEED`, or `forge test --seed=N`, fixes it). Two
things followed on the first full run:

- `Query.raycast agrees with a fine sampler` failed on run 69: a ray that
  passes through the very edge of a voxel. The ray cast reports the hit; the
  test's own check that the ray is inside the voxel 0.002 past the hit is
  false, because it has already left. The test was wrong, not the ray cast;
  it now accepts a hit whose point lies on the voxel when the 1/64 sampler
  saw nothing.
- `Fluid properties: the amount is conserved...` runs a 400-step simulation
  a hundred times and takes minutes.

The properties are worth having. What is owed: a fixed seed in the gate so a
failure can be replayed, and a pass over the slow ones.

## S39: a match with two arms on one line does not parse

`fn slot(o : Option(Int)) : Int do match o do Some(s) -> s _ -> -1 end end`
is "parse error: I was expecting `end` to close the match here" at the
second arm (met writing `Strata.Population`'s readers). The same match
with each arm on its own line parses. A one-line match with one arm
(`match a do (x, _) -> x end`) is fine. Workaround: put each arm on its own
line.

## S40: on March main a bare `println` as a statement still builds and prints

Met moving the animation onto March main (`07b58d065`). `println` answers
`Result(Unit, CapError)` (S37), but one standing as a statement in a block,
its result unused, compiles with no word and prints:
`test/erosion_test.march` and `test/lattice_test.march` still have one each,
and the library's `forge test` builds them and prints their lines. So the
compiler is no help in finding the prints a move like S37's left unwrapped.
Workaround: a scan for effect calls not inside `Authority.must(` (the
animation's merge used one; it found 31 in the viewer, none in `lib/`).

## S41: a module alias is not private to its module in a test build

`alias StrataViewer.Combat as Cb` in `viewer/lib/match.march` and
`alias Strata.Coord as Cb` in `viewer/test/blast_test.march`: the test build
resolved the test's `Cb.chunk_of` to `StrataViewer.Combat.chunk_of`, which
clang then could not find ("use of undefined value"). The same family as
S27. Workaround: aliases unique across the app (`Rules` in the match, `Cmb`
in its test).

## S42: `needs` cannot be a parameter name, nor `linear` (again)

`fn unsupported(..., needs : Int -> Bool, ...)` reads as the capability
keyword: "This `mod` has no matching `end`", reported at the module's
first line. `linear` (S22) is refused as a parameter too. Workaround:
`wants`, `steady`.

## S43: a small function answering a tuple, called per voxel, allocates; across a module boundary an accessor owns its argument

`Strata.Template.blit` undoes the quarter turn per voxel with
`unturn(t, rot, rx, rz) : (Int, Int)` and reads the template (a seven-field
constructor holding three arrays) through `t_sx(t)`, `t_ids(t)` and the
like. Two costs, both in `bench/march/stamp_loop` against C (14 instructions
a template voxel):

- The tuple is allocated and freed every voxel: 160 to 190 instructions.
  The loop is 545 to 591 instructions a voxel with it and 386 to 395 with
  the turn as two Int-valued functions.
- Compiled in the program's own module, an accessor borrows the template
  and touches no reference count. Compiled as a second module, as every
  library function is, the same accessor takes it owned and adjusts the
  counts of the template and its three arrays (seven `rc_` calls in its
  IR). The same loop is then 815 to 836 instructions a voxel, 245 to 290
  more. That gap is the three size accessors (`t_sx`, `t_sy`, `t_sz`) the
  loop calls a voxel: with them written as constants in a scratch build,
  the two builds measure the same (376 to 416 with the tuple, 246 to 248
  without), so each read costs about 85 to 92 instructions more across the
  boundary, as executed. In a bare loop that keeps the template
  (`acc_one`/`acc_two` in the same bench) a `t_sx` call adds about 5
  instructions in one module and 127 in a second; the cost depends on the
  caller. `t_ids` costs nothing measurable across the boundary. (An earlier
  figure of 157 against 24 for one accessor was not from instructions run
  and is withdrawn.) Making the accessors private to their module was
  tried and changed nothing measurable (834), though that build was
  reported as cached and is not relied on.

Met measuring scatter layers (`bench/populate_fields` mode 8: 11.8 M of
24.1 M instructions a chunk with 27 stamps are this loop, 781 a voxel
visited; mode 12 is blit alone). The Strata code is left plain; wanted from
March: a tuple answered by a small function kept out of the heap (or the
function inlined), and a constructor argument that is only read borrowed
across modules as it is within one.

## S44: a Float-valued `if` allocates a box (closed on March `07b58d065`)

An `if` or `match` whose value is a Float boxed it: a `malloc` and a `free`
per evaluation. Reduced case `bench/march/float_if`: 22.3 ns against C's
0.55 ns (40 times) on March 0.5.0 and on main at 1a4251bc8; 0.85 ns against
0.56 after march-language/march#1263 (merged as `07b58d065`). Met in the
population plan's Task 2: a curve is piecewise linear with Float-valued
branches, and the five curve passes of `bench/populate_fields` mode 1 cost
210 us of CPU before the fix and 23.5 us after, with the Strata code
unchanged. Closed; recorded because Strata kept the plain code through it
and the hand rewrites are now slower than the plain code
(`docs/2026-11-12-chunk-performance-findings.md`).

## S45: a loop pays per turn for array calls, a scheduler poll and a stack save

Each array read or write is a call into the runtime (with its own bounds
check), and each turn of a loop reloads the scheduler's preempt flag and
saves and restores the stack pointer, so no loop vectorises. Reduced case
`bench/march/voxel_loop`: 3.70 ns and 77 instructions a voxel against C's
0.53 ns and 16 on March 0.5.0; 1.75 ns against 0.56 on `07b58d065`.
Met in every per-column loop of the population: the cover placer's loop
(about 270 instructions a floor, Task 3), `adapt` (about 445 a column, Task
5), the cover-field count (about 49 a test, Task 6), the island's per-column
passes (Task 9), and the loop of `Strata.Template.blit` beside S43. The
Strata code is left plain; open.

## S46: `Strata.Seed` hashes in 32-bit lanes

March Ints are 63 bits and overflow traps, so `Strata.Seed` masks every
product into 32-bit lanes. Reduced case `bench/march/hash64`: one
`Seed.cell` 27.8 ns and 241 instructions against 1.54 ns for the 64-bit mix
C would write (18 times); 28.3 ns on `07b58d065`. March main has had
`int_wrapping_mul` since the move (S37), and the same mix in one 63-bit lane
costs 3.50 ns there, so the language has closed this gap; `Strata.Seed`
stays in lanes because moving it would change every hash, every placement
and every stored golden. Met in the population plan: the roll and `next`
are about 155 instructions a floor rolled in the cover placer (208 k of
740 k a layer, Task 3) and the two draw hashes 4.8 M of 24.1 M in a chunk
of 27 stamps (Task 4, before blit skipped draws at probability 255). Open,
on Strata's side: a decision to change every hash at once.

## S47: `/` and `%` by a constant cost more than shifts

Reduced case `bench/march/divmod`: four `/` and `%` by 32 a voxel, 2.44 ns
and 29 instructions on March main against C's 0.85 ns and 20 (2.9 times; on
0.5.0 each is a runtime call). Met in the cover placer's loop, which
recovers a column's x and z from its index (Task 3). Small; the Strata code
is left plain.

## S48: `IO.Random` in a library module is charged to every test module

Met writing `Strata.Save`'s lock (save Task 2): a module declaring `needs
IO.Random` and calling `Crypto.random_hex` built alone, but the test build
then stopped at an unrelated test module: "Module `AcousticsTest` uses
`IO.Random` but does not declare `needs IO.Random`" (the S3 charge, for
another capability). Workaround: the lock's token is the process id and the
clock's microseconds (`IO.Process` and `IO.Clock`, which every test module
declares already for S3), not random bytes.

## S49: no way to force a file's data to disk

March's standard library has `File.write` and `File.rename` and nothing
that waits for the data to reach the disk (no fsync, on a file or on its
folder). `Strata.Save` writes every file to `<name>.tmp` and renames it,
which is safe against the game crashing: a reader sees the old file or the
new. After a power cut or an operating-system crash the rename can have
reached the disk before the data it names, and the file is then short or
garbage. Workaround: none; every save file carries a checksum, so such a
file is set aside as `.bad` and counted, and a region's edits regenerate
away. Wanted from March: `File.sync` (and a way to sync a folder).

## S50: a write whose later arguments read the array it writes copies the array

In `cset(cset(st, i, cget(st, i) +. a), i + 1, cget(st, i + 1) +. b)` the outer
`cget` reads the array as it was before the inner write, so value semantics
keeps the old array alive across that write: March holds a second reference
(`__march_rc_incrc_local`) and `NativeArray.set_float` copies the whole
array, every call. Reduced case `bench/march/solver_copy`: about 6 000 copies
of the state array for 4 096 contacts, and none once every read comes before
the first write. Not a regression: the same on March 0.5.0, `07b58d065`,
`6c88ab912` and `f04f0dc0c`. A compiler could hoist the pure reads ahead of
the write, but March does not. Met 2026-11-19 in `Strata.Contact.apply` and
`pseudo_apply`, and in `Strata.Physics.integrate`, `tally` and `drag`, which
copied the state, push-out and tally arrays (32 floats a body) on every
impulse. With 275 bodies a step took 15 to 20 s. Those functions now bind
every read before writing, with the same arithmetic in the same order, so
results are bit for bit as before. The same step now takes about 3 s, which
is contact generation for that many bodies. Rule for Strata code: read
first, then write.

## S51: `within` cannot be a function name

`pfn within(r : Reach, t : (Int, Int)) : Bool do ... end` is "I got stuck
here" at the name, as `on` is (S4). Workaround: another name
(`plugins/strata_tactics/lib/reach.march` uses `on_map`).

## S52: a program's type named `Unit` shadows March's `Unit` everywhere

A consumer declaring `type Unit = Unit(Int, Int, (Int, Int))` breaks every
function in Strata that answers `Unit`: "expected `Unit` but got `()`" in
render.march, save.march, entities.march, from the one global type namespace.
Workaround: never name a type `Unit` (the tactics example's is `Squaddie`).

## S53: a lambda inside another lambda's `let` chain is misparsed

In `fn t -> match t do (a, b) -> let h = ... \n List.filter_map(xs, fn sd -> match sd do (n, x, y) -> ... end)`
the inner `match sd do` is read as the condition form `match do`
("I cannot find `sd`", "Each condition in `match do` must be Bool"). The
same shape fails after a `let` inside a test's arm, and with a trailing
lambda argument at the end of a `let` line followed by more lines.
Workaround: lift the inner work into a named helper (strata_tactics' Marks,
strata_kit's motion tests and example).

## S54: `(Int, Int, Int) -> Bool` is a function of one tuple

A parameter typed `occupied : (Int, Int, Int) -> Bool` refuses the
three-argument lambda `fn (x, y, z) -> ...`: the type is a function of one
tuple. A function of three Ints is `Int -> Int -> Int -> Bool`.

## S55: an array field written through a record update copies the array

`{ d with md_water: NativeArray.set_int(d.md_water, i, v) }` copies the
whole array on every call: the record `d` still owns the array while it is
written, because the update itself uses `d`, so the array has two owners and
`set_int` takes its copy path. A compiler could see that `d` dies in the
update and move the field out first, but March does not. Reduced case
`bench/march/record_field`: about 400 000 instructions a write for an array
of 262 144 ints, against 900 with the bare array threaded through the loop
(the same with a helper taking the record). Met 2026-11-21 in StrataMaps:
the water stage wrote the per-column water array and height grid this way
once per river column, so its cost grew with the cube of the map's side
(3.9 s for a 768-voxel square, release); the lava stage did the same per
scorched column (3.7 s). Rule for Strata code: in a loop, thread the bare
arrays and build the record once.

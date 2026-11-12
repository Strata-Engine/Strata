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

## S31: `by` cannot be a parameter name; `1.0e12` is not a number

`pfn hits(fs, victim : Int, by : Int, ...)` is "I got stuck here" at `by`:
it is reserved. A float written with an exponent (`1.0e12`) is a bare
"parse error" on its line. Workaround: another name (`who` in
`StrataViewer.Combat.hit`); the number written out (`1000000000000.0`).

## S32: another module's constructors cannot be matched by their qualified names

`match o do StrataViewer.Combat.CbKilled(c) -> ...` in a test is "I got
stuck here" at the dot, and through an alias (`Cb.CbKilled`) "I don't know
a constructor". A qualified constructor is fine in an expression, not in a
pattern. Workaround: the module exports accessors (`Combat.credit`,
`intent_code`, `kind_code`), as the rest of Strata reads `Strata.Query.Hit`.

## S33: an unquoted shell variable holding several settings is one word in zsh

Not March: `base="A=1 B=2"; env $base cmd` under zsh gives `env` the single
assignment `A="1 B=2"`. Three scripted viewer runs lost STRATA_HIDDEN that
way and opened a window. Workaround: a shell function that lists the
settings and takes the rest as "$@" (`scripts/combat_check.sh`'s `run`).


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

## S16: `&&` can evaluate its right side when the left is false

`k >= 0 && NativeArray.get_int(bs, k) != NativeArray.get_int(bs, i)`, the
body of a small helper whose `k` came from another call, trapped with
"index -1 out of bounds" when k was -1 (`Strata.Erosion`'s pass test,
compiled under `forge test`). The same shape in a standalone test function
short-circuits correctly, so it is likely an optimisation that inlines or
flattens the helper. Workaround: `if k < 0 do false else ... end` wherever
the right side would trap.

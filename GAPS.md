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

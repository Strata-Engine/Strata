# short_circuit

GAPS S16, still true on march-main (1a4251bc8) and on 0.5.0: `a && f(x)`
calls `f` when `a` is false. `./run.sh` prints the line the right side
prints. In a loop where the left side is always false and the right side is
a call that reads 16 array elements, March spends 18.2 ns and 373
instructions an iteration against 0.61 ns for the same test written as a
nested `if`, and 0.28 ns in C: 65 times C for work that should never run. It
is first a correctness hazard (a guarded read traps) and only then a cost; in
the island chunk the `&&`s in hot loops have cheap comparisons on the right,
so its share there is near nothing. The March session "short-circuit || and
inline array access" is fixing it.

Run `./run.sh`.

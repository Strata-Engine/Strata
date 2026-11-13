# closure

A function value called once per element, as `Strata.Surface.scan_wet` calls
its rule for each of a chunk's 32768 voxels. Through a closure that captures
three values the March loop takes 4.65 ns and 119 instructions an element;
calling the same rule by name takes 2.29 ns and 62: the closure doubles the
loop. C through a function pointer with a context struct takes 1.19 ns (3.9
times less than March's closure) and 0.96 ns direct. March does specialise
`scan` for the lambda (`scan$hspec$1__lam0_rep_apply_0_rep` in
`out/closure.ll`), but the specialised loop still calls the lambda's `apply`
function with every argument and the result as an erased `ptr` (Ints and the
Bool converted each way), reloads the three captured values from the closure
record on each call, and touches the closure's reference count
(`__march_rc_incrc_local` before the call, `__march_rc_decrc_local` inside).

Run `./run.sh`.

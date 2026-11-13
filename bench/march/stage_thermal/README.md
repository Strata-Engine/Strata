# stage_thermal

Not a minimal case: `Strata.Thermal.relax` whole, on the 66 x 66 patch and 4
passes an island chunk uses, to size the Float-valued-`if` gap
(`../float_if`) on a real stage. As it stands a column of a pass costs 110 ns
and 2370 instructions (1.92 ms a relax; 1.70 ms measured in the generator on
real heights). With every Float-valued `if` rewritten as arithmetic or moved
onto an Int (`excess`, `fmax`, and the four edge tests), giving the same
heights, it costs 21.6 ns (0.38 ms): 5 times less. The same passes in C cost
3.2 ns (0.056 ms): 34 times less than the code as it stands, 6.7 times less
than the rewrite; what is left after the rewrite is the out-of-line array
calls of `../voxel_loop`.

Run `./run.sh`; it prints a weighted sum of the relaxed patch from each of
the three, which agree.

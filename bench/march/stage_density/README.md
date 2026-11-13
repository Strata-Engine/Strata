# stage_density

Not a minimal case: the viewer's `dens_loop`, `voxel_density` and `shaped`
whole, over a chunk's 32768 voxels on synthetic inputs (a hill through the
chunk, caves cut in 8% of the voxels), to size the Float-valued-`if` gap
(`../float_if`) on a real stage. As it stands a voxel costs 71 to 85 ns
(run to run; the allocator's state) and 1450 to 1700 instructions: 2.3 to
2.8 ms a chunk, as measured in the generator (2.26 ms). With every
Float-valued `if` rewritten (`fmin`, `fmax`, `shaped`'s two, and the chain
that returns `d` or -1), giving the same densities, it costs 16.3 ns (0.53
ms): 4.4 to 5.2 times less. The same loop in C costs 2.7 ns (0.088 ms): 26
to 31 times less than the code as it stands.

Run `./run.sh`; it prints a weighted sum of the densities from each of the
three, which agree.

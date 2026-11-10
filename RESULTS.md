# Results

Measured on an Apple M3 Max, macOS 26.6.1, March main `1a4251bc8` (0.7.0),
release builds. Times use the millisecond wall clock, so a per-chunk figure
is a total divided by a count; "worst" is the slowest single chunk in whole
milliseconds.

## The headless demo (`demo/`, seed 7)

| | Run A: 1 m voxels, radius 6 x 3 | Run B: 10 cm voxels, radius 3 x 2 |
|---|---|---|
| chunks loaded | 1274 | 245 |
| uniform / paletted / direct | 1038 / 236 / 0 | 178 / 67 / 0 |
| voxel storage | 8040 KB, 6462 bytes a chunk | 2280 KB, 9532 bytes a chunk |
| terrain, a chunk | 0.29 ms (worst 5) | 0.35 ms (worst 1) |
| mesh, a chunk, all meshable chunks | 1.42 ms over 660 (worst 14) | 2.23 ms over 75 (worst 6) |
| mesh, a chunk, chunks that draw something | 4.1 ms over 156 | 3.89 ms over 35 |
| quads / instances | 89 222 / 8 950 | 15 529 / 1 317 |
| mesh bytes | 766 KB, 8 bytes each | 131 KB, 8 bytes each |
| single block edit, until its meshes are back | 7 ms, 4 chunks | - |
| sphere edit, until its meshes are back | 8 ms, radius 12, 4 chunks | 14 ms, radius 20, 8 chunks |
| edited chunks in the cache | 0 (all still loaded) | 0 |
| world hash | 84790968213238 | 15643685159536 |

Run A streams a 256 m walk in 64 steps (about 1.2 s in all). Both world
hashes are identical across two executions. `docs/demo-surface.png` is the
top-down picture run A writes: terrain, ponds, plants, lamps, and the
crater the sphere edit leaves.

A flat byte array would be 32 768 bytes a chunk; uniform chunks bring the
average to a fifth of that at metre scale.

## The viewer (`viewer/`, radius 6, 676 chunks, 22 million voxels)

| | |
|---|---|
| terrain | 0.3 ms a chunk |
| mesh and upload | 2.3 ms a chunk, sequential, at startup |
| quads on the GPU | 153 595 in 1.2 MB |
| drawn after culling | 220 chunks, 94 874 quads |
| frame rate | 118-120 fps, the display's vsync cap |

The frame rate is capped by presentation, so it says only that the frame
fits in 8.3 ms with room to spare; an uncapped figure needs a present mode
the first slice does not offer.

## Against cube_forge

The design asks for a comparison on equal voxel volume against the most
recent cube_forge entry whose mesher shades corners. That entry is
"Light is sampled per fragment from a 3D texture now" in cube_forge's
`RESULTS.md`: 377 ms to mesh all 144 chunks of 16 x 256 x 16, giving
334 878 vertices. One such chunk is 65 536 voxels, two Strata chunks.

| per 65 536 voxels of world | cube_forge | Strata |
|---|---|---|
| mesh time | 2.6 ms | 2.8 ms (two chunks at 1.42 ms) |
| mesh memory, per quad | 240 bytes (6 vertices of 10 floats) | 8 bytes |
| voxel memory | 65 536 bytes | 12 924 bytes (run A average) |

Two things make the time row inexact, and they pull in opposite
directions. cube_forge's figure is wall time for a parallel pass over
several scheduler threads; Strata's is one thread. And cube_forge's mesher
in that entry no longer carries corner shading in its merge key (light
moved to the fragment shader), while Strata's does. On this evidence
Strata is not faster at meshing per voxel; it is at rough parity on one
thread. The gains are in memory: thirty times less per quad, five times
less per stored voxel.

Single-edit latency is 7 ms, under the 16.7 ms frame the design names as
the point where sub-chunk dirty regions would be needed. They are not
built.

## Where the mesher's time goes

A chunk that draws something costs about 4 ms; a non-uniform chunk that
ends up with nothing to draw about 0.5 ms, which is the fixed cost of the
padded 34-cubed grid. Two changes got there from 5.25 ms and 1.5 ms: the
centre of the padded grid is copied straight from the chunk instead of
through the neighbourhood lookup, and per-layer tallies skip any slice
that has no cube block or looks into a fully solid layer. What remains is
the mask build for live slices and two whole-chunk passes (occupancy and
instances) that could read the padded grid instead.

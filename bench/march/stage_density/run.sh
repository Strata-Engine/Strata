#!/bin/sh
# units: one voxel (32768 a chunk)
cd "$(dirname "$0")" && . ../common.sh
build stage_density
row "March, the viewer's density as it stands" out/stage_density_march 0 500 32768
row "March, no Float-valued if"                out/stage_density_march 1 2000 32768
row "C"                                        out/stage_density_c     0 10000 32768
echo "weighted sum of the densities: March $(VARIANT=0 REPS=0 out/stage_density_march | tail -1), no-if $(VARIANT=1 REPS=0 out/stage_density_march | tail -1), C $(REPS=0 out/stage_density_c | sed -n 2p)"
ir stage_density

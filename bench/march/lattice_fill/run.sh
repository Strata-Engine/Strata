#!/bin/sh
cd "$(dirname "$0")" && . ../common.sh
build lattice_fill
row "March, per voxel, / and %"        out/lattice_fill_march 0 3000 32768
row "March, per voxel, shifts"         out/lattice_fill_march 1 3000 32768
row "C, per voxel, / and %"            out/lattice_fill_c     0 10000 32768
row "C, separable (sl_fill)"           out/lattice_fill_c     1 50000 32768
ir lattice_fill

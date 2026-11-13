#!/bin/sh
cd "$(dirname "$0")" && . ../common.sh
build voxel_loop
row "March" out/voxel_loop_march 0 8000 32768
row "C"     out/voxel_loop_c     0 40000 32768
ir voxel_loop

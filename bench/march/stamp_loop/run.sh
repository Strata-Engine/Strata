#!/bin/sh
cd "$(dirname "$0")" && . ../common.sh
build stamp_loop
row "March, as Strata.Template.blit"   out/stamp_loop_march 0 20000 972
row "March, the turn as two Int calls" out/stamp_loop_march 1 20000 972
row "C"                                out/stamp_loop_c     0 400000 972
ir stamp_loop

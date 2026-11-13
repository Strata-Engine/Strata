#!/bin/sh
cd "$(dirname "$0")" && . ../common.sh
build stamp_loop
# the same loop with the template and blit in a second module (stamp_lib.march)
MARCH_LIB_PATH="$PWD" "$MARCH" --compile --opt 2 -o out/stamp_two_march stamp_two.march > out/stamp_two.build.log 2>&1 || { cat out/stamp_two.build.log; exit 1; }
rm -f stamp_two.ll
row "March, as Strata.Template.blit"       out/stamp_loop_march 0 20000 972
row "March, the turn as two Int calls"     out/stamp_loop_march 1 20000 972
row "March, the loop in a second module"   out/stamp_two_march  0 20000 972
row "the same, the turn as two Int calls"  out/stamp_two_march  1 20000 972
row "C"                                    out/stamp_loop_c     0 400000 972
ir stamp_loop

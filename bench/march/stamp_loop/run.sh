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
# one accessor call a turn of a bare loop, executed (acc_one, acc_lib, acc_two):
# VARIANT 0 calls none, 1 calls t_sx, 3 calls t_sx, t_sy and t_sz
"$MARCH" --compile --opt 2 -o out/acc_one_march acc_one.march > out/acc_one.build.log 2>&1 || { cat out/acc_one.build.log; exit 1; }
MARCH_LIB_PATH="$PWD" "$MARCH" --compile --opt 2 -o out/acc_two_march acc_two.march > out/acc_two.build.log 2>&1 || { cat out/acc_two.build.log; exit 1; }
rm -f acc_one.ll acc_two.ll
row "bare loop, one module"                out/acc_one_march 0 10000000 1
row "  and one t_sx a turn"                out/acc_one_march 1 10000000 1
row "  and t_sx, t_sy, t_sz a turn"        out/acc_one_march 3 10000000 1
row "bare loop, second module"             out/acc_two_march 0 10000000 1
row "  and one t_sx a turn"                out/acc_two_march 1 10000000 1
row "  and t_sx, t_sy, t_sz a turn"        out/acc_two_march 3 10000000 1

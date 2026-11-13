#!/bin/sh
cd "$(dirname "$0")" && . ../common.sh
build callee_fill
row "March, callee fills caller's array" out/callee_fill_march 0 8000 32768
row "March, callee returns the array"    out/callee_fill_march 1 8000 32768
row "C, callee writes through pointer"   out/callee_fill_c     0 40000 32768
echo "sum of out[100] over 3 reps (300 when the fill landed in the caller's array):"
echo "  March, callee fills caller's array: $(VARIANT=0 REPS=3 out/callee_fill_march)"
echo "  March, callee returns the array:    $(VARIANT=1 REPS=3 out/callee_fill_march)"
echo "  C:                                  $(REPS=3 out/callee_fill_c)"
ir callee_fill

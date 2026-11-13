#!/bin/sh
cd "$(dirname "$0")" && . ../common.sh
build divmod
row "March, / and % (4 a unit)"     out/divmod_march 0 10000 32768
row "March, int_shr and int_and"    out/divmod_march 1 10000 32768
row "C, / and %"                    out/divmod_c     0 20000 32768
row "C, >> and &"                   out/divmod_c     1 40000 32768
ir divmod

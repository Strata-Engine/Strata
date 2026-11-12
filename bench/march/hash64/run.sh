#!/bin/sh
cd "$(dirname "$0")" && . ../common.sh
build hash64
row "March, 32-bit lanes (Strata.Seed.cell)" out/hash64_march 0 10000 4096
row "March, 63-bit int_wrapping_mul"         out/hash64_march 1 60000 4096
row "C, 32-bit lanes"                        out/hash64_c     0 60000 4096
row "C, 63-bit lane"                         out/hash64_c     1 100000 4096
row "C, 64-bit lane"                         out/hash64_c     2 100000 4096
echo "same values, March and C (1 rep): lanes $(VARIANT=0 out/hash64_march) $(VARIANT=0 out/hash64_c); 63-bit $(VARIANT=1 out/hash64_march) $(VARIANT=1 out/hash64_c)"
ir hash64

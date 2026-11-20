#!/bin/sh
# Strata.Contact's solver writes, reduced (README.md). A row is per contact;
# SIZE is the state array's length in floats. A write that copies costs in
# proportion to SIZE; an in-place one does not.
cd "$(dirname "$0")" && . ../common.sh
build solver_copy
for v in 4 5 6 8; do
  for n in 64 8192; do
    SIZE=$n row "variant $v, SIZE $n" out/solver_copy_march $v 20 4096
  done
done
ir solver_copy

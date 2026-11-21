#!/bin/sh
# Functional record updates of an array field, reduced (README.md). A row is
# per write; SIZE is the array's length in ints. A write that copies costs in
# proportion to SIZE; an in-place one does not.
cd "$(dirname "$0")" && . ../common.sh
build record_field
for v in 0 1 2; do
  for n in 64 262144; do
    SIZE=$n row "variant $v, SIZE $n" out/record_field_march $v 4 1000
  done
done
ir record_field

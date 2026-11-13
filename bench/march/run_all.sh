#!/bin/sh
# Every case, one after another. MARCH=path picks another compiler (see common.sh).
cd "$(dirname "$0")" || exit 1
for c in float_if lattice_fill voxel_loop callee_fill shared_tasks hash64 closure short_circuit list_walk divmod stage_thermal stage_density; do
  echo "== $c"
  "./$c/run.sh" || echo "$c: did not build or run with this compiler"
done

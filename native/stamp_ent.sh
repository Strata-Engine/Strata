#!/bin/sh
# forge decides whether to recompile a C source from its text alone, not from
# the headers it includes (GAPS S14). strata_ent.c includes strata_ent.h,
# which includes strata_ent_grid.h, so it carries the hash of both; run this
# after editing either.
# `native/stamp_ent.sh --check` fails when the stamp is stale.
cd "$(dirname "$0")" || exit 1
h=$(cat strata_ent.h strata_ent_grid.h | shasum -a 256 | cut -c1-16)
if [ "$1" = "--check" ]; then
  grep -q "#define STRATA_ENT_STAMP \"$h\"" strata_ent.c && exit 0
  echo "stale: native/strata_ent.c's STRATA_ENT_STAMP does not match strata_ent.h and strata_ent_grid.h; run native/stamp_ent.sh"; exit 1
fi
sed -i '' "s/^#define STRATA_ENT_STAMP .*/#define STRATA_ENT_STAMP \"$h\"/" strata_ent.c
echo "stamp $h"

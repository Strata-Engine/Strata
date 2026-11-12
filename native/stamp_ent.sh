#!/bin/sh
# forge decides whether to recompile a C source from its text alone, not from
# the headers it includes (GAPS S14). strata_ent.c includes strata_ent.h,
# so it carries its hash; run this after editing it.
# `native/stamp_ent.sh --check` fails when the stamp is stale.
cd "$(dirname "$0")" || exit 1
h=$(shasum -a 256 < strata_ent.h | cut -c1-16)
if [ "$1" = "--check" ]; then
  grep -q "#define STRATA_ENT_STAMP \"$h\"" strata_ent.c && exit 0
  echo "stale: native/strata_ent.c's STRATA_ENT_STAMP does not match strata_ent.h; run native/stamp_ent.sh"; exit 1
fi
sed -i '' "s/^#define STRATA_ENT_STAMP .*/#define STRATA_ENT_STAMP \"$h\"/" strata_ent.c
echo "stamp $h"

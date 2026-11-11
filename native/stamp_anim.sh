#!/bin/sh
# forge decides whether to recompile a C source from its text alone, not from
# the headers it includes (GAPS S14). strata_anim.c includes strata_anim.h,
# so it carries its hash; run this after editing it.
# `native/stamp_anim.sh --check` fails when the stamp is stale.
cd "$(dirname "$0")" || exit 1
h=$(shasum -a 256 < strata_anim.h | cut -c1-16)
if [ "$1" = "--check" ]; then
  grep -q "#define STRATA_ANIM_STAMP \"$h\"" strata_anim.c && exit 0
  echo "stale: native/strata_anim.c's STRATA_ANIM_STAMP does not match strata_anim.h; run native/stamp_anim.sh"; exit 1
fi
sed -i '' "s/^#define STRATA_ANIM_STAMP .*/#define STRATA_ANIM_STAMP \"$h\"/" strata_anim.c
echo "stamp $h"

#!/bin/sh
# forge decides whether to recompile a C source from its text alone, not from
# the headers it includes (GAPS S14). strata_audio.c includes strata_mix.h and
# strata_hrir.h, so it carries their hash; run this after editing either.
# `native/stamp_audio.sh --check` fails when the stamp is stale.
cd "$(dirname "$0")" || exit 1
h=$(cat strata_mix.h strata_hrir.h | shasum -a 256 | cut -c1-16)
if [ "$1" = "--check" ]; then
  grep -q "#define STRATA_AUDIO_STAMP \"$h\"" strata_audio.c && exit 0
  echo "stale: native/strata_audio.c's STRATA_AUDIO_STAMP does not match its headers; run native/stamp_audio.sh"; exit 1
fi
if grep -q '^#define STRATA_AUDIO_STAMP' strata_audio.c; then
  sed -i '' "s/^#define STRATA_AUDIO_STAMP .*/#define STRATA_AUDIO_STAMP \"$h\"/" strata_audio.c
else
  sed -i '' "s|^#define MA_NO_ENCODING|/* Rewritten by stamp_audio.sh whenever strata_mix.h or strata_hrir.h changes (GAPS S14). */\\
#define STRATA_AUDIO_STAMP \"$h\"\\
#define MA_NO_ENCODING|" strata_audio.c
fi
echo "stamp $h"

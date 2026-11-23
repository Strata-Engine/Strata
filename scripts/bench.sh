#!/bin/sh
# scripts/bench.sh [NAME] [ENV...]: the frame-rate benchmark. A hidden team match with the player at the arena, full lighting, 2400 frames at the
# window's size and the display's scale (no STRATA_PIXELS: a Retina display renders at twice the size asked for, as a played window does).
# Prints, for the frames from 600 on, the median, 99th percentile and worst of: the frame's time, the main thread's march, the GPU and its passes
# (the light pass also in two: tracing, with the figure shadow map, then spreading the traced light over the screen);
# and how many frames went over 8.33 ms (120 Hz). Settings after NAME are added to the run (STRATA_LIGHT=1, ...).
cd "$(dirname "$0")/.." || exit 1
. scripts/env.sh >/dev/null 2>&1
name=${1:-bench}; [ $# -gt 0 ] && shift
out="${TMPDIR:-/tmp}/strata-bench-$name.log"
env MARCH_PIN_MAIN=1 STRATA_HIDDEN=1 STRATA_OFFSCREEN=1 STRATA_SIZE=1280x800 STRATA_SHORE=1 STRATA_COMBAT=1 STRATA_LIGHT=4 \
    STRATA_FRAMES=2400 STRATA_SLOT=0 STRATA_BENCH=1 STRATA_STREAM_POLL="${STRATA_STREAM_POLL:-1}" STRATA_SHOT=/dev/null "$@" "${STRATA_BIN:-./viewer/.march/build/release/strata_viewer}" > "$out" 2>&1
grep '^bench: ' "$out" | awk -v name="$name" '
  NR > 590 { for (i = 2; i <= 10; i++) v[i, ++n[i]] = $i; if ($2 > 8.33) over++ ; if ($4 > 8.33) gover++; m++ }
  function pct(i, p,   k, a, j) { for (k = 1; k <= n[i]; k++) a[k] = v[i, k]; asort_(a, n[i]); j = int(p * n[i]); if (j < 1) j = 1; return a[j] }
  function asort_(a, c,   x, y, t) { for (x = 2; x <= c; x++) { t = a[x]; y = x - 1; while (y > 0 && a[y] + 0 > t + 0) { a[y + 1] = a[y]; y-- } a[y + 1] = t } }
  END {
    split("frame march gpu before geometry light after trace spread", lab, " ")
    printf "bench %s: %d frames; over 8.33 ms: frame %d, gpu %d\n", name, m, over, gover
    for (i = 2; i <= 10; i++) printf "  %-9s median %6.2f  p99 %6.2f  worst %7.2f\n", lab[i - 1], pct(i, 0.5), pct(i, 0.99), pct(i, 1.0)
  }'

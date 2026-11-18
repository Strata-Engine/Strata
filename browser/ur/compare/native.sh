#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
source scripts/env.sh
root="$PWD"
ur_root="$(cd "${STRATA_UR_HOME:-$HOME/code/ur}" && pwd)"
out="$root/browser/.build/ur-comparison"
project="$out/native-project"
mkdir -p "$project/lib/ur"
for source in "$ur_root"/lib/ur/*.march; do
    ln -sfn "$source" "$project/lib/ur/$(basename "$source")"
done
ln -sfn "$root/browser/ur/fixture.march" "$project/lib/ur/browser_fixture.march"
cp browser/ur/compare/ur_compare.march "$project/lib/"
python3 - "$ur_root" "$root" "$project" <<'PY'
from pathlib import Path
import sys

ur, strata, project = map(Path, sys.argv[1:])
manifest = (ur / "forge.toml").read_text()
manifest = manifest.replace('name = "ur"', 'name = "ur_compare"', 1)
manifest = manifest.replace("../strata", str(strata))
(project / "forge.toml").write_text(manifest)
PY
cd "$project"
forge build --release
for variant in day night; do
    night=0
    if [[ "$variant" == night ]]; then night=1; fi
    MARCH_PIN_MAIN=1 STRATA_HIDDEN=1 STRATA_PIXELS=1 UR_COMPARE_NIGHT="$night" \
        STRATA_SHOT="$out/desktop-$variant.ppm" .march/build/release/ur_compare
done

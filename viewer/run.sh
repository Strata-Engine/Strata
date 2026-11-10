#!/bin/sh
# Build and fly the viewer. GLFW must run on the OS main thread, which March
# only guarantees with MARCH_PIN_MAIN=1.
cd "$(dirname "$0")" || exit 1
. ../scripts/env.sh
forge build --release || exit 1
MARCH_PIN_MAIN=1 exec ./.march/build/release/strata_viewer "$@"

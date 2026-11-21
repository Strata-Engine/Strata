#!/bin/sh
# Build and run the Steel Rain prototype. MARCH_PIN_MAIN=1 keeps main on the
# main thread, which GLFW needs on macOS (GAPS S8).
cd "$(dirname "$0")" || exit 1
. ../../scripts/env.sh
forge build --release || exit 1
MARCH_PIN_MAIN=1 ./.march/build/release/steel_rain "$@"

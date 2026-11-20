#!/bin/sh
# The devtools relay end to end: an MCP client launches a hidden viewer
# through tools/strata-mcp/server.mjs and drives it with every tool. Needs a GPU.
cd "$(dirname "$0")/.." || exit 1
. scripts/env.sh
(cd viewer && forge build --release > /dev/null 2>&1) || { echo "FAIL: viewer build"; exit 1; }
node tools/strata-mcp/check.mjs

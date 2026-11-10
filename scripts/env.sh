# Source this: puts the March-main compiler and forge first on PATH.
# The toolchain is a git worktree of ~/code/march at origin/main, built with
# `dune build`; nothing is installed over the system March.
STRATA_MARCH_HOME="${STRATA_MARCH_HOME:-$HOME/code/march-main}"
mkdir -p "$STRATA_MARCH_HOME/.strata-bin"
ln -sf "$STRATA_MARCH_HOME/_build/default/bin/main.exe" "$STRATA_MARCH_HOME/.strata-bin/march"
ln -sf "$STRATA_MARCH_HOME/_build/default/forge/bin/main.exe" "$STRATA_MARCH_HOME/.strata-bin/forge"
export PATH="$STRATA_MARCH_HOME/.strata-bin:$PATH"

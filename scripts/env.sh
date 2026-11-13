# Source this: puts the March-main compiler and forge first on PATH.
# The toolchain is a git worktree of ~/code/march at origin/main, built with
# `dune build`; nothing is installed over the system March.
STRATA_MARCH_HOME="${STRATA_MARCH_HOME:-$HOME/code/march-main}"
mkdir -p "$STRATA_MARCH_HOME/.strata-bin"
ln -sf "$STRATA_MARCH_HOME/_build/default/bin/main.exe" "$STRATA_MARCH_HOME/.strata-bin/march"
ln -sf "$STRATA_MARCH_HOME/_build/default/forge/bin/main.exe" "$STRATA_MARCH_HOME/.strata-bin/forge"
export PATH="$STRATA_MARCH_HOME/.strata-bin:$PATH"
# forge takes the toolchain ~/.march/current names (0.5.0 on the development
# machine) ahead of PATH; an empty MARCH_HOME leaves it none to find, so it
# compiles with the March above.
mkdir -p "$STRATA_MARCH_HOME/.strata-home"
export MARCH_HOME="$STRATA_MARCH_HOME/.strata-home"

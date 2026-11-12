# Source this: puts the March-main compiler and forge first on PATH.
# The toolchain is a git worktree of ~/code/march at origin/main, built with
# `dune build`; nothing is installed over the system March.
STRATA_MARCH_HOME="${STRATA_MARCH_HOME:-$HOME/code/march-main}"
mkdir -p "$STRATA_MARCH_HOME/.strata-bin"
ln -sf "$STRATA_MARCH_HOME/_build/default/bin/main.exe" "$STRATA_MARCH_HOME/.strata-bin/march"
ln -sf "$STRATA_MARCH_HOME/_build/default/forge/bin/main.exe" "$STRATA_MARCH_HOME/.strata-bin/forge"
export PATH="$STRATA_MARCH_HOME/.strata-bin:$PATH"
# forge does not run the `march` on PATH if it can resolve a toolchain: it
# puts the one ~/.march/current names (or a .march-version pin) ahead of PATH,
# which built Strata with March 0.5.0 until 12 November 2026. A MARCH_HOME
# with no `current` link makes forge resolve nothing and fall back to PATH.
mkdir -p "$STRATA_MARCH_HOME/.strata-march-home"
export MARCH_HOME="$STRATA_MARCH_HOME/.strata-march-home"

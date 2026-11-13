# Shared by every case's run.sh (sourced). Builds the March and the C program
# of a case and measures each by CPU time (user + system) and instructions
# retired, from /usr/bin/time -l. Each program reads REPS (how many times the
# kernel runs) and VARIANT from the environment; a row is a REPS=n run minus a
# REPS=0 run, so start-up (the March runtime's, mostly) is not counted, divided
# by the units of work done. Wall-clock time is never used: this machine is
# usually loaded.
#
#   build <case>                    out/<case>_march and out/<case>_c
#   MARCH=path ./run.sh             another compiler (default: the march on PATH after scripts/env.sh,
#                                   march-main; `forge build` on this machine uses ~/.march/current instead)
#   row <label> <binary> <variant> <reps> <units per rep>
#   ir <case>                       out/<case>.ll (March's LLVM IR, unoptimised)
. ../../../scripts/env.sh  # run.sh has already moved into its case directory
mkdir -p out
CFLAGS="-O2 -ffp-contract=off"
MARCH=${MARCH:-march}

build() {
  "$MARCH" --compile --opt 2 -o "out/$1_march" "$1.march" > "out/$1.build.log" 2>&1 || { cat "out/$1.build.log"; exit 1; }
  [ -f "$1.c" ] && { cc $CFLAGS -o "out/$1_c" "$1.c" -lpthread || exit 1; }
  printf "%-44s %12s %14s\n" "case (per unit of work)" "cpu ns" "instructions"
}

ir() {
  cp "$1.march" "out/$1_ir.march" && (cd out && "$MARCH" --emit-llvm "$1_ir.march" > /dev/null 2>&1; mv "$1_ir.ll" "$1.ll" 2>/dev/null; rm -f "$1_ir.march")
}

measure() { # binary variant reps -> "cpu_seconds instructions"
  VARIANT=$2 REPS=$3 /usr/bin/time -l "$1" 2>&1 >/dev/null | awk '
    / real / { cpu = $3 + $5 } /instructions retired/ { ins = $1 } END { printf "%s %s\n", cpu, ins }'
}

row() { # label binary variant reps units_per_rep
  a=$(measure "$2" "$3" 0); b=$(measure "$2" "$3" "$4")
  echo "$a $b" | awk -v l="$1" -v d="$(($4 * $5))" '{ printf "%-44s %12.2f %14.1f\n", l, ($3 - $1) * 1e9 / d, ($4 - $2) / d }'
}

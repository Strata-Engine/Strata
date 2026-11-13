#!/bin/sh
cd "$(dirname "$0")" && . ../common.sh
build short_circuit
row "March, i < 0 && costly(i)"        out/short_circuit_march 0 15000 4096
row "March, nested if"                 out/short_circuit_march 1 300000 4096
row "C, i < 0 && costly(i)"            out/short_circuit_c     0 300000 4096
echo "does the right side of \`false && said(1)\` run? (a line saying so, then the value):"
PROBE=1 REPS=0 out/short_circuit_march | tail -n +2 | sed 's/^/  /'
ir short_circuit

#!/bin/sh
cd "$(dirname "$0")" && . ../common.sh
build closure
row "March, closure per element"       out/closure_march 0 8000 32768
row "March, direct call"               out/closure_march 1 8000 32768
row "C, function pointer per element"  out/closure_c     0 20000 32768
row "C, direct call"                   out/closure_c     1 20000 32768
ir closure

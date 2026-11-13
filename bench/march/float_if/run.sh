#!/bin/sh
cd "$(dirname "$0")" && . ../common.sh
build float_if
row "March, Float-valued if"      out/float_if_march 0 20000 4095
row "March, arithmetic instead"   out/float_if_march 1 60000 4095
row "C, ?: "                      out/float_if_c     0 200000 4095
row "C, arithmetic instead"       out/float_if_c     1 100000 4095
ir float_if

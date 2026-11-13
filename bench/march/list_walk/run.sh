#!/bin/sh
cd "$(dirname "$0")" && . ../common.sh
build list_walk
row "March, List of Float tuples"   out/list_walk_march 0 3000 32768
row "March, flat NativeF32Arr"      out/list_walk_march 1 5000 32768
row "C, linked list of structs"     out/list_walk_c     0 10000 32768
row "C, flat float array"           out/list_walk_c     1 10000 32768
ir list_walk

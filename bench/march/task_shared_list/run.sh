#!/bin/sh
# Not a timing: how many of 20 runs die. MARCH=path picks another compiler.
cd "$(dirname "$0")" && . ../../../scripts/env.sh
mkdir -p out
"${MARCH:-march}" --compile --opt 2 -o out/task_shared_list_march task_shared_list.march > out/build.log 2>&1 || { cat out/build.log; exit 1; }
for v in 0 1; do
  died=0; last=0
  for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
    VARIANT=$v REPS=20000 out/task_shared_list_march > /dev/null 2>&1; rc=$?
    [ $rc -ne 0 ] && { died=$((died + 1)); last=$rc; }
  done
  [ $v -eq 0 ] && name="spawner reads the list after starting the task" || name="spawner finishes with the list first"
  echo "$name: $died of 20 runs died (last exit code $last)"
done

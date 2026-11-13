#!/bin/sh
# units: one read of the record's array through the record
cd "$(dirname "$0")" && . ../common.sh
build shared_tasks
export TASKS=1
row "March, shared record, 1 task"           out/shared_tasks_march 0 60000 4096
row "March, own copy, 1 task"                out/shared_tasks_march 1 60000 4096
row "March, shared, unpacked once, 1 task"   out/shared_tasks_march 2 150000 4096
row "C, shared struct, 1 thread"             out/shared_tasks_c     0 400000 4096
export TASKS=8
row "March, shared record, 8 tasks"          out/shared_tasks_march 0 60 32768
row "March, own copy, 8 tasks"               out/shared_tasks_march 1 8000 32768
row "March, shared, unpacked once, 8 tasks"  out/shared_tasks_march 2 30000 32768
row "C, shared struct, 8 threads"            out/shared_tasks_c     0 100000 32768
ir shared_tasks

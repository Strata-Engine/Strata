# task_shared_list

A memory-safety bug, not a speed gap. A list captured by a task's closure
and then read again by the thread that spawned the task (here
`List.length(items)` straight after `task_spawn(fn _ -> walk(items, 0))`)
kills the program: 18 of 20 runs die with a segmentation fault or a bus
error (exit 139 or 138) on March 07b58d065. When the spawner takes the
length first and the `task_spawn` is its last use of the list, 0 of 20 die.
Cause not read from the generated code; the behaviour fits two threads
updating the same list cells' reference counts without atomic operations
(the spawner walking the list while the task walks and releases it). Met in
`Strata.Stream.send_gens` when a generation task was first given a list of
keys: the stream tests died in 6 of 14 runs until the spawner stopped
touching the list after the spawn. Until March fixes it, the rule in
Strata is: whatever a task's closure captures is not used again by the
spawning thread, except values it only compares or passes on whole.

Run `./run.sh`.

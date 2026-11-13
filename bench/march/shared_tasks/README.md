# shared_tasks

GAPS S15: a read-only record handed to several tasks and read in each one's
hot loop through an accessor (`match r do Reg(a, _, _) -> a end`). With one
task a read costs 4.56 ns; with 8 tasks reading the one record it costs 778
ns of CPU a read (580 ns on 0.5.0), 160 times the 4.84 ns of 8 tasks that
each built their own copy first, and 4600 times C's 0.17 ns for 8 threads
reading one shared struct. The accessor takes the record owned: the loop
increments the record's reference count before the call, the accessor
decrements it, increments the array field's count because the record is
still alive, and the loop decrements that after the read (`r_arr` and
`through` in `out/shared_tasks.ll`: `__march_rc_incrc_local`,
`__march_rc_decrc_freed`, `__march_rc_incrc` (an `atomicrmw add`),
`__march_rc_decrc_local`). Four count updates a read on two cache lines that
all eight cores write. Unpacking the field once outside the loop and passing
the array down costs 1.25 ns a read whether shared or not, so the workaround
is to unpack before the loop, not only to copy.

Run `./run.sh` (CPU time is summed over the tasks).

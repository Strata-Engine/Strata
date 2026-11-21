# record_field — an array field written through a record update copies the array

`{ r with rf_a: NativeArray.set_int(r.rf_a, i, v) }` reads the array out of
`r`, but `r` is still alive (the update uses it), so the array has two owners
when it is written and the runtime copies it whole. Written in a loop, every
turn copies. Variant 2 threads the bare array and builds the record once.

Measured 2026-11-21 on march-main `f04f0dc0c`, `./run.sh` (per write):

| variant | SIZE 64 | SIZE 262144 |
|---|---|---|
| 0, the update in the loop | 300 instructions | 401 302 instructions (about 30 us) |
| 1, through a helper (as `StrataMaps.Data.with_water`) | 535 | 400 849 |
| 2, the bare array threaded | 30 | 919 |

Met in StrataMaps' water and lava stages (GAPS S55).

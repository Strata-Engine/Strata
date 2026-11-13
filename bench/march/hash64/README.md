# hash64

March Ints are 63 bits and `+ - *` trap on overflow, so `Strata.Seed` hashes
in 32-bit lanes, masking every product. One `Seed.cell(seed, salt, x, y, z)`
costs 27.8 ns and 241 instructions in March against 3.99 ns for the same
lane arithmetic in C (7 times; C also folds the constant `mix(seed, salt)`
out of the loop, which March's overflow-checked multiplies keep in) and 1.54
ns for the 64-bit splitmix-style mix C would write (18 times). march-main
now has `int_wrapping_mul` (wrapping at 63 bits): the same mix in one 63-bit
lane costs 3.50 ns in March, 8 times faster than the lanes and 1.9 times C's
1.88 ns for the 63-bit form. So on march-main this gap is closed by the
language and `Strata.Seed` could move to one lane (its values would change).
March 0.5.0, which `forge build` uses on this machine, has no wrapping
arithmetic: this case does not compile there.

Run `./run.sh`.

/* The C of divmod.march (signed 64-bit, as March's Int). */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

__attribute__((noinline)) static int64_t with_div(int64_t i) {
    int64_t acc = 0;
    for (; i < 32768; i++) acc += i % 32 + (i / 32) % 32 + i / 1024;
    return acc;
}
__attribute__((noinline)) static int64_t with_shift(int64_t i) {
    int64_t acc = 0;
    for (; i < 32768; i++) acc += (i & 31) + ((i >> 5) & 31) + (i >> 10);
    return acc;
}

int main(void) {
    int reps = getenv("REPS") ? atoi(getenv("REPS")) : 1;
    int variant = getenv("VARIANT") ? atoi(getenv("VARIANT")) : 0;
    int64_t acc = 0;
    for (int r = reps; r > 0; r--) acc = (acc + (variant == 0 ? with_div(r % 3) : with_shift(r % 3))) & 1048575;
    printf("%lld\n", (long long)acc);
    return 0;
}

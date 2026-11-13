/* The C of short_circuit.march. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

static int64_t a[1024];
__attribute__((noinline)) static int costly(const int64_t *a, int64_t i) {
    int64_t acc = 0;
    for (int k = 0; k < 16; k++) acc += a[(i + k) & 1023];
    return acc > 0;
}
__attribute__((noinline)) static int64_t with_and(const int64_t *a, int64_t i, int64_t n) {
    int64_t acc = 0;
    for (; i < n; i++) if (i < 0 && costly(a, i)) acc++;
    return acc;
}

int main(void) {
    int reps = getenv("REPS") ? atoi(getenv("REPS")) : 1;
    for (int k = 0; k < 1024; k++) a[k] = 1;
    int64_t acc = 0;
    for (int r = reps; r > 0; r--) acc += with_and(a, r % 3, 4096);
    printf("%lld\n", (long long)acc);
    return 0;
}

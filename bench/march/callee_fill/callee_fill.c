/* The C of callee_fill.march: the callee writes through the caller's pointer. */
#include <stdio.h>
#include <stdlib.h>

__attribute__((noinline)) static int fill_into(float *out) {
    for (int i = 0; i < 32768; i++) out[i] = (float)i;
    return 32768;
}

int main(void) {
    int reps = getenv("REPS") ? atoi(getenv("REPS")) : 1;
    double acc = 0.0;
    for (int r = reps; r > 0; r--) {
        float *out = calloc(32768, sizeof(float));
        int n = fill_into(out);
        acc += out[100] + (double)(n - 32768);
        free(out);
    }
    printf("%f\n", acc);
    return 0;
}

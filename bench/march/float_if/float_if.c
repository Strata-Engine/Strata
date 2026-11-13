/* The C of float_if.march: the largest clamped step down a row. */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static float h[4096];

__attribute__((noinline)) static double with_if(const float *h, int n) {
    double acc = 0.0;
    for (int k = 0; k < n - 1; k++) {
        double e = (double)h[k] - (double)h[k + 1] - 0.25;
        double c = e > 0.0 ? e : 0.0;
        acc = acc > c ? acc : c;
    }
    return acc;
}

__attribute__((noinline)) static double no_if(const float *h, int n) {
    double acc = 0.0;
    for (int k = 0; k < n - 1; k++) {
        double e = (double)h[k] - (double)h[k + 1] - 0.25;
        double c = 0.5 * (e + fabs(e));
        acc = 0.5 * (acc + c + fabs(acc - c));
    }
    return acc;
}

int main(void) {
    int reps = getenv("REPS") ? atoi(getenv("REPS")) : 1;
    int variant = getenv("VARIANT") ? atoi(getenv("VARIANT")) : 0;
    for (int k = 0; k < 4096; k++) h[k] = (float)((k * 7) % 17) * 0.5f;
    double acc = 0.0;
    for (int r = 0; r < reps; r++) acc += variant == 0 ? with_if(h, 4096) : no_if(h, 4096);
    printf("%f\n", acc);
    return 0;
}

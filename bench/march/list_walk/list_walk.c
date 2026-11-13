/* The C of list_walk.march. VARIANT 0: a linked list of samples. VARIANT 1: a
 * flat float array, four floats a sample. */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

typedef struct node { double x, z, w, h; struct node *next; } node;

static inline double fmin2(double a, double b) { return 0.5 * (a + b - fabs(a - b)); }
static inline double clamp01(double v) { return fmin2(1.0, 0.5 * (v + fabs(v))); }

__attribute__((noinline)) static double damp_list(const node *rs, double xm, double zm) {
    double best = 1.0;
    for (; rs; rs = rs->next) {
        double dx = xm - rs->x, dz = zm - rs->z;
        best = fmin2(best, clamp01((sqrt(dx * dx + dz * dz) - 0.5 * rs->w) / (2.0 * rs->w)));
    }
    return best;
}
__attribute__((noinline)) static double damp_flat(const float *rs, int n, double xm, double zm) {
    double best = 1.0;
    for (int k = 0; k < n; k++) {
        double dx = xm - rs[4 * k], dz = zm - rs[4 * k + 1], w = rs[4 * k + 2];
        best = fmin2(best, clamp01((sqrt(dx * dx + dz * dz) - 0.5 * w) / (2.0 * w)));
    }
    return best;
}

int main(void) {
    int reps = getenv("REPS") ? atoi(getenv("REPS")) : 1;
    int variant = getenv("VARIANT") ? atoi(getenv("VARIANT")) : 0;
    node *ls = NULL;
    float fl[128];
    for (int k = 31; k >= 0; k--) {
        node *n = malloc(sizeof(node));
        n->x = (k * 13) % 64; n->z = (k * 29) % 64; n->w = 2.0 + k % 5; n->h = 40.0; n->next = ls; ls = n;
        fl[4 * k] = (float)n->x; fl[4 * k + 1] = (float)n->z; fl[4 * k + 2] = (float)n->w; fl[4 * k + 3] = 40.0f;
    }
    double acc = 0.0;
    for (int r = reps; r > 0; r--)
        for (int c = 0; c < 1024; c++)
            acc += variant == 0 ? damp_list(ls, c % 32, c / 32) : damp_flat(fl, 32, c % 32, c / 32);
    printf("%f\n", acc);
    return 0;
}

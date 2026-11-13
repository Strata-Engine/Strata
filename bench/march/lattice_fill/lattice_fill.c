/* The C of lattice_fill.march. VARIANT 0: the same per-voxel trilinear fill.
 * VARIANT 1: Strata's separable kernel (sl_fill in native/strata_noise.c). */
#include <stdio.h>
#include <stdlib.h>

static float s[405];

static inline double lerp(double a, double b, double t) { return a + (b - a) * t; }

__attribute__((noinline)) static void fill_naive(const float *s, float *out) {
    for (int i = 0; i < 32768; i++) {
        int x = i % 32, z = (i / 32) % 32, y = i / 1024;
        int b = x / 4 + 9 * (y / 8 + 5 * (z / 4));
        double tx = (double)(x % 4) * 0.25, ty = (double)(y % 8) * 0.125, tz = (double)(z % 4) * 0.25;
        double c00 = lerp(s[b], s[b + 1], tx), c10 = lerp(s[b + 9], s[b + 10], tx);
        double c01 = lerp(s[b + 45], s[b + 46], tx), c11 = lerp(s[b + 54], s[b + 55], tx);
        out[i] = (float)lerp(lerp(c00, c10, ty), lerp(c01, c11, ty), tz);
    }
}

__attribute__((noinline)) static void fill_separable(const float *s, float *o) {
    float plane[81], row[9];
    for (int y = 0; y < 32; y++) {
        int j = y >> 3;
        float ty = (float)(y & 7) * 0.125f;
        for (int k = 0; k < 9; k++)
            for (int i = 0; i < 9; i++) {
                float a = s[i + 9 * (j + 5 * k)], b = s[i + 9 * (j + 1 + 5 * k)];
                plane[i + 9 * k] = a + (b - a) * ty;
            }
        for (int z = 0; z < 32; z++) {
            int k = z >> 2;
            float tz = (float)(z & 3) * 0.25f;
            for (int i = 0; i < 9; i++) {
                float a = plane[i + 9 * k], b = plane[i + 9 * (k + 1)];
                row[i] = a + (b - a) * tz;
            }
            float *line = o + 32 * (z + 32 * y);
            for (int c = 0; c < 8; c++) {
                float a = row[c], d = row[c + 1] - a;
                line[4 * c] = a; line[4 * c + 1] = a + 0.25f * d;
                line[4 * c + 2] = a + 0.5f * d; line[4 * c + 3] = a + 0.75f * d;
            }
        }
    }
}

int main(void) {
    int reps = getenv("REPS") ? atoi(getenv("REPS")) : 1;
    int variant = getenv("VARIANT") ? atoi(getenv("VARIANT")) : 0;
    for (int k = 0; k < 405; k++) s[k] = (float)((k * 37) % 101) * 0.01f;
    double acc = 0.0;
    for (int r = reps; r > 0; r--) {
        float *out = calloc(32768, sizeof(float));
        if (variant == 0) fill_naive(s, out); else fill_separable(s, out);
        acc += out[12345 + r % 7];
        free(out);
    }
    printf("%f\n", acc);
    return 0;
}

/* The C of voxel_loop.march. */
#include <stdio.h>
#include <stdlib.h>

static float hs[1024], ns[32768];

__attribute__((noinline)) static void dens(const float *hs, const float *ns, float *out, double y0, double sq, double amp) {
    for (int i = 0; i < 32768; i++)
        out[i] = (float)(((double)hs[i % 1024] - (y0 + (double)(i / 1024))) * sq + amp * (double)ns[i]);
}

int main(void) {
    int reps = getenv("REPS") ? atoi(getenv("REPS")) : 1;
    for (int k = 0; k < 1024; k++) hs[k] = (float)((k * 31) % 257) * 0.125f;
    for (int k = 0; k < 32768; k++) ns[k] = (float)((k * 31) % 17) * 0.125f;
    double acc = 0.0;
    for (int r = reps; r > 0; r--) {
        float *out = calloc(32768, sizeof(float));
        dens(hs, ns, out, 8.0, 1.0, 12.0);
        acc += out[12345 + r % 7];
        free(out);
    }
    printf("%f\n", acc);
    return 0;
}

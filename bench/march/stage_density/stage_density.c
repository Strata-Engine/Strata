/* The C of stage_density.march: the viewer's per-voxel density over 32768
 * voxels, the same order of float operations (doubles from float arrays). */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static inline double fmax2(double p, double q) { return p > q ? p : q; }
static inline double fmin2(double p, double q) { return p < q ? p : q; }

static inline double shaped(double a, double sea, double n, double h, double cm, double ym) {
    int low = h - a < sea;
    double n1 = n >= 0.0 ? n : cm < 1.0 ? 0.0 : low ? 0.0 : n;
    return (ym > h && n1 > 0.0) ? 0.25 * n1 : n1;
}

static inline double voxel_density(double a, double sq, double sea, double tw, double co, double roof, double depth, double ent,
                                   double h, double cm, double en, double f, double n, double ta, double tb, double cav, double ym, double seam) {
    double d = (h - ym) * sq + a * cm * shaped(a, sea, n, h, cm, ym);
    double carve = fmin2(fmax2(fabs(ta), fabs(tb)) - tw * f, (1.0 - co * f) - cav);
    if (carve >= 0.0) return d;
    if (cm < 1.0) return d;
    if (ym <= seam) return d;
    if (ym < h - a - depth) return d;
    if (d > roof) return -1.0;
    if (en > ent) return -1.0;
    return d;
}

static float hs[1024], cms[1024], ens[1024], fs[1024], dn[32768], da[32768], db[32768], dc[32768];

__attribute__((noinline)) static void dens_loop(float *out, double y0m, double scale, double seam) {
    for (int i = 0; i < 32768; i++) {
        int c = i % 1024;
        out[i] = (float)voxel_density(12.0, 1.0, 40.0, 0.08, 0.45, 6.0, 40.0, 0.6, hs[c], cms[c], ens[c], fs[c],
                                      dn[i], da[i], db[i], dc[i], y0m + (double)(i / 1024) * scale, seam);
    }
}

static void cols(float *a, double base, double amp, double fx, double fz) {
    for (int k = 0; k < 1024; k++) a[k] = (float)(base + amp * sin((double)(k % 32) * fx) * cos((double)(k / 32) * fz));
}
static void vox(float *a, double amp, double fx, double fy, double fz) {
    for (int k = 0; k < 32768; k++)
        a[k] = (float)(amp * sin((double)(k % 32) * fx + (double)(k / 1024) * fy) * cos((double)((k / 32) % 32) * fz));
}

int main(void) {
    int reps = getenv("REPS") ? atoi(getenv("REPS")) : 1;
    cols(hs, 80.0, 8.0, 0.21, 0.17); cols(cms, 1.0, 0.0, 0.0, 0.0); cols(ens, 0.3, 0.5, 0.13, 0.19); cols(fs, 1.0, 0.0, 0.0, 0.0);
    vox(dn, 0.8, 0.11, 0.05, 0.13); vox(da, 0.9, 0.31, 0.23, 0.29); vox(db, 0.9, 0.27, 0.33, 0.21); vox(dc, 0.7, 0.07, 0.09, 0.08);
    double acc = 0.0;
    float *out = calloc(32768, sizeof(float));
    for (int r = reps; r > 0; r--) { dens_loop(out, 64.0, 1.0, 41.0); acc += out[12345 + r % 7]; }
    printf("%f\n", acc);
    dens_loop(out, 64.0, 1.0, 41.0);
    double s = 0.0; int cut = 0;
    for (int k = 0; k < 32768; k++) { s += (double)out[k] * (double)(k % 7); cut += out[k] == -1.0f; }
    printf("%f\n", s);
    if (getenv("CUTS")) printf("cut voxels: %d\n", cut);
    return 0;
}

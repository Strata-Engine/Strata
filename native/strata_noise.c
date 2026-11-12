/* strata_noise.c -- Strata's noise substrate: FastNoiseLite behind a flat C
 * interface. A field call fills a caller-owned native float array in one go,
 * so a generator pays the FFI crossing once per field and not once per voxel.
 *
 * Everything is a pure function of its arguments: no state survives a call,
 * so calls are safe from any number of worker tasks at once. Build with
 * -ffp-contract=off so the same seed gives the same field on every compiler
 * and architecture.
 *
 * Kinds: 0 OpenSimplex2 (3D rotated, ImproveXZ), 1 OpenSimplex2S, 2 cellular
 * cell value, 3 cellular F2 - F1. Fractals: 0 none, 1 fBm, 2 ridged.
 * Output is [-1, 1], except ridged, which is mapped to [0, 1].
 *
 * March values: a NativeArray arrives as its heap pointer; the element count
 * is at +16 and the data at +32.
 */
#include <stdint.h>
#define FNL_IMPL
#include "FastNoiseLite.h"

#define NARR_HDR 32
static inline int64_t narr_len(void *a)  { return *(int64_t *)((char *)a + 16); }
static inline void   *narr_data(void *a) { return (char *)a + NARR_HDR; }

/* Seeds are truncated to 32 bits; the warp gets a seed of its own so the
 * displacement is not the noise's own gradient. */
#define SN_WARP_SEED_OFFSET 1013

static fnl_state sn_state(int64_t kind, int64_t octaves, double lac, double gain,
                          int64_t fractal, int64_t seed, double freq) {
    fnl_state s = fnlCreateState();
    s.seed = (int)(uint32_t)seed;
    s.frequency = (float)freq;
    s.rotation_type_3d = FNL_ROTATION_IMPROVE_XZ_PLANES;
    switch (kind) {
    case 1:  s.noise_type = FNL_NOISE_OPENSIMPLEX2S; break;
    case 2:  s.noise_type = FNL_NOISE_CELLULAR;
             s.cellular_return_type = FNL_CELLULAR_RETURN_TYPE_CELLVALUE; break;
    case 3:  s.noise_type = FNL_NOISE_CELLULAR;
             s.cellular_return_type = FNL_CELLULAR_RETURN_TYPE_DISTANCE2SUB; break;
    default: s.noise_type = FNL_NOISE_OPENSIMPLEX2; break;
    }
    s.fractal_type = fractal == 1 ? FNL_FRACTAL_FBM : fractal == 2 ? FNL_FRACTAL_RIDGED : FNL_FRACTAL_NONE;
    s.octaves = octaves < 1 ? 1 : octaves > 16 ? 16 : (int)octaves;
    s.lacunarity = (float)lac;
    s.gain = (float)gain;
    return s;
}

static fnl_state sn_warp_state(int64_t seed, double freq, double warp) {
    fnl_state w = fnlCreateState();
    w.seed = (int)((uint32_t)seed + SN_WARP_SEED_OFFSET);
    w.frequency = (float)freq;
    w.fractal_type = FNL_FRACTAL_NONE;
    w.domain_warp_amp = (float)warp;
    return w;
}

static float sn_shape(float v, int64_t fractal) {
    if (fractal == 2) v = (v + 1.0f) * 0.5f;
    float lo = fractal == 2 ? 0.0f : -1.0f;
    return v < lo ? lo : v > 1.0f ? 1.0f : v;
}

static float sn_one2(fnl_state *s, fnl_state *w, int64_t fractal, double warp, double x, double z) {
    FNLfloat fx = (FNLfloat)x, fz = (FNLfloat)z;
    if (warp != 0.0) fnlDomainWarp2D(w, &fx, &fz);
    return sn_shape(fnlGetNoise2D(s, fx, fz), fractal);
}

static float sn_one3(fnl_state *s, fnl_state *w, int64_t fractal, double warp, double x, double y, double z) {
    FNLfloat fx = (FNLfloat)x, fy = (FNLfloat)y, fz = (FNLfloat)z;
    if (warp != 0.0) fnlDomainWarp3D(w, &fx, &fy, &fz);
    return sn_shape(fnlGetNoise3D(s, fx, fy, fz), fractal);
}

int64_t sn_field2(int64_t kind, int64_t octaves, double lac, double gain, int64_t fractal, double warp,
                  int64_t seed, double freq, double x0, double z0, double step,
                  int64_t w, int64_t d, void *out) {
    if (w <= 0 || d <= 0 || w > INT64_MAX / d || narr_len(out) < w * d) return 0;
    fnl_state s = sn_state(kind, octaves, lac, gain, fractal, seed, freq);
    fnl_state ws = sn_warp_state(seed, freq, warp);
    float *o = (float *)narr_data(out);
    for (int64_t j = 0; j < d; j++)
        for (int64_t i = 0; i < w; i++)
            o[i + w * j] = sn_one2(&s, &ws, fractal, warp, x0 + (double)i * step, z0 + (double)j * step);
    return w * d;
}

int64_t sn_field3(int64_t kind, int64_t octaves, double lac, double gain, int64_t fractal, double warp,
                  int64_t seed, double freq, double x0, double y0, double z0,
                  double sx, double sy, double sz, int64_t w, int64_t h, int64_t d, void *out) {
    if (w <= 0 || h <= 0 || d <= 0 || w > INT64_MAX / h || w * h > INT64_MAX / d || narr_len(out) < w * h * d) return 0;
    fnl_state s = sn_state(kind, octaves, lac, gain, fractal, seed, freq);
    fnl_state ws = sn_warp_state(seed, freq, warp);
    float *o = (float *)narr_data(out);
    for (int64_t k = 0; k < d; k++)
        for (int64_t j = 0; j < h; j++)
            for (int64_t i = 0; i < w; i++)
                o[i + w * (j + h * k)] = sn_one3(&s, &ws, fractal, warp,
                    x0 + (double)i * sx, y0 + (double)j * sy, z0 + (double)k * sz);
    return w * h * d;
}

double sn_sample2(int64_t kind, int64_t octaves, double lac, double gain, int64_t fractal, double warp,
                  int64_t seed, double freq, double x, double z) {
    fnl_state s = sn_state(kind, octaves, lac, gain, fractal, seed, freq);
    fnl_state ws = sn_warp_state(seed, freq, warp);
    return (double)sn_one2(&s, &ws, fractal, warp, x, z);
}

double sn_sample3(int64_t kind, int64_t octaves, double lac, double gain, int64_t fractal, double warp,
                  int64_t seed, double freq, double x, double y, double z) {
    fnl_state s = sn_state(kind, octaves, lac, gain, fractal, seed, freq);
    fnl_state ws = sn_warp_state(seed, freq, warp);
    return (double)sn_one3(&s, &ws, fractal, warp, x, y, z);
}

/* Strata.Lattice's fill: 9 x 5 x 9 samples (one every 4, 8, 4 voxels, index
 * i + 9 * (j + 5 * k)) to a 32-cubed chunk's 32768 values at
 * x + 32 * (z + 32 * y), trilinearly. Here and not in March because the
 * caller owns `out` and March native arrays are copy-on-write: only a C
 * write fills the caller's array. Separable: a 9 x 9 plane per voxel row,
 * a row of 9 per z, then four voxels a cell along x. The fractions are the
 * constants k / 8 and k / 4, so there are no weight tables. Returns 32768,
 * or 0 (writing nothing) when either array is too small. */
int64_t sl_fill(void *samples, void *out) {
    if (narr_len(samples) < 405 || narr_len(out) < 32768) return 0;
    const float *s = (const float *)narr_data(samples);
    float *o = (float *)narr_data(out);
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
                line[4 * c] = a;
                line[4 * c + 1] = a + 0.25f * d;
                line[4 * c + 2] = a + 0.5f * d;
                line[4 * c + 3] = a + 0.75f * d;
            }
        }
    }
    return 32768;
}

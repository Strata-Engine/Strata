/* mix_util.h — helpers the mixer's C tests and demo share: build a patch as
 * floats, send it as PATCH records, play, render, measure. Test code only;
 * the library sends patches from March. */
#ifndef STRATA_MIX_UTIL_H
#define STRATA_MIX_UTIL_H
#include <math.h>
#include <string.h>
#include "strata_mix.h"

static inline void mu_patch_clear(float *p) {
    memset(p, 0, sizeof(float) * SM_PATCH_FLOATS);
    p[SM_P_GAIN] = 1.0f;
    /* an envelope that is fully open: instant attack, sustain 1, short release */
    p[SM_P_ENV(0) + 2] = 1.0f; p[SM_P_ENV(0) + 3] = 0.005f;
}
static inline void mu_source(float *p, int i, int kind, float hz, float level) {
    p[SM_P_SRC(i)] = (float)kind; p[SM_P_SRC(i) + 1] = hz; p[SM_P_SRC(i) + 2] = level;
}
static inline void mu_filter(float *p, int i, int kind, float cutoff, float res) {
    p[SM_P_FLT(i)] = (float)kind; p[SM_P_FLT(i) + 1] = cutoff; p[SM_P_FLT(i) + 2] = res;
}
static inline void mu_env(float *p, int i, float a, float d, float s, float r, float depth) {
    p[SM_P_ENV(i)] = a; p[SM_P_ENV(i) + 1] = d; p[SM_P_ENV(i) + 2] = s; p[SM_P_ENV(i) + 3] = r; p[SM_P_ENV(i) + 4] = depth;
}
static inline void mu_control(float *p, int i, int target, float low, float high, int curve, float initial) {
    p[SM_P_CTL(i)] = (float)target; p[SM_P_CTL(i) + 1] = low; p[SM_P_CTL(i) + 2] = high;
    p[SM_P_CTL(i) + 3] = (float)curve; p[SM_P_CTL(i) + 4] = initial;
}
static inline void mu_send_patch(SmMixer *m, int id, const float *p) {
    for (int r = 0; r * 12 < SM_PATCH_FLOATS; r++) {
        SmCmd c; memset(&c, 0, sizeof c);
        c.op = SM_OP_PATCH; c.voice = id; c.a = r;
        for (int k = 0; k < 12 && r * 12 + k < SM_PATCH_FLOATS; k++) c.f[k] = p[r * 12 + k];
        sm_command(m, &c);
    }
}
static inline void mu_play_flat(SmMixer *m, int voice, int src, float gain, float pan) {
    SmCmd c; memset(&c, 0, sizeof c);
    c.op = SM_OP_PLAY; c.voice = voice; c.a = src; c.b = 0;
    c.f[0] = gain; c.f[1] = 1.0f; c.f[2] = 1.0f; c.f[6] = 1.0f; c.f[7] = pan;
    sm_command(m, &c);
}
static inline void mu_cmd(SmMixer *m, int op, int voice, int a, float f0) {
    SmCmd c; memset(&c, 0, sizeof c);
    c.op = op; c.voice = voice; c.a = a; c.f[0] = f0;
    sm_command(m, &c);
}
/* power of both channels over [from, to) frames, as the RMS of the mono source */
static inline double mu_rms(const float *st, int from, int to) {
    double s = 0.0;
    for (int i = from; i < to; i++) s += (double)st[2 * i] * st[2 * i] + (double)st[2 * i + 1] * st[2 * i + 1];
    return sqrt(s / (to - from));
}
static inline double mu_db(double v) { return 20.0 * log10(v > 1e-9 ? v : 1e-9); }
static inline float mu_peak(const float *st, int from, int to) {
    float p = 0.0f;
    for (int i = 2 * from; i < 2 * to; i++) { float a = fabsf(st[i]); if (a > p) p = a; }
    return p;
}
#endif

/* strata_mix.h — Strata's mixer: the command ring, voices, patches, the
 * filters and envelopes, the limiter, WAV writing and fingerprints. Plain C:
 * no miniaudio, no device, no globals, so it is tested headless
 * (native/tests/mix_test.c). native/strata_audio.c binds it to a device or
 * to an offline render.
 *
 * Threads: sm_command and sm_mix run on ONE thread (the device thread, or
 * the caller's in game time). Other threads reach the mixer only through
 * an SmRing. Nothing here allocates after sm_open. */
#ifndef STRATA_MIX_H
#define STRATA_MIX_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdatomic.h>

#define SM_RATE 48000
#define SM_BLOCK 256
#define SM_MIXED 64
#define SM_TRACKED 1024
#define SM_PATCHES 2048          /* patch ids 1..2047 */
#define SM_REC_FIRST 2048        /* recording ids 2048..4095 */
#define SM_RECS 2048
#define SM_GROUPS 4
#define SM_LIMIT 0.8912f         /* -1 dBFS */
#ifndef SM_PI
#define SM_PI 3.14159265358979323846
#endif

/* ---- the command record and its ops ---- */
typedef struct { int32_t op, voice, a, b; float f[12]; } SmCmd;
enum {
    SM_OP_PATCH = 1,     /* voice = patch id, a = record index, f = 12 patch floats */
    SM_OP_PLAY = 2,      /* a = source id, b = group; f = gain, pitch, priority, x, y, z, flat, pan */
    SM_OP_MOVE = 3,      /* f = x, y, z */
    SM_OP_CONTROL = 4,   /* a = control 0..3, f0 = value 0..1 */
    SM_OP_STOP = 5,      /* f0 = fade seconds; negative: release through the patch's envelope */
    SM_OP_LISTENER = 6,
    SM_OP_OCCLUDE = 7,
    SM_OP_ENVIRON = 8,
    SM_OP_GROUP = 9,     /* a = group, f0 = gain */
    SM_OP_OUTPUT = 10    /* a = 0 speakers, 1 headphones */
};

/* ---- the patch: SM_PATCH_FLOATS floats, sent as PATCH records of 12 ---- */
#define SM_PATCH_FLOATS 72
#define SM_PATCH_RECORDS 6
#define SM_P_SRC(i) ((i) * 5)          /* kind, hz (or rate for a recording), level, detune cents, recording id */
#define SM_P_FLT(i) (20 + (i) * 3)     /* kind, cutoff Hz, resonance (Q) */
#define SM_P_ENV(i) (26 + (i) * 5)     /* attack, decay, sustain, release (s), depth (env 1: octaves on filter 1) */
#define SM_P_MOD(i) (36 + (i) * 4)     /* kind, rate Hz, target, depth */
#define SM_P_CTL(i) (44 + (i) * 5)     /* target, low, high, curve, initial value */
#define SM_P_HOLD 64                   /* seconds before release begins by itself; 0 = until stopped */
#define SM_P_GAIN 65
#define SM_P_LOOP 66                   /* 1: recording sources loop */
enum { SM_SRC_OFF, SM_SRC_SINE, SM_SRC_TRIANGLE, SM_SRC_SAW, SM_SRC_SQUARE, SM_SRC_WHITE, SM_SRC_PINK, SM_SRC_BROWN, SM_SRC_RECORDING };
enum { SM_FLT_OFF, SM_FLT_LOW, SM_FLT_HIGH, SM_FLT_BAND, SM_FLT_PEAK };
enum { SM_MOD_OFF, SM_MOD_LFO, SM_MOD_WALK };
/* modulator and control targets. A modulator's depth is semitones (pitch),
 * octaves (cutoff) or a share of the gain. A control maps 0..1 to low..high
 * and multiplies the pitch or the gain, or sets a level or a cutoff. */
enum { SM_TGT_PITCH, SM_TGT_CUTOFF, SM_TGT_GAIN, SM_TGT_LEVEL1, SM_TGT_CUTOFF2 };

/* ---- the ring: single producer, single consumer, wait-free ---- */
typedef struct {
    SmCmd *buf; uint32_t cap;      /* a power of two */
    _Atomic uint32_t head, tail;   /* pushed, popped; head - tail is the fill */
} SmRing;
static inline void sm_ring_init(SmRing *r, SmCmd *buf, uint32_t cap) {
    r->buf = buf; r->cap = cap; atomic_store(&r->head, 0); atomic_store(&r->tail, 0);
}
static inline int sm_ring_push(SmRing *r, const SmCmd *c) {
    uint32_t h = atomic_load_explicit(&r->head, memory_order_relaxed);
    uint32_t t = atomic_load_explicit(&r->tail, memory_order_acquire);
    if (h - t >= r->cap) return 0;
    r->buf[h & (r->cap - 1)] = *c;
    atomic_store_explicit(&r->head, h + 1, memory_order_release);
    return 1;
}
static inline int sm_ring_pop(SmRing *r, SmCmd *out) {
    uint32_t t = atomic_load_explicit(&r->tail, memory_order_relaxed);
    uint32_t h = atomic_load_explicit(&r->head, memory_order_acquire);
    if (t == h) return 0;
    *out = r->buf[t & (r->cap - 1)];
    atomic_store_explicit(&r->tail, t + 1, memory_order_release);
    return 1;
}

/* ---- recordings ----
 * Decoded audio at SM_RATE, interleaved, one or two channels. Owned by
 * whoever publishes it (strata_audio.c's loader thread): the mixer only reads
 * it and counts the voices holding it in [refs]. A withdrawn recording may be
 * freed once refs is 0 and the mixer has finished a block since. */
typedef struct { float *data; int frames, channels; _Atomic int refs; } SmRec;

/* ---- the mixer ---- */
typedef struct { int playing, virtual_, refused, ring_refused, underruns; float cpu, peak_db, rms_db; } SmStats;

typedef struct { float ic1, ic2; } SmSvf;
typedef struct {
    int32_t number;                /* the game's voice number; 0 = free */
    SmRec *rec[4]; double rpos[4]; uint8_t rec_done[4];
    int stereo;                    /* a stereo recording played flat, both channels kept */
    int src, group;
    float p[SM_PATCH_FLOATS];      /* the patch as it was when played */
    double phase[4];
    uint32_t rng;
    float pink[4][7], brown[4];
    SmSvf svf[2];
    float a1[2], a2[2], a3[2], k[2]; int coef_age;
    float walk[2], walk_to[2]; double mphase[2]; int walk_left[2];
    float ctl[4], ctl_to[4];
    float gain, pitch, pan;
    float gain_now;                /* smoothed: voice gain times gain controls */
    int stage;                     /* 0 attack, 1 decay, 2 sustain, 3 release, 4 done */
    int64_t stage_left, stage_len; float stage_from, env;
    float env2;
    int64_t age, hold;
    int fading; int64_t fade_left, fade_len;
} SmVoice;

typedef struct {
    SmVoice voices[SM_TRACKED];
    float patches[SM_PATCHES][SM_PATCH_FLOATS];
    uint8_t patch_ok[SM_PATCHES];
    float group_gain[SM_GROUPS], group_now[SM_GROUPS];
    float lim_env;
    SmStats st;
    _Atomic(SmRec *) recs[SM_RECS];
    _Atomic uint64_t blocks;
} SmMixer;

static inline void sm_rec_publish(SmMixer *m, int id, SmRec *r) {
    if (id >= SM_REC_FIRST && id < SM_REC_FIRST + SM_RECS) atomic_store(&m->recs[id - SM_REC_FIRST], r);
}
static inline void sm_rec_withdraw(SmMixer *m, int id) { sm_rec_publish(m, id, NULL); }
/* blocks mixed so far: a loader that withdrew a recording frees it once this has moved on and its refs are 0 */
static inline uint64_t sm_blocks(SmMixer *m) { return atomic_load(&m->blocks); }
static inline SmRec *sm_rec_get(SmMixer *m, int id) {
    if (id < SM_REC_FIRST || id >= SM_REC_FIRST + SM_RECS) return NULL;
    return atomic_load(&m->recs[id - SM_REC_FIRST]);
}
static inline void sm_voice_release(SmVoice *v) {
    for (int i = 0; i < 4; i++) if (v->rec[i]) { atomic_fetch_sub(&v->rec[i]->refs, 1); v->rec[i] = NULL; }
    v->number = 0;
}

static inline SmMixer *sm_open(void) {
    SmMixer *m = (SmMixer *)calloc(1, sizeof(SmMixer));
    if (!m) return NULL;
    for (int g = 0; g < SM_GROUPS; g++) { m->group_gain[g] = 1.0f; m->group_now[g] = 1.0f; }
    m->st.peak_db = -120.0f; m->st.rms_db = -120.0f;
    return m;
}
static inline void sm_close(SmMixer *m) { free(m); }
static inline SmStats sm_stats(const SmMixer *m) { return m->st; }
static inline void sm_note_ring_refused(SmMixer *m, int n) { m->st.ring_refused += n; }
static inline void sm_note_underrun(SmMixer *m) { m->st.underruns++; }
static inline void sm_set_cpu(SmMixer *m, float share) { m->st.cpu = share; }

static inline SmVoice *sm_find(SmMixer *m, int32_t number) {
    if (number == 0) return NULL;
    for (int i = 0; i < SM_TRACKED; i++) if (m->voices[i].number == number) return &m->voices[i];
    return NULL;
}
static inline float sm_clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline int64_t sm_samples(float seconds) { return seconds <= 0.0f ? 0 : (int64_t)(seconds * SM_RATE + 0.5f); }

static inline void sm_env_stage(SmVoice *v, int stage) {
    v->stage = stage; v->stage_from = v->env;
    float secs = stage == 0 ? v->p[SM_P_ENV(0)] : stage == 1 ? v->p[SM_P_ENV(0) + 1] : stage == 3 ? v->p[SM_P_ENV(0) + 3] : 0.0f;
    v->stage_len = sm_samples(secs); v->stage_left = v->stage_len;
}

static inline void sm_play(SmMixer *m, const SmCmd *c) {
    int src = c->a;
    int direct = src >= SM_REC_FIRST;
    SmRec *held[4] = { NULL, NULL, NULL, NULL };
    if (c->voice == 0 || sm_find(m, c->voice)) { m->st.refused++; return; }
    if (direct) {
        held[0] = sm_rec_get(m, src);
        if (!held[0]) { m->st.refused++; return; }
    } else {
        if (src <= 0 || src >= SM_PATCHES || !m->patch_ok[src]) { m->st.refused++; return; }
        for (int i = 0; i < 4; i++) if ((int)m->patches[src][SM_P_SRC(i)] == SM_SRC_RECORDING) {
            held[i] = sm_rec_get(m, (int)m->patches[src][SM_P_SRC(i) + 4]);
            if (!held[i]) { m->st.refused++; return; }
        }
    }
    SmVoice *v = NULL;
    for (int i = 0; i < SM_TRACKED; i++) if (m->voices[i].number == 0) { v = &m->voices[i]; break; }
    if (!v) { m->st.refused++; return; }
    memset(v, 0, sizeof *v);
    v->number = c->voice; v->src = src; v->group = c->b >= 0 && c->b < SM_GROUPS ? c->b : 0;
    if (direct) {
        memset(v->p, 0, sizeof v->p);
        v->p[SM_P_SRC(0)] = SM_SRC_RECORDING; v->p[SM_P_SRC(0) + 1] = 1.0f; v->p[SM_P_SRC(0) + 2] = 1.0f; v->p[SM_P_SRC(0) + 4] = (float)src;
        v->p[SM_P_ENV(0) + 2] = 1.0f; v->p[SM_P_ENV(0) + 3] = 0.005f; v->p[SM_P_GAIN] = 1.0f;
        v->stereo = held[0]->channels == 2 && c->f[6] != 0.0f;
    } else memcpy(v->p, m->patches[src], sizeof v->p);
    for (int i = 0; i < 4; i++) if (held[i]) { v->rec[i] = held[i]; atomic_fetch_add(&held[i]->refs, 1); }
    v->rng = 0x9e3779b9u ^ (uint32_t)c->voice * 2654435761u; if (!v->rng) v->rng = 1;
    v->gain = c->f[0]; v->pitch = c->f[1] > 0.0f ? c->f[1] : 1.0f; v->pan = sm_clampf(c->f[7], -1.0f, 1.0f);
    for (int i = 0; i < 4; i++) { v->ctl[i] = v->ctl_to[i] = sm_clampf(v->p[SM_P_CTL(i) + 4], 0.0f, 1.0f); }
    v->hold = sm_samples(v->p[SM_P_HOLD]);
    v->coef_age = 1 << 30;
    v->gain_now = -1.0f;           /* set on the first sample, unsmoothed */
    v->env = 0.0f; sm_env_stage(v, 0);
}

static inline void sm_command(SmMixer *m, const SmCmd *c) {
    switch (c->op) {
    case SM_OP_PATCH:
        if (c->voice > 0 && c->voice < SM_PATCHES && c->a >= 0 && c->a < SM_PATCH_RECORDS) {
            for (int k = 0; k < 12 && c->a * 12 + k < SM_PATCH_FLOATS; k++) m->patches[c->voice][c->a * 12 + k] = c->f[k];
            if (c->a == SM_PATCH_RECORDS - 1) m->patch_ok[c->voice] = 1;
        }
        break;
    case SM_OP_PLAY: sm_play(m, c); break;
    case SM_OP_CONTROL: {
        SmVoice *v = sm_find(m, c->voice);
        if (v && c->a >= 0 && c->a < 4) v->ctl_to[c->a] = sm_clampf(c->f[0], 0.0f, 1.0f);
        break; }
    case SM_OP_STOP: {
        SmVoice *v = sm_find(m, c->voice);
        if (!v) break;
        if (c->f[0] < 0.0f) { if (v->stage < 3) sm_env_stage(v, 3); }
        else if (!v->fading) { v->fading = 1; v->fade_len = v->fade_left = sm_samples(c->f[0] > 0.005f ? c->f[0] : 0.005f); }
        break; }
    case SM_OP_GROUP:
        if (c->a >= 0 && c->a < SM_GROUPS) m->group_gain[c->a] = c->f[0] < 0.0f ? 0.0f : c->f[0];
        break;
    default: break;   /* MOVE, LISTENER, OCCLUDE, ENVIRON, OUTPUT: given meaning by later work */
    }
}

/* ---- one voice, one sample ---- */
static inline float sm_white(SmVoice *v) {
    uint32_t x = v->rng; x ^= x << 13; x ^= x >> 17; x ^= x << 5; v->rng = x;
    return (float)((double)x / 2147483648.0 - 1.0);
}
static inline float sm_blep(double t, double dt) {      /* polyBLEP for saw and square */
    if (t < dt) { t /= dt; return (float)(t + t - t * t - 1.0); }
    if (t > 1.0 - dt) { t = (t - 1.0) / dt; return (float)(t * t + t + t + 1.0); }
    return 0.0f;
}
static inline float sm_source(SmVoice *v, int i, float pitch_mul) {
    const float *s = &v->p[SM_P_SRC(i)];
    int kind = (int)s[0];
    if (kind == SM_SRC_OFF) return 0.0f;
    float level = s[2];
    float out = 0.0f;
    if (kind >= SM_SRC_SINE && kind <= SM_SRC_SQUARE) {
        double hz = s[1] * pitch_mul * pow(2.0, s[3] / 1200.0);
        double dt = hz / SM_RATE; if (dt > 0.49) dt = 0.49; if (dt < 0.0) dt = 0.0;
        double t = v->phase[i];
        switch (kind) {
        case SM_SRC_SINE: out = (float)sin(2.0 * SM_PI * t); break;
        case SM_SRC_TRIANGLE: out = (float)(t < 0.5 ? 4.0 * t - 1.0 : 3.0 - 4.0 * t); break;
        case SM_SRC_SAW: out = (float)(2.0 * t - 1.0) - sm_blep(t, dt); break;
        case SM_SRC_SQUARE: { double t2 = t + 0.5; if (t2 >= 1.0) t2 -= 1.0;
            out = (t < 0.5 ? 1.0f : -1.0f) + sm_blep(t, dt) - sm_blep(t2, dt); break; }
        }
        t += dt; if (t >= 1.0) t -= 1.0;
        v->phase[i] = t;
    } else if (kind == SM_SRC_RECORDING) {
        SmRec *r = v->rec[i];
        if (!r || v->rec_done[i]) return 0.0f;
        double rate = (s[1] > 0.0f ? s[1] : 1.0f) * pitch_mul;
        int at = (int)v->rpos[i]; double fr = v->rpos[i] - at;
        int nx = at + 1 < r->frames ? at + 1 : (v->p[SM_P_LOOP] != 0.0f ? 0 : at);
        float a = 0.0f, b = 0.0f;
        for (int c = 0; c < r->channels; c++) { a += r->data[at * r->channels + c]; b += r->data[nx * r->channels + c]; }
        out = (float)((a + (b - a) * fr) / r->channels);
        v->rpos[i] += rate;
        if (v->rpos[i] >= r->frames) { if (v->p[SM_P_LOOP] != 0.0f) v->rpos[i] -= r->frames; else v->rec_done[i] = 1; }
    } else if (kind == SM_SRC_WHITE) {
        out = sm_white(v);
    } else if (kind == SM_SRC_PINK) {           /* Paul Kellet's refined filter */
        float w = sm_white(v), *b = v->pink[i];
        b[0] = 0.99886f * b[0] + w * 0.0555179f; b[1] = 0.99332f * b[1] + w * 0.0750759f;
        b[2] = 0.96900f * b[2] + w * 0.1538520f; b[3] = 0.86650f * b[3] + w * 0.3104856f;
        b[4] = 0.55000f * b[4] + w * 0.5329522f; b[5] = -0.7616f * b[5] - w * 0.0168980f;
        out = (b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6] + w * 0.5362f) * 0.11f;
        b[6] = w * 0.115926f;
    } else if (kind == SM_SRC_BROWN) {
        float w = sm_white(v);
        v->brown[i] = (v->brown[i] + 0.02f * w) / 1.02f;
        out = v->brown[i] * 3.5f;
    }
    return out * level;
}

static inline void sm_svf_coef(SmVoice *v, int i, float cutoff, float q) {
    float fc = sm_clampf(cutoff, 20.0f, 0.45f * SM_RATE);
    float g = (float)tan(SM_PI * fc / SM_RATE);
    float k = 1.0f / (q > 0.05f ? q : 0.05f);
    v->a1[i] = 1.0f / (1.0f + g * (g + k)); v->a2[i] = g * v->a1[i]; v->a3[i] = g * v->a2[i]; v->k[i] = k;
}
static inline float sm_svf(SmVoice *v, int i, int kind, float x) {
    SmSvf *s = &v->svf[i];
    float v3 = x - s->ic2;
    float v1 = v->a1[i] * s->ic1 + v->a2[i] * v3;
    float v2 = s->ic2 + v->a2[i] * s->ic1 + v->a3[i] * v3;
    s->ic1 = 2.0f * v1 - s->ic1; s->ic2 = 2.0f * v2 - s->ic2;
    switch (kind) {
    case SM_FLT_LOW: return v2;
    case SM_FLT_HIGH: return x - v->k[i] * v1 - v2;
    case SM_FLT_BAND: return v1;
    case SM_FLT_PEAK: return x + 2.0f * v->k[i] * v1;   /* about +9.5 dB at the cutoff */
    default: return x;
    }
}

static inline float sm_ctl_map(const SmVoice *v, int c) {
    const float *d = &v->p[SM_P_CTL(c)];
    float lo = d[1], hi = d[2], x = v->ctl[c];
    if ((int)d[3] == 1 && lo > 0.0f && hi > 0.0f) return lo * powf(hi / lo, x);
    return lo + (hi - lo) * x;
}

/* the envelope's next value; 1 when the voice is done */
static inline int sm_env_step(SmVoice *v) {
    float sustain = sm_clampf(v->p[SM_P_ENV(0) + 2], 0.0f, 1.0f);
    for (;;) {
        if (v->stage == 0 || v->stage == 1 || v->stage == 3) {
            float to = v->stage == 0 ? 1.0f : v->stage == 1 ? sustain : 0.0f;
            if (v->stage_left <= 0) {
                v->env = to;
                if (v->stage == 3) { v->stage = 4; return 1; }
                sm_env_stage(v, v->stage + 1);
                continue;
            }
            v->stage_left--;
            v->env = to + (v->stage_from - to) * (float)v->stage_left / (float)v->stage_len;
            return 0;
        }
        if (v->stage == 2) { v->env = sustain; return 0; }
        return 1;
    }
}

static const float sm_smooth = 0.0041580f;   /* 1 - exp(-1 / (5 ms * 48 kHz)) */

/* add one voice's [frames] into stereo [out]; answers 1 when it has ended */
static inline int sm_voice_mix(SmMixer *m, SmVoice *v, float *out, int frames) {
    (void)m;
    float ga = cosf((float)((v->pan + 1.0f) * SM_PI / 4.0)), gb = sinf((float)((v->pan + 1.0f) * SM_PI / 4.0));
    for (int n = 0; n < frames; n++) {
        if (v->hold > 0 && v->age == v->hold && v->stage < 3) sm_env_stage(v, 3);
        v->age++;
        if (sm_env_step(v)) return 1;
        /* controls glide toward what they were last set to */
        float pitch_mul = v->pitch, gain_mul = v->gain * v->p[SM_P_GAIN];
        float cut1 = v->p[SM_P_FLT(0) + 1], cut2 = v->p[SM_P_FLT(1) + 1], level1 = -1.0f;
        for (int c = 0; c < 4; c++) {
            int tgt = (int)v->p[SM_P_CTL(c)];
            if (v->p[SM_P_CTL(c) + 1] == 0.0f && v->p[SM_P_CTL(c) + 2] == 0.0f) continue;   /* unused */
            v->ctl[c] += (v->ctl_to[c] - v->ctl[c]) * sm_smooth;
            float val = sm_ctl_map(v, c);
            if (tgt == SM_TGT_PITCH) pitch_mul *= val;
            else if (tgt == SM_TGT_GAIN) gain_mul *= val;
            else if (tgt == SM_TGT_CUTOFF) cut1 = val;
            else if (tgt == SM_TGT_CUTOFF2) cut2 = val;
            else if (tgt == SM_TGT_LEVEL1) level1 = val;
        }
        float cut_oct = v->p[SM_P_ENV(1) + 4] * v->env;   /* env 1 rides on the gain envelope's shape */
        for (int i = 0; i < 2; i++) {
            const float *d = &v->p[SM_P_MOD(i)];
            int kind = (int)d[0];
            if (kind == SM_MOD_OFF) continue;
            float val;
            if (kind == SM_MOD_LFO) {
                val = (float)sin(2.0 * SM_PI * v->mphase[i]);
                v->mphase[i] += d[1] / SM_RATE; if (v->mphase[i] >= 1.0) v->mphase[i] -= 1.0;
            } else {
                if (v->walk_left[i] <= 0) { v->walk_to[i] = sm_white(v); v->walk_left[i] = (int)(SM_RATE / (d[1] > 0.05f ? d[1] : 0.05f)); }
                v->walk_left[i]--;
                v->walk[i] += (v->walk_to[i] - v->walk[i]) * (d[1] * 2.0f / SM_RATE);
                val = v->walk[i];
            }
            int tgt = (int)d[2];
            if (tgt == SM_TGT_PITCH) pitch_mul *= powf(2.0f, d[3] * val / 12.0f);
            else if (tgt == SM_TGT_CUTOFF) cut_oct += d[3] * val;
            else if (tgt == SM_TGT_GAIN) { float g = 1.0f + d[3] * val; gain_mul *= g < 0.0f ? 0.0f : g; }
        }
        if (v->gain_now < 0.0f) v->gain_now = gain_mul;
        v->gain_now += (gain_mul - v->gain_now) * sm_smooth;
        int recs = 0, done = 0, others = 0;
        for (int i = 0; i < 4; i++) {
            int kd = (int)v->p[SM_P_SRC(i)];
            if (kd == SM_SRC_RECORDING) { recs++; if (v->rec_done[i]) done++; } else if (kd != SM_SRC_OFF) others++;
        }
        if (recs > 0 && done == recs && others == 0) return 1;
        if (v->stereo) {   /* a stereo recording played flat: both channels, balanced by pan, no filters */
            SmRec *r = v->rec[0];
            int at = (int)v->rpos[0];
            float amp = v->env * v->gain_now * m->group_now[v->group];
            if (v->fading) { if (v->fade_left <= 0) return 1; amp *= (float)v->fade_left / (float)v->fade_len; v->fade_left--; }
            float gl = v->pan > 0.0f ? 1.0f - v->pan : 1.0f, gr = v->pan < 0.0f ? 1.0f + v->pan : 1.0f;
            out[2 * n] += r->data[2 * at] * amp * gl; out[2 * n + 1] += r->data[2 * at + 1] * amp * gr;
            v->rpos[0] += v->pitch;
            if (v->rpos[0] >= r->frames) v->rec_done[0] = 1;
            continue;
        }
        float x = 0.0f;
        for (int i = 0; i < 4; i++) {
            float s = sm_source(v, i, pitch_mul);
            if (i == 0 && level1 >= 0.0f && v->p[SM_P_SRC(0) + 2] != 0.0f) s *= level1 / v->p[SM_P_SRC(0) + 2];
            x += s;
        }
        if (++v->coef_age >= 32) {
            v->coef_age = 0;
            sm_svf_coef(v, 0, cut1 * powf(2.0f, cut_oct), v->p[SM_P_FLT(0) + 2]);
            sm_svf_coef(v, 1, cut2, v->p[SM_P_FLT(1) + 2]);
        }
        int k0 = (int)v->p[SM_P_FLT(0)], k1 = (int)v->p[SM_P_FLT(1)];
        if (k0) x = sm_svf(v, 0, k0, x);
        if (k1) x = sm_svf(v, 1, k1, x);
        float amp = x * v->env * v->gain_now * m->group_now[v->group];
        if (v->fading) {
            if (v->fade_left <= 0) return 1;
            amp *= (float)v->fade_left / (float)v->fade_len;
            v->fade_left--;
        }
        out[2 * n] += amp * ga; out[2 * n + 1] += amp * gb;
    }
    return 0;
}

/* Mix [frames] (any count) into interleaved stereo [out], replacing it. */
static inline void sm_mix(SmMixer *m, float *out, int frames) {
    memset(out, 0, sizeof(float) * 2 * (size_t)frames);
    for (int g = 0; g < SM_GROUPS; g++) m->group_now[g] += (m->group_gain[g] - m->group_now[g]) * 0.5f;
    int playing = 0, mixed = 0;
    for (int i = 0; i < SM_TRACKED; i++) {
        SmVoice *v = &m->voices[i];
        if (v->number == 0) continue;
        if (mixed < SM_MIXED) {
            mixed++;
            if (sm_voice_mix(m, v, out, frames)) { sm_voice_release(v); continue; }
        }
        playing++;
    }
    /* the limiter: instant attack, 100 ms release, so the output never passes SM_LIMIT */
    double sum = 0.0; float peak = 0.0f;
    const float rel = 0.99979f;
    for (int n = 0; n < frames; n++) {
        float a = fabsf(out[2 * n]), b = fabsf(out[2 * n + 1]);
        float p = a > b ? a : b;
        m->lim_env = p > m->lim_env * rel ? p : m->lim_env * rel;
        float g = m->lim_env > SM_LIMIT ? SM_LIMIT / m->lim_env : 1.0f;
        out[2 * n] = sm_clampf(out[2 * n] * g, -SM_LIMIT, SM_LIMIT);
        out[2 * n + 1] = sm_clampf(out[2 * n + 1] * g, -SM_LIMIT, SM_LIMIT);
        float q = fabsf(out[2 * n]) > fabsf(out[2 * n + 1]) ? fabsf(out[2 * n]) : fabsf(out[2 * n + 1]);
        if (q > peak) peak = q;
        sum += (double)out[2 * n] * out[2 * n] + (double)out[2 * n + 1] * out[2 * n + 1];
    }
    m->st.playing = playing; m->st.virtual_ = 0;
    atomic_fetch_add(&m->blocks, 1);
    m->st.peak_db = (float)(20.0 * log10(peak > 1e-6f ? peak : 1e-6f));
    double rms = frames > 0 ? sqrt(sum / (2.0 * frames)) : 0.0;
    m->st.rms_db = (float)(20.0 * log10(rms > 1e-6 ? rms : 1e-6));
}

/* ---- files ---- */
static inline void sm_put32(unsigned char *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
/* Write [frames] of interleaved stereo as 16-bit PCM at SM_RATE. 1 on success. */
static inline int sm_wav_write(const char *path, const float *stereo, int frames) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    unsigned char h[44];
    uint32_t bytes = (uint32_t)frames * 4u;
    memcpy(h, "RIFF", 4); sm_put32(h + 4, 36 + bytes); memcpy(h + 8, "WAVEfmt ", 8);
    sm_put32(h + 16, 16); h[20] = 1; h[21] = 0; h[22] = 2; h[23] = 0;
    sm_put32(h + 24, SM_RATE); sm_put32(h + 28, SM_RATE * 4); h[32] = 4; h[33] = 0; h[34] = 16; h[35] = 0;
    memcpy(h + 36, "data", 4); sm_put32(h + 40, bytes);
    int ok = fwrite(h, 1, 44, f) == 44;
    for (int i = 0; ok && i < 2 * frames; i++) {
        float s = sm_clampf(stereo[i], -1.0f, 1.0f);
        int16_t q = (int16_t)lrintf(s * 32767.0f);
        unsigned char b[2] = { (unsigned char)(q & 255), (unsigned char)((q >> 8) & 255) };
        ok = fwrite(b, 1, 2, f) == 2;
    }
    return fclose(f) == 0 && ok;
}

/* ---- fingerprints: loudness in octave bands over 100 ms windows ---- */
#define SM_FP_BANDS 10                 /* 31.5 Hz .. 16 kHz */
#define SM_FP_VALUES (2 * SM_FP_BANDS) /* left bands, then right */
#define SM_FP_WINDOW (SM_RATE / 10)
#define SM_FP_STAGES 4                 /* cascaded band-passes: an octave away is about 30 dB down */
/* Fill [out] with SM_FP_VALUES dB figures per whole window; answers the
 * window count. [out] must hold (frames / SM_FP_WINDOW) * SM_FP_VALUES. */
static inline int sm_fingerprint(const float *stereo, int frames, float *out) {
    int windows = frames / SM_FP_WINDOW;
    for (int ch = 0; ch < 2; ch++)
        for (int b = 0; b < SM_FP_BANDS; b++) {
            double f0 = 31.25 * pow(2.0, b);
            double w0 = 2.0 * SM_PI * f0 / SM_RATE, alpha = sin(w0) / (2.0 * 1.4142);
            double a0 = 1.0 + alpha;
            double b0 = alpha / a0, b2 = -alpha / a0, a1 = -2.0 * cos(w0) / a0, a2 = (1.0 - alpha) / a0;
            double z1[SM_FP_STAGES] = { 0 }, z2[SM_FP_STAGES] = { 0 };
            for (int w = 0; w < windows; w++) {
                double e = 0.0;
                for (int n = w * SM_FP_WINDOW; n < (w + 1) * SM_FP_WINDOW; n++) {
                    double x = stereo[2 * n + ch];
                    for (int s = 0; s < SM_FP_STAGES; s++) {   /* transposed direct form II */
                        double y = b0 * x + z1[s];
                        z1[s] = -a1 * y + z2[s];
                        z2[s] = b2 * x - a2 * y;
                        x = y;
                    }
                    e += x * x;
                }
                double rms = sqrt(e / SM_FP_WINDOW);
                out[w * SM_FP_VALUES + ch * SM_FP_BANDS + b] = (float)(20.0 * log10(rms > 1e-6 ? rms : 1e-6));
            }
        }
    return windows;
}
/* The .fp text format: a header "fp 1 bands=10 channels=2 windows=N", then
 * one line of SM_FP_VALUES numbers a window. 1 on success. */
static inline int sm_fp_write(const char *path, const float *fp, int windows) {
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    fprintf(f, "fp 1 bands=%d channels=2 windows=%d\n", SM_FP_BANDS, windows);
    for (int w = 0; w < windows; w++) {
        for (int i = 0; i < SM_FP_VALUES; i++) fprintf(f, i ? " %.2f" : "%.2f", fp[w * SM_FP_VALUES + i]);
        fputc('\n', f);
    }
    return fclose(f) == 0;
}
/* Read at most [max] windows into [fp]; answers the window count, -1 on a bad file. */
static inline int sm_fp_read(const char *path, float *fp, int max) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int bands = 0, windows = 0;
    if (fscanf(f, "fp 1 bands=%d channels=2 windows=%d", &bands, &windows) != 2 || bands != SM_FP_BANDS || windows < 0 || windows > max) { fclose(f); return -1; }
    for (int i = 0; i < windows * SM_FP_VALUES; i++) if (fscanf(f, "%f", &fp[i]) != 1) { fclose(f); return -1; }
    fclose(f);
    return windows;
}
#define SM_FP_MAX_WINDOWS 6000   /* ten minutes */
/* 0 when every band of every window is within [band_db] and every window's
 * total loudness within [total_db]; 1 when not; 2 when the shapes differ
 * or a file cannot be read. Differences go to stderr. */
static inline int sm_fp_compare(const char *stored, const char *fresh, float band_db, float total_db) {
    static float a[SM_FP_MAX_WINDOWS * SM_FP_VALUES], b[SM_FP_MAX_WINDOWS * SM_FP_VALUES];
    int na = sm_fp_read(stored, a, SM_FP_MAX_WINDOWS), nb = sm_fp_read(fresh, b, SM_FP_MAX_WINDOWS);
    if (na < 0 || nb < 0 || na != nb) { fprintf(stderr, "fp: %s and %s differ in shape (%d, %d windows)\n", stored, fresh, na, nb); return 2; }
    int bad = 0;
    for (int w = 0; w < na; w++) {
        double ea = 0.0, eb = 0.0;
        for (int i = 0; i < SM_FP_VALUES; i++) {
            float x = a[w * SM_FP_VALUES + i], y = b[w * SM_FP_VALUES + i];
            ea += pow(10.0, x / 10.0); eb += pow(10.0, y / 10.0);
            if (fabsf(x - y) > band_db) { if (bad < 8) fprintf(stderr, "fp: window %d value %d: %.2f against %.2f\n", w, i, x, y); bad++; }
        }
        double d = 10.0 * log10(ea) - 10.0 * log10(eb);
        if (fabs(d) > total_db) { if (bad < 8) fprintf(stderr, "fp: window %d total differs by %.2f dB\n", w, d); bad++; }
    }
    return bad ? 1 : 0;
}
#endif

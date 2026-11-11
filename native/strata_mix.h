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
#include "strata_hrir.h"

#define SM_RATE 48000
#define SM_BLOCK 256
#define SM_MIXED 64
#define SM_TRACKED 1024
#define SM_PATCHES 2048          /* patch ids 1..2047 */
#define SM_REC_FIRST 2048        /* recording ids 2048..4095 */
#define SM_RECS 2048
#define SM_GROUPS 4
#define SM_OCC_GLIDE (SM_RATE / 10)
#define SM_REFL 8                     /* early reflections */
#define SM_HIST 32768                 /* the sends' history: 0.68 s, past the farthest reflection */
#define SM_FDN_MAX 4096               /* the longest delay line of the late reverb */   /* muffling glides to a new value over 100 ms, the acoustics' update interval */
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
    SM_OP_OUTPUT = 10,   /* a = 0 speakers, 1 headphones */
    SM_OP_RANGE = 11     /* f0 = reference distance, f1 = maximum distance (m) */
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
/* Push as many of [n] records as fit, then publish them together, so the
 * consumer never sees part of a batch (a PLAY without the OCCLUDE that
 * follows it). Answers how many were taken; the rest are the caller's. */
static inline int sm_ring_push_n(SmRing *r, const SmCmd *c, int n) {
    uint32_t h = atomic_load_explicit(&r->head, memory_order_relaxed);
    uint32_t t = atomic_load_explicit(&r->tail, memory_order_acquire);
    uint32_t room = r->cap - (h - t);
    int k = n < (int)room ? n : (int)room;
    for (int i = 0; i < k; i++) r->buf[(h + (uint32_t)i) & (r->cap - 1)] = c[i];
    atomic_store_explicit(&r->head, h + (uint32_t)k, memory_order_release);
    return k;
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
    /* in the world */
    int flat; float pos[3], ref, maxd, prio;
    float occ_gain, occ_cut;       /* muffling through blocks, as it is now */
    float occ_gain_from, occ_cut_from, occ_gain_to, occ_cut_to; int occ_left;   /* gliding to OCCLUDE's values over SM_OCC_GLIDE */
    float loud;                    /* this block's rank: how loud it would be at the listener */
    int mixed;                     /* mixed last block: entering or leaving the mix fades */
    float coef[16];                /* last block's bus gains, to glide from */
    float pan_l, pan_r;            /* last block's speaker gains */
    SmSvf air; float air_a1, air_a2, air_a3, air_k;
    int placed;                    /* has been placed once: until then its gains start at their targets, not from 0 */
    float detune[4];               /* each source's detune as a ratio */
    int krate;                     /* samples to the next control-rate update */
    float k_pitch, k_gain, k_cut1, k_cut2, k_level1, k_oct;   /* what that update decided */
} SmVoice;

typedef struct {
    SmVoice voices[SM_TRACKED];
    float patches[SM_PATCHES][SM_PATCH_FLOATS];
    uint8_t patch_ok[SM_PATCHES];
    float group_gain[SM_GROUPS], group_now[SM_GROUPS];
    float group_from[SM_GROUPS], group_step[SM_GROUPS];   /* this block's glide: from, and per sample */
    float lim_env;
    SmStats st;
    _Atomic(SmRec *) recs[SM_RECS];
    _Atomic uint64_t blocks;
    /* the listener: position, and right, up and front unit vectors */
    float lis[3], lr[3], lu[3], lf[3];
    int headphones;
    float bus[16][SM_BLOCK];
    float hist[16][SH_TAPS - 1 + SM_BLOCK];
    float mono[SM_BLOCK];
    int order[SM_TRACKED];
    /* the listener's space: what ENVIRON last asked, and where the glide to it stands */
    float rv_decay, rv_damp, rv_wet;                  /* now */
    float rv_decay_to, rv_damp_to, rv_wet_to;          /* asked */
    float er_to[SM_REFL][5], er_now[SM_REFL][5];      /* reflections: delay s, direction x y z, gain */
    float send[SM_BLOCK];                              /* the block's sum of voices' sends */
    float shist[SM_HIST];                             /* the sends, past, for the early reflections */
    int hist_at;
    float fdn[8][SM_FDN_MAX]; int fdn_at[8]; float fdn_lp[8];
    float er_coef[SM_REFL][16], er_pan[SM_REFL][2];
    int rv_cleared;
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
    m->lr[0] = 1.0f; m->lu[1] = 1.0f; m->lf[2] = -1.0f;
    m->rv_decay = m->rv_decay_to = 0.1f;
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
/* NaN clamps to [lo]: one bad float from a game must not poison the mix */
static inline float sm_clampf(float v, float lo, float hi) { return !(v >= lo) ? lo : (v > hi ? hi : v); }
static inline float sm_fin(float v, float def) { return isfinite(v) ? v : def; }
static inline int64_t sm_samples(float seconds) { return seconds <= 0.0f ? 0 : (int64_t)(seconds * SM_RATE + 0.5f); }

static inline void sm_env_stage(SmVoice *v, int stage) {
    v->stage = stage; v->stage_from = v->env;
    float secs = stage == 0 ? v->p[SM_P_ENV(0)] : stage == 1 ? v->p[SM_P_ENV(0) + 1] : stage == 3 ? v->p[SM_P_ENV(0) + 3] : 0.0f;
    v->stage_len = sm_samples(secs); v->stage_left = v->stage_len;
}

static inline float sm_dist(const SmMixer *m, const float *p) {
    float d0 = p[0] - m->lis[0], d1 = p[1] - m->lis[1], d2 = p[2] - m->lis[2];
    return sqrtf(d0 * d0 + d1 * d1 + d2 * d2);
}
/* how loud a voice would be at the listener, times its priority: the rank */
static inline float sm_loudness(const SmMixer *m, float gain, float prio, int flat, const float *pos, float ref, float maxd, float occ, int group) {
    float g = fabsf(gain) * (prio > 0.0f ? prio : 1.0f) * occ * m->group_gain[group >= 0 && group < SM_GROUPS ? group : 0];
    if (flat) return g;
    float d = sm_dist(m, pos);
    if (d > maxd) return 0.0f;
    return g * ref / (d > ref ? d : ref);
}
static inline void sm_norm3(float *v, float dx, float dy, float dz) {
    float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l < 1e-6f) { v[0] = dx; v[1] = dy; v[2] = dz; return; }
    v[0] /= l; v[1] /= l; v[2] /= l;
}
static inline void sm_cross(float *o, const float *a, const float *b) {
    o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0];
}
static inline void sm_listener(SmMixer *m, const float *f) {
    for (int i = 0; i < 3; i++) m->lis[i] = isfinite(f[i]) ? f[i] : 0.0f;
    float fw[3] = { f[3], f[4], f[5] }, up[3] = { f[6], f[7], f[8] };
    sm_norm3(fw, 0.0f, 0.0f, -1.0f); sm_norm3(up, 0.0f, 1.0f, 0.0f);
    float r[3]; sm_cross(r, fw, up);
    if (sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]) < 1e-4f) { float alt[3] = { 0.0f, 0.0f, 1.0f }; if (fabsf(fw[2]) > 0.9f) alt[2] = 0.0f, alt[1] = 1.0f; sm_cross(r, fw, alt); }
    sm_norm3(r, 1.0f, 0.0f, 0.0f);
    float u[3]; sm_cross(u, r, fw); sm_norm3(u, 0.0f, 1.0f, 0.0f);
    memcpy(m->lr, r, sizeof r); memcpy(m->lu, u, sizeof u); memcpy(m->lf, fw, sizeof fw);
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
    if (!v) {   /* every slot taken: the newcomer replaces the quietest only if it would be louder */
        float newl = sm_loudness(m, c->f[0], c->f[2], c->f[6] != 0.0f, &c->f[3], 1.0f, 100.0f, 1.0f, c->b);
        SmVoice *low = NULL;
        for (int i = 0; i < SM_TRACKED; i++) {
            SmVoice *u = &m->voices[i];
            u->loud = sm_loudness(m, u->gain * u->p[SM_P_GAIN], u->prio, u->flat, u->pos, u->ref, u->maxd, u->occ_gain, u->group);
            if (!low || u->loud < low->loud) low = u;
        }
        m->st.refused++;
        if (!low || newl <= low->loud) return;
        sm_voice_release(low);
        v = low;
    }
    memset(v, 0, sizeof *v);
    v->flat = c->f[6] != 0.0f; v->pos[0] = c->f[3]; v->pos[1] = c->f[4]; v->pos[2] = c->f[5];
    v->ref = 1.0f; v->maxd = 100.0f; v->prio = c->f[2] > 0.0f ? c->f[2] : 1.0f;
    v->occ_gain = 1.0f; v->occ_cut = 20000.0f;
    v->mixed = 1;                  /* a new voice starts in the mix, its own attack intact; one ranked out fades from its first block */
    v->number = c->voice; v->src = src; v->group = c->b >= 0 && c->b < SM_GROUPS ? c->b : 0;
    if (direct) {
        memset(v->p, 0, sizeof v->p);
        v->p[SM_P_SRC(0)] = SM_SRC_RECORDING; v->p[SM_P_SRC(0) + 1] = 1.0f; v->p[SM_P_SRC(0) + 2] = 1.0f; v->p[SM_P_SRC(0) + 4] = (float)src;
        v->p[SM_P_ENV(0) + 2] = 1.0f; v->p[SM_P_ENV(0) + 3] = 0.005f; v->p[SM_P_GAIN] = 1.0f;
        v->stereo = held[0]->channels == 2 && c->f[6] != 0.0f;
    } else memcpy(v->p, m->patches[src], sizeof v->p);
    for (int i = 0; i < 4; i++) if (held[i]) { v->rec[i] = held[i]; atomic_fetch_add(&held[i]->refs, 1); }
    v->rng = 0x9e3779b9u ^ (uint32_t)c->voice * 2654435761u; if (!v->rng) v->rng = 1;
    v->gain = sm_clampf(c->f[0], 0.0f, 1000.0f); v->pitch = c->f[1] > 0.0f ? c->f[1] : 1.0f; v->pan = sm_clampf(c->f[7], -1.0f, 1.0f);
    for (int i = 0; i < 4; i++) { v->ctl[i] = v->ctl_to[i] = sm_clampf(v->p[SM_P_CTL(i) + 4], 0.0f, 1.0f); }
    v->hold = sm_samples(v->p[SM_P_HOLD]);
    for (int i = 0; i < 4; i++) v->detune[i] = (float)pow(2.0, v->p[SM_P_SRC(i) + 3] / 1200.0);
    v->krate = 0;
    v->coef_age = 1 << 30;
    v->gain_now = -1.0f;           /* set on the first sample, unsmoothed */
    v->env = 0.0f; sm_env_stage(v, 0);
}

static inline void sm_command(SmMixer *m, const SmCmd *c0) {
    SmCmd cc = *c0, *c = &cc;
    for (int k = 0; k < 12; k++) c->f[k] = sm_fin(c->f[k], 0.0f);   /* a non-finite float from a game becomes 0 */
    if (c->op == SM_OP_PLAY && c0->f[1] != c0->f[1]) c->f[1] = 1.0f;  /* a NaN pitch plays as made */
    if (c->op == SM_OP_ENVIRON && c->a == 0 && !isfinite(c0->f[0])) c->f[0] = 0.1f;
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
    case SM_OP_MOVE: {
        SmVoice *v = sm_find(m, c->voice);
        if (v) for (int i = 0; i < 3; i++) if (isfinite(c->f[i])) v->pos[i] = c->f[i];
        break; }
    case SM_OP_RANGE: {
        SmVoice *v = sm_find(m, c->voice);
        if (v) { v->ref = c->f[0] > 0.01f ? c->f[0] : 0.01f; v->maxd = c->f[1] > v->ref ? c->f[1] : v->ref; }
        break; }
    case SM_OP_OCCLUDE: {
        SmVoice *v = sm_find(m, c->voice);
        if (v) {
            float g = sm_clampf(c->f[0], 0.0f, 1.0f), cu = sm_clampf(c->f[1], 200.0f, 20000.0f);
            if (v->age == 0) { v->occ_gain = g; v->occ_cut = cu; v->occ_left = 0; }   /* not yet heard: at once */
            else { v->occ_gain_from = v->occ_gain; v->occ_cut_from = v->occ_cut; v->occ_left = SM_OCC_GLIDE; }
            v->occ_gain_to = g; v->occ_cut_to = cu;
        }
        break; }
    case SM_OP_LISTENER: sm_listener(m, c->f); break;
    case SM_OP_ENVIRON:
        if (c->a == 0) {
            m->rv_decay_to = sm_clampf(c->f[0], 0.1f, 6.0f); m->rv_damp_to = sm_clampf(c->f[1], 0.0f, 1.0f); m->rv_wet_to = sm_clampf(c->f[2], 0.0f, 1.0f);
            int n = (int)c->f[3];
            for (int r = n < 0 ? 0 : n; r < SM_REFL; r++) m->er_to[r][4] = 0.0f;   /* the reflections it does not name fade away */
        } else if (c->a >= 1 && c->a <= SM_REFL / 2) {
            for (int k = 0; k < 2; k++) {
                int r = (c->a - 1) * 2 + k;
                const float *f = &c->f[k * 5];
                if (f[0] <= 0.0f) { m->er_to[r][4] = 0.0f; continue; }
                m->er_to[r][0] = sm_clampf(f[0], 0.0f, (float)(SM_HIST - SM_BLOCK - 2) / SM_RATE);
                for (int q = 1; q < 4; q++) m->er_to[r][q] = isfinite(f[q]) ? f[q] : 0.0f;
                m->er_to[r][4] = sm_clampf(f[4], 0.0f, 1.0f);
                if (m->er_now[r][4] == 0.0f) { m->er_now[r][0] = m->er_to[r][0]; for (int q = 1; q < 4; q++) m->er_now[r][q] = m->er_to[r][q]; }
            }
        }
        break;
    case SM_OP_OUTPUT: m->headphones = c->a == 1; break;
    case SM_OP_GROUP:
        if (c->a >= 0 && c->a < SM_GROUPS) m->group_gain[c->a] = c->f[0] < 0.0f ? 0.0f : c->f[0];
        break;
    default: break;   /* ENVIRON: given meaning by later work */
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
        double hz = s[1] * pitch_mul * v->detune[i];
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
        double rate = (s[1] > 0.0f ? s[1] : 1.0f) * (pitch_mul > 0.0f ? pitch_mul : 0.0f);
        if (v->rpos[i] < 0.0 || v->rpos[i] >= r->frames) v->rpos[i] = 0.0;
        int at = (int)v->rpos[i]; double fr = v->rpos[i] - at;
        int nx = at + 1 < r->frames ? at + 1 : (v->p[SM_P_LOOP] != 0.0f ? 0 : at);
        float a = 0.0f, b = 0.0f;
        for (int c = 0; c < r->channels; c++) { a += r->data[at * r->channels + c]; b += r->data[nx * r->channels + c]; }
        out = (float)((a + (b - a) * fr) / r->channels);
        v->rpos[i] += rate;
        if (v->rpos[i] >= r->frames) { if (v->p[SM_P_LOOP] != 0.0f) v->rpos[i] = fmod(v->rpos[i], (double)r->frames); else v->rec_done[i] = 1; }
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
#define SM_KRATE 16                           /* control rate: every 16 samples, 3 kHz */
static const float sm_smooth_k = 0.0644f;     /* the same 5 ms glide, taken in steps of SM_KRATE */

/* Render one voice's [frames]: into [mono] (added), or, for a stereo
 * recording played flat, into stereo [out] with the gain ramp x0..x1.
 * Answers 1 when the voice has ended. */
static inline int sm_voice_render(SmMixer *m, SmVoice *v, float *mono, float *out, int frames, float x0, float x1) {
    for (int n = 0; n < frames; n++) {
        if (v->hold > 0 && v->age == v->hold && v->stage < 3) sm_env_stage(v, 3);
        v->age++;
        if (sm_env_step(v)) return 1;
        /* controls and modulators run at a control rate: every SM_KRATE samples */
        if (v->krate <= 0) {
            v->krate = SM_KRATE;
            float pitch_mul = v->pitch, gain_mul = v->gain * v->p[SM_P_GAIN];
            float cut1 = v->p[SM_P_FLT(0) + 1], cut2 = v->p[SM_P_FLT(1) + 1], level1 = -1.0f;
            for (int c = 0; c < 4; c++) {
                int tgt = (int)v->p[SM_P_CTL(c)];
                if (v->p[SM_P_CTL(c) + 1] == 0.0f && v->p[SM_P_CTL(c) + 2] == 0.0f) continue;   /* unused */
                v->ctl[c] += (v->ctl_to[c] - v->ctl[c]) * sm_smooth_k;
                float val = sm_ctl_map(v, c);
                if (tgt == SM_TGT_PITCH) pitch_mul *= val;
                else if (tgt == SM_TGT_GAIN) gain_mul *= val;
                else if (tgt == SM_TGT_CUTOFF) cut1 = val;
                else if (tgt == SM_TGT_CUTOFF2) cut2 = val;
                else if (tgt == SM_TGT_LEVEL1) level1 = val;
            }
            float oct = 0.0f;
            for (int i = 0; i < 2; i++) {
                const float *d = &v->p[SM_P_MOD(i)];
                int kind = (int)d[0];
                if (kind == SM_MOD_OFF) continue;
                float val;
                if (kind == SM_MOD_LFO) {
                    val = sinf((float)(2.0 * SM_PI * v->mphase[i]));
                    v->mphase[i] += d[1] * SM_KRATE / SM_RATE; if (v->mphase[i] >= 1.0) v->mphase[i] -= floor(v->mphase[i]);
                } else {
                    if (v->walk_left[i] <= 0) { v->walk_to[i] = sm_white(v); v->walk_left[i] = (int)(SM_RATE / (d[1] > 0.05f ? d[1] : 0.05f)); }
                    v->walk_left[i] -= SM_KRATE;
                    v->walk[i] += (v->walk_to[i] - v->walk[i]) * (d[1] * 2.0f * SM_KRATE / SM_RATE);
                    val = v->walk[i];
                }
                int tgt = (int)d[2];
                if (tgt == SM_TGT_PITCH) pitch_mul *= powf(2.0f, d[3] * val / 12.0f);
                else if (tgt == SM_TGT_CUTOFF) oct += d[3] * val;
                else if (tgt == SM_TGT_GAIN) { float g = 1.0f + d[3] * val; gain_mul *= g < 0.0f ? 0.0f : g; }
            }
            v->k_pitch = pitch_mul; v->k_gain = gain_mul; v->k_cut1 = cut1; v->k_cut2 = cut2; v->k_level1 = level1; v->k_oct = oct;
            v->coef_age = 1 << 30;   /* filters follow at once */
        }
        v->krate--;
        float pitch_mul = v->k_pitch, gain_mul = v->k_gain, cut1 = v->k_cut1, cut2 = v->k_cut2, level1 = v->k_level1;
        float cut_oct = v->p[SM_P_ENV(1) + 4] * v->env + v->k_oct;   /* env 1 rides on the gain envelope's shape */
        if (v->gain_now < 0.0f) v->gain_now = gain_mul;
        v->gain_now += (gain_mul - v->gain_now) * sm_smooth;
        int recs = 0, done = 0, others = 0;
        for (int i = 0; i < 4; i++) {
            int kd = (int)v->p[SM_P_SRC(i)];
            if (kd == SM_SRC_RECORDING) { recs++; if (v->rec_done[i]) done++; } else if (kd != SM_SRC_OFF) others++;
        }
        if (recs > 0 && done == recs && others == 0) return 1;
        float xf = x0 + (x1 - x0) * (float)n / (float)frames;
        if (v->stereo) {   /* a stereo recording played flat: both channels, balanced by pan, no filters */
            SmRec *r = v->rec[0];
            if (v->rpos[0] < 0.0 || v->rpos[0] >= r->frames) { v->rec_done[0] = 1; continue; }
            int at = (int)v->rpos[0];
            float amp = v->env * v->gain_now * (m->group_from[v->group] + m->group_step[v->group] * (float)n) * xf;
            if (v->fading) { if (v->fade_left <= 0) return 1; amp *= (float)v->fade_left / (float)v->fade_len; v->fade_left--; }
            float gl = v->pan > 0.0f ? 1.0f - v->pan : 1.0f, gr = v->pan < 0.0f ? 1.0f + v->pan : 1.0f;
            out[2 * n] += r->data[2 * at] * amp * gl; out[2 * n + 1] += r->data[2 * at + 1] * amp * gr;
            v->rpos[0] += v->pitch;
            if (v->rpos[0] >= r->frames) v->rec_done[0] = 1;
            continue;
        }
        float x = 0.0f;
        for (int i = 0; i < 4; i++) {
            float sv = sm_source(v, i, pitch_mul);
            if (i == 0 && level1 >= 0.0f && v->p[SM_P_SRC(0) + 2] != 0.0f) sv *= level1 / v->p[SM_P_SRC(0) + 2];
            x += sv;
        }
        if (++v->coef_age >= SM_KRATE) {
            v->coef_age = 0;
            sm_svf_coef(v, 0, cut1 * powf(2.0f, cut_oct), v->p[SM_P_FLT(0) + 2]);
            sm_svf_coef(v, 1, cut2, v->p[SM_P_FLT(1) + 2]);
        }
        int k0 = (int)v->p[SM_P_FLT(0)], k1 = (int)v->p[SM_P_FLT(1)];
        if (k0) x = sm_svf(v, 0, k0, x);
        if (k1) x = sm_svf(v, 1, k1, x);
        float amp = x * v->env * v->gain_now * (m->group_from[v->group] + m->group_step[v->group] * (float)n) * xf;
        if (v->fading) {
            if (v->fade_left <= 0) return 1;
            amp *= (float)v->fade_left / (float)v->fade_len;
            v->fade_left--;
        }
        mono[n] += amp;
    }
    return 0;
}

/* A virtual voice: time passes, nothing is computed. Answers 1 when it ends. */
static inline int sm_voice_skip(SmVoice *v, int frames) {
    if (v->fading) { v->fade_left -= frames; if (v->fade_left <= 0) return 1; }
    int left = frames;
    while (left > 0) {
        if (v->hold > 0 && v->age < v->hold && v->age + left >= v->hold && v->stage < 3) {
            int k = (int)(v->hold - v->age); v->age += k; left -= k;
            if (v->stage == 0 || v->stage == 1 || v->stage == 3) v->stage_left = v->stage_left > k ? v->stage_left - k : 0;
            sm_env_stage(v, 3);
            continue;
        }
        if (v->stage == 0 || v->stage == 1 || v->stage == 3) {
            int64_t k = v->stage_left < left ? v->stage_left : left;
            v->stage_left -= k; v->age += k; left -= (int)k;
            float to = v->stage == 0 ? 1.0f : v->stage == 1 ? sm_clampf(v->p[SM_P_ENV(0) + 2], 0.0f, 1.0f) : 0.0f;
            v->env = v->stage_len > 0 ? to + (v->stage_from - to) * (float)v->stage_left / (float)v->stage_len : to;
            if (v->stage_left <= 0) { if (v->stage == 3) return 1; sm_env_stage(v, v->stage + 1); }
        } else if (v->stage == 2) { v->age += left; left = 0; }
        else return 1;
    }
    int recs = 0, done = 0, others = 0;
    for (int i = 0; i < 4; i++) {
        int kd = (int)v->p[SM_P_SRC(i)];
        if (kd == SM_SRC_RECORDING && v->rec[i]) {
            recs++;
            double rate = (v->p[SM_P_SRC(i) + 1] > 0.0f ? v->p[SM_P_SRC(i) + 1] : 1.0f) * v->pitch;
            v->rpos[i] += rate * frames;
            if (v->rpos[i] >= v->rec[i]->frames) {
                if (v->p[SM_P_LOOP] != 0.0f && !v->stereo) v->rpos[i] = fmod(v->rpos[i], (double)v->rec[i]->frames);
                else v->rec_done[i] = 1;
            }
            if (v->rec_done[i]) done++;
        } else if (kd != SM_SRC_OFF) others++;
    }
    return recs > 0 && done == recs && others == 0;
}

/* SN3D real spherical harmonics to order 3, ACN order; X front, Y left, Z up */
static inline void sm_sh(float X, float Y, float Z, float *o) {
    const float s3 = 1.7320508f, s15 = 3.8729833f, s58 = 0.7905694f, s38 = 0.6123724f;
    o[0] = 1.0f; o[1] = Y; o[2] = Z; o[3] = X;
    o[4] = s3 * X * Y; o[5] = s3 * Y * Z; o[6] = 0.5f * (3 * Z * Z - 1); o[7] = s3 * X * Z; o[8] = 0.5f * s3 * (X * X - Y * Y);
    o[9] = s58 * Y * (3 * X * X - Y * Y); o[10] = s15 * X * Y * Z; o[11] = s38 * Y * (5 * Z * Z - 1); o[12] = 0.5f * Z * (5 * Z * Z - 3);
    o[13] = s38 * X * (5 * Z * Z - 1); o[14] = 0.5f * s15 * Z * (X * X - Y * Y); o[15] = s58 * X * (X * X - 3 * Y * Y);
}
/* a channel's symmetry left to right: channels with m < 0 change sign in the other ear */
static inline int sm_sh_odd(int k) { int n = k < 1 ? 0 : k < 4 ? 1 : k < 9 ? 2 : 3; return k - n * (n + 1) < 0; }

/* place a positional voice's mono block: air absorption, distance, then the bus (headphones) or a pan (speakers) */
static inline void sm_occ_step(SmVoice *v, int frames) {
    if (v->occ_left <= 0) return;
    v->occ_left = v->occ_left > frames ? v->occ_left - frames : 0;
    float t = 1.0f - (float)v->occ_left / (float)SM_OCC_GLIDE;
    v->occ_gain = v->occ_gain_from + (v->occ_gain_to - v->occ_gain_from) * t;
    v->occ_cut = v->occ_cut_from * powf(v->occ_cut_to / v->occ_cut_from, t);
}
static inline void sm_place(SmMixer *m, SmVoice *v, float *out, int frames) {
    sm_occ_step(v, frames);
    float d[3] = { v->pos[0] - m->lis[0], v->pos[1] - m->lis[1], v->pos[2] - m->lis[2] };
    float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    float X = 1.0f, Y = 0.0f, Z = 0.0f;
    if (dist > 1e-4f) {
        X = (d[0] * m->lf[0] + d[1] * m->lf[1] + d[2] * m->lf[2]) / dist;
        Y = -(d[0] * m->lr[0] + d[1] * m->lr[1] + d[2] * m->lr[2]) / dist;
        Z = (d[0] * m->lu[0] + d[1] * m->lu[1] + d[2] * m->lu[2]) / dist;
    }
    float g = v->occ_gain * v->ref / (dist > v->ref ? dist : v->ref);
    /* air absorption: 20 kHz at the listener down to 4 kHz at 200 m; muffling may close it further */
    float air = 20000.0f * powf(0.2f, (dist < 200.0f ? dist : 200.0f) / 200.0f);
    float cut = air < v->occ_cut ? air : v->occ_cut;
    if (cut < 19000.0f) {
        float fc = sm_clampf(cut, 20.0f, 0.45f * SM_RATE), gg = (float)tan(SM_PI * fc / SM_RATE), k = 1.41421f;
        v->air_a1 = 1.0f / (1.0f + gg * (gg + k)); v->air_a2 = gg * v->air_a1; v->air_a3 = gg * v->air_a2; v->air_k = k;
        for (int n = 0; n < frames; n++) {
            SmSvf *f = &v->air; float x = m->mono[n];
            float v3 = x - f->ic2, v1 = v->air_a1 * f->ic1 + v->air_a2 * v3, v2 = f->ic2 + v->air_a2 * f->ic1 + v->air_a3 * v3;
            f->ic1 = 2.0f * v1 - f->ic1; f->ic2 = 2.0f * v2 - f->ic2;
            m->mono[n] = v2;
        }
    }
    /* the echo hears every placed voice, muffled as it is, but not fading
     * with distance: far off, a voice is mostly echo */
    float sg = v->occ_gain * 0.5f;
    for (int n = 0; n < frames; n++) m->send[n] += m->mono[n] * sg;
    if (m->headphones) {
        float c[16]; sm_sh(X, Y, Z, c);
        for (int k = 0; k < 16; k++) {
            float c1 = c[k] * g, c0 = v->placed ? v->coef[k] : c1, step = (c1 - c0) / (float)frames;
            float *b = m->bus[k];
            for (int n = 0; n < frames; n++) b[n] += m->mono[n] * (c0 + step * (float)n);
            v->coef[k] = c1;
        }
    } else {
        float lat = sm_clampf(-Y, -1.0f, 1.0f), ang = (lat + 1.0f) * (float)SM_PI / 4.0f;
        float l1 = cosf(ang) * g, r1 = sinf(ang) * g, l0 = v->placed ? v->pan_l : l1, r0 = v->placed ? v->pan_r : r1;
        for (int n = 0; n < frames; n++) {
            float t = (float)n / (float)frames;
            out[2 * n] += m->mono[n] * (l0 + (l1 - l0) * t); out[2 * n + 1] += m->mono[n] * (r0 + (r1 - r0) * t);
        }
        v->pan_l = l1; v->pan_r = r1;
    }
    v->placed = 1;
}

/* The late reverb's delay lines, in ms: spread, none a multiple of another. */
static const float sm_fdn_ms[8] = { 29.7f, 37.1f, 41.1f, 43.7f, 53.0f, 59.9f, 67.1f, 73.3f };

/* The echo of the listener's space: early reflections, each from its own
 * direction, then a feedback delay network whose loops lose 60 dB in the
 * decay asked and lose their highs faster as the damping rises. Everything
 * the space asks glides over 100 ms. */
static inline void sm_reverb(SmMixer *m, float *out, int frames) {
    float k = (float)frames / (float)SM_OCC_GLIDE; if (k > 1.0f) k = 1.0f;
    float d0 = m->rv_decay, w0 = m->rv_wet;
    m->rv_decay += (m->rv_decay_to - m->rv_decay) * k;
    m->rv_damp += (m->rv_damp_to - m->rv_damp) * k;
    m->rv_wet += (m->rv_wet_to - m->rv_wet) * k;
    /* a glide that has all but arrived arrives, so a dry space is truly dry */
    if (fabsf(m->rv_wet_to - m->rv_wet) < 1e-4f) m->rv_wet = m->rv_wet_to;
    if (fabsf(m->rv_decay_to - m->rv_decay) < 1e-4f) m->rv_decay = m->rv_decay_to;
    if (fabsf(m->rv_damp_to - m->rv_damp) < 1e-4f) m->rv_damp = m->rv_damp_to;
    int quiet = m->rv_wet < 1e-5f && w0 < 1e-5f;
    /* the sends' history */
    for (int n = 0; n < frames; n++) m->shist[(m->hist_at + n) & (SM_HIST - 1)] = m->send[n];
    if (quiet) {
        if (!m->rv_cleared) {   /* dry: the next space starts from silence, not from this one's frozen tail */
            memset(m->fdn, 0, sizeof m->fdn); memset(m->fdn_lp, 0, sizeof m->fdn_lp);
            memset(m->er_coef, 0, sizeof m->er_coef); memset(m->er_pan, 0, sizeof m->er_pan);
            m->rv_cleared = 1;
        }
        m->hist_at = (m->hist_at + frames) & (SM_HIST - 1);
        return;
    }
    m->rv_cleared = 0;
    /* early reflections: a tap into the history each, placed by its direction */
    for (int r = 0; r < SM_REFL; r++) {
        float *now = m->er_now[r], *to = m->er_to[r];
        float g0 = now[4];
        now[0] += (to[0] - now[0]) * k; now[4] += (to[4] - now[4]) * k;
        for (int q = 1; q < 4; q++) now[q] += (to[q] - now[q]) * k;
        if (g0 < 1e-5f && now[4] < 1e-5f) continue;
        float dx = now[1], dy = now[2], dz = now[3], dn = sqrtf(dx * dx + dy * dy + dz * dz);
        float X = 1.0f, Y = 0.0f, Z = 0.0f;
        if (dn > 1e-4f) {
            X = (dx * m->lf[0] + dy * m->lf[1] + dz * m->lf[2]) / dn;
            Y = -(dx * m->lr[0] + dy * m->lr[1] + dz * m->lr[2]) / dn;
            Z = (dx * m->lu[0] + dy * m->lu[1] + dz * m->lu[2]) / dn;
        }
        float g1 = now[4] * m->rv_wet, ga = g0 * w0;
        int dl = (int)(now[0] * SM_RATE);
        if (dl < 1) dl = 1;
        if (m->headphones) {
            float c[16]; sm_sh(X, Y, Z, c);
            for (int q = 0; q < 16; q++) {
                float c0 = m->er_coef[r][q], c1 = c[q] * g1, st = (c1 - c0) / (float)frames;
                for (int n = 0; n < frames; n++) m->bus[q][n] += m->shist[(m->hist_at + n - dl) & (SM_HIST - 1)] * (c0 + st * (float)n);
                m->er_coef[r][q] = c1;
            }
        } else {
            float lat = sm_clampf(-Y, -1.0f, 1.0f), ang = (lat + 1.0f) * (float)SM_PI / 4.0f;
            float l1 = cosf(ang) * g1, r1 = sinf(ang) * g1, l0 = m->er_pan[r][0], r0 = m->er_pan[r][1];
            for (int n = 0; n < frames; n++) {
                float t = (float)n / (float)frames, x = m->shist[(m->hist_at + n - dl) & (SM_HIST - 1)];
                out[2 * n] += x * (l0 + (l1 - l0) * t); out[2 * n + 1] += x * (r0 + (r1 - r0) * t);
            }
            m->er_pan[r][0] = l1; m->er_pan[r][1] = r1;
        }
        (void)ga;
    }
    /* the late reverb: 8 lines, mixed by a Hadamard matrix, each losing what the decay asks over its length */
    float gl[8], lp = 0.6f * m->rv_damp;   /* how much each loop's low-pass holds back the highs */
    int len[8];
    for (int i = 0; i < 8; i++) {
        len[i] = (int)(sm_fdn_ms[i] * SM_RATE / 1000.0f);
        gl[i] = powf(10.0f, -3.0f * (float)len[i] / (m->rv_decay * SM_RATE));
    }
    (void)d0;
    float wet = m->rv_wet, w_step = (m->rv_wet - w0) / (float)frames;
    for (int n = 0; n < frames; n++) {
        float x[8], in = m->shist[(m->hist_at + n - 480) & (SM_HIST - 1)] * 0.35f;   /* 10 ms before the tail starts */
        for (int i = 0; i < 8; i++) {
            float y = m->fdn[i][(m->fdn_at[i] - len[i] + SM_FDN_MAX) % SM_FDN_MAX];
            m->fdn_lp[i] = y + (m->fdn_lp[i] - y) * lp;
            x[i] = m->fdn_lp[i] * gl[i];
        }
        /* a fast Hadamard transform, scaled to keep energy */
        for (int h = 1; h < 8; h <<= 1)
            for (int i = 0; i < 8; i += h << 1)
                for (int j = i; j < i + h; j++) { float a = x[j], b = x[j + h]; x[j] = a + b; x[j + h] = a - b; }
        float l = 0.0f, r = 0.0f;
        for (int i = 0; i < 8; i++) {
            float v = x[i] * 0.35355339f + ((i & 1) ? -in : in);
            m->fdn[i][m->fdn_at[i]] = v;
            m->fdn_at[i] = (m->fdn_at[i] + 1) % SM_FDN_MAX;
            if (i & 1) r += v; else l += v;
        }
        float wn = (wet - (m->rv_wet - w0)) + w_step * (float)n;
        out[2 * n] += l * 0.5f * wn; out[2 * n + 1] += r * 0.5f * wn;
    }
    m->hist_at = (m->hist_at + frames) & (SM_HIST - 1);
}

static SmMixer *sm_sort_m;
static int sm_by_loud(const void *a, const void *b) {
    float x = sm_sort_m->voices[*(const int *)a].loud, y = sm_sort_m->voices[*(const int *)b].loud;
    if (x != y) return x < y ? 1 : -1;
    return *(const int *)a - *(const int *)b;
}

/* one block of at most SM_BLOCK frames */
static inline void sm_mix_block(SmMixer *m, float *out, int frames, int *playing, int *mixed_n) {
    /* a group's gain glides toward what was asked, 5 ms at a time, sample by sample */
    for (int g = 0; g < SM_GROUPS; g++) {
        float to = m->group_now[g] + (m->group_gain[g] - m->group_now[g]) * (frames >= SM_BLOCK ? 0.5f : 0.5f * frames / SM_BLOCK);
        m->group_from[g] = m->group_now[g]; m->group_step[g] = (to - m->group_now[g]) / (float)frames; m->group_now[g] = to;
    }
    /* rank: the loudest at the listener, times priority, are mixed */
    int n = 0;
    for (int i = 0; i < SM_TRACKED; i++) {
        SmVoice *v = &m->voices[i];
        if (v->number == 0) continue;
        v->loud = sm_loudness(m, v->gain * v->p[SM_P_GAIN], v->prio, v->flat, v->pos, v->ref, v->maxd, v->occ_gain, v->group);
        m->order[n++] = i;
    }
    sm_sort_m = m;
    qsort(m->order, (size_t)n, sizeof(int), sm_by_loud);
    for (int k = 0; k < 16; k++) memset(m->bus[k], 0, sizeof(float) * (size_t)frames);
    memset(m->send, 0, sizeof(float) * (size_t)frames);
    int mixed = 0;
    for (int j = 0; j < n; j++) {
        SmVoice *v = &m->voices[m->order[j]];
        int want = j < SM_MIXED && v->loud > 0.0f;
        int ended;
        if (want || v->mixed) {   /* mixed, or leaving the mix: render, fading in or out over this block */
            float x0 = v->mixed ? 1.0f : 0.0f, x1 = want ? 1.0f : 0.0f;
            memset(m->mono, 0, sizeof(float) * (size_t)frames);
            ended = sm_voice_render(m, v, m->mono, out, frames, x0, x1);
            if (!v->stereo) {
                if (v->flat) {
                    float ga = cosf((v->pan + 1.0f) * (float)SM_PI / 4.0f), gb = sinf((v->pan + 1.0f) * (float)SM_PI / 4.0f);
                    for (int t = 0; t < frames; t++) { out[2 * t] += m->mono[t] * ga; out[2 * t + 1] += m->mono[t] * gb; }
                } else sm_place(m, v, out, frames);
            }
            v->mixed = want;
            if (want) mixed++;
        } else { sm_occ_step(v, frames); ended = sm_voice_skip(v, frames); }
        if (ended) sm_voice_release(v);
        else (*playing)++;
    }
    *mixed_n = mixed;
    sm_reverb(m, out, frames);
    /* decode the bus: binaural for headphones (one ear's filters, mirrored), first-order for speakers */
    if (m->headphones) {
        float S[SM_BLOCK], A[SM_BLOCK];
        memset(S, 0, sizeof(float) * (size_t)frames); memset(A, 0, sizeof(float) * (size_t)frames);
        for (int k = 0; k < 16; k++) {
            float *h = m->hist[k];
            memcpy(h + SH_TAPS - 1, m->bus[k], sizeof(float) * (size_t)frames);
            float *acc = sm_sh_odd(k) ? A : S;
            const float *f = strata_hrir[k];
            for (int q = 0; q < SH_TAPS; q++) {   /* tap by tap, so the inner loop runs along the block */
                const float fq = f[q], *x = h + SH_TAPS - 1 - q;
                for (int t = 0; t < frames; t++) acc[t] += fq * x[t];
            }
            memmove(h, h + frames, sizeof(float) * (SH_TAPS - 1));
        }
        for (int t = 0; t < frames; t++) { out[2 * t] += S[t] + A[t]; out[2 * t + 1] += S[t] - A[t]; }
    } else {
        for (int t = 0; t < frames; t++) { float w = m->bus[0][t], y = m->bus[1][t]; out[2 * t] += 0.5f * (w + y); out[2 * t + 1] += 0.5f * (w - y); }
    }
}

/* Mix [frames] (any count) into interleaved stereo [out], replacing it. */
static inline void sm_mix(SmMixer *m, float *out, int frames) {
    memset(out, 0, sizeof(float) * 2 * (size_t)frames);
    int playing = 0, mixed = 0;
    for (int at = 0; at < frames; at += SM_BLOCK) {
        int k = frames - at < SM_BLOCK ? frames - at : SM_BLOCK;
        playing = 0;
        sm_mix_block(m, out + 2 * at, k, &playing, &mixed);
    }
    /* last line of defence: anything non-finite is silenced, and the state it may have entered is reset */
    int bad = 0;
    for (int n = 0; n < 2 * frames; n++) if (!isfinite(out[n])) { out[n] = 0.0f; bad = 1; }
    if (bad) {
        memset(m->fdn, 0, sizeof m->fdn); memset(m->fdn_lp, 0, sizeof m->fdn_lp); memset(m->shist, 0, sizeof m->shist);
        memset(m->hist, 0, sizeof m->hist); m->lim_env = 0.0f;
    }
    if (!isfinite(m->lim_env)) m->lim_env = 0.0f;
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
    m->st.playing = playing; m->st.virtual_ = playing - mixed;
    atomic_fetch_add(&m->blocks, 1);
    m->st.peak_db = (float)(20.0 * log10(peak > 1e-6f ? peak : 1e-6f));
    double rms = frames > 0 ? sqrt(sum / (2.0 * frames)) : 0.0;
    m->st.rms_db = (float)(20.0 * log10(rms > 1e-6 ? rms : 1e-6));
}

/* For tests and tools: is voice [number] mixed (not virtual)? Where is its source [i] in its recording, in frames? */
static inline int sm_is_mixed(SmMixer *m, int32_t number) { SmVoice *v = sm_find(m, number); return v && v->mixed; }
static inline double sm_voice_position(SmMixer *m, int32_t number, int i) { SmVoice *v = sm_find(m, number); return v && i >= 0 && i < 4 ? v->rpos[i] : -1.0; }

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
#define SM_FP_FLOOR -80.0f       /* bands quieter than this on both sides are not compared */
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
            if (x < SM_FP_FLOOR && y < SM_FP_FLOOR) continue;   /* both near silence: noise, not a difference */
            if (fabsf(x - y) > band_db) { if (bad < 8) fprintf(stderr, "fp: window %d value %d: %.2f against %.2f\n", w, i, x, y); bad++; }
        }
        double d = 10.0 * log10(ea) - 10.0 * log10(eb);
        if (fabs(d) > total_db) { if (bad < 8) fprintf(stderr, "fp: window %d total differs by %.2f dB\n", w, d); bad++; }
    }
    return bad ? 1 : 0;
}
#endif

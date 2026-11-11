/* strata_anim.h — Strata's pose kernel: rigs, masks, clips, figures, layers,
 * blends, crossfades, marks, and the hierarchy pass that writes one 3x4
 * world matrix per part. Plain C: no Vulkan, no GLFW, no globals, so it is
 * tested headless (native/tests/anim_test.c). native/strata_anim.c binds it
 * to March.
 *
 * Threads: one. Every call on an AnWorld runs on the same thread, in order.
 * Time is the dt passed to an_update, never the wall clock: the same calls
 * with the same dt give the same an_hash on the same build and machine.
 * Nothing here allocates after an_open.
 *
 * Refused calls change nothing; they are counted in an_stat(3) and the
 * reason (AN_R_*) is kept in an_stat(5). Marks that cannot be reported are
 * counted in an_stat(6), never dropped silently.
 *
 * The procedural stage (after the layers, in this order): the drop, the
 * overrides, aim, reach, then springs within the hierarchy pass. Its
 * settings live in one pool of records shared by every figure (AN_PROC_PER_FIG
 * a figure of capacity); a full pool refuses with AN_R_FULL. */
#ifndef STRATA_ANIM_H
#define STRATA_ANIM_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define AN_FIGURES 4096
#define AN_LAYERS 8
#define AN_PARTS 64
#define AN_BLEND 4
#define AN_RIGS 256
#define AN_MASKS 4096
#define AN_CLIPS 4096
#define AN_KEY_POOL (1 << 18)      /* keys over every clip; at most 65 535 in one clip */
#define AN_MARK_POOL (1 << 15)     /* marks over every clip */
#define AN_FIG_EVENTS 8            /* marks one figure reports from one update; more are counted in stat 6 */
#define AN_FADES 3                 /* sources one layer holds while crossfading */
#define AN_WRAPS 64                /* loops of one clip walked for marks in one update; more are counted in stat 6 */
#define AN_IDS (1 << 20)           /* figure ids: the n-th spawn is id n mod 2^20 */
#define AN_PROC_PER_FIG 8          /* procedural records in the pool, per figure of capacity */
#define AN_AIMS 4                  /* aim slots a figure */
#define AN_REACHES 8               /* reach slots a figure */
#define AN_SPRING_STEP (1.0f / 120)   /* springs are sub-stepped at this */
#define AN_SPRING_SNAP 0.5f        /* an update longer than this settles every spring at once */

/* why the last refused call was refused: an_stat(5) */
enum {
    AN_R_NONE = 0,
    AN_R_PARTS = 1,     /* a rig of no parts or more than AN_PARTS */
    AN_R_PARENT = 2,    /* a parent not before its child, or a second root */
    AN_R_VALUE = 3,     /* a value not finite, a zero quaternion, a box inside out, a negative duration */
    AN_R_RIG = 4,       /* no such rig */
    AN_R_CLIP = 5,      /* no such clip, or a clip of another rig */
    AN_R_FIGURE = 6,    /* no such figure (never spawned, or removed) */
    AN_R_LAYER = 7,     /* a layer outside 0..AN_LAYERS-1 */
    AN_R_FULL = 8,      /* a table or pool is full */
    AN_R_MASK = 9,      /* no such mask, or a mask of another rig, or a root outside the rig */
    AN_R_KEY = 10,      /* a key's part, channel or interpolation out of range */
    AN_R_BLEND = 11,    /* a blend of 0 or more than AN_BLEND clips, or positions not increasing */
    AN_R_WHAT = 12,     /* an_set: no such setting, or nothing playing on the layer */
    AN_R_PART = 13      /* the procedural stage: a part outside the rig, a slot outside its range, or a chain not running down the rig */
};

/* ---- small quaternion and vector arithmetic (xyzw, Hamilton product) ---- */
static inline void an_qmul(const float a[4], const float b[4], float o[4]) {
    float x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    float y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    float z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    float w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    o[0] = x; o[1] = y; o[2] = z; o[3] = w;
}
static inline void an_qrot(const float q[4], const float v[3], float o[3]) {
    /* v + 2w (u x v) + 2 u x (u x v) */
    float tx = 2 * (q[1] * v[2] - q[2] * v[1]);
    float ty = 2 * (q[2] * v[0] - q[0] * v[2]);
    float tz = 2 * (q[0] * v[1] - q[1] * v[0]);
    o[0] = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
    o[1] = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
    o[2] = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
}
static inline int an_qnorm(float q[4]) {   /* 0 if q has no length (left as identity) */
    float l = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!(l > 1e-12f) || !isfinite(l)) { q[0] = q[1] = q[2] = 0; q[3] = 1; return 0; }
    for (int i = 0; i < 4; i++) q[i] /= l;
    return 1;
}
/* the short way from a to b; u = 0 gives a, 1 gives b; unit length out */
static inline void an_slerp(const float a[4], const float b[4], float u, float o[4]) {
    float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    float s = 1.0f;
    if (d < 0) { d = -d; s = -1.0f; }
    float ka, kb;
    if (d > 0.9995f) { ka = 1.0f - u; kb = u; }
    else {
        float th = acosf(d), sn = sinf(th);
        ka = sinf((1.0f - u) * th) / sn; kb = sinf(u * th) / sn;
    }
    kb *= s;
    for (int i = 0; i < 4; i++) o[i] = ka * a[i] + kb * b[i];
    an_qnorm(o);
}
static inline int an_finite(const float *v, int n) {
    for (int i = 0; i < n; i++) if (!isfinite(v[i])) return 0;
    return 1;
}
static const float an_qid[4] = {0, 0, 0, 1};
static inline float an_dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static inline float an_len(const float a[3]) { return sqrtf(an_dot(a, a)); }
static inline void an_cross(const float a[3], const float b[3], float o[3]) {
    float x = a[1] * b[2] - a[2] * b[1], y = a[2] * b[0] - a[0] * b[2], z = a[0] * b[1] - a[1] * b[0];
    o[0] = x; o[1] = y; o[2] = z;
}
static inline float an_clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
/* a turn of angle radians about a unit axis */
static inline void an_qaxis(const float k[3], float angle, float o[4]) {
    float s = sinf(angle * 0.5f);
    o[0] = k[0] * s; o[1] = k[1] * s; o[2] = k[2] * s; o[3] = cosf(angle * 0.5f);
}
/* a unit vector perpendicular to v (v not zero): across the axis v leans on least */
static inline void an_perp(const float v[3], float o[3]) {
    float ax = fabsf(v[0]), ay = fabsf(v[1]), az = fabsf(v[2]);
    float e[3] = {0, 0, 0};
    e[ax <= ay && ax <= az ? 0 : (ay <= az ? 1 : 2)] = 1;
    an_cross(v, e, o);
    float l = an_len(o);
    for (int i = 0; i < 3; i++) o[i] /= l;
}
/* the shortest turn taking unit a onto unit b; half a turn about a perpendicular when opposite */
static inline void an_qfromto(const float a[3], const float b[3], float o[4]) {
    float d = an_dot(a, b);
    if (d < -0.999999f) { float k[3]; an_perp(a, k); an_qaxis(k, 3.14159265f, o); return; }
    an_cross(a, b, o);
    o[3] = 1 + d;
    an_qnorm(o);
}

/* ---- the world ---- */
typedef struct { float from, to, t, len; } AnRamp;   /* a value moving linearly from..to over len seconds */
static inline float an_ramp(const AnRamp *r) { return r->t >= r->len ? r->to : r->from + (r->to - r->from) * (r->t / r->len); }
static inline void an_ramp_to(AnRamp *r, float to, float len) {
    r->from = an_ramp(r); r->to = to; r->t = 0; r->len = len > 0 ? len : 0;
}
static inline void an_ramp_step(AnRamp *r, float dt) { r->t = r->t + dt < r->len ? r->t + dt : r->len; }

typedef struct {
    int nparts;
    int8_t parent[AN_PARTS];
    float pivot[AN_PARTS][3], rest[AN_PARTS][4], box[AN_PARTS][6];
} AnRig;
typedef struct { int rig; uint64_t parts; } AnMask;
typedef struct { float t, v[4]; } AnKey;   /* v: xyzw for a turn, xyz for a move; interpolation kept beside, in key_interp */
typedef struct {
    int rig, loops;
    float duration, stride;
    int32_t key_base;                                  /* the clip's keys in the pool start here */
    uint16_t first[AN_PARTS][2], count[AN_PARTS][2];   /* each part's track per channel, from key_base */
    int32_t mark_first, nmarks;
} AnClip;

/* one clip or one blend playing in a layer. A layer holds up to AN_FADES of
 * them: the newest fades in, the rest keep running while they fade out. */
typedef struct {
    float t;                     /* seconds into the clip, or the blend's phase 0..1 */
    float rate;
    float w, base;               /* share of the layer, and its share when the newest began */
    AnRamp value;                /* where the blend sits along pos */
    float pos[AN_BLEND];
    int16_t clip[AN_BLEND];
    int16_t mask;                /* a mask id, or -1 for every part */
    uint8_t n;                   /* clips: 1 for an_play, 1..AN_BLEND for an_blend */
    uint8_t blend;               /* time is a normalised phase */
    uint8_t paused, fresh, loops, additive;   /* fresh: the next step includes its starting point (for marks) */
} AnSource;
typedef struct {
    AnRamp weight;
    float fade_t, fade_len;      /* the newest source's crossfade */
    int32_t nsrc;                /* src[nsrc-1] is the newest */
    AnSource src[AN_FADES];
} AnLayer;
typedef struct {
    int32_t id;                  /* -1: free */
    int32_t rig, dirty;
    float pos[3], q[4];
    int32_t proc;                /* the first of its procedural records, -1 for none */
    float drop;                  /* metres the root is lowered after the layers */
    AnLayer layer[AN_LAYERS];
} AnFig;
typedef struct { float u; int32_t fig, layer, tag; float weight; } AnEvent;   /* u: how far through the update */

/* one setting of the procedural stage, in a pool shared by every figure.
 * A figure's records are a list sorted by kind, then slot.
 *   override: slot = part; v = q xyzw, move xyz, weight; n = 1 if it sets the move
 *   aim:      slot 0..3; chain[0..n-1]; v = axis xyz (unit, in the last part's frame), target xyz, limit (radians), weight
 *   reach:    slot 0..7; chain[0..2]; v = target xyz, pole xyz, weight, the weight it fades to, fade speed (weight a second)
 *   spring:   slot = part; v = stiffness, damping, world rotation xyzw, angular velocity xyz (world); fresh until first posed */
enum { AN_K_OVERRIDE = 0, AN_K_AIM = 1, AN_K_REACH = 2, AN_K_SPRING = 3 };
typedef struct {
    int32_t next;                /* the figure's next record, -1 at the end */
    uint8_t kind, slot, n, fresh;
    int8_t chain[4];
    float v[9];
} AnProc;

struct AnWorld {
    int capacity;
    AnRig *rigs; int nrigs;
    AnMask *masks; int nmasks;
    AnClip *clips; int nclips;
    AnKey *keys; uint8_t *key_interp; int nkeys;
    float *mark_t; int32_t *mark_tag; int nmarks_pool;
    AnFig *figs;
    float *mats;                 /* capacity x AN_PARTS x 12: column-major 3x4, world space */
    int32_t *ids; uint32_t ids_mask;   /* id -> slot: open addressing, linear probing, -1 empty; 2 x capacity rounded up to a power of two */
    int32_t *free_slots; int nfree;
    int32_t next_id;
    AnEvent *events, *events_tmp; int nevents, fig_events;   /* capacity x AN_FIG_EVENTS */
    int live, posed;
    double refused, negative_dt, dropped; int reason;
    AnProc *procs; int32_t *proc_free; int nprocs, nproc_free;   /* the procedural pool and its free stack */
};
typedef struct AnWorld AnWorld;

static inline int an_refuse(AnWorld *w, int reason) { w->refused += 1; w->reason = reason; return -1; }

static inline void an_close(AnWorld *w) {
    if (!w) return;
    free(w->rigs); free(w->masks); free(w->clips); free(w->keys); free(w->key_interp);
    free(w->mark_t); free(w->mark_tag); free(w->figs); free(w->mats);
    free(w->ids); free(w->free_slots); free(w->events); free(w->events_tmp);
    free(w->procs); free(w->proc_free);
    free(w);
}

static inline AnWorld *an_open(int capacity) {
    if (capacity < 1 || capacity > AN_FIGURES) return NULL;
    AnWorld *w = (AnWorld *)calloc(1, sizeof *w);
    if (!w) return NULL;
    w->capacity = capacity;
    w->rigs = (AnRig *)calloc(AN_RIGS, sizeof *w->rigs);
    w->masks = (AnMask *)calloc(AN_MASKS, sizeof *w->masks);
    w->clips = (AnClip *)calloc(AN_CLIPS, sizeof *w->clips);
    w->keys = (AnKey *)calloc(AN_KEY_POOL, sizeof *w->keys);
    w->key_interp = (uint8_t *)calloc(AN_KEY_POOL, 1);
    w->mark_t = (float *)calloc(AN_MARK_POOL, sizeof *w->mark_t);
    w->mark_tag = (int32_t *)calloc(AN_MARK_POOL, sizeof *w->mark_tag);
    w->figs = (AnFig *)calloc((size_t)capacity, sizeof *w->figs);
    w->mats = (float *)calloc((size_t)capacity * AN_PARTS * 12, sizeof *w->mats);
    uint32_t hn = 2;
    while (hn < 2u * (uint32_t)capacity) hn *= 2;
    w->ids_mask = hn - 1;
    w->ids = (int32_t *)malloc(hn * sizeof *w->ids);
    w->free_slots = (int32_t *)malloc((size_t)capacity * sizeof *w->free_slots);
    w->events = (AnEvent *)calloc((size_t)capacity * AN_FIG_EVENTS, sizeof *w->events);
    w->events_tmp = (AnEvent *)calloc((size_t)capacity * AN_FIG_EVENTS, sizeof *w->events_tmp);
    w->nprocs = capacity * AN_PROC_PER_FIG;
    w->procs = (AnProc *)calloc((size_t)w->nprocs, sizeof *w->procs);
    w->proc_free = (int32_t *)malloc((size_t)w->nprocs * sizeof *w->proc_free);
    if (!w->rigs || !w->masks || !w->clips || !w->keys || !w->key_interp || !w->mark_t || !w->mark_tag || !w->figs ||
        !w->mats || !w->ids || !w->free_slots || !w->events || !w->events_tmp || !w->procs || !w->proc_free) { an_close(w); return NULL; }
    for (int i = 0; i < w->nprocs; i++) w->proc_free[i] = w->nprocs - 1 - i;   /* record 0 handed out first */
    w->nproc_free = w->nprocs;
    for (uint32_t i = 0; i < hn; i++) w->ids[i] = -1;
    for (int i = 0; i < capacity; i++) { w->figs[i].id = -1; w->free_slots[i] = capacity - 1 - i; }   /* slot 0 handed out first */
    w->nfree = capacity;
    return w;
}

/* ---- rigs and masks ---- */
static inline int an_rig(AnWorld *w, int nparts, const int32_t *parent, const float *pivot,
                         const float *rest_q, const float *box) {
    if (nparts < 1 || nparts > AN_PARTS) return an_refuse(w, AN_R_PARTS);
    if (!parent || !pivot || !rest_q || !box) return an_refuse(w, AN_R_VALUE);
    if (parent[0] != -1) return an_refuse(w, AN_R_PARENT);
    for (int i = 1; i < nparts; i++) if (parent[i] < 0 || parent[i] >= i) return an_refuse(w, AN_R_PARENT);
    if (!an_finite(pivot, 3 * nparts) || !an_finite(rest_q, 4 * nparts) || !an_finite(box, 6 * nparts)) return an_refuse(w, AN_R_VALUE);
    for (int i = 0; i < nparts; i++) {
        float q[4] = {rest_q[i * 4], rest_q[i * 4 + 1], rest_q[i * 4 + 2], rest_q[i * 4 + 3]};
        if (!an_qnorm(q)) return an_refuse(w, AN_R_VALUE);
        for (int k = 0; k < 3; k++) if (box[i * 6 + k] > box[i * 6 + 3 + k]) return an_refuse(w, AN_R_VALUE);
    }
    if (w->nrigs >= AN_RIGS) return an_refuse(w, AN_R_FULL);
    AnRig *r = &w->rigs[w->nrigs];
    r->nparts = nparts;
    for (int i = 0; i < nparts; i++) {
        r->parent[i] = (int8_t)parent[i];
        memcpy(r->pivot[i], pivot + i * 3, sizeof r->pivot[i]);
        memcpy(r->rest[i], rest_q + i * 4, sizeof r->rest[i]);
        an_qnorm(r->rest[i]);
        memcpy(r->box[i], box + i * 6, sizeof r->box[i]);
    }
    return w->nrigs++;
}

static inline int an_mask(AnWorld *w, int rig, int nroots, const int32_t *roots) {
    if (rig < 0 || rig >= w->nrigs) return an_refuse(w, AN_R_RIG);
    const AnRig *r = &w->rigs[rig];
    if (nroots < 0 || (nroots > 0 && !roots)) return an_refuse(w, AN_R_MASK);
    uint64_t bits = 0;
    for (int i = 0; i < nroots; i++) {
        if (roots[i] < 0 || roots[i] >= r->nparts) return an_refuse(w, AN_R_MASK);
        bits |= 1ull << roots[i];
    }
    for (int i = 1; i < r->nparts; i++) if (bits >> r->parent[i] & 1) bits |= 1ull << i;   /* parent-first: one pass */
    if (w->nmasks >= AN_MASKS) return an_refuse(w, AN_R_FULL);
    w->masks[w->nmasks].rig = rig;
    w->masks[w->nmasks].parts = bits;
    return w->nmasks++;
}

/* ---- clips ---- */
static inline int an_clip(AnWorld *w, int rig, float duration, int loops, float stride,
                          int nkeys, const int32_t *key_ints, const float *key_floats,
                          int nmarks, const float *mark_t, const int32_t *mark_tag) {
    if (rig < 0 || rig >= w->nrigs) return an_refuse(w, AN_R_RIG);
    const AnRig *r = &w->rigs[rig];
    if (!isfinite(duration) || duration < 0 || !isfinite(stride)) return an_refuse(w, AN_R_VALUE);
    if (nkeys < 0 || nmarks < 0 || (nkeys > 0 && (!key_ints || !key_floats)) || (nmarks > 0 && (!mark_t || !mark_tag)))
        return an_refuse(w, AN_R_VALUE);
    for (int i = 0; i < nkeys; i++) {
        const int32_t *ki = key_ints + i * 3;
        if (ki[0] < 0 || ki[0] >= r->nparts || ki[1] < 0 || ki[1] > 1 || ki[2] < 0 || ki[2] > 2) return an_refuse(w, AN_R_KEY);
        if (!an_finite(key_floats + i * 5, ki[1] == 0 ? 5 : 4)) return an_refuse(w, AN_R_VALUE);
        if (ki[1] == 0) {
            float q[4] = {key_floats[i * 5 + 1], key_floats[i * 5 + 2], key_floats[i * 5 + 3], key_floats[i * 5 + 4]};
            if (!an_qnorm(q)) return an_refuse(w, AN_R_VALUE);
        }
    }
    if (!an_finite(mark_t, nmarks)) return an_refuse(w, AN_R_VALUE);
    if (w->nclips >= AN_CLIPS || nkeys > 65535 || w->nkeys + nkeys > AN_KEY_POOL || w->nmarks_pool + nmarks > AN_MARK_POOL)
        return an_refuse(w, AN_R_FULL);

    AnClip *c = &w->clips[w->nclips];
    memset(c, 0, sizeof *c);
    c->rig = rig; c->loops = loops != 0; c->duration = duration; c->stride = stride;
    /* tracks are laid out part by part, turn then move; within a track keys
     * are in time order, keys at the same time in the order given */
    c->key_base = w->nkeys;
    int at = 0;
    for (int i = 0; i < nkeys; i++) c->count[key_ints[i * 3]][key_ints[i * 3 + 1]]++;
    for (int p = 0; p < AN_PARTS; p++)
        for (int ch = 0; ch < 2; ch++) { c->first[p][ch] = (uint16_t)at; at += c->count[p][ch]; c->count[p][ch] = 0; }
    for (int i = 0; i < nkeys; i++) {
        int p = key_ints[i * 3], ch = key_ints[i * 3 + 1];
        int start = c->key_base + c->first[p][ch], j = start + c->count[p][ch]++;
        AnKey k;
        const float *f = key_floats + i * 5;
        k.t = f[0] < 0 ? 0 : (f[0] > duration ? duration : f[0]);
        memcpy(k.v, f + 1, sizeof k.v);
        if (ch == 0) an_qnorm(k.v); else k.v[3] = 0;
        for (; j > start && w->keys[j - 1].t > k.t; j--) { w->keys[j] = w->keys[j - 1]; w->key_interp[j] = w->key_interp[j - 1]; }
        w->keys[j] = k; w->key_interp[j] = (uint8_t)key_ints[i * 3 + 2];
    }
    w->nkeys += at;
    /* marks in time order; on a loop a mark at the duration is the mark at 0 */
    c->mark_first = w->nmarks_pool; c->nmarks = nmarks;
    for (int i = 0; i < nmarks; i++) {
        float t = mark_t[i] < 0 ? 0 : (mark_t[i] > duration ? duration : mark_t[i]);
        if (c->loops && t >= duration) t = 0;
        int j = c->mark_first + i;
        while (j > c->mark_first && w->mark_t[j - 1] > t) { w->mark_t[j] = w->mark_t[j - 1]; w->mark_tag[j] = w->mark_tag[j - 1]; j--; }
        w->mark_t[j] = t; w->mark_tag[j] = mark_tag[i];
    }
    w->nmarks_pool += nmarks;
    return w->nclips++;
}

/* a track's value at time t: hold before the first key and after the last */
static inline void an_sample(const AnKey *k, const uint8_t *interp, int n, float t, int turn, float o[4]) {
    if (n == 1 || t <= k[0].t) { memcpy(o, k[0].v, 4 * sizeof(float)); return; }
    if (t >= k[n - 1].t) { memcpy(o, k[n - 1].v, 4 * sizeof(float)); return; }
    int lo = 0, hi = n - 1;                    /* k[lo].t <= t < k[hi].t */
    while (hi - lo > 1) { int mid = (lo + hi) / 2; if (k[mid].t <= t) lo = mid; else hi = mid; }
    const AnKey *a = &k[lo], *b = &k[hi];      /* b->t > t >= a->t, so the span is never 0 */
    if (interp[lo] == 0) { memcpy(o, a->v, 4 * sizeof(float)); return; }
    float u = (t - a->t) / (b->t - a->t);
    if (interp[lo] == 2) u = u * u * (3.0f - 2.0f * u);
    if (turn) an_slerp(a->v, b->v, u, o);
    else for (int i = 0; i < 4; i++) o[i] = a->v[i] + (b->v[i] - a->v[i]) * u;
}

/* ---- figures ---- */
static inline uint32_t an_id_home(const AnWorld *w, int id) { return (uint32_t)id & w->ids_mask; }
static inline int an_slot(const AnWorld *w, int id) {   /* -1 if no live figure has this id */
    if (id < 0 || id >= AN_IDS) return -1;
    for (uint32_t i = an_id_home(w, id);; i = (i + 1) & w->ids_mask) {
        int s = w->ids[i];
        if (s < 0) return -1;
        if (w->figs[s].id == id) return s;
    }
}
static inline void an_id_insert(AnWorld *w, int id, int slot) {
    uint32_t i = an_id_home(w, id);
    while (w->ids[i] >= 0) i = (i + 1) & w->ids_mask;
    w->ids[i] = slot;
}
static inline void an_id_erase(AnWorld *w, int id) {   /* backward-shift deletion keeps every probe chain whole */
    uint32_t i = an_id_home(w, id);
    while (w->figs[w->ids[i]].id != id) i = (i + 1) & w->ids_mask;
    w->ids[i] = -1;
    for (uint32_t j = (i + 1) & w->ids_mask; w->ids[j] >= 0; j = (j + 1) & w->ids_mask) {
        uint32_t home = an_id_home(w, w->figs[w->ids[j]].id);
        /* the entry at j may move to the hole at i if its home is not in (i, j] */
        if (((j - home) & w->ids_mask) >= ((j - i) & w->ids_mask)) { w->ids[i] = w->ids[j]; w->ids[j] = -1; i = j; }
    }
}
static inline AnFig *an_fig(AnWorld *w, int fig) {
    int s = an_slot(w, fig);
    return s < 0 ? NULL : &w->figs[s];
}
static inline float *an_fig_mats(const AnWorld *w, const AnFig *f) { return w->mats + (size_t)(f - w->figs) * AN_PARTS * 12; }
static inline void an_pose(AnWorld *w, AnFig *f, float dt);

static inline int an_spawn(AnWorld *w, int rig) {
    if (rig < 0 || rig >= w->nrigs) return an_refuse(w, AN_R_RIG);
    if (w->nfree == 0) return an_refuse(w, AN_R_FULL);
    while (an_slot(w, w->next_id) >= 0) w->next_id = (w->next_id + 1) & (AN_IDS - 1);   /* at most capacity live ids */
    int id = w->next_id;
    w->next_id = (w->next_id + 1) & (AN_IDS - 1);
    int s = w->free_slots[--w->nfree];
    AnFig *f = &w->figs[s];
    memset(f, 0, sizeof *f);
    f->id = id; f->rig = rig; f->q[3] = 1; f->proc = -1;
    an_id_insert(w, id, s);
    w->live++;
    an_pose(w, f, 0);                          /* the rest pose, at the origin */
    return id;
}

static inline void an_remove(AnWorld *w, int fig) {
    AnFig *f = an_fig(w, fig);
    if (!f) { an_refuse(w, AN_R_FIGURE); return; }
    an_id_erase(w, fig);
    for (int i = f->proc; i >= 0; i = w->procs[i].next) w->proc_free[w->nproc_free++] = i;
    f->proc = -1;
    f->id = -1;
    w->free_slots[w->nfree++] = (int32_t)(f - w->figs);
    w->live--;
}

static inline void an_place(AnWorld *w, int fig, const float pos[3], const float q[4]) {
    AnFig *f = an_fig(w, fig);
    if (!f) { an_refuse(w, AN_R_FIGURE); return; }
    float n[4] = {q[0], q[1], q[2], q[3]};
    if (!an_finite(pos, 3) || !an_finite(q, 4) || !an_qnorm(n)) { an_refuse(w, AN_R_VALUE); return; }
    memcpy(f->pos, pos, sizeof f->pos);
    memcpy(f->q, n, sizeof f->q);
    f->dirty = 1;
}

/* the checks an_play and an_blend share; the layer, or NULL if refused */
static inline AnLayer *an_layer_for(AnWorld *w, int fig, int layer, float fade, float rate, float weight,
                                    int mask, AnFig **out) {
    AnFig *f = an_fig(w, fig);
    if (!f) { an_refuse(w, AN_R_FIGURE); return NULL; }
    if (layer < 0 || layer >= AN_LAYERS) { an_refuse(w, AN_R_LAYER); return NULL; }
    if (!isfinite(fade) || !isfinite(rate) || !isfinite(weight)) { an_refuse(w, AN_R_VALUE); return NULL; }
    if (mask >= 0 && (mask >= w->nmasks || w->masks[mask].rig != f->rig)) { an_refuse(w, AN_R_MASK); return NULL; }
    *out = f;
    return &f->layer[layer];
}

/* the new source fades in over fade while every older one fades out from
 * where it stands; a fifth source pushes out the faintest */
static inline void an_push(AnLayer *L, const AnSource *s, float fade, float weight) {
    if (L->nsrc == AN_FADES) {
        int lo = 0;
        for (int i = 1; i < L->nsrc; i++) if (L->src[i].w < L->src[lo].w) lo = i;
        for (int i = lo; i < L->nsrc - 1; i++) L->src[i] = L->src[i + 1];
        L->nsrc--;
    }
    float sum = 0;
    for (int i = 0; i < L->nsrc; i++) sum += L->src[i].w;
    for (int i = 0; i < L->nsrc; i++) L->src[i].base = L->src[i].w = sum > 0 ? L->src[i].w / sum : 0;
    L->src[L->nsrc] = *s;
    if (fade <= 0 || L->nsrc == 0) {           /* a cut, or the first clip: no one to fade from */
        L->src[0] = *s; L->nsrc = 1;
        L->src[0].w = L->src[0].base = 1;
        L->fade_t = L->fade_len = 0;
    } else {
        L->src[L->nsrc].w = L->src[L->nsrc].base = 0;
        L->nsrc++;
        L->fade_t = 0; L->fade_len = fade;
    }
    an_ramp_to(&L->weight, weight < 0 ? 0 : (weight > 1 ? 1 : weight), fade);
}

static inline void an_play(AnWorld *w, int fig, int layer, int clip, float fade, float rate, float weight, int mask, int additive) {
    AnFig *f;
    AnLayer *L = an_layer_for(w, fig, layer, fade, rate, weight, mask, &f);
    if (!L) return;
    if (clip < 0 || clip >= w->nclips || w->clips[clip].rig != f->rig) { an_refuse(w, AN_R_CLIP); return; }
    AnSource s;
    memset(&s, 0, sizeof s);
    s.n = 1; s.clip[0] = (int16_t)clip; s.rate = rate; s.fresh = 1; s.loops = (uint8_t)w->clips[clip].loops;
    s.additive = additive != 0;
    s.mask = (int16_t)(mask < 0 ? -1 : mask);
    an_push(L, &s, fade, weight);
    f->dirty = 1;
}

static inline void an_blend(AnWorld *w, int fig, int layer, int n, const int32_t *clips, const float *positions,
                            float value, float fade, float weight, int mask) {
    AnFig *f;
    AnLayer *L = an_layer_for(w, fig, layer, fade, 1, weight, mask, &f);
    if (!L) return;
    if (n < 1 || n > AN_BLEND || !clips || !positions || !isfinite(value)) { an_refuse(w, AN_R_BLEND); return; }
    AnSource s;
    memset(&s, 0, sizeof s);
    s.n = (uint8_t)n; s.blend = 1; s.rate = 1; s.fresh = 1; s.loops = 1;
    for (int i = 0; i < n; i++) {
        if (clips[i] < 0 || clips[i] >= w->nclips || w->clips[clips[i]].rig != f->rig) { an_refuse(w, AN_R_CLIP); return; }
        if (!isfinite(positions[i]) || (i > 0 && !(positions[i] > positions[i - 1]))) { an_refuse(w, AN_R_BLEND); return; }
        s.clip[i] = (int16_t)clips[i]; s.pos[i] = positions[i];
        if (!w->clips[clips[i]].loops) s.loops = 0;
    }
    s.value.from = s.value.to = value;
    s.mask = (int16_t)(mask < 0 ? -1 : mask);
    an_push(L, &s, fade, weight);
    f->dirty = 1;
}

static inline void an_set(AnWorld *w, int fig, int layer, int what, float v, float fade) {
    AnFig *f = an_fig(w, fig);
    if (!f) { an_refuse(w, AN_R_FIGURE); return; }
    if (layer < 0 || layer >= AN_LAYERS) { an_refuse(w, AN_R_LAYER); return; }
    if (!isfinite(v) || !isfinite(fade)) { an_refuse(w, AN_R_VALUE); return; }
    AnLayer *L = &f->layer[layer];
    if (what == 1) { an_ramp_to(&L->weight, v < 0 ? 0 : (v > 1 ? 1 : v), fade); f->dirty = 1; return; }
    if (what < 0 || what > 4 || L->nsrc == 0) { an_refuse(w, AN_R_WHAT); return; }
    AnSource *s = &L->src[L->nsrc - 1];
    if (what == 0) s->rate = v;
    else if (what == 2) {
        float len = s->blend ? 1.0f : w->clips[s->clip[0]].duration;
        float t = v < 0 ? 0 : v;
        if (s->loops && len > 0) t = fmodf(t, len);
        else if (t > len) t = len;
        s->t = t; s->fresh = 1;
    } else if (what == 3) {
        if (!s->blend) { an_refuse(w, AN_R_WHAT); return; }
        an_ramp_to(&s->value, v, fade);
    } else s->paused = v != 0 ? 1 : 0;
    f->dirty = 1;
}

/* ---- time and marks ---- */

/* a blend's two neighbours of its value and the share of the second */
static inline void an_neighbours(const AnSource *s, int *i, int *j, float *share) {
    float v = an_ramp(&s->value);
    *i = 0; *j = 0; *share = 0;
    if (s->n == 1 || v <= s->pos[0]) return;
    if (v >= s->pos[s->n - 1]) { *i = *j = s->n - 1; return; }
    int k = 0;
    while (v >= s->pos[k + 1]) k++;
    *i = k; *j = k + 1;
    *share = (v - s->pos[k]) / (s->pos[k + 1] - s->pos[k]);
}

typedef struct { double a, b, trav; int incl, fwd; } AnSeg;   /* a step from a to b, after trav of travel */

/* moves t by delta along a clip of length len, wrapping or holding at the
 * ends, and cuts the move into segments for the marks. Forward segments
 * cover (a, b], backward ones [b, a); a segment that starts the source or
 * starts after a wrap includes its start. */
static inline int an_step(float *t, uint8_t *fresh, double delta, double len, int loops, AnSeg *seg, double *skipped) {
    *skipped = 0;
    if (!(len > 0) || delta == 0) return 0;
    int n = 0, incl = *fresh, carry = 0;
    double left = fabs(delta), trav = 0;
    if (loops && left > AN_WRAPS * len) {     /* skip whole loops past the cap; the caller counts their marks */
        double skip = floor(left / len) - AN_WRAPS;
        left -= skip * len; trav += skip * len;
        *skipped = skip;
    }
    double x = *t;
    if (delta > 0) {
        while (left > 0 && n < AN_WRAPS + 3) {
            double room = len - x;
            if (left < room || !loops) {
                double b = left < room ? x + left : len;
                seg[n++] = (AnSeg){x, b, trav, incl, 1};
                x = b; left = 0;
            } else {
                seg[n++] = (AnSeg){x, len, trav, incl, 1};   /* marks at len were moved to 0 */
                trav += room; left -= room; x = 0; incl = 1;
                carry = left == 0;                         /* ended on the wrap: the next step includes 0 */
            }
        }
    } else {
        while (left > 0 && n < AN_WRAPS + 3) {
            double room = x;
            if (left < room || !loops) {
                double b = left < room ? x - left : 0;
                seg[n++] = (AnSeg){x, b, trav, incl, 0};
                x = b; left = 0;
            } else {
                seg[n++] = (AnSeg){x, 0, trav, incl, 0};
                trav += room; left -= room; x = len; incl = 1;
            }
        }
    }
    if (loops && (x >= len || x < 0)) x = 0;
    *t = (float)x; *fresh = (uint8_t)carry;
    return n;
}

static inline void an_emit(AnWorld *w, const AnClip *c, double scale, const AnSeg *seg, int nseg, double total,
                           int fig, int layer, float weight) {
    for (int g = 0; g < nseg; g++) {
        const AnSeg *s = &seg[g];
        int fwd = s->fwd;
        if (s->a == s->b && !s->incl) continue;   /* standing at a held end */
        for (int k = 0; k < c->nmarks; k++) {
            int m_i = fwd ? k : c->nmarks - 1 - k;   /* time order along the step */
            double m = w->mark_t[c->mark_first + m_i] * scale;
            int in = fwd ? ((s->incl ? m >= s->a : m > s->a) && m <= s->b)
                         : ((s->incl ? m <= s->a : m < s->a) && m >= s->b);
            if (!in) continue;
            if (w->fig_events >= AN_FIG_EVENTS) { w->dropped += 1; continue; }   /* this figure's share is full */
            w->fig_events++;
            AnEvent *e = &w->events[w->nevents++];
            e->u = total > 0 ? (float)((s->trav + fabs(m - s->a)) / total) : 0;
            e->fig = fig; e->layer = layer; e->tag = w->mark_tag[c->mark_first + m_i]; e->weight = weight;
        }
    }
}

/* advances one figure's layers by dt: ramps, crossfades, clip times, marks */
static inline void an_advance(AnWorld *w, AnFig *f, float dt) {
    AnSeg seg[AN_WRAPS + 3];
    double skipped;
    w->fig_events = 0;
    for (int l = 0; l < AN_LAYERS; l++) {
        AnLayer *L = &f->layer[l];
        if (L->nsrc == 0) continue;
        an_ramp_step(&L->weight, dt);
        if (L->fade_len > 0) {
            L->fade_t += dt;
            float u = L->fade_t >= L->fade_len ? 1.0f : L->fade_t / L->fade_len;
            for (int i = 0; i < L->nsrc - 1; i++) L->src[i].w = L->src[i].base * (1.0f - u);
            L->src[L->nsrc - 1].w = u;
            if (u >= 1.0f) {
                L->src[0] = L->src[L->nsrc - 1]; L->nsrc = 1;
                L->src[0].w = L->src[0].base = 1;
                L->fade_t = L->fade_len = 0;
            }
        }
        float lw = an_ramp(&L->weight);
        for (int i = 0; i < L->nsrc; i++) {
            AnSource *s = &L->src[i];
            an_ramp_step(&s->value, dt);
            if (s->paused) continue;
            if (!s->blend) {
                const AnClip *c = &w->clips[s->clip[0]];
                double delta = (double)dt * s->rate;
                int n = an_step(&s->t, &s->fresh, delta, c->duration, s->loops, seg, &skipped);
                w->dropped += skipped * c->nmarks;
                an_emit(w, c, 1.0, seg, n, fabs(delta), f->id, l, lw * s->w);
            } else {
                int a, b; float share;
                an_neighbours(s, &a, &b, &share);
                double da = w->clips[s->clip[a]].duration, db = w->clips[s->clip[b]].duration;
                double len = da + (db - da) * share;
                if (!(len > 0)) continue;
                double delta = (double)dt * s->rate / len;
                int n = an_step(&s->t, &s->fresh, delta, 1.0, s->loops, seg, &skipped);
                w->dropped += skipped * (w->clips[s->clip[a]].nmarks + (b != a ? w->clips[s->clip[b]].nmarks : 0));
                if (da > 0) an_emit(w, &w->clips[s->clip[a]], 1.0 / da, seg, n, fabs(delta), f->id, l, lw * s->w * (1.0f - share));
                if (b != a && db > 0) an_emit(w, &w->clips[s->clip[b]], 1.0 / db, seg, n, fabs(delta), f->id, l, lw * s->w * share);
            }
        }
    }
    /* reach weights fading; one that has faded to 0 is let go */
    for (int32_t *link = &f->proc; *link >= 0;) {
        AnProc *p = &w->procs[*link];
        if (p->kind == AN_K_REACH && p->v[8] > 0) {
            float step = p->v[8] * dt, d = p->v[7] - p->v[6];
            if (fabsf(d) <= step) { p->v[6] = p->v[7]; p->v[8] = 0; }
            else p->v[6] += d > 0 ? step : -step;
            if (p->v[8] == 0 && !(p->v[6] > 0)) {
                int32_t i = *link;
                *link = p->next;
                w->proc_free[w->nproc_free++] = i;
                continue;
            }
        }
        link = &p->next;
    }
}

/* ---- posing ---- */
typedef struct { int clip; float t, w; int additive; uint64_t mask; } AnTap;

/* one figure's pose in the making: local rotations and moves, and the world
 * rotations and origins the hierarchy gives them */
typedef struct { float q[AN_PARTS][4], m[AN_PARTS][3], wq[AN_PARTS][4], wp[AN_PARTS][3], root[3]; } AnBody;

/* the layers: each part's local rotation and move */
static inline void an_layers(const AnWorld *w, const AnFig *f, AnBody *b) {
    const AnRig *r = &w->rigs[f->rig];
    for (int p = 0; p < r->nparts; p++) { memcpy(b->q[p], r->rest[p], sizeof b->q[p]); b->m[p][0] = b->m[p][1] = b->m[p][2] = 0; }

    for (int l = 0; l < AN_LAYERS; l++) {
        const AnLayer *L = &f->layer[l];
        float lw = an_ramp(&L->weight);
        if (L->nsrc == 0 || !(lw > 0)) continue;
        AnTap tap[AN_FADES * 2]; int nt = 0;
        for (int i = 0; i < L->nsrc; i++) {
            const AnSource *s = &L->src[i];
            if (!(s->w > 0)) continue;
            uint64_t mask = s->mask < 0 ? ~0ull : w->masks[s->mask].parts;
            if (!s->blend) { tap[nt++] = (AnTap){s->clip[0], s->t, s->w, s->additive, mask}; continue; }
            int a, b; float share;
            an_neighbours(s, &a, &b, &share);
            tap[nt++] = (AnTap){s->clip[a], s->t * w->clips[s->clip[a]].duration, s->w * (1.0f - share), s->additive, mask};
            if (b != a) tap[nt++] = (AnTap){s->clip[b], s->t * w->clips[s->clip[b]].duration, s->w * share, s->additive, mask};
        }
        for (int p = 0; p < r->nparts; p++) {
            /* the layer's pose of the part: the taps that have a track for
             * it, weighted; the share they cover is how far the layer moves it */
            float oq[4] = {0, 0, 0, 1}, om[3] = {0, 0, 0}, ow = 0;
            float aq[4] = {0, 0, 0, 1}, am[3] = {0, 0, 0}, aw = 0;
            for (int i = 0; i < nt; i++) {
                const AnTap *t = &tap[i];
                if (!(t->mask >> p & 1) || !(t->w > 0)) continue;
                const AnClip *c = &w->clips[t->clip];
                int nr = c->count[p][0], nm = c->count[p][1];
                if (nr == 0 && nm == 0) continue;
                float sq[4], sm[4] = {0, 0, 0, 0};
                int ir = c->key_base + c->first[p][0], im = c->key_base + c->first[p][1];
                const AnKey *kr = &w->keys[ir], *km = &w->keys[im];
                if (nr) an_sample(kr, &w->key_interp[ir], nr, t->t, 1, sq);
                if (nm) an_sample(km, &w->key_interp[im], nm, t->t, 0, sm);
                if (t->additive) {
                    /* relative to the track's first key */
                    if (nr) { float inv[4] = {-kr[0].v[0], -kr[0].v[1], -kr[0].v[2], kr[0].v[3]}; an_qmul(sq, inv, sq); }
                    else memcpy(sq, an_qid, sizeof sq);
                    if (nm) for (int k = 0; k < 3; k++) sm[k] -= km[0].v[k];
                    aw += t->w;
                    an_slerp(aq, sq, t->w / aw, aq);
                    for (int k = 0; k < 3; k++) am[k] += (sm[k] - am[k]) * (t->w / aw);
                } else {
                    if (!nr) memcpy(sq, r->rest[p], sizeof sq);
                    ow += t->w;
                    an_slerp(oq, sq, t->w / ow, oq);
                    for (int k = 0; k < 3; k++) om[k] += (sm[k] - om[k]) * (t->w / ow);
                }
            }
            if (ow > 0) {
                float k = lw * (ow > 1 ? 1 : ow);
                an_slerp(b->q[p], oq, k, b->q[p]);
                for (int i = 0; i < 3; i++) b->m[p][i] += (om[i] - b->m[p][i]) * k;
            }
            if (aw > 0) {
                float k = lw * (aw > 1 ? 1 : aw), d[4];
                an_slerp(an_qid, aq, k, d);
                an_qmul(d, b->q[p], b->q[p]);
                an_qnorm(b->q[p]);
                for (int i = 0; i < 3; i++) b->m[p][i] += am[i] * k;
            }
        }
    }

}

/* world rotations and origins of parts from..nparts-1 (parents come first) */
static inline void an_fk(const AnRig *r, const AnFig *f, AnBody *b, int from) {
    for (int p = from; p < r->nparts; p++) {
        const float *pq = p == 0 ? f->q : b->wq[r->parent[p]];
        const float *pp = p == 0 ? b->root : b->wp[r->parent[p]];
        float off[3] = {r->pivot[p][0] + b->m[p][0], r->pivot[p][1] + b->m[p][1], r->pivot[p][2] + b->m[p][2]}, ro[3];
        an_qrot(pq, off, ro);
        for (int i = 0; i < 3; i++) b->wp[p][i] = pp[i] + ro[i];
        an_qmul(pq, b->q[p], b->wq[p]);
        an_qnorm(b->wq[p]);
    }
}

/* turn part p by d, a turn in world space, by changing its local rotation */
static inline void an_turn_world(const AnRig *r, const AnFig *f, AnBody *b, int p, const float d[4]) {
    const float *pq = p == 0 ? f->q : b->wq[r->parent[p]];
    float inv[4] = {-pq[0], -pq[1], -pq[2], pq[3]}, t[4];
    an_qmul(d, b->wq[p], t);
    an_qmul(inv, t, b->q[p]);
    an_qnorm(b->q[p]);
}

/* aim: the turn from the last part's axis to the target, shared equally
 * along the chain, each share at most the limit, about one world axis */
static inline void an_do_aim(const AnRig *r, const AnFig *f, AnBody *b, const AnProc *a) {
    int n = a->n, last = a->chain[n - 1];
    float ax[3], d[3];
    an_qrot(b->wq[last], a->v, ax);
    for (int i = 0; i < 3; i++) d[i] = a->v[3 + i] - b->wp[last][i];
    float dl = an_len(d), al = an_len(ax);
    if (!(dl > 1e-6f) || !(al > 1e-6f)) return;   /* the target at the part's origin: no direction */
    for (int i = 0; i < 3; i++) { d[i] /= dl; ax[i] /= al; }
    float th = acosf(an_clampf(an_dot(ax, d), -1, 1));
    if (!(th > 1e-7f)) return;
    float k[3];
    an_cross(ax, d, k);
    float kl = an_len(k);
    if (kl > 1e-6f) for (int i = 0; i < 3; i++) k[i] /= kl;
    else an_perp(ax, k);                       /* straight behind: any axis across */
    float share = th / (float)n;
    if (share > a->v[6]) share = a->v[6];
    share *= an_clampf(a->v[7], 0, 1);
    float dq[4];
    an_qaxis(k, share, dq);
    for (int i = 0; i < n; i++) { an_turn_world(r, f, b, a->chain[i], dq); an_fk(r, f, b, a->chain[i]); }
}

/* reach: two-bone IK in the plane of the root, the target and the pole */
static inline void an_do_reach(const AnRig *r, const AnFig *f, AnBody *b, const AnProc *a) {
    float wgt = an_clampf(a->v[6], 0, 1);
    if (!(wgt > 0)) return;
    int u = a->chain[0], l = a->chain[1], e = a->chain[2];
    float A[3], K[3], E[3], ak[3], ke[3], at[3], dir[3], v[3];
    memcpy(A, b->wp[u], sizeof A); memcpy(K, b->wp[l], sizeof K); memcpy(E, b->wp[e], sizeof E);
    for (int i = 0; i < 3; i++) { ak[i] = K[i] - A[i]; ke[i] = E[i] - K[i]; at[i] = a->v[i] - A[i]; }
    float L1 = an_len(ak), L2 = an_len(ke), D = an_len(at), dmax = L1 + L2;
    if (!(L1 > 1e-6f) || !(L2 > 1e-6f)) return;   /* a bone of no length has nothing to bend */
    /* the direction to the target; at the root, the way the end lies now, else the upper bone */
    if (D > 1e-6f * dmax) for (int i = 0; i < 3; i++) dir[i] = at[i] / D;
    else {
        float ae[3] = {E[0] - A[0], E[1] - A[1], E[2] - A[2]}, el = an_len(ae);
        for (int i = 0; i < 3; i++) dir[i] = el > 1e-6f * dmax ? ae[i] / el : ak[i] / L1;
    }
    float dmin = fabsf(L1 - L2);
    if (dmin < 1e-4f * dmax) dmin = 1e-4f * dmax;
    float Dc = an_clampf(D, dmin, dmax);
    /* the bend: toward the pole, across the line; a pole on the line falls
     * back to the side the knee is on now, then to any side */
    float pa[3] = {a->v[3] - A[0], a->v[4] - A[1], a->v[5] - A[2]}, pd = an_dot(pa, dir);
    for (int i = 0; i < 3; i++) v[i] = pa[i] - pd * dir[i];
    if (!(an_len(v) > 1e-5f * dmax)) {
        float kd = an_dot(ak, dir);
        for (int i = 0; i < 3; i++) v[i] = ak[i] - kd * dir[i];
        if (!(an_len(v) > 1e-5f * dmax)) an_perp(dir, v);
    }
    float vl = an_len(v);
    for (int i = 0; i < 3; i++) v[i] /= vl;
    float ca = an_clampf((L1 * L1 + Dc * Dc - L2 * L2) / (2 * L1 * Dc), -1, 1), sa = sqrtf(1 - ca * ca);
    float kn[3], en[3], from[3], to[3], dq[4], q0u[4], q0l[4];
    for (int i = 0; i < 3; i++) { kn[i] = A[i] + L1 * (ca * dir[i] + sa * v[i]); en[i] = A[i] + Dc * dir[i]; }
    memcpy(q0u, b->q[u], sizeof q0u); memcpy(q0l, b->q[l], sizeof q0l);
    for (int i = 0; i < 3; i++) { from[i] = ak[i] / L1; to[i] = (kn[i] - A[i]) / L1; }   /* |kn - A| is L1 */
    an_qfromto(from, to, dq);
    an_turn_world(r, f, b, u, dq);
    an_fk(r, f, b, u);
    {
        float k2[3], e2[3], t3[3];
        memcpy(k2, b->wp[l], sizeof k2);
        for (int i = 0; i < 3; i++) { e2[i] = b->wp[e][i] - k2[i]; t3[i] = en[i] - k2[i]; }
        float fl = an_len(e2), tl = an_len(t3);
        if (fl > 1e-6f && tl > 1e-6f) {
            for (int i = 0; i < 3; i++) { e2[i] /= fl; t3[i] /= tl; }
            an_qfromto(e2, t3, dq);
            an_turn_world(r, f, b, l, dq);
        }
    }
    if (wgt < 1) { an_slerp(q0u, b->q[u], wgt, b->q[u]); an_slerp(q0l, b->q[l], wgt, b->q[l]); }
    an_fk(r, f, b, u);
}

/* the pose up to the springs. stage 0: the layers and overrides only (the
 * animated pose, no drop); stage 1: with the drop, aim and reach. */
static inline void an_build(const AnWorld *w, const AnFig *f, AnBody *b, int stage) {
    const AnRig *r = &w->rigs[f->rig];
    an_layers(w, f, b);
    for (int i = f->proc; i >= 0; i = w->procs[i].next) {
        const AnProc *o = &w->procs[i];
        if (o->kind != AN_K_OVERRIDE) continue;
        float k = an_clampf(o->v[7], 0, 1);
        an_slerp(b->q[o->slot], o->v, k, b->q[o->slot]);
        if (o->n) for (int j = 0; j < 3; j++) b->m[o->slot][j] += (o->v[4 + j] - b->m[o->slot][j]) * k;
    }
    memcpy(b->root, f->pos, sizeof b->root);
    if (stage >= 1) b->root[1] -= f->drop;
    an_fk(r, f, b, 0);
    if (stage < 1) return;
    for (int i = f->proc; i >= 0; i = w->procs[i].next) {   /* aims, then reaches, each in slot order */
        const AnProc *o = &w->procs[i];
        if (o->kind == AN_K_AIM) an_do_aim(r, f, b, o);
        else if (o->kind == AN_K_REACH) an_do_reach(r, f, b, o);
    }
}

/* a spring: semi-implicit steps of at most AN_SPRING_STEP toward the target world rotation */
static inline void an_spring_step(AnProc *s, const float target[4], float dt) {
    float *q = s->v + 2, *om = s->v + 6;
    if (s->fresh || dt > AN_SPRING_SNAP) {
        memcpy(q, target, 4 * sizeof(float)); om[0] = om[1] = om[2] = 0; s->fresh = 0;
        return;
    }
    if (!(dt > 0)) return;
    int n = (int)ceilf(dt / AN_SPRING_STEP - 1e-3f);
    if (n < 1) n = 1;
    float h = dt / (float)n, k = s->v[0], c = s->v[1];
    for (int i = 0; i < n; i++) {
        float inv[4] = {-q[0], -q[1], -q[2], q[3]}, d[4], e[3];
        an_qmul(target, inv, d);               /* the turn from where it is to where it is drawn */
        if (d[3] < 0) for (int j = 0; j < 4; j++) d[j] = -d[j];
        float sn = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        float ang = 2 * atan2f(sn, d[3]);
        for (int j = 0; j < 3; j++) e[j] = sn > 1e-9f ? d[j] * ang / sn : 2 * d[j];
        for (int j = 0; j < 3; j++) om[j] += h * (k * e[j] - c * om[j]);
        float rv[3] = {om[0] * h, om[1] * h, om[2] * h}, rl = an_len(rv), dq[4];
        if (rl > 1e-9f) { float ax[3] = {rv[0] / rl, rv[1] / rl, rv[2] / rl}; an_qaxis(ax, rl, dq); }
        else { dq[0] = rv[0] / 2; dq[1] = rv[1] / 2; dq[2] = rv[2] / 2; dq[3] = 1; an_qnorm(dq); }
        float t[4];
        an_qmul(dq, q, t);
        memcpy(q, t, sizeof t);
        an_qnorm(q);
    }
}

/* poses figure f: the pose up to the springs, then the hierarchy with the
 * springs advanced by dt (0 holds them), into its matrices */
static inline void an_pose(AnWorld *w, AnFig *f, float dt) {
    const AnRig *r = &w->rigs[f->rig];
    AnBody b;
    an_build(w, f, &b, 1);
    int16_t spring[AN_PARTS];
    int any = 0;
    for (int p = 0; p < r->nparts; p++) spring[p] = -1;
    for (int i = f->proc; i >= 0; i = w->procs[i].next)
        if (w->procs[i].kind == AN_K_SPRING) { spring[w->procs[i].slot] = (int16_t)i; any = 1; }
    float *out = an_fig_mats(w, f);
    for (int p = 0; p < r->nparts; p++) {
        if (any) {   /* parents are final before their children: rebuild in place */
            const float *pq = p == 0 ? f->q : b.wq[r->parent[p]];
            const float *pp = p == 0 ? b.root : b.wp[r->parent[p]];
            float off[3] = {r->pivot[p][0] + b.m[p][0], r->pivot[p][1] + b.m[p][1], r->pivot[p][2] + b.m[p][2]}, ro[3];
            an_qrot(pq, off, ro);
            for (int i = 0; i < 3; i++) b.wp[p][i] = pp[i] + ro[i];
            an_qmul(pq, b.q[p], b.wq[p]);
            an_qnorm(b.wq[p]);
            if (spring[p] >= 0) {
                AnProc *s = &w->procs[spring[p]];
                an_spring_step(s, b.wq[p], dt);
                memcpy(b.wq[p], s->v + 2, sizeof b.wq[p]);
            }
        }
        const float x = b.wq[p][0], y = b.wq[p][1], z = b.wq[p][2], sw = b.wq[p][3];
        float *o = out + p * 12;
        o[0] = 1 - 2 * (y * y + z * z); o[1] = 2 * (x * y + sw * z);     o[2] = 2 * (x * z - sw * y);
        o[3] = 2 * (x * y - sw * z);     o[4] = 1 - 2 * (x * x + z * z); o[5] = 2 * (y * z + sw * x);
        o[6] = 2 * (x * z + sw * y);     o[7] = 2 * (y * z - sw * x);     o[8] = 1 - 2 * (x * x + y * y);
        o[9] = b.wp[p][0]; o[10] = b.wp[p][1]; o[11] = b.wp[p][2];
    }
    f->dirty = 0;
}

/* events in the order they happened within the update; ties keep the order
 * they were found in (figure slot, layer, source). A stable merge sort. */
static inline void an_sort_events(AnWorld *w) {
    int n = w->nevents;
    AnEvent *a = w->events, *b = w->events_tmp;
    for (int width = 1; width < n; width *= 2) {
        for (int lo = 0; lo < n; lo += 2 * width) {
            int mid = lo + width < n ? lo + width : n, hi = lo + 2 * width < n ? lo + 2 * width : n;
            int i = lo, j = mid, k = lo;
            while (i < mid && j < hi) b[k++] = a[j].u < a[i].u ? a[j++] : a[i++];
            while (i < mid) b[k++] = a[i++];
            while (j < hi) b[k++] = a[j++];
        }
        AnEvent *t = a; a = b; b = t;
    }
    if (a != w->events) memcpy(w->events, a, (size_t)n * sizeof *a);
}

static inline void an_update(AnWorld *w, float dt) {
    if (!(dt >= 0) || !isfinite(dt)) { w->negative_dt += 1; return; }
    w->nevents = 0;
    w->posed = 0;
    for (int s = 0; s < w->capacity; s++) {
        AnFig *f = &w->figs[s];
        if (f->id < 0) continue;
        an_advance(w, f, dt);
        an_pose(w, f, dt);
        w->posed++;
    }
    an_sort_events(w);
}

static inline const float *an_matrices(const AnWorld *w, int fig) {
    int s = an_slot(w, fig);
    return s < 0 ? NULL : w->mats + (size_t)s * AN_PARTS * 12;
}

/* ---- the procedural stage's calls ---- */

/* a figure's record of kind and slot, made (sorted in) if it has none;
 * NULL, refused with AN_R_FULL, when the pool is empty */
static inline AnProc *an_proc_get(AnWorld *w, AnFig *f, int kind, int slot, int *made) {
    int32_t *link = &f->proc;
    while (*link >= 0) {
        AnProc *p = &w->procs[*link];
        if (p->kind == kind && p->slot == slot) { *made = 0; return p; }
        if (p->kind > kind || (p->kind == kind && p->slot > slot)) break;
        link = &p->next;
    }
    if (w->nproc_free == 0) { an_refuse(w, AN_R_FULL); return NULL; }
    int32_t i = w->proc_free[--w->nproc_free];
    AnProc *p = &w->procs[i];
    memset(p, 0, sizeof *p);
    p->kind = (uint8_t)kind; p->slot = (uint8_t)slot; p->next = *link;
    *link = i; *made = 1;
    return p;
}
static inline AnProc *an_proc_find(const AnWorld *w, const AnFig *f, int kind, int slot) {
    for (int32_t i = f->proc; i >= 0; i = w->procs[i].next)
        if (w->procs[i].kind == kind && w->procs[i].slot == slot) return &w->procs[i];
    return NULL;
}
static inline void an_proc_clear(AnWorld *w, AnFig *f, int kind, int slot) {
    for (int32_t *link = &f->proc; *link >= 0; link = &w->procs[*link].next) {
        AnProc *p = &w->procs[*link];
        if (p->kind == kind && p->slot == slot) {
            int32_t i = *link;
            *link = p->next;
            w->proc_free[w->nproc_free++] = i;
            return;
        }
    }
}
/* is part p below part a (a strict descendant)? */
static inline int an_below(const AnRig *r, int a, int p) {
    if (p <= a) return 0;
    while (p > a) p = r->parent[p];
    return p == a;
}
/* n parts, each in the rig and below the one before */
static inline int an_chain_ok(const AnRig *r, int n, const int32_t *chain) {
    for (int i = 0; i < n; i++) {
        if (chain[i] < 0 || chain[i] >= r->nparts) return 0;
        if (i > 0 && !an_below(r, chain[i - 1], chain[i])) return 0;
    }
    return 1;
}

/* part's local rotation (and move, unless move is NULL) pulled toward q by weight; weight 0 clears */
static inline void an_override(AnWorld *w, int fig, int part, const float q[4], const float move[3], float weight) {
    AnFig *f = an_fig(w, fig);
    if (!f) { an_refuse(w, AN_R_FIGURE); return; }
    if (part < 0 || part >= w->rigs[f->rig].nparts) { an_refuse(w, AN_R_PART); return; }
    if (!isfinite(weight)) { an_refuse(w, AN_R_VALUE); return; }
    if (weight <= 0) { an_proc_clear(w, f, AN_K_OVERRIDE, part); f->dirty = 1; return; }
    float n[4];
    if (!q || !an_finite(q, 4) || (move && !an_finite(move, 3))) { an_refuse(w, AN_R_VALUE); return; }
    memcpy(n, q, sizeof n);
    if (!an_qnorm(n)) { an_refuse(w, AN_R_VALUE); return; }
    int made;
    AnProc *p = an_proc_get(w, f, AN_K_OVERRIDE, part, &made);
    if (!p) return;
    memcpy(p->v, n, sizeof n);
    for (int i = 0; i < 3; i++) p->v[4 + i] = move ? move[i] : 0;
    p->n = move != NULL;
    p->v[7] = weight > 1 ? 1 : weight;
    f->dirty = 1;
}

/* slot 0..3: the chain's last part turns its axis (in its own frame) toward
 * target (world), the turn shared along the n parts, each at most limit_deg;
 * weight 0 clears */
static inline void an_aim(AnWorld *w, int fig, int slot, int n, const int32_t *chain, const float axis[3],
                          const float target[3], float limit_deg, float weight) {
    AnFig *f = an_fig(w, fig);
    if (!f) { an_refuse(w, AN_R_FIGURE); return; }
    if (slot < 0 || slot >= AN_AIMS) { an_refuse(w, AN_R_PART); return; }
    if (!isfinite(weight)) { an_refuse(w, AN_R_VALUE); return; }
    if (weight <= 0) { an_proc_clear(w, f, AN_K_AIM, slot); f->dirty = 1; return; }
    if (n < 1 || n > 4 || !chain || !an_chain_ok(&w->rigs[f->rig], n, chain)) { an_refuse(w, AN_R_PART); return; }
    if (!axis || !target || !an_finite(axis, 3) || !an_finite(target, 3) || !isfinite(limit_deg) || limit_deg < 0 ||
        !(an_len(axis) > 1e-9f)) { an_refuse(w, AN_R_VALUE); return; }
    int made;
    AnProc *p = an_proc_get(w, f, AN_K_AIM, slot, &made);
    if (!p) return;
    p->n = (uint8_t)n;
    for (int i = 0; i < 4; i++) p->chain[i] = (int8_t)(i < n ? chain[i] : -1);
    float al = an_len(axis);
    for (int i = 0; i < 3; i++) { p->v[i] = axis[i] / al; p->v[3 + i] = target[i]; }
    p->v[6] = limit_deg * 3.14159265f / 180.0f;
    p->v[7] = weight > 1 ? 1 : weight;
    f->dirty = 1;
}

/* slot 0..7: the chain (upper, lower, end) places the end at target, bending
 * toward pole; its weight moves to weight over seconds (0: at once). A NULL
 * chain keeps the slot's chain, target and pole and only moves its weight
 * (nothing happens if the slot is empty). A weight that reaches 0 clears. */
static inline void an_reach_to(AnWorld *w, int fig, int slot, const int32_t chain[3], const float target[3],
                               const float pole[3], float weight, float seconds) {
    AnFig *f = an_fig(w, fig);
    if (!f) { an_refuse(w, AN_R_FIGURE); return; }
    if (slot < 0 || slot >= AN_REACHES) { an_refuse(w, AN_R_PART); return; }
    if (!isfinite(weight) || !isfinite(seconds)) { an_refuse(w, AN_R_VALUE); return; }
    float goal = an_clampf(weight, 0, 1);
    if (chain) {
        if (!an_chain_ok(&w->rigs[f->rig], 3, chain)) { an_refuse(w, AN_R_PART); return; }
        if (!target || !pole || !an_finite(target, 3) || !an_finite(pole, 3)) { an_refuse(w, AN_R_VALUE); return; }
    }
    AnProc *p = an_proc_find(w, f, AN_K_REACH, slot);
    if (!chain && !p) return;
    if (seconds <= 0 && goal <= 0) { an_proc_clear(w, f, AN_K_REACH, slot); f->dirty = 1; return; }
    if (!p) {
        int made;
        p = an_proc_get(w, f, AN_K_REACH, slot, &made);
        if (!p) return;
        p->v[6] = 0;                           /* a new reach fades in from 0 */
    }
    if (chain) {
        p->n = 3;
        for (int i = 0; i < 3; i++) { p->chain[i] = (int8_t)chain[i]; p->v[i] = target[i]; p->v[3 + i] = pole[i]; }
        p->chain[3] = -1;
    }
    p->v[7] = goal;
    if (seconds > 0) p->v[8] = 1.0f / seconds;
    else { p->v[6] = goal; p->v[8] = 0; }
    f->dirty = 1;
}
static inline void an_reach(AnWorld *w, int fig, int slot, const int32_t chain[3], const float target[3], const float pole[3], float weight) {
    if (!chain) { an_refuse(w, AN_R_PART); return; }
    an_reach_to(w, fig, slot, chain, target, pole, weight, 0);
}

/* part's world rotation lags its animated one: stiffness and damping per
 * second; stiffness 0 clears */
static inline void an_spring(AnWorld *w, int fig, int part, float stiffness, float damping) {
    AnFig *f = an_fig(w, fig);
    if (!f) { an_refuse(w, AN_R_FIGURE); return; }
    if (part < 0 || part >= w->rigs[f->rig].nparts) { an_refuse(w, AN_R_PART); return; }
    if (!isfinite(stiffness) || !isfinite(damping) || stiffness < 0 || damping < 0) { an_refuse(w, AN_R_VALUE); return; }
    if (stiffness == 0) { an_proc_clear(w, f, AN_K_SPRING, part); f->dirty = 1; return; }
    int made;
    AnProc *p = an_proc_get(w, f, AN_K_SPRING, part, &made);
    if (!p) return;
    if (made) { p->fresh = 1; p->v[5] = 1; }
    p->v[0] = stiffness; p->v[1] = damping;
    f->dirty = 1;
}

/* the root lowered by metres (world -y) after the layers, for foot placement */
static inline void an_drop(AnWorld *w, int fig, float metres) {
    AnFig *f = an_fig(w, fig);
    if (!f) { an_refuse(w, AN_R_FIGURE); return; }
    if (!isfinite(metres)) { an_refuse(w, AN_R_VALUE); return; }
    f->drop = metres;
    f->dirty = 1;
}

/* a part's origin in world space before the springs: stage 1 with the drop,
 * aim and reach; stage 0 the animated pose (layers and overrides, no drop).
 * Computed from the figure as it stands: the last update's pose unless it was
 * changed since. 0 if there is no such figure or part. */
static inline int an_part_origin_at(const AnWorld *w, int fig, int part, int stage, float out[3]) {
    int s = an_slot(w, fig);
    if (s < 0 || !out) return 0;
    const AnFig *f = &w->figs[s];
    if (part < 0 || part >= w->rigs[f->rig].nparts) return 0;
    AnBody b;
    an_build(w, f, &b, stage);
    memcpy(out, b.wp[part], 3 * sizeof(float));
    return 1;
}
static inline int an_part_origin(const AnWorld *w, int fig, int part, float out[3]) { return an_part_origin_at(w, fig, part, 1, out); }

static inline int an_marks(const AnWorld *w, int max, int32_t *ints, float *weights) {
    int n = w->nevents < max ? w->nevents : max;
    for (int i = 0; i < n; i++) {
        const AnEvent *e = &w->events[i];
        if (ints) { ints[i * 3] = e->fig; ints[i * 3 + 1] = e->layer; ints[i * 3 + 2] = e->tag; }
        if (weights) weights[i] = e->weight;
    }
    return n < 0 ? 0 : n;
}

static inline uint64_t an_hash(const AnWorld *w) {
    uint64_t h = 1469598103934665603ull;
    for (int s = 0; s < w->capacity; s++) {
        const AnFig *f = &w->figs[s];
        if (f->id < 0) continue;
        const unsigned char *p = (const unsigned char *)&f->id;
        for (size_t i = 0; i < sizeof f->id; i++) { h ^= p[i]; h *= 1099511628211ull; }
        p = (const unsigned char *)an_fig_mats(w, f);
        size_t len = (size_t)w->rigs[f->rig].nparts * 12 * sizeof(float);
        for (size_t i = 0; i < len; i++) { h ^= p[i]; h *= 1099511628211ull; }
        for (int i = f->proc; i >= 0; i = w->procs[i].next) {   /* spring state: what the matrices do not show */
            const AnProc *q = &w->procs[i];
            if (q->kind != AN_K_SPRING) continue;
            unsigned char tag[2] = {q->slot, q->fresh};
            for (size_t k = 0; k < 2; k++) { h ^= tag[k]; h *= 1099511628211ull; }
            p = (const unsigned char *)q->v;
            for (size_t k = 0; k < sizeof q->v; k++) { h ^= p[k]; h *= 1099511628211ull; }
        }
    }
    return h;
}

/* 0 figures, 1 posed, 2 stale, 3 refused calls, 4 negative dt, 5 last refusal reason, 6 marks dropped,
 * 7 procedural records in use */
static inline double an_stat(const AnWorld *w, int which) {
    switch (which) {
    case 0: return w->live;
    case 1: return w->posed;
    case 2: {
        int n = 0;
        for (int s = 0; s < w->capacity; s++) if (w->figs[s].id >= 0 && w->figs[s].dirty) n++;
        return n;
    }
    case 3: return w->refused;
    case 4: return w->negative_dt;
    case 5: return w->reason;
    case 6: return w->dropped;
    case 7: return w->nprocs - w->nproc_free;
    default: return 0;
    }
}

#endif

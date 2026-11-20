/* strata_particles.h — particles without the GPU: the ring's bookkeeping
 * (which slots hold particles that may still be alive), the layout of a
 * particle's record and of an effect in the effect table, and the closed
 * form particle.vert computes, here for tests. strata_vk.c owns the buffers.
 *
 * Particles are appended in birth order and never removed: a slot is live
 * until it is older than the longest life any particle has been given, and
 * the live slots are the span from `first` round to the newest. A full ring
 * overwrites its oldest slot. */
#ifndef STRATA_PARTICLES_H
#define STRATA_PARTICLES_H
#include <stdint.h>
#include <math.h>

#define SP_MAX_PER_CALL 512

typedef struct {
    float *births;        /* per slot, when its particle was born (seconds) */
    uint32_t cap;         /* slots in the ring */
    uint64_t head;        /* particles ever pushed; the next goes to head % cap */
    uint64_t tail;        /* the oldest that may still be alive */
    float longest;        /* the longest life given so far */
    uint64_t overwritten; /* pushes that took a slot still in the live span */
} SprayRing;

static inline void sp_init(SprayRing *r, float *births, uint32_t cap) {
    r->births = births; r->cap = cap; r->head = 0; r->tail = 0; r->longest = 0.0f; r->overwritten = 0;
}

/* How many particles one call may add. */
static inline int64_t sp_clamp(int64_t n) { return n < 0 ? 0 : (n > SP_MAX_PER_CALL ? SP_MAX_PER_CALL : n); }

/* Take the next slot for a particle born at [birth] that lives [life] seconds. */
static inline uint32_t sp_push(SprayRing *r, float birth, float life) {
    uint32_t slot = (uint32_t)(r->head % r->cap);
    r->births[slot] = birth;
    if (life > r->longest) r->longest = life;
    r->head++;
    if (r->head - r->tail > r->cap) { r->tail = r->head - r->cap; r->overwritten++; }      /* the oldest was overwritten */
    return slot;
}

/* Drop from the live span everything older than the longest life at time [now]. */
static inline void sp_expire(SprayRing *r, float now) {
    while (r->tail < r->head && now - r->births[r->tail % r->cap] > r->longest) r->tail++;
}

static inline uint32_t sp_count(const SprayRing *r) { return (uint32_t)(r->head - r->tail); }
static inline uint32_t sp_first(const SprayRing *r) { return (uint32_t)(r->tail % r->cap); }

/* A particle's record, 16 floats, written once at its birth:
 * [0..2] start (m) [3] birth (s) [4..6] velocity (m/s) [7] life (s)
 * [8] effect [9] size scale [10] seed 0..1 [11] rotation at birth (rad)
 * [12] texture layer override (-1: the effect's own)
 * [13..15] 0 for an effect that meets nothing. For one that meets the world
 * (PT_MEET_*), written by the renderer's collision pass (collide.comp) in
 * the frame after the birth: [13] the age (s) of its first meeting with a
 * solid voxel, PT_MEET_NEVER for none, PT_MEET_UNKNOWN until the pass has
 * run; [14] the face met first + 8 x the face met second (0 +y, 1 -y, 2 +x,
 * 3 -x, 4 +z, 5 -z); [15] the age of the second meeting, after a bounce. */
#define PT_FLOATS 16
#define PT_MEET_UNKNOWN (-1.0f)
#define PT_MEET_NEVER 1.0e9f
/* What an effect's particles do on meeting a solid voxel (effect float 41). */
#define PT_MEET_NOTHING 0
#define PT_MEET_DIE 1
#define PT_MEET_REST 2
#define PT_MEET_BOUNCE 3
/* An effect in the table, 11 vec4s (std430):
 * [0..3]   look (0 shaded, 1 glowing), layer, lit (0 per pixel, 1 per particle), colour keys used (1..4)
 * [4..19]  four colour keys (r, g, b, opacity)    [20..23] their ages (0..1)
 * [24..27] four sizes (m)                         [28..31] their ages (0..1)
 * [32..35] size keys used (1..4), drag k, wobble metres, wobble Hz
 * [36..39] ax, ay, az (m/s2), spin (rad/s)
 * [40] shape (0 a square facing the eye, 1 a cube)
 * [41] on meeting a solid voxel: PT_MEET_NOTHING, _DIE, _REST (stop there
 *      and live out its life) or _BOUNCE (once, then rest at the next)
 * [42] a bounce's restitution: the share of the speed into the face that
 *      comes back out   [43] the share of the speed along the face kept */
#define PT_EFFECT_FLOATS 44
#define PT_EFFECTS 64

/* Effect 0, spray: a shaded cube in the record's layer (the block it was
 * thrown from), white, size 1 (the record's size scale is spray's size),
 * ballistic under gravity, tumbling 4 radians a second, lit per pixel. Its fade over the last third is
 * spray's own (the shader keeps it for effect 0). */
static inline void pt_effect_spray(float *e) {
    for (int i = 0; i < PT_EFFECT_FLOATS; i++) e[i] = 0.0f;
    e[1] = 13.0f; e[3] = 1.0f;
    e[4] = 1.0f; e[5] = 1.0f; e[6] = 1.0f; e[7] = 1.0f;
    e[24] = 1.0f; e[32] = 1.0f;
    e[37] = -9.8f; e[39] = 4.0f;                       /* falling, tumbling */
    e[40] = 1.0f;
}

/* Does effect [e] (its PT_EFFECT_FLOATS floats) ask to meet the world? */
static inline int pt_effect_meets(const float *e) { return e[41] > 0.5f; }

/* How fast a particle is going at age [t], sway aside: the derivative of
 * pt_position's closed form. */
static inline void pt_velocity(const float *rec, const float *e, float t, float out[3]) {
    float k = e[33];
    for (int i = 0; i < 3; i++) {
        float v0 = rec[4 + i], a = e[36 + i];
        out[i] = k > 1e-6f ? a / k + (v0 - a / k) * expf(-k * t) : v0 + a * t;
    }
}

/* A velocity after a bounce off face [face] (0 +y, 1 -y, 2 +x, 3 -x, 4 +z,
 * 5 -z): the part along the face's normal turned back and scaled by
 * [restitution], the rest scaled by [keep]. */
static inline void pt_bounced(const float v[3], int face, float restitution, float keep, float out[3]) {
    int axis = face < 2 ? 1 : (face < 4 ? 0 : 2);
    for (int i = 0; i < 3; i++) out[i] = v[i] * (i == axis ? -restitution : keep);
}

/* Where a particle of an effect that meets the world is at age [t], as
 * particle.vert draws it (the half-size lift of a resting particle aside):
 * its free flight before the first meeting rec[13]; then nothing (answers
 * 0: it has died), the meeting point, or a second flight from there to the
 * second meeting rec[15]. Answers 1 while the particle is drawn. */
static inline void pt_position(const float *rec, const float *e, float t, float out[3]);
static inline int pt_position_met(const float *rec, const float *e, float t, float out[3]) {
    int mode = (int)(e[41] + 0.5f);
    float t1 = rec[13];
    if (mode == PT_MEET_NOTHING || t1 < 0.0f || t < t1) { pt_position(rec, e, t, out); return 1; }
    if (mode == PT_MEET_DIE) return 0;
    pt_position(rec, e, t1, out);
    if (mode == PT_MEET_REST) return 1;
    float v1[3], leg[16], still[PT_EFFECT_FLOATS];
    pt_velocity(rec, e, t1, v1);
    for (int i = 0; i < 16; i++) leg[i] = rec[i];
    for (int i = 0; i < PT_EFFECT_FLOATS; i++) still[i] = e[i];
    still[34] = 0.0f;                                   /* the second leg does not sway */
    pt_bounced(v1, (int)rec[14] & 7, e[42], e[43], leg + 4);
    for (int i = 0; i < 3; i++) leg[i] = out[i];
    float t2 = rec[15];
    pt_position(leg, still, (t < t2 ? t : t2) - t1, out);
    return 1;
}

/* Corner [v] (0..35) of the unit cube a particle is drawn as, about its
 * middle, and its face's outward normal: face v / 6 (+y, -y, +x, -x, +z,
 * -z), two triangles a face, each counter-clockwise seen from outside (the
 * winding the quad pipeline keeps as front). particle.vert holds the same
 * table. */
static inline void pt_cube_vertex(int v, float pos[3], float nrm[3]) {
    static const float N[6][3] = { {0,1,0}, {0,-1,0}, {1,0,0}, {-1,0,0}, {0,0,1}, {0,0,-1} };
    static const float T1[6][3] = { {0,0,1}, {1,0,0}, {0,1,0}, {0,0,1}, {1,0,0}, {0,1,0} };
    static const float T2[6][3] = { {1,0,0}, {0,0,1}, {0,0,1}, {0,1,0}, {0,1,0}, {1,0,0} };
    static const float S[6] = { 0, 1, 1, 0, 1, 0 }, T[6] = { 0, 0, 1, 0, 1, 1 };
    int f = (v / 6) % 6, c = v % 6;
    for (int k = 0; k < 3; k++) {
        nrm[k] = N[f][k];
        pos[k] = 0.5f * N[f][k] + (S[c] - 0.5f) * T1[f][k] + (T[c] - 0.5f) * T2[f][k];
    }
}

/* Where a particle is at age [t]: with drag k > 0,
 * p0 + (v0 - a/k)(1 - e^-kt)/k + (a/k) t; with none, p0 + v0 t + a t^2 / 2;
 * plus a sideways sway of w metres at f Hz, phased by the seed. */
static inline void pt_position(const float *rec, const float *e, float t, float out[3]) {
    float k = e[33];
    for (int i = 0; i < 3; i++) {
        float p0 = rec[i], v0 = rec[4 + i], a = e[36 + i];
        if (k > 1e-6f) out[i] = p0 + (v0 - a / k) * (1.0f - expf(-k * t)) / k + (a / k) * t;
        else out[i] = p0 + v0 * t + 0.5f * a * t * t;
    }
    float ph = 6.2831853f * (e[35] * t + rec[10]);
    out[0] += e[34] * sinf(ph);
    out[2] += e[34] * sinf(ph + 1.7f);
}

/* -1..1 from a seed, a particle's index and a purpose. */
static inline float pt_hash(uint32_t seed, uint32_t i, uint32_t salt) {
    uint32_t h = seed * 0x9e3779b9u ^ (i * 2654435761u + salt * 40503u);
    h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12; h *= 0x297a2d39u; h ^= h >> 15;
    return (float)(h & 0xffffu) / 32767.5f - 1.0f;
}

/* A point in the ball of radius [r] from three hashes: a direction and a
 * distance whose cube is uniform, so the ball is filled evenly. */
static inline void pt_in_ball(uint32_t seed, uint32_t i, uint32_t salt, float r, float out[3]) {
    float z = pt_hash(seed, i, salt), a = 3.14159265f * pt_hash(seed, i, salt + 1);
    float d = r * cbrtf(0.5f * (pt_hash(seed, i, salt + 2) + 1.0f));
    float s = sqrtf(fmaxf(0.0f, 1.0f - z * z));
    out[0] = d * s * cosf(a); out[1] = d * z; out[2] = d * s * sinf(a);
}

/* Particle [i] of [n] of a burst, born at [birth]: its start spread evenly
 * along the segment seg (x0, y0, z0, x1, y1, z1) by (i + 0.5) / n, then
 * offset inside the shape (0 a point, 1 a sphere of radius abc[0], 2 a box
 * of half sizes abc); its velocity vel plus a point in the ball of radius
 * [spread]; its life life * (1 + life_spread u), its size scale
 * 1 + size_spread u, its turn u pi, u in -1..1 each from pt_hash. */
static inline void pt_write(float *rec, int effect, int shape, const float seg[6], const float abc[3],
                            const float vel[3], float spread, float life, float life_spread, float size_spread,
                            uint32_t seed, uint32_t i, uint32_t n, float birth) {
    float f = n > 0 ? ((float)i + 0.5f) / (float)n : 0.0f;
    float off[3] = { 0.0f, 0.0f, 0.0f }, dv[3];
    if (shape == 1) pt_in_ball(seed, i, 1, abc[0], off);
    else if (shape == 2) { off[0] = abc[0] * pt_hash(seed, i, 1); off[1] = abc[1] * pt_hash(seed, i, 2); off[2] = abc[2] * pt_hash(seed, i, 3); }
    pt_in_ball(seed, i, 4, spread, dv);
    for (int k = 0; k < 3; k++) {
        rec[k] = seg[k] + (seg[3 + k] - seg[k]) * f + off[k];
        rec[4 + k] = vel[k] + dv[k];
    }
    rec[3] = birth;
    rec[7] = life * (1.0f + life_spread * pt_hash(seed, i, 7));
    rec[8] = (float)effect;
    rec[9] = 1.0f + size_spread * pt_hash(seed, i, 8);
    rec[10] = 0.5f * (pt_hash(seed, i, 9) + 1.0f);
    rec[11] = 3.14159265f * pt_hash(seed, i, 10);
    rec[12] = -1.0f; rec[13] = 0.0f; rec[14] = 0.0f; rec[15] = 0.0f;
}
#endif

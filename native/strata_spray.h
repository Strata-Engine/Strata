/* strata_spray.h — the bookkeeping of the spray ring: which slots hold
 * particles that may still be alive. No GPU here; strata_vk.c owns the
 * buffer the slots index.
 *
 * Particles are appended in birth order and never removed: a slot is live
 * until it is older than the longest life any particle has been given, and
 * the live slots are the span from `first` round to the newest. A full ring
 * overwrites its oldest slot. */
#ifndef STRATA_SPRAY_H
#define STRATA_SPRAY_H
#include <stdint.h>

#define SP_MAX_PER_CALL 512

typedef struct {
    float *births;        /* per slot, when its particle was born (seconds) */
    uint32_t cap;         /* slots in the ring */
    uint64_t head;        /* particles ever pushed; the next goes to head % cap */
    uint64_t tail;        /* the oldest that may still be alive */
    float longest;        /* the longest life given so far */
} SprayRing;

static inline void sp_init(SprayRing *r, float *births, uint32_t cap) {
    r->births = births; r->cap = cap; r->head = 0; r->tail = 0; r->longest = 0.0f;
}

/* How many particles one call may add. */
static inline int64_t sp_clamp(int64_t n) { return n < 0 ? 0 : (n > SP_MAX_PER_CALL ? SP_MAX_PER_CALL : n); }

/* Take the next slot for a particle born at [birth] that lives [life] seconds. */
static inline uint32_t sp_push(SprayRing *r, float birth, float life) {
    uint32_t slot = (uint32_t)(r->head % r->cap);
    r->births[slot] = birth;
    if (life > r->longest) r->longest = life;
    r->head++;
    if (r->head - r->tail > r->cap) r->tail = r->head - r->cap;      /* the oldest was overwritten */
    return slot;
}

/* Drop from the live span everything older than the longest life at time [now]. */
static inline void sp_expire(SprayRing *r, float now) {
    while (r->tail < r->head && now - r->births[r->tail % r->cap] > r->longest) r->tail++;
}

static inline uint32_t sp_count(const SprayRing *r) { return (uint32_t)(r->head - r->tail); }
static inline uint32_t sp_first(const SprayRing *r) { return (uint32_t)(r->tail % r->cap); }
#endif

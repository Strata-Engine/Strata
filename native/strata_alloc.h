/* strata_alloc.h -- a first-fit allocator of ranges in one large GPU buffer,
 * whose released ranges are HELD for two frames before they can be reused.
 *
 * The hold is what lets the renderer free a chunk's geometry without waiting
 * for the GPU: a frame already submitted may still be reading the range, and
 * with two frames in flight it is certainly idle two frames later.
 *
 * Header-only (static functions) so the shim and its test share one copy.
 */
#ifndef STRATA_ALLOC_H
#define STRATA_ALLOC_H
#include <stdint.h>
#include <string.h>

#define SA_MAX_FREE 16384
#define SA_MAX_HELD 65536

typedef struct { uint32_t first, count; } SaRange;

typedef struct {
    SaRange  free_list[SA_MAX_FREE];    /* sorted by first, never adjacent */
    int      nfree;
    SaRange  held[SA_MAX_HELD];         /* released, waiting out their two frames */
    uint64_t held_frame[SA_MAX_HELD];
    int      nheld;
    uint32_t total, live;
    uint32_t leaked;                    /* dropped because a list was full; never reused, never corrupts */
} SaAlloc;

static inline void sa_init(SaAlloc *a, uint32_t total) {
    memset(a, 0, sizeof *a);
    a->total = total;
    if (total) { a->free_list[0] = (SaRange){ 0, total }; a->nfree = 1; }
}

/* First fit. Returns 1 and the range's start, or 0 when nothing is big enough. */
static inline int sa_alloc(SaAlloc *a, uint32_t n, uint32_t *first) {
    if (n == 0) { *first = 0; return 1; }
    for (int i = 0; i < a->nfree; i++) {
        if (a->free_list[i].count >= n) {
            *first = a->free_list[i].first;
            a->free_list[i].first += n; a->free_list[i].count -= n;
            if (a->free_list[i].count == 0) {
                memmove(&a->free_list[i], &a->free_list[i + 1], (size_t)(a->nfree - i - 1) * sizeof(SaRange));
                a->nfree--;
            }
            a->live += n;
            return 1;
        }
    }
    return 0;
}

/* Put a range back on the free list, merging with whatever it touches. */
static inline void sa_free_now(SaAlloc *a, SaRange r) {
    int i = 0;
    while (i < a->nfree && a->free_list[i].first < r.first) i++;
    int prev = (i > 0 && a->free_list[i - 1].first + a->free_list[i - 1].count == r.first);
    int next = (i < a->nfree && r.first + r.count == a->free_list[i].first);
    if (prev && next) {
        a->free_list[i - 1].count += r.count + a->free_list[i].count;
        memmove(&a->free_list[i], &a->free_list[i + 1], (size_t)(a->nfree - i - 1) * sizeof(SaRange));
        a->nfree--;
    } else if (prev) {
        a->free_list[i - 1].count += r.count;
    } else if (next) {
        a->free_list[i].first = r.first; a->free_list[i].count += r.count;
    } else if (a->nfree < SA_MAX_FREE) {
        memmove(&a->free_list[i + 1], &a->free_list[i], (size_t)(a->nfree - i) * sizeof(SaRange));
        a->free_list[i] = r; a->nfree++;
    } else {
        a->leaked += r.count;
    }
}

/* Release a range at [frame]. It stops being live now and becomes reusable
 * once sa_collect is called with frame + 2 or later. */
static inline void sa_release(SaAlloc *a, SaRange r, uint64_t frame) {
    if (r.count == 0) return;
    a->live -= r.count;
    if (a->nheld < SA_MAX_HELD) { a->held[a->nheld] = r; a->held_frame[a->nheld] = frame; a->nheld++; }
    else a->leaked += r.count;
}

/* Return to the free list everything released at frame - 2 or earlier. */
static inline void sa_collect(SaAlloc *a, uint64_t frame) {
    int keep = 0;
    for (int i = 0; i < a->nheld; i++) {
        if (a->held_frame[i] + 2 <= frame) sa_free_now(a, a->held[i]);
        else { a->held[keep] = a->held[i]; a->held_frame[keep] = a->held_frame[i]; keep++; }
    }
    a->nheld = keep;
}

/* Give back a range that was allocated and never used: no frame can be
 * reading it, so it returns to the free list at once. */
static inline void sa_cancel(SaAlloc *a, SaRange r) {
    if (r.count == 0) return;
    a->live -= r.count;
    sa_free_now(a, r);
}

/* Allocate [n] ranges or none. On failure the allocator is exactly as it
 * was. A zero count yields a range at 0 and takes nothing. */
static inline int sa_alloc_many(SaAlloc *a, const uint32_t *counts, int n, uint32_t *firsts) {
    for (int i = 0; i < n; i++) {
        if (!sa_alloc(a, counts[i], &firsts[i])) {
            for (int j = 0; j < i; j++) sa_cancel(a, (SaRange){ firsts[j], counts[j] });
            return 0;
        }
    }
    return 1;
}

static inline uint32_t sa_live(const SaAlloc *a) { return a->live; }
static inline uint32_t sa_free_total(const SaAlloc *a) { uint32_t t = 0; for (int i = 0; i < a->nfree; i++) t += a->free_list[i].count; return t; }
static inline uint32_t sa_held_total(const SaAlloc *a) { uint32_t t = 0; for (int i = 0; i < a->nheld; i++) t += a->held[i].count; return t; }

#endif

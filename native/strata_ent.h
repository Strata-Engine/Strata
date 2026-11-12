/* strata_ent.h — Strata's entity kernel: kinds, rows, ids, slots, parts and
 * whole columns. Plain C: no Vulkan, no GLFW, no physics or audio, no
 * globals, so it is tested headless (native/tests/ent_test.c).
 *
 * Threads: one. Every call on an EnWorld runs on the same thread, in order.
 * Nothing here allocates after en_open except the block masks: 4,096 bytes
 * for each held chunk that is neither all solid nor all clear, and the chunk
 * table, which doubles as it fills (section "blocks").
 *
 * Storage. An entity is a row in packed columns, one array per column, each
 * capacity long. A row keeps its index for its whole life; removing it raises
 * the row's generation and puts the row back on a free stack (the last freed
 * row is reused first). Each kind keeps a dense array of its live rows' numbers
 * (each row keeps its index in that array): an add appends, a remove moves the
 * kind's last entry into the hole. Walking a kind is a sequential pass over
 * that array, costing what the kind holds and never touching dead rows or
 * other kinds. "Dense order" is add order until a removal, after which the
 * kind's last entity takes the removed one's place.
 *
 * An id is ((int64_t)generation << EN_ROW_BITS) | row. Generations run
 * 1..EN_GEN_MAX and then wrap to 1, so every id is positive and an old id
 * stops matching its row once the row is removed.
 *
 * A dead or unknown id reads as defaults (position 0, kind -1, slots 0,
 * parts -1, alive 0). Refused calls change nothing; they are counted in
 * en_stat(3) and the reason (EN_R_*) is kept in en_stat(6).
 *
 * Sections: kinds; rows and ids; slots and parts; each and columns; hash and
 * stats; blocks (the kernel's one-bit-a-voxel copy of the loaded chunks);
 * the ray; events; the tick. */
#ifndef STRATA_ENT_H
#define STRATA_ENT_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

#define EN_KINDS 62
#define EN_SLOTS 8                 /* float slots, and int slots, a kind at most */
#define EN_VOICES 2
#define EN_PARTS (2 + EN_VOICES)   /* figure, body, voice 0, voice 1 */
#define EN_CAPACITY_MAX (1 << 20)  /* rows, route points or bodies in one world */
#define EN_ROW_BITS 20
#define EN_ROW_MASK ((int64_t)EN_CAPACITY_MAX - 1)
#define EN_GEN_MAX 0x7fffffffu
#define EN_EVENTS 65536            /* events a tick, and removals queued between ticks, at most */
#define EN_BLOCK_IDS 65536         /* block ids are sixteen bits */
#define EN_CHUNK_COORD_MAX (1 << 25) /* |chunk coordinate| at most: its voxels fit an int */

/* a row's flags */
enum { EN_F_SEEN = 1, EN_F_STUCK = 2 };   /* seen this tick; a flyer that hit a block, still until the game moves it */
/* event codes */
enum { EN_E_TOUCH = 1, EN_E_BLOCK = 2, EN_E_ENTERED = 3, EN_E_LEFT = 4, EN_E_ROUTE = 5, EN_E_REMOVED = 6, EN_E_BODY_LOST = 7, EN_E_BLOCKED = 8 };

/* why the last refused call was refused: en_stat(6) */
enum {
    EN_R_NONE = 0,
    EN_R_KIND = 1,      /* no such kind */
    EN_R_ENTITY = 2,    /* no such entity (never added, or removed) */
    EN_R_VALUE = 3,     /* a number not finite or out of range */
    EN_R_FULL = 4,      /* the kind table or the world is full */
    EN_R_MOVER = 5,     /* a mover outside 0..3 */
    EN_R_UNSEEN = 6,    /* an unseen rule outside 0..2 */
    EN_R_DIVISOR = 7,   /* a tick divisor below 1 */
    EN_R_SLOTS = 8,     /* a slot count outside 0..EN_SLOTS, or a slot past the kind's */
    EN_R_BODY = 9,      /* a body on a mover other than Body, or a Body mover without one */
    EN_R_PART = 10,     /* a part outside 0..3, a flag not 0/1, too many voices, or a part the kind lacks */
    EN_R_WHICH = 11,    /* en_set or a column: no such field, or one a game may not set (age) */
    EN_R_ROWS = 12      /* en_set_column: rows not the kind's live count */
};

/* kind ints, in en_kind's order */
enum { EN_K_MOVER, EN_K_UNSEEN, EN_K_DIVISOR, EN_K_FSLOTS, EN_K_ISLOTS, EN_K_GRAVITY, EN_K_FIGURE, EN_K_BODY, EN_K_VOICES, EN_K_INTS };
/* kind floats, in en_kind's order */
enum { EN_K_HX, EN_K_HY, EN_K_HZ, EN_K_STEP, EN_K_LIFETIME, EN_K_SEEN, EN_K_GRAVITY_MS2, EN_K_FLOATS };

/* a held chunk: state 0 empty slot, 1 deleted slot, 2 all clear, 3 all solid, 4 mixed (mask) */
typedef struct {
    int32_t cx, cy, cz, state;
    uint8_t *mask;               /* 4096 bytes, bit Coord.index(x, y, z) is bit i % 8 of byte i / 8; state 4 only */
} EnChunk;

/* a removal made between ticks, written as event 6 at the start of the next tick */
typedef struct { int64_t id; int32_t reason; double pos[3]; } EnRemoval;

typedef struct {
    int32_t ints[EN_K_INTS];
    float floats[EN_K_FLOATS];   /* the seen distance is stored resolved: <= 0 is kept as the seen cap */
    int32_t live;                /* the kind's live rows: dense[kind * capacity + 0 .. live-1] */
} EnKind;

struct EnWorld {
    int capacity, route_points, bodies;
    double seen_cap, chunk_metres, voxel_metres;
    EnKind kinds[EN_KINDS]; int nkinds;
    /* the columns, one entry a row */
    int16_t *kind;               /* -1: a free row */
    uint32_t *gen;
    int32_t *dense_at;           /* the row's index in its kind's dense array; on a free row, unused */
    double *pos;                 /* 3 a row, metres */
    float *vel;                  /* 3 a row, metres a second */
    float *facing;               /* radians about the vertical */
    double *age;                 /* seconds since added */
    float *fslot;                /* EN_SLOTS a row; the kind's first n are used */
    int64_t *islot;              /* EN_SLOTS a row */
    int64_t *part;               /* EN_PARTS a row; -1 none */
    /* each kind's live rows, capacity entries a kind; calloc'd, so a kind's untouched
     * pages stay unmapped and resident memory follows what the kinds hold */
    int32_t *dense;
    int32_t *free_rows; int nfree;
    int live;
    double refused; int reason;
    uint8_t *flags;              /* EN_F_* a row */
    /* blocks */
    uint8_t *stops;              /* EN_BLOCK_IDS: 1 where a block id stops as its whole cell */
    EnChunk *chunks; int chunk_cap, chunk_used, chunk_dead;   /* open addressing, chunk_cap a power of 2 */
    /* events */
    int64_t *ev_ints;            /* 7 an event: code, a, b, i0, i1, i2, i3 */
    double *ev_floats;           /* 3 an event: x, y, z */
    int nev;
    EnRemoval *removed; int nremoved;
    int in_tick;
    double bad_dt, dropped; int seen;
};
typedef struct EnWorld EnWorld;

static inline int en_refuse(EnWorld *w, int reason) { w->refused += 1; w->reason = reason; return -1; }
static inline int en_float_ok(double v) { return isfinite(v) && fabs(v) <= FLT_MAX; }   /* finite once stored as a float */

static inline void en_close(EnWorld *w) {
    if (!w) return;
    free(w->kind); free(w->gen); free(w->dense_at); free(w->dense); free(w->pos); free(w->vel);
    free(w->facing); free(w->age); free(w->fslot); free(w->islot); free(w->part); free(w->free_rows);
    free(w->flags); free(w->stops); free(w->ev_ints); free(w->ev_floats); free(w->removed);
    if (w->chunks) for (int i = 0; i < w->chunk_cap; i++) free(w->chunks[i].mask);
    free(w->chunks);
    free(w);
}

/* route_points, bodies, seen_cap, chunk_metres and voxel_metres are checked
 * and kept for the movers, routes and seen test that come later. NULL if
 * any is out of range or memory runs out. */
static inline EnWorld *en_open(int capacity, int route_points, int bodies, double seen_cap,
                               double chunk_metres, double voxel_metres) {
    if (capacity < 1 || capacity > EN_CAPACITY_MAX || route_points < 1 || route_points > EN_CAPACITY_MAX ||
        bodies < 1 || bodies > EN_CAPACITY_MAX) return NULL;
    if (!(isfinite(seen_cap) && seen_cap > 0) || !(isfinite(chunk_metres) && chunk_metres > 0) ||
        !(isfinite(voxel_metres) && voxel_metres > 0)) return NULL;
    EnWorld *w = (EnWorld *)calloc(1, sizeof *w);
    if (!w) return NULL;
    w->capacity = capacity; w->route_points = route_points; w->bodies = bodies;
    w->seen_cap = seen_cap; w->chunk_metres = chunk_metres; w->voxel_metres = voxel_metres;
    size_t n = (size_t)capacity;
    w->kind = (int16_t *)malloc(n * sizeof *w->kind);
    w->gen = (uint32_t *)malloc(n * sizeof *w->gen);
    w->dense_at = (int32_t *)calloc(n, sizeof *w->dense_at);
    w->dense = (int32_t *)calloc(n * EN_KINDS, sizeof *w->dense);
    w->pos = (double *)calloc(n * 3, sizeof *w->pos);
    w->vel = (float *)calloc(n * 3, sizeof *w->vel);
    w->facing = (float *)calloc(n, sizeof *w->facing);
    w->age = (double *)calloc(n, sizeof *w->age);
    w->fslot = (float *)calloc(n * EN_SLOTS, sizeof *w->fslot);
    w->islot = (int64_t *)calloc(n * EN_SLOTS, sizeof *w->islot);
    w->part = (int64_t *)malloc(n * EN_PARTS * sizeof *w->part);
    w->free_rows = (int32_t *)malloc(n * sizeof *w->free_rows);
    w->flags = (uint8_t *)calloc(n, 1);
    w->stops = (uint8_t *)calloc(EN_BLOCK_IDS, 1);
    w->chunk_cap = 1024;
    w->chunks = (EnChunk *)calloc((size_t)w->chunk_cap, sizeof *w->chunks);
    w->ev_ints = (int64_t *)malloc((size_t)EN_EVENTS * 7 * sizeof *w->ev_ints);
    w->ev_floats = (double *)malloc((size_t)EN_EVENTS * 3 * sizeof *w->ev_floats);
    w->removed = (EnRemoval *)malloc((size_t)EN_EVENTS * sizeof *w->removed);
    if (!w->kind || !w->gen || !w->dense_at || !w->dense || !w->pos || !w->vel || !w->facing || !w->age ||
        !w->fslot || !w->islot || !w->part || !w->free_rows || !w->flags || !w->stops || !w->chunks ||
        !w->ev_ints || !w->ev_floats || !w->removed) { en_close(w); return NULL; }
    for (int r = 0; r < capacity; r++) {
        w->kind[r] = -1; w->gen[r] = 1;
        w->free_rows[r] = capacity - 1 - r;   /* row 0 handed out first */
    }
    for (size_t i = 0; i < n * EN_PARTS; i++) w->part[i] = -1;
    w->nfree = capacity;
    return w;
}

/* ---- kinds ---- */
/* ints: mover (0 placed, 1 flying, 2 walking, 3 body), unseen (0 live, 1 freeze, 2 remove), divisor >= 1,
 * float slots, int slots, gravity 0/1, has figure 0/1, has body 0/1, voices 0..2.
 * floats: hx, hy, hz (half extents, metres, >= 0), step height (>= 0), lifetime (<= 0 none),
 * seen distance (<= 0: the seen cap; at most the seen cap), gravity m/s2. The kind's index, or -1. */
static inline int en_kind(EnWorld *w, const int32_t ints[9], const float floats[7]) {
    if (!ints || !floats) return en_refuse(w, EN_R_VALUE);
    if (ints[EN_K_MOVER] < 0 || ints[EN_K_MOVER] > 3) return en_refuse(w, EN_R_MOVER);
    if (ints[EN_K_UNSEEN] < 0 || ints[EN_K_UNSEEN] > 2) return en_refuse(w, EN_R_UNSEEN);
    if (ints[EN_K_DIVISOR] < 1) return en_refuse(w, EN_R_DIVISOR);
    if (ints[EN_K_FSLOTS] < 0 || ints[EN_K_FSLOTS] > EN_SLOTS || ints[EN_K_ISLOTS] < 0 || ints[EN_K_ISLOTS] > EN_SLOTS)
        return en_refuse(w, EN_R_SLOTS);
    if ((ints[EN_K_GRAVITY] | 1) != 1) return en_refuse(w, EN_R_VALUE);
    if ((ints[EN_K_FIGURE] | 1) != 1 || (ints[EN_K_BODY] | 1) != 1 || ints[EN_K_VOICES] < 0 || ints[EN_K_VOICES] > EN_VOICES)
        return en_refuse(w, EN_R_PART);
    if ((ints[EN_K_BODY] == 1) != (ints[EN_K_MOVER] == 3)) return en_refuse(w, EN_R_BODY);
    for (int i = 0; i < EN_K_FLOATS; i++) if (!isfinite(floats[i])) return en_refuse(w, EN_R_VALUE);
    if (floats[EN_K_HX] < 0 || floats[EN_K_HY] < 0 || floats[EN_K_HZ] < 0 || floats[EN_K_STEP] < 0 ||
        floats[EN_K_SEEN] > (float)w->seen_cap) return en_refuse(w, EN_R_VALUE);
    if (w->nkinds >= EN_KINDS) return en_refuse(w, EN_R_FULL);
    EnKind *k = &w->kinds[w->nkinds];
    memcpy(k->ints, ints, sizeof k->ints);
    memcpy(k->floats, floats, sizeof k->floats);
    if (k->floats[EN_K_SEEN] <= 0) k->floats[EN_K_SEEN] = (float)w->seen_cap;
    k->live = 0;
    return w->nkinds++;
}

/* a kind's int or float, in en_kind's order; -1 or 0 for no such kind or field */
static inline int en_kind_int(const EnWorld *w, int kind, int which) {
    if (kind < 0 || kind >= w->nkinds || which < 0 || which >= EN_K_INTS) return -1;
    return w->kinds[kind].ints[which];
}
static inline double en_kind_float(const EnWorld *w, int kind, int which) {
    if (kind < 0 || kind >= w->nkinds || which < 0 || which >= EN_K_FLOATS) return 0;
    return w->kinds[kind].floats[which];
}

/* ---- rows and ids ---- */
/* the row an id names, or -1 if it names no live entity */
static inline int en_row(const EnWorld *w, int64_t id) {
    if (id < 0) return -1;
    int64_t row = id & EN_ROW_MASK, gen = id >> EN_ROW_BITS;
    if (row >= w->capacity || gen > (int64_t)EN_GEN_MAX) return -1;
    if (w->kind[row] < 0 || w->gen[row] != (uint32_t)gen) return -1;
    return (int)row;
}
static inline int64_t en_id(const EnWorld *w, int row) { return ((int64_t)w->gen[row] << EN_ROW_BITS) | row; }

/* the id of a new entity of kind, or -1 */
static inline int64_t en_add(EnWorld *w, int kind, double x, double y, double z, double facing) {
    if (kind < 0 || kind >= w->nkinds) return en_refuse(w, EN_R_KIND);
    if (!isfinite(x) || !isfinite(y) || !isfinite(z) || !en_float_ok(facing)) return en_refuse(w, EN_R_VALUE);
    if (w->nfree == 0) return en_refuse(w, EN_R_FULL);
    int r = w->free_rows[--w->nfree];
    EnKind *k = &w->kinds[kind];
    w->kind[r] = (int16_t)kind;
    w->dense[(size_t)kind * w->capacity + k->live] = r;
    w->dense_at[r] = k->live++;
    w->pos[r * 3] = x; w->pos[r * 3 + 1] = y; w->pos[r * 3 + 2] = z;
    w->vel[r * 3] = w->vel[r * 3 + 1] = w->vel[r * 3 + 2] = 0;
    w->facing[r] = (float)facing;
    w->age[r] = 0;
    w->flags[r] = 0;
    memset(w->fslot + (size_t)r * EN_SLOTS, 0, EN_SLOTS * sizeof *w->fslot);
    memset(w->islot + (size_t)r * EN_SLOTS, 0, EN_SLOTS * sizeof *w->islot);
    for (int p = 0; p < EN_PARTS; p++) w->part[(size_t)r * EN_PARTS + p] = -1;
    w->live++;
    return en_id(w, r);
}

static inline void en_event(EnWorld *w, int code, int64_t a, int64_t b, int64_t i0, int64_t i1, int64_t i2, int64_t i3,
                            double x, double y, double z);

/* reason: 0 the game, 1 lifetime, 2 the unseen rule. Each removal is reported as event 6 (i0 the reason, the
 * point its last position): one made inside en_tick in that tick's events; one made between ticks is queued
 * (the id and position copied, so the row can be reused at once) and written first by the next en_tick. */
static inline void en_remove(EnWorld *w, int64_t id, int reason) {
    int r = en_row(w, id);
    if (r < 0) { en_refuse(w, EN_R_ENTITY); return; }
    if (reason < 0 || reason > 2) { en_refuse(w, EN_R_VALUE); return; }
    const double *p = w->pos + r * 3;
    if (w->in_tick) en_event(w, EN_E_REMOVED, id, 0, reason, 0, 0, 0, p[0], p[1], p[2]);
    else if (w->nremoved >= EN_EVENTS) w->dropped += 1;
    else {
        EnRemoval *q = &w->removed[w->nremoved++];
        q->id = id; q->reason = reason; q->pos[0] = p[0]; q->pos[1] = p[1]; q->pos[2] = p[2];
    }
    EnKind *k = &w->kinds[w->kind[r]];
    int32_t *dense = w->dense + (size_t)w->kind[r] * w->capacity;
    int last = dense[--k->live];   /* the kind's last entry fills the hole */
    dense[w->dense_at[r]] = last;
    w->dense_at[last] = w->dense_at[r];
    w->kind[r] = -1;
    w->gen[r] = w->gen[r] >= EN_GEN_MAX ? 1 : w->gen[r] + 1;
    w->free_rows[w->nfree++] = r;
    w->live--;
}

static inline int en_alive(const EnWorld *w, int64_t id) { return en_row(w, id) >= 0; }
static inline int en_kind_of(const EnWorld *w, int64_t id) {
    int r = en_row(w, id);
    return r < 0 ? -1 : w->kind[r];
}

/* which: 0 position, 1 velocity (3 doubles); 2 facing, 3 age (1 double, in out[0]).
 * 1 if read; 0 for no such field (out untouched) or a dead id (the field reads 0). */
static inline int en_get(const EnWorld *w, int64_t id, int which, double out[3]) {
    if (which < 0 || which > 3) return 0;
    int r = en_row(w, id);
    if (r < 0) { for (int i = 0; i < (which <= 1 ? 3 : 1); i++) out[i] = 0; return 0; }
    switch (which) {
    case 0: for (int i = 0; i < 3; i++) out[i] = w->pos[r * 3 + i]; break;
    case 1: for (int i = 0; i < 3; i++) out[i] = w->vel[r * 3 + i]; break;
    case 2: out[0] = w->facing[r]; break;
    default: out[0] = w->age[r]; break;
    }
    return 1;
}

/* which: 0 position, 1 velocity (3 doubles); 2 facing (v[0]). The age is the kernel's: setting it is refused.
 * Setting the position or velocity frees a flyer stopped by a block (EN_F_STUCK). */
static inline void en_set(EnWorld *w, int64_t id, int which, const double v[3]) {
    int r = en_row(w, id);
    if (r < 0) { en_refuse(w, EN_R_ENTITY); return; }
    if (which < 0 || which > 2) { en_refuse(w, EN_R_WHICH); return; }
    switch (which) {
    case 0:
        if (!isfinite(v[0]) || !isfinite(v[1]) || !isfinite(v[2])) { en_refuse(w, EN_R_VALUE); return; }
        for (int i = 0; i < 3; i++) w->pos[r * 3 + i] = v[i];
        w->flags[r] &= (uint8_t)~EN_F_STUCK;
        break;
    case 1:
        if (!en_float_ok(v[0]) || !en_float_ok(v[1]) || !en_float_ok(v[2])) { en_refuse(w, EN_R_VALUE); return; }
        for (int i = 0; i < 3; i++) w->vel[r * 3 + i] = (float)v[i];
        w->flags[r] &= (uint8_t)~EN_F_STUCK;
        break;
    default:
        if (!en_float_ok(v[0])) { en_refuse(w, EN_R_VALUE); return; }
        w->facing[r] = (float)v[0];
        break;
    }
}

/* ---- slots and parts ---- */
static inline double en_float(const EnWorld *w, int64_t id, int n) {
    int r = en_row(w, id);
    if (r < 0 || n < 0 || n >= w->kinds[w->kind[r]].ints[EN_K_FSLOTS]) return 0;
    return w->fslot[(size_t)r * EN_SLOTS + n];
}
static inline void en_set_float(EnWorld *w, int64_t id, int n, double v) {
    int r = en_row(w, id);
    if (r < 0) { en_refuse(w, EN_R_ENTITY); return; }
    if (n < 0 || n >= w->kinds[w->kind[r]].ints[EN_K_FSLOTS]) { en_refuse(w, EN_R_SLOTS); return; }
    if (!en_float_ok(v)) { en_refuse(w, EN_R_VALUE); return; }
    w->fslot[(size_t)r * EN_SLOTS + n] = (float)v;
}
static inline int64_t en_int(const EnWorld *w, int64_t id, int n) {
    int r = en_row(w, id);
    if (r < 0 || n < 0 || n >= w->kinds[w->kind[r]].ints[EN_K_ISLOTS]) return 0;
    return w->islot[(size_t)r * EN_SLOTS + n];
}
static inline void en_set_int(EnWorld *w, int64_t id, int n, int64_t v) {
    int r = en_row(w, id);
    if (r < 0) { en_refuse(w, EN_R_ENTITY); return; }
    if (n < 0 || n >= w->kinds[w->kind[r]].ints[EN_K_ISLOTS]) { en_refuse(w, EN_R_SLOTS); return; }
    w->islot[(size_t)r * EN_SLOTS + n] = v;
}

/* part: 0 figure, 1 body, 2 voice 0, 3 voice 1; -1 for none */
static inline int64_t en_part(const EnWorld *w, int64_t id, int part) {
    int r = en_row(w, id);
    if (r < 0 || part < 0 || part >= EN_PARTS) return -1;
    return w->part[(size_t)r * EN_PARTS + part];
}
/* refused for a part the kind does not have (no figure, no body, fewer voices); v is an id >= 0 or -1 */
static inline void en_set_part(EnWorld *w, int64_t id, int part, int64_t v) {
    int r = en_row(w, id);
    if (r < 0) { en_refuse(w, EN_R_ENTITY); return; }
    if (part < 0 || part >= EN_PARTS) { en_refuse(w, EN_R_PART); return; }
    const int32_t *ki = w->kinds[w->kind[r]].ints;
    int has = part == 0 ? ki[EN_K_FIGURE] : part == 1 ? ki[EN_K_BODY] : part - 2 < ki[EN_K_VOICES];
    if (!has) { en_refuse(w, EN_R_PART); return; }
    if (v < -1) { en_refuse(w, EN_R_VALUE); return; }
    w->part[(size_t)r * EN_PARTS + part] = v;
}

/* ---- each and columns ---- */
/* the live ids of a kind in dense order, at most max; how many written */
static inline int en_each(const EnWorld *w, int kind, int max, int64_t *ids) {
    if (kind < 0 || kind >= w->nkinds || !ids) return 0;
    int n = 0;
    const int32_t *dense = w->dense + (size_t)kind * w->capacity;
    for (int live = w->kinds[kind].live; n < live && n < max; n++) ids[n] = en_id(w, dense[n]);
    return n;
}

/* floats a row of column col of kind: 0 position, 1 velocity (3), 2 facing, 10+n float slot n (1); 0 for none */
static inline int en_column_width(const EnWorld *w, int kind, int col) {
    if (col == 0 || col == 1) return 3;
    if (col == 2) return 1;
    if (col >= 10 && col - 10 < w->kinds[kind].ints[EN_K_FSLOTS]) return 1;
    return 0;
}

/* a whole column of one kind, in en_each order, at most max_rows rows; how many rows written */
static inline int en_column(const EnWorld *w, int kind, int col, int max_rows, float *out) {
    if (kind < 0 || kind >= w->nkinds || !out) return 0;
    int width = en_column_width(w, kind, col);
    if (width == 0) return 0;
    int n = 0;
    const int32_t *dense = w->dense + (size_t)kind * w->capacity;
    for (int live = w->kinds[kind].live; n < live && n < max_rows; n++) {
        int r = dense[n];
        float *o = out + (size_t)n * width;
        switch (col) {
        case 0: for (int i = 0; i < 3; i++) o[i] = (float)w->pos[r * 3 + i]; break;
        case 1: for (int i = 0; i < 3; i++) o[i] = w->vel[r * 3 + i]; break;
        case 2: o[0] = w->facing[r]; break;
        default: o[0] = w->fslot[(size_t)r * EN_SLOTS + (col - 10)]; break;
        }
    }
    return n;
}

/* writes a whole column of one kind in en_each order; rows must equal the kind's live count, and every value be finite.
 * A position or velocity column frees the kind's flyers stopped by a block, as en_set does. */
static inline void en_set_column(EnWorld *w, int kind, int col, int rows, const float *in) {
    if (kind < 0 || kind >= w->nkinds) { en_refuse(w, EN_R_KIND); return; }
    int width = en_column_width(w, kind, col);
    if (width == 0) { en_refuse(w, EN_R_WHICH); return; }
    if (rows != w->kinds[kind].live) { en_refuse(w, EN_R_ROWS); return; }
    if (rows > 0 && !in) { en_refuse(w, EN_R_VALUE); return; }
    for (size_t i = 0; i < (size_t)rows * width; i++) if (!isfinite(in[i])) { en_refuse(w, EN_R_VALUE); return; }
    const int32_t *dense = w->dense + (size_t)kind * w->capacity;
    for (int n = 0; n < rows; n++) {
        int r = dense[n];
        const float *v = in + (size_t)n * width;
        if (col <= 1) w->flags[r] &= (uint8_t)~EN_F_STUCK;
        switch (col) {
        case 0: for (int i = 0; i < 3; i++) w->pos[r * 3 + i] = v[i]; break;
        case 1: for (int i = 0; i < 3; i++) w->vel[r * 3 + i] = v[i]; break;
        case 2: w->facing[r] = v[0]; break;
        default: w->fslot[(size_t)r * EN_SLOTS + (col - 10)] = v[0]; break;
        }
    }
}

/* ---- hash and stats ---- */
static inline uint64_t en_fnv(uint64_t h, const void *data, size_t len) {
    const unsigned char *p = (const unsigned char *)data;
    for (size_t i = 0; i < len; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

/* FNV-1a over each live row, kind by kind in en_each order: id, kind, position, velocity, facing, age,
 * the kind's slots, the parts and whether a block has stopped it. Routes join it when routes exist. */
static inline uint64_t en_hash(const EnWorld *w) {
    uint64_t h = 1469598103934665603ull;
    for (int k = 0; k < w->nkinds; k++) {
        const EnKind *kd = &w->kinds[k];
        const int32_t *dense = w->dense + (size_t)k * w->capacity;
        for (int i = 0; i < kd->live; i++) {
            int r = dense[i];
            int64_t id = en_id(w, r);
            h = en_fnv(h, &id, sizeof id);
            h = en_fnv(h, &w->kind[r], sizeof w->kind[r]);
            h = en_fnv(h, w->pos + r * 3, 3 * sizeof *w->pos);
            h = en_fnv(h, w->vel + r * 3, 3 * sizeof *w->vel);
            h = en_fnv(h, w->facing + r, sizeof *w->facing);
            h = en_fnv(h, w->age + r, sizeof *w->age);
            h = en_fnv(h, w->fslot + (size_t)r * EN_SLOTS, (size_t)kd->ints[EN_K_FSLOTS] * sizeof *w->fslot);
            h = en_fnv(h, w->islot + (size_t)r * EN_SLOTS, (size_t)kd->ints[EN_K_ISLOTS] * sizeof *w->islot);
            h = en_fnv(h, w->part + (size_t)r * EN_PARTS, EN_PARTS * sizeof *w->part);
            uint8_t stuck = w->flags[r] & EN_F_STUCK;
            h = en_fnv(h, &stuck, 1);
        }
    }
    return h;
}

/* 0 live, 1 seen in the last tick, 2 waiting for ground, 3 refused calls, 4 ticks given a dt negative or not
 * finite, 5 events dropped (past EN_EVENTS in a tick, or removals queued between ticks), 6 last refusal reason
 * (EN_R_*), 7 entities over the body budget. Grounding and bodies come in later tasks; until then 2 and 7 read 0. */
static inline double en_stat(const EnWorld *w, int which) {
    switch (which) {
    case 0: return w->live;
    case 1: return w->seen;
    case 3: return w->refused;
    case 4: return w->bad_dt;
    case 5: return w->dropped;
    case 6: return w->reason;
    default: return 0;
    }
}

/* ---- blocks ---- */
/* The kernel's copy of the loaded blocks: per chunk, one bit a voxel, set where the block stops an entity
 * as its whole cell. A chunk all one way is held as a state, with no mask. The chunks sit in an
 * open-addressing table keyed by chunk coordinates, grown (doubled) when live and deleted slots pass half.
 * The block table says which ids stop; send it before the chunks, since a chunk's ids are not kept. */

/* n ids, collides[id] nonzero where the id stops; ids from n on pass. Refused (nothing changed) for n outside
 * 0..EN_BLOCK_IDS or no table. */
static inline void en_block_table(EnWorld *w, int n, const uint8_t *collides) {
    if (n < 0 || n > EN_BLOCK_IDS || (n > 0 && !collides)) { en_refuse(w, EN_R_VALUE); return; }
    memset(w->stops, 0, EN_BLOCK_IDS);
    for (int i = 0; i < n; i++) w->stops[i] = collides[i] != 0;
}

static inline int en_chunk_coord_ok(int cx, int cy, int cz) {
    return abs(cx) <= EN_CHUNK_COORD_MAX && abs(cy) <= EN_CHUNK_COORD_MAX && abs(cz) <= EN_CHUNK_COORD_MAX;
}
static inline uint32_t en_chunk_hash(int cx, int cy, int cz) {
    uint32_t h = (uint32_t)cx * 73856093u ^ (uint32_t)cy * 19349663u ^ (uint32_t)cz * 83492791u;
    h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15;
    return h;
}
/* the held chunk, or NULL */
static inline const EnChunk *en_chunk_find(const EnWorld *w, int cx, int cy, int cz) {
    uint32_t m = (uint32_t)w->chunk_cap - 1;
    for (uint32_t i = en_chunk_hash(cx, cy, cz) & m;; i = (i + 1) & m) {
        const EnChunk *c = &w->chunks[i];
        if (c->state == 0) return NULL;
        if (c->state >= 2 && c->cx == cx && c->cy == cy && c->cz == cz) return c;
    }
}
/* rehash into cap slots (a power of 2); 0 if memory ran out (the table is unchanged) */
static inline int en_chunk_rehash(EnWorld *w, int cap) {
    EnChunk *t = (EnChunk *)calloc((size_t)cap, sizeof *t);
    if (!t) return 0;
    uint32_t m = (uint32_t)cap - 1;
    for (int j = 0; j < w->chunk_cap; j++) {
        EnChunk *c = &w->chunks[j];
        if (c->state < 2) continue;
        uint32_t i = en_chunk_hash(c->cx, c->cy, c->cz) & m;
        while (t[i].state != 0) i = (i + 1) & m;
        t[i] = *c;
    }
    free(w->chunks);
    w->chunks = t; w->chunk_cap = cap; w->chunk_dead = 0;
    return 1;
}
/* hold chunk (cx, cy, cz) as state 2..4, taking ownership of mask (state 4), replacing any held there */
static inline void en_chunk_put(EnWorld *w, int cx, int cy, int cz, int state, uint8_t *mask) {
    if ((w->chunk_used + w->chunk_dead + 1) * 2 > w->chunk_cap) {
        int cap = (w->chunk_used + 1) * 4 > w->chunk_cap ? w->chunk_cap * 2 : w->chunk_cap;
        if (cap > (1 << 28) || !en_chunk_rehash(w, cap)) { free(mask); en_refuse(w, EN_R_FULL); return; }
    }
    uint32_t m = (uint32_t)w->chunk_cap - 1;
    EnChunk *slot = NULL;
    for (uint32_t i = en_chunk_hash(cx, cy, cz) & m;; i = (i + 1) & m) {
        EnChunk *c = &w->chunks[i];
        if (c->state >= 2 && c->cx == cx && c->cy == cy && c->cz == cz) { free(c->mask); slot = c; break; }
        if (c->state == 1 && !slot) slot = c;
        if (c->state == 0) {
            if (!slot) slot = c;
            if (slot->state == 1) w->chunk_dead--;
            w->chunk_used++;
            break;
        }
    }
    slot->cx = cx; slot->cy = cy; slot->cz = cz; slot->state = state; slot->mask = mask;
}

/* hold chunk (cx, cy, cz) from its 32,768 block ids in Coord.index order */
static inline void en_chunk(EnWorld *w, int cx, int cy, int cz, const int32_t *ids) {
    if (!ids || !en_chunk_coord_ok(cx, cy, cz)) { en_refuse(w, EN_R_VALUE); return; }
    int solid = 0;
    for (int i = 0; i < 32768; i++) { int32_t id = ids[i]; solid += id >= 0 && id < EN_BLOCK_IDS && w->stops[id]; }
    if (solid == 0 || solid == 32768) { en_chunk_put(w, cx, cy, cz, solid ? 3 : 2, NULL); return; }
    uint8_t *mask = (uint8_t *)calloc(4096, 1);
    if (!mask) { en_refuse(w, EN_R_FULL); return; }
    for (int i = 0; i < 32768; i++) {
        int32_t id = ids[i];
        if (id >= 0 && id < EN_BLOCK_IDS && w->stops[id]) mask[i >> 3] |= (uint8_t)(1u << (i & 7));
    }
    en_chunk_put(w, cx, cy, cz, 4, mask);
}
/* hold chunk (cx, cy, cz) as all block id */
static inline void en_chunk_uniform(EnWorld *w, int cx, int cy, int cz, int id) {
    if (!en_chunk_coord_ok(cx, cy, cz)) { en_refuse(w, EN_R_VALUE); return; }
    en_chunk_put(w, cx, cy, cz, id >= 0 && id < EN_BLOCK_IDS && w->stops[id] ? 3 : 2, NULL);
}
/* stop holding chunk (cx, cy, cz); nothing if it is not held */
static inline void en_chunk_drop(EnWorld *w, int cx, int cy, int cz) {
    if (!en_chunk_coord_ok(cx, cy, cz)) return;
    EnChunk *c = (EnChunk *)en_chunk_find(w, cx, cy, cz);
    if (!c) return;
    free(c->mask);
    c->mask = NULL; c->state = 1;
    w->chunk_used--; w->chunk_dead++;
}

static inline int en_floor32(int v) { return (v - (v & 31)) / 32; }   /* floor(v / 32), any sign */
/* the voxel in a held chunk: 1 solid, 0 clear */
static inline int en_chunk_bit(const EnChunk *c, int vx, int vy, int vz) {
    if (c->state != 4) return c->state == 3;
    int i = (vx & 31) + 32 * ((vz & 31) + 32 * (vy & 31));
    return (c->mask[i >> 3] >> (i & 7)) & 1;
}
/* 1 solid, 0 clear, -1 no chunk held there */
static inline int en_solid(const EnWorld *w, int vx, int vy, int vz) {
    const EnChunk *c = en_chunk_find(w, en_floor32(vx), en_floor32(vy), en_floor32(vz));
    return c ? en_chunk_bit(c, vx, vy, vz) : -1;
}

/* ---- the ray ---- */
/* Strata.Query.raycast's walk, step for step, in doubles: voxels in voxel units (metres / voxel_metres), the
 * face entered by (0 +y, 1 -y, 2 +x, 3 -x, 4 +z, 5 -z), the face facing back along the strongest axis for a
 * ray starting inside a solid voxel. Products are not fused (FP_CONTRACT off) so ties break as in March. */
#pragma STDC FP_CONTRACT OFF
static inline int en_ray_face(int axis, double u) {
    return axis == 0 ? (u > 0 ? 3 : 2) : axis == 1 ? (u > 0 ? 1 : 0) : (u > 0 ? 5 : 4);
}
static inline double en_ray_cross(double p, double u, int v) {
    if (fabs(u) < 0.000000000001) return 1e30;
    return u > 0 ? ((double)(v + 1) - p) / u : ((double)v - p) / u;
}

/* the first solid voxel along the ray from o (metres) along d (any length) for at most max metres. out: voxel
 * x, y, z and the face; *t the distance in metres. 1 hit, 0 miss (also a zero direction, max <= 0, a number
 * not finite), -1 the ray entered a chunk not held before hitting anything. */
static inline int en_ray(const EnWorld *w, const double o[3], const double d[3], double max, int32_t out[4], double *t) {
    double sc = w->voxel_metres;
    double len = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!isfinite(len) || !isfinite(max) || !isfinite(o[0]) || !isfinite(o[1]) || !isfinite(o[2])) return 0;
    if (len < 0.000000000001 || max <= 0) return 0;
    double ux = d[0] / len, uy = d[1] / len, uz = d[2] / len;
    double px = o[0] / sc, py = o[1] / sc, pz = o[2] / sc;
    double lim = (double)EN_CHUNK_COORD_MAX * 32;
    if (fabs(px) >= lim || fabs(py) >= lim || fabs(pz) >= lim) return -1;   /* no chunk is held out there */
    int vx = (int)floor(px), vy = (int)floor(py), vz = (int)floor(pz);
    double tmax = max / sc;
    double nx = en_ray_cross(px, ux, vx), ny = en_ray_cross(py, uy, vy), nz = en_ray_cross(pz, uz, vz);
    double ax = fabs(ux), ay = fabs(uy), az = fabs(uz);
    int face = ax >= ay && ax >= az ? en_ray_face(0, ux) : ay >= az ? en_ray_face(1, uy) : en_ray_face(2, uz);
    double tv = 0;
    double fsteps = 3 * floor(tmax) + 8;
    int64_t steps = fsteps > 4e18 ? INT64_MAX : (int64_t)fsteps;
    const EnChunk *c = NULL; int ccx = 0, ccy = 0, ccz = 0;
    for (;;) {
        if (tv > tmax || steps <= 0) return 0;
        int kx = en_floor32(vx), ky = en_floor32(vy), kz = en_floor32(vz);
        if (!c || kx != ccx || ky != ccy || kz != ccz) {
            c = en_chunk_find(w, kx, ky, kz); ccx = kx; ccy = ky; ccz = kz;
            if (!c) return -1;
        }
        if (en_chunk_bit(c, vx, vy, vz)) {
            out[0] = vx; out[1] = vy; out[2] = vz; out[3] = face;
            *t = tv * sc;
            return 1;
        }
        if (nx <= ny && nx <= nz) { vx += ux > 0 ? 1 : -1; tv = nx; face = en_ray_face(0, ux); nx = nx + fabs(1.0 / ux); }
        else if (ny <= nz) { vy += uy > 0 ? 1 : -1; tv = ny; face = en_ray_face(1, uy); ny = ny + fabs(1.0 / uy); }
        else { vz += uz > 0 ? 1 : -1; tv = nz; face = en_ray_face(2, uz); nz = nz + fabs(1.0 / uz); }
        steps--;
    }
}
#pragma STDC FP_CONTRACT DEFAULT

/* ---- events ---- */
/* The events of the last en_tick, in the order written: first the removals made since the tick before, then
 * the tick's own. At most EN_EVENTS a tick; the rest are dropped and counted (en_stat(5)). */
static inline void en_event(EnWorld *w, int code, int64_t a, int64_t b, int64_t i0, int64_t i1, int64_t i2, int64_t i3,
                            double x, double y, double z) {
    if (w->nev >= EN_EVENTS) { w->dropped += 1; return; }
    int64_t *e = w->ev_ints + (size_t)w->nev * 7;
    double *f = w->ev_floats + (size_t)w->nev * 3;
    e[0] = code; e[1] = a; e[2] = b; e[3] = i0; e[4] = i1; e[5] = i2; e[6] = i3;
    f[0] = x; f[1] = y; f[2] = z;
    w->nev++;
}
/* copies at most max events: ints 7 each (code, a, b, i0, i1, i2, i3), floats 3 each (x, y, z); how many copied.
 * With ints or floats NULL, copies nothing and answers how many the last tick wrote. */
static inline int en_events(const EnWorld *w, int max, int64_t *ints, double *floats) {
    if (!ints || !floats) return w->nev;
    int n = max < w->nev ? (max < 0 ? 0 : max) : w->nev;
    memcpy(ints, w->ev_ints, (size_t)n * 7 * sizeof *ints);
    memcpy(floats, w->ev_floats, (size_t)n * 3 * sizeof *floats);
    return n;
}

/* ---- the tick ---- */
/* seen: within the kind's seen distance of any focus, measured horizontally (x and z) */
static inline int en_seen_by(const double *p, double dist, int nfocus, const double *focus) {
    double dd = dist * dist;
    for (int f = 0; f < nfocus; f++) {
        double dx = p[0] - focus[f * 3], dz = p[2] - focus[f * 3 + 2];
        if (dx * dx + dz * dz <= dd) return 1;
    }
    return 0;
}

/* one flying step of row r over dt (> 0): gravity into the velocity, then the position by the velocity; while
 * seen, a ray along the step, and on a hit the entity stops at the hit point, still, with event 2 */
static inline void en_fly(EnWorld *w, int r, const EnKind *kd, double dt) {
    float *v = w->vel + r * 3;
    double *p = w->pos + r * 3;
    if (kd->ints[EN_K_GRAVITY]) {
        double vy = v[1] - kd->floats[EN_K_GRAVITY_MS2] * dt;
        if (en_float_ok(vy)) v[1] = (float)vy;
    }
    double d[3] = {v[0], v[1], v[2]};
    if (w->flags[r] & EN_F_SEEN) {
        double len = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        int32_t hit[4]; double t;
        if (en_ray(w, p, d, len * dt, hit, &t) == 1) {
            for (int i = 0; i < 3; i++) { p[i] = p[i] + d[i] / len * t; v[i] = 0; }
            w->flags[r] |= EN_F_STUCK;
            en_event(w, EN_E_BLOCK, en_id(w, r), 0, hit[0], hit[1], hit[2], hit[3], p[0], p[1], p[2]);
            return;
        }
    }
    double q[3] = {p[0] + d[0] * dt, p[1] + d[1] * dt, p[2] + d[2] * dt};
    if (isfinite(q[0]) && isfinite(q[1]) && isfinite(q[2])) { p[0] = q[0]; p[1] = q[1]; p[2] = q[2]; }
}

/* One step of dt seconds. Clears the last tick's events, writes the removals queued since, then walks each
 * kind's dense array: the age advances by dt; an entity whose age passes its kind's lifetime is removed
 * (reason 1, event 6); the seen flag is set from the focus points (nfocus of them, 3 doubles each; none: all
 * unseen); flying entities move (en_fly), placed ones do not. A flyer stopped by a block stays until the game
 * sets its position or velocity. dt negative or not finite is counted (en_stat(4)) and taken as 0. Entities
 * added between ticks take part from the next tick: every live entity does. */
static inline void en_tick(EnWorld *w, int nfocus, const double *focus, double dt) {
    if (!isfinite(dt) || dt < 0) { w->bad_dt += 1; dt = 0; }
    if (nfocus < 0 || !focus) nfocus = 0;
    w->nev = 0;
    for (int i = 0; i < w->nremoved; i++) {
        const EnRemoval *q = &w->removed[i];
        en_event(w, EN_E_REMOVED, q->id, 0, q->reason, 0, 0, 0, q->pos[0], q->pos[1], q->pos[2]);
    }
    w->nremoved = 0;
    w->in_tick = 1;
    int seen = 0;
    for (int k = 0; k < w->nkinds; k++) {
        const EnKind *kd = &w->kinds[k];
        const int32_t *dense = w->dense + (size_t)k * w->capacity;
        double life = kd->floats[EN_K_LIFETIME];
        for (int i = 0; i < kd->live;) {   /* a removal moves the kind's last entry into i: look at i again */
            int r = dense[i];
            w->age[r] += dt;
            if (life > 0 && w->age[r] > life) { en_remove(w, en_id(w, r), 1); continue; }
            int s = en_seen_by(w->pos + r * 3, kd->floats[EN_K_SEEN], nfocus, focus);
            w->flags[r] = (uint8_t)((w->flags[r] & ~EN_F_SEEN) | (s ? EN_F_SEEN : 0));
            seen += s;
            if (kd->ints[EN_K_MOVER] == 1 && dt > 0 && !(w->flags[r] & EN_F_STUCK)) en_fly(w, r, kd, dt);
            i++;
        }
    }
    w->seen = seen;
    w->in_tick = 0;
}

#endif

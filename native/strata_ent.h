/* strata_ent.h — Strata's entity kernel: kinds, rows, ids, slots, parts and
 * whole columns. Plain C: no Vulkan, no GLFW, no physics or audio, no
 * globals, so it is tested headless (native/tests/ent_test.c).
 *
 * Threads: one. Every call on an EnWorld runs on the same thread, in order.
 * Nothing here allocates after en_open except the block masks: 4,096 bytes
 * for each held chunk that is neither all solid nor all clear, the chunk
 * table, which doubles as it fills (section "blocks"), and the copy of the
 * focus points, which grows to the most a tick has been given (section "parts
 * of seen entities"), and a restore's check, which borrows a scratch array for
 * the length of the call (strata_ent_snap.h).
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
 * the ray; events; the grid, near, the entity ray and touch pairs (in
 * strata_ent_grid.h, included here); the sweep, walking and routes (in
 * strata_ent_move.h, included here); seen and unseen, and setting on the
 * ground (in strata_ent_seen.h, included here); the tick; parts of seen
 * entities (what the game gives a figure or voices, and places each tick);
 * snapshot and restore (in strata_ent_snap.h, included last). */
#ifndef STRATA_ENT_H
#define STRATA_ENT_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

/* No product is fused into an add anywhere in the entity headers (strata_ent.h, strata_ent_grid.h): the ray must
 * break ties as Strata.Query does in March, and equal calls must give equal hashes whatever the compiler's default.
 * Never switched back on here; it stays off for the rest of a file that includes this one. */
#pragma STDC FP_CONTRACT OFF

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
#define EN_CELL 4.0                /* the grid's cell, metres */
#define EN_CELL_MAX 1000000000.0   /* |grid cell coordinate| at most; farther positions share the last cell */
#define EN_ROUTE_MAX 256           /* points a route at most */

/* a row's flags */
/* seen (strata_ent_seen.h); a flyer that hit a block, still until the game moves it; a walker standing on something; a
 * follower whose route loops; a follower that has reported event 8 for its present stall; an unseen walker waiting to
 * be set on the ground; an unseen entity frozen by its kind's rule; a seen entity whose body was lost (en_body_lost),
 * wanting no other until it has left view */
enum { EN_F_SEEN = 1, EN_F_STUCK = 2, EN_F_GROUNDED = 4, EN_F_LOOPS = 8, EN_F_BLOCKED = 16, EN_F_WAITING = 32, EN_F_FROZEN = 64,
       EN_F_LOST = 128 };
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
    EN_R_ROWS = 12,     /* en_set_column: rows not the kind's live count */
    EN_R_ROUTE = 13,    /* no such route, or one freed (strata_ent_move.h) */
    EN_R_SNAPSHOT = 14  /* a snapshot that does not fit the world (strata_ent_snap.h) */
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

/* an entity in the grid, as the last tick left it (strata_ent_grid.h) */
typedef struct {
    int64_t id;
    double p[3];                 /* position */
    double lo[3], hi[3];         /* touch box: the box swept over the tick's step */
    int32_t cell[3], kind;
} EnGridEnt;

/* a route: its points are pool[start .. start + n); state 0 a free slot, 1 held, 2 freed but still followed */
typedef struct { int32_t start, n, users, state; } EnRoute;

/* a removal made between ticks, written as event 6 at the start of the next tick */
typedef struct { int64_t id; int32_t reason; double pos[3]; } EnRemoval;

/* an entity wanting a part: its horizontal distance squared to the nearest focus, and its id (section "parts of seen
 * entities") */
typedef struct { double d2; int64_t id; } EnWant;

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
    double bad_dt, dropped; int seen, waiting;   /* seen and waiting for ground at the end of the last tick */
    /* the grid (strata_ent_grid.h) */
    double *from;                /* 3 a row: the position the tick's step began at */
    EnGridEnt *g_ent;            /* capacity: the last tick's live entities, bucket by bucket */
    int32_t *g_start;            /* g_nbmax + 1: bucket b is g_ent[g_start[b] .. g_start[b+1]) */
    int32_t *g_bucket;           /* capacity: each entry's bucket, while building */
    int32_t *g_byk;              /* capacity: kind by kind in dense order, each entry's index in g_ent */
    int32_t g_kstart[EN_KINDS + 1];   /* kind k's entries are g_byk[g_kstart[k] .. g_kstart[k+1]) */
    double g_reach[EN_KINDS][3]; /* how far any of the kind's touch boxes reaches from its position, a side */
    int g_n, g_nb, g_nbmax;      /* entries, buckets this tick (a power of 2), buckets at most */
    uint64_t pairs[EN_KINDS];    /* bit j of pairs[i], i <= j: touches between kinds i and j are asked for */
    /* routes and walking (strata_ent_move.h) */
    double *rpts;                /* route_points x 3: the point pool */
    EnRoute *routes;             /* route_points: the route table (a route holds a point, so never more) */
    int32_t *route_free; int nroute_free, routes_hw;   /* freed route ids, a stack; ids handed out so far */
    int32_t *fx_start, *fx_len; int nfx;   /* the pool's free blocks, in order of start (route_points + 1) */
    int route_pts_held, routes_held;
    int32_t *rt_route;           /* a row: the route it follows, -1 none */
    int32_t *rt_next;            /* a row: the index of its next waypoint */
    float *rt_speed;             /* a row: metres a second along its route */
    float *rt_best, *rt_stall;   /* a row: distance to the waypoint at the last progress, seconds since */
    int64_t tick_count;          /* ticks begun; spreads divisor stepping across ticks */
    /* parts */
    int32_t *rel_code; int64_t *rel_id; int nrel;   /* capacity x EN_PARTS: (part, id) pairs cleared and not yet drained */
    double fig_refused[EN_KINDS];   /* figures the game could not make for each kind (en_figure_refused) */
    int body_over;               /* entities wanting a body left over by the game's last tick (en_body_over) */
    double *focus; int nfocus, focus_cap;   /* the last tick's focus points, 3 doubles each */
    EnWant *want;                /* capacity: en_wanting's scratch */
};
typedef struct EnWorld EnWorld;

static inline int en_refuse(EnWorld *w, int reason) { w->refused += 1; w->reason = reason; return -1; }
static inline int en_float_ok(double v) { return isfinite(v) && fabs(v) <= FLT_MAX; }   /* finite once stored as a float */

static inline void en_close(EnWorld *w) {
    if (!w) return;
    free(w->kind); free(w->gen); free(w->dense_at); free(w->dense); free(w->pos); free(w->vel);
    free(w->facing); free(w->age); free(w->fslot); free(w->islot); free(w->part); free(w->free_rows);
    free(w->flags); free(w->stops); free(w->ev_ints); free(w->ev_floats); free(w->removed);
    free(w->from); free(w->g_ent); free(w->g_start); free(w->g_bucket); free(w->g_byk);
    free(w->rpts); free(w->routes); free(w->route_free); free(w->fx_start); free(w->fx_len);
    free(w->rt_route); free(w->rt_next); free(w->rt_speed); free(w->rt_best); free(w->rt_stall);
    free(w->rel_code); free(w->rel_id); free(w->focus); free(w->want);
    if (w->chunks) for (int i = 0; i < w->chunk_cap; i++) free(w->chunks[i].mask);
    free(w->chunks);
    free(w);
}

/* capacity rows, a pool of route_points route points, a body budget, the seen cap and the chunk and voxel sizes in
 * metres (bodies, seen_cap and chunk_metres are kept for later tasks). NULL if any is out of range or memory runs out. */
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
    w->g_nbmax = 16;
    while (w->g_nbmax < 2 * capacity) w->g_nbmax <<= 1;
    w->from = (double *)calloc(n * 3, sizeof *w->from);
    w->g_ent = (EnGridEnt *)malloc(n * sizeof *w->g_ent);
    w->g_start = (int32_t *)calloc((size_t)w->g_nbmax + 1, sizeof *w->g_start);
    w->g_bucket = (int32_t *)malloc(n * sizeof *w->g_bucket);
    w->g_byk = (int32_t *)malloc(n * sizeof *w->g_byk);
    size_t rp = (size_t)route_points;
    w->rpts = (double *)calloc(rp * 3, sizeof *w->rpts);
    w->routes = (EnRoute *)calloc(rp, sizeof *w->routes);
    w->route_free = (int32_t *)malloc(rp * sizeof *w->route_free);
    w->fx_start = (int32_t *)malloc((rp + 1) * sizeof *w->fx_start);
    w->fx_len = (int32_t *)malloc((rp + 1) * sizeof *w->fx_len);
    w->rt_route = (int32_t *)malloc(n * sizeof *w->rt_route);
    w->rt_next = (int32_t *)calloc(n, sizeof *w->rt_next);
    w->rt_speed = (float *)calloc(n, sizeof *w->rt_speed);
    w->rt_best = (float *)calloc(n, sizeof *w->rt_best);
    w->rt_stall = (float *)calloc(n, sizeof *w->rt_stall);
    w->rel_code = (int32_t *)malloc(n * EN_PARTS * sizeof *w->rel_code);
    w->rel_id = (int64_t *)malloc(n * EN_PARTS * sizeof *w->rel_id);
    w->want = (EnWant *)malloc(n * sizeof *w->want);
    if (!w->kind || !w->gen || !w->dense_at || !w->dense || !w->pos || !w->vel || !w->facing || !w->age ||
        !w->fslot || !w->islot || !w->part || !w->free_rows || !w->flags || !w->stops || !w->chunks ||
        !w->ev_ints || !w->ev_floats || !w->removed || !w->from || !w->g_ent || !w->g_start || !w->g_bucket ||
        !w->g_byk || !w->rpts || !w->routes || !w->route_free || !w->fx_start || !w->fx_len || !w->rt_route ||
        !w->rt_next || !w->rt_speed || !w->rt_best || !w->rt_stall || !w->rel_code || !w->rel_id || !w->want) {
        en_close(w); return NULL;
    }
    w->fx_start[0] = 0; w->fx_len[0] = route_points; w->nfx = 1;
    for (int r = 0; r < capacity; r++) {
        w->kind[r] = -1; w->gen[r] = 1; w->rt_route[r] = -1;
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
static inline void en_unfollow(EnWorld *w, int r);

/* Row r's parts are cleared: each it held (part 0 figure, 1 body, 2 and 3 voices) goes on the release list as (part,
 * id), for the game to free (en_releases); past capacity x EN_PARTS pairs waiting, the rest are dropped and counted
 * in en_stat(5). Called when an entity is removed (any reason) and when it becomes unseen (strata_ent_seen.h). */
static inline void en_release_parts(EnWorld *w, int r) {
    int64_t *pt = w->part + (size_t)r * EN_PARTS;
    for (int p = 0; p < EN_PARTS; p++) {
        if (pt[p] < 0) continue;
        if (w->nrel >= w->capacity * EN_PARTS) w->dropped += 1;
        else { w->rel_code[w->nrel] = p; w->rel_id[w->nrel] = pt[p]; w->nrel++; }
        pt[p] = -1;
    }
}

/* reason: 0 the game, 1 lifetime, 2 the unseen rule. Each removal is reported as event 6 (i0 the reason, the
 * point its last position): one made inside en_tick in that tick's events; one made between ticks is queued
 * (the id and position copied, so the row can be reused at once) and written first by the next en_tick.
 * A follower stops following its route (strata_ent_move.h). Its parts are released (en_release_parts). */
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
    en_unfollow(w, r);
    en_release_parts(w, r);
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

/* Drains the release list: copies at most max (part, id) pairs, oldest first, into codes and ids and takes them off
 * the list; how many copied. With codes or ids NULL, copies nothing and answers how many wait. */
static inline int en_releases(EnWorld *w, int max, int32_t *codes, int64_t *ids) {
    if (!codes || !ids) return w->nrel;
    int n = max < w->nrel ? (max < 0 ? 0 : max) : w->nrel;
    memcpy(codes, w->rel_code, (size_t)n * sizeof *codes);
    memcpy(ids, w->rel_id, (size_t)n * sizeof *ids);
    memmove(w->rel_code, w->rel_code + n, (size_t)(w->nrel - n) * sizeof *w->rel_code);
    memmove(w->rel_id, w->rel_id + n, (size_t)(w->nrel - n) * sizeof *w->rel_id);
    w->nrel -= n;
    return n;
}

/* the game could not make a figure for an entity of kind: counted for the kind in en_stat(10 + kind) */
static inline void en_figure_refused(EnWorld *w, int kind) {
    if (kind < 0 || kind >= w->nkinds) { en_refuse(w, EN_R_KIND); return; }
    w->fig_refused[kind] += 1;
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
 * the kind's slots, the parts, whether a block has stopped it, whether it is grounded, and its route, waypoint,
 * speed, looping and stall; then each held route in id order: id, state, followers and points. */
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
            uint8_t kept = w->flags[r] & (EN_F_STUCK | EN_F_GROUNDED | EN_F_LOOPS | EN_F_BLOCKED);
            h = en_fnv(h, &kept, 1);
            h = en_fnv(h, w->rt_route + r, sizeof *w->rt_route);
            if (w->rt_route[r] >= 0) {
                h = en_fnv(h, w->rt_next + r, sizeof *w->rt_next);
                h = en_fnv(h, w->rt_speed + r, sizeof *w->rt_speed);
                h = en_fnv(h, w->rt_best + r, sizeof *w->rt_best);
                h = en_fnv(h, w->rt_stall + r, sizeof *w->rt_stall);
            }
        }
    }
    for (int i = 0; i < w->routes_hw; i++) {
        const EnRoute *rt = &w->routes[i];
        if (rt->state == 0) continue;
        h = en_fnv(h, &i, sizeof i);
        h = en_fnv(h, &rt->state, sizeof rt->state);
        h = en_fnv(h, &rt->users, sizeof rt->users);
        h = en_fnv(h, w->rpts + (size_t)rt->start * 3, (size_t)rt->n * 3 * sizeof *w->rpts);
    }
    return h;
}

/* 0 live, 1 seen at the end of the last tick, 2 walkers waiting there to be set on the ground (strata_ent_seen.h),
 * 3 refused calls, 4 ticks given a dt negative or not finite, 5 events dropped (past EN_EVENTS in a tick, or removals
 * queued between ticks), 6 last refusal reason (EN_R_*), 7 entities over the body budget, 8 route points held, 9 routes
 * held (a freed route still followed is held), 10 + k figures the game could not make for kind k (en_figure_refused).
 * Releases dropped (en_release_parts) are counted in 5 too. 7 is what the game last said (en_body_over). */
static inline double en_stat(const EnWorld *w, int which) {
    if (which >= 10 && which < 10 + EN_KINDS) return w->fig_refused[which - 10];
    switch (which) {
    case 0: return w->live;
    case 1: return w->seen;
    case 2: return w->waiting;
    case 3: return w->refused;
    case 4: return w->bad_dt;
    case 5: return w->dropped;
    case 6: return w->reason;
    case 7: return w->body_over;
    case 8: return w->route_pts_held;
    case 9: return w->routes_held;
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
 * ray starting inside a solid voxel. Products are not fused (FP_CONTRACT is off for the whole header) so ties break as in March. */
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

#include "strata_ent_grid.h"
#include "strata_ent_move.h"
#include "strata_ent_seen.h"

/* ---- the tick ---- */
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

/* One step of dt seconds, in passes over each kind's dense array, kind by kind:
 *  1. the last tick's events cleared and the removals queued since written (event 6); each entity's age advances
 *     by dt (not a frozen one's) and one whose age passes its kind's lifetime is removed (reason 1, event 6);
 *  2. seen movement: flyers (en_fly; one stopped by a block stays until the game sets its position or velocity) and
 *     walkers (en_walk); placed entities do not move;
 *  3. unseen movement due this tick: an unseen entity of a kind with divisor n steps on the ticks where
 *     (ticks begun before + its row) % n == 0, by n * dt, so a kind's entities are spread across the n ticks;
 *     flyers fly on through everything, walkers follow their routes (en_walk_unseen); frozen entities and walkers
 *     waiting to be set on the ground stay;
 *  4. the seen and unseen switch from the focus points (nfocus of them, 3 doubles each; none: nothing is seen), on
 *     the positions movement left (en_switch, strata_ent_seen.h): events 3 and 4, the unseen rules, grounding;
 *  5. the grid is rebuilt from the rows still live (en_grid_build) and the declared touch pairs tested on it
 *     (en_touches): an entity removed in the tick is in neither.
 * dt negative or not finite is counted (en_stat(4)) and taken as 0, which moves nothing; the switch still runs. An
 * entity added between ticks is unseen and takes part from the next tick, becoming seen at its end if it is near. */
static inline void en_tick(EnWorld *w, int nfocus, const double *focus, double dt) {
    if (!isfinite(dt) || dt < 0) { w->bad_dt += 1; dt = 0; }
    if (nfocus < 0 || !focus) nfocus = 0;
    if (nfocus > w->focus_cap) {   /* kept for en_wanting; on no memory, the first focus_cap are kept */
        double *f = (double *)realloc(w->focus, (size_t)nfocus * 3 * sizeof *f);
        if (f) { w->focus = f; w->focus_cap = nfocus; }
    }
    w->nfocus = nfocus < w->focus_cap ? nfocus : w->focus_cap;
    if (w->nfocus > 0) memcpy(w->focus, focus, (size_t)w->nfocus * 3 * sizeof *focus);
    w->nev = 0;
    for (int i = 0; i < w->nremoved; i++) {
        const EnRemoval *q = &w->removed[i];
        en_event(w, EN_E_REMOVED, q->id, 0, q->reason, 0, 0, 0, q->pos[0], q->pos[1], q->pos[2]);
    }
    w->nremoved = 0;
    w->in_tick = 1;
    for (int k = 0; k < w->nkinds; k++) {
        const EnKind *kd = &w->kinds[k];
        const int32_t *dense = w->dense + (size_t)k * w->capacity;
        double life = kd->floats[EN_K_LIFETIME];
        for (int i = 0; i < kd->live;) {   /* a removal moves the kind's last entry into i: look at i again */
            int r = dense[i];
            if (!(w->flags[r] & EN_F_FROZEN)) {
                w->age[r] += dt;
                if (life > 0 && w->age[r] > life) { en_remove(w, en_id(w, r), 1); continue; }
            }
            memcpy(w->from + r * 3, w->pos + r * 3, 3 * sizeof *w->pos);
            i++;
        }
    }
    if (dt > 0) {
        for (int k = 0; k < w->nkinds; k++) {   /* seen movement */
            const EnKind *kd = &w->kinds[k];
            int mover = kd->ints[EN_K_MOVER];
            if (mover != 1 && mover != 2) continue;
            const int32_t *dense = w->dense + (size_t)k * w->capacity;
            for (int i = 0; i < kd->live; i++) {
                int r = dense[i];
                if (!(w->flags[r] & EN_F_SEEN)) continue;
                if (mover == 2) en_walk(w, r, kd, dt);
                else if (!(w->flags[r] & EN_F_STUCK)) en_fly(w, r, kd, dt);
            }
        }
        for (int k = 0; k < w->nkinds; k++) {   /* unseen movement due this tick */
            const EnKind *kd = &w->kinds[k];
            int mover = kd->ints[EN_K_MOVER], n = kd->ints[EN_K_DIVISOR];
            if (mover != 1 && mover != 2) continue;
            const int32_t *dense = w->dense + (size_t)k * w->capacity;
            int due = (int)(w->tick_count % n);
            for (int i = 0; i < kd->live; i++) {
                int r = dense[i];
                if ((w->flags[r] & (EN_F_SEEN | EN_F_FROZEN | EN_F_WAITING)) || (due + r % n) % n != 0) continue;
                if (mover == 2) en_walk_unseen(w, r, n * dt);
                else if (!(w->flags[r] & EN_F_STUCK)) en_fly(w, r, kd, n * dt);
            }
        }
    }
    w->tick_count++;
    en_switch(w, nfocus, focus);
    en_grid_build(w);
    en_touches(w);
    w->in_tick = 0;
}

/* ---- parts of seen entities ---- */
/* What a game hangs on an entity (a figure, a body, voices) is its own; the kernel keeps the ids (en_set_part), says
 * which seen entities want one and which hold one, and releases them when the entity is removed or leaves view. */

/* 1 if kind k's entities may hold part (0 figure, 1 body, 2 voice 0, 3 voice 1) */
static inline int en_kind_has_part(const EnKind *kd, int part) {
    if (part == 0) return kd->ints[EN_K_FIGURE];
    if (part == 1) return kd->ints[EN_K_BODY];
    return part >= 2 && part < EN_PARTS && part - 2 < kd->ints[EN_K_VOICES];
}

/* The seen entities holding part, kind by kind in dense order: their part ids (as int32) into part_ids and their poses
 * into poses, 7 floats each: the position, then the facing as a turn about +y (0, sin a/2, 0, cos a/2). Writes at most
 * max and answers how many it wrote; with part_ids or poses NULL, answers how many there are. */
static inline int en_seen_parts(const EnWorld *w, int part, int max, int32_t *part_ids, float *poses) {
    if (part < 0 || part >= EN_PARTS) return 0;
    int counting = !part_ids || !poses, n = 0;
    for (int k = 0; k < w->nkinds; k++) {
        const EnKind *kd = &w->kinds[k];
        if (!en_kind_has_part(kd, part)) continue;
        const int32_t *dense = w->dense + (size_t)k * w->capacity;
        for (int i = 0; i < kd->live; i++) {
            int r = dense[i];
            int64_t v = w->part[(size_t)r * EN_PARTS + part];
            if (!(w->flags[r] & EN_F_SEEN) || v < 0) continue;
            if (!counting) {
                if (n >= max) return n;
                const double *p = w->pos + r * 3;
                double h = (double)w->facing[r] * 0.5;
                float *o = poses + (size_t)n * 7;
                part_ids[n] = (int32_t)v;
                o[0] = (float)p[0]; o[1] = (float)p[1]; o[2] = (float)p[2];
                o[3] = 0; o[4] = (float)sin(h); o[5] = 0; o[6] = (float)cos(h);
            }
            n++;
        }
    }
    return n;
}

static inline int en_want_order(const void *a, const void *b) {
    const EnWant *x = (const EnWant *)a, *y = (const EnWant *)b;
    if (x->d2 != y->d2) return x->d2 < y->d2 ? -1 : 1;
    return x->id < y->id ? -1 : x->id > y->id;
}

/* The seen entities whose kind has part but that hold none, nearest a focus of the last tick first (measured
 * horizontally, to the nearest focus), equal distances by id: their ids into ids, at most max; how many written. With ids
 * NULL, answers how many there are. An entity whose body was lost (en_body_lost) does not want another while seen. */
static inline int en_wanting(EnWorld *w, int part, int max, int64_t *ids) {
    if (part < 0 || part >= EN_PARTS) return 0;
    int n = 0;
    for (int k = 0; k < w->nkinds; k++) {
        const EnKind *kd = &w->kinds[k];
        if (!en_kind_has_part(kd, part)) continue;
        const int32_t *dense = w->dense + (size_t)k * w->capacity;
        for (int i = 0; i < kd->live; i++) {
            int r = dense[i];
            if (!(w->flags[r] & EN_F_SEEN) || w->part[(size_t)r * EN_PARTS + part] >= 0) continue;
            if (part == 1 && (w->flags[r] & EN_F_LOST)) continue;   /* its body was lost: none again until it leaves view */
            if (ids) {
                const double *p = w->pos + r * 3;
                double best = INFINITY;
                for (int f = 0; f < w->nfocus; f++) {
                    double dx = p[0] - w->focus[f * 3], dz = p[2] - w->focus[f * 3 + 2], d2 = dx * dx + dz * dz;
                    if (d2 < best) best = d2;
                }
                w->want[n].d2 = best; w->want[n].id = en_id(w, r);
            }
            n++;
        }
    }
    if (!ids) return n;
    qsort(w->want, (size_t)n, sizeof *w->want, en_want_order);
    int m = max < n ? (max < 0 ? 0 : max) : n;
    for (int i = 0; i < m; i++) ids[i] = w->want[i].id;
    return m;
}

/* The seen entities holding part, kind by kind in dense order: their entity ids into ids, at most max; how many
 * written. With ids NULL, answers how many there are. */
static inline int en_holding(const EnWorld *w, int part, int max, int64_t *ids) {
    if (part < 0 || part >= EN_PARTS) return 0;
    int n = 0;
    for (int k = 0; k < w->nkinds; k++) {
        const EnKind *kd = &w->kinds[k];
        if (!en_kind_has_part(kd, part)) continue;
        const int32_t *dense = w->dense + (size_t)k * w->capacity;
        for (int i = 0; i < kd->live; i++) {
            int r = dense[i];
            if (!(w->flags[r] & EN_F_SEEN) || w->part[(size_t)r * EN_PARTS + part] < 0) continue;
            if (ids) { if (n >= max) return n; ids[n] = en_id(w, r); }
            n++;
        }
    }
    return n;
}

/* The game found entity id's body gone (Strata.Physics.exists false: removed, or written into the world as blocks):
 * the body part is cleared without going on the release list (there is nothing left to free) and event 7 is appended
 * to the last tick's events (a the id, i0 the body's id, the point its position). While it stays seen it wants no
 * other body (en_wanting skips it); leaving view clears that. Refused for a dead id (EN_R_ENTITY) or one holding no
 * body (EN_R_PART). */
static inline void en_body_lost(EnWorld *w, int64_t id) {
    int r = en_row(w, id);
    if (r < 0) { en_refuse(w, EN_R_ENTITY); return; }
    int64_t *b = w->part + (size_t)r * EN_PARTS + 1;
    if (*b < 0) { en_refuse(w, EN_R_PART); return; }
    const double *p = w->pos + r * 3;
    en_event(w, EN_E_BODY_LOST, id, 0, *b, 0, 0, 0, p[0], p[1], p[2]);
    *b = -1;
    w->flags[r] |= EN_F_LOST;
}

/* the game's count of entities wanting a body that its budget left without one this tick: en_stat(7). Refused (EN_R_VALUE)
 * below 0. */
static inline void en_body_over(EnWorld *w, int n) {
    if (n < 0) { en_refuse(w, EN_R_VALUE); return; }
    w->body_over = n;
}

#include "strata_ent_snap.h"

#endif

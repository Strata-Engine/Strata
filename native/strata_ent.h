/* strata_ent.h — Strata's entity kernel: kinds, rows, ids, slots, parts and
 * whole columns. Plain C: no Vulkan, no GLFW, no physics or audio, no
 * globals, so it is tested headless (native/tests/ent_test.c).
 *
 * Threads: one. Every call on an EnWorld runs on the same thread, in order.
 * Nothing here allocates after en_open.
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
 * en_stat(3) and the reason (EN_R_*) is kept in en_stat(6). */
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
};
typedef struct EnWorld EnWorld;

static inline int en_refuse(EnWorld *w, int reason) { w->refused += 1; w->reason = reason; return -1; }
static inline int en_float_ok(double v) { return isfinite(v) && fabs(v) <= FLT_MAX; }   /* finite once stored as a float */

static inline void en_close(EnWorld *w) {
    if (!w) return;
    free(w->kind); free(w->gen); free(w->dense_at); free(w->dense); free(w->pos); free(w->vel);
    free(w->facing); free(w->age); free(w->fslot); free(w->islot); free(w->part); free(w->free_rows);
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
    if (!w->kind || !w->gen || !w->dense_at || !w->dense || !w->pos || !w->vel || !w->facing || !w->age ||
        !w->fslot || !w->islot || !w->part || !w->free_rows) { en_close(w); return NULL; }
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
    memset(w->fslot + (size_t)r * EN_SLOTS, 0, EN_SLOTS * sizeof *w->fslot);
    memset(w->islot + (size_t)r * EN_SLOTS, 0, EN_SLOTS * sizeof *w->islot);
    for (int p = 0; p < EN_PARTS; p++) w->part[(size_t)r * EN_PARTS + p] = -1;
    w->live++;
    return en_id(w, r);
}

/* reason: 0 the game, 1 lifetime, 2 the unseen rule (reported with the removal events that come later) */
static inline void en_remove(EnWorld *w, int64_t id, int reason) {
    int r = en_row(w, id);
    if (r < 0) { en_refuse(w, EN_R_ENTITY); return; }
    if (reason < 0 || reason > 2) { en_refuse(w, EN_R_VALUE); return; }
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

/* which: 0 position, 1 velocity (3 doubles); 2 facing (v[0]). The age is the kernel's: setting it is refused. */
static inline void en_set(EnWorld *w, int64_t id, int which, const double v[3]) {
    int r = en_row(w, id);
    if (r < 0) { en_refuse(w, EN_R_ENTITY); return; }
    if (which < 0 || which > 2) { en_refuse(w, EN_R_WHICH); return; }
    switch (which) {
    case 0:
        if (!isfinite(v[0]) || !isfinite(v[1]) || !isfinite(v[2])) { en_refuse(w, EN_R_VALUE); return; }
        for (int i = 0; i < 3; i++) w->pos[r * 3 + i] = v[i];
        break;
    case 1:
        if (!en_float_ok(v[0]) || !en_float_ok(v[1]) || !en_float_ok(v[2])) { en_refuse(w, EN_R_VALUE); return; }
        for (int i = 0; i < 3; i++) w->vel[r * 3 + i] = (float)v[i];
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

/* writes a whole column of one kind in en_each order; rows must equal the kind's live count, and every value be finite */
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
 * the kind's slots and the parts. Routes join it when routes exist. */
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
        }
    }
    return h;
}

/* 0 live, 1 seen, 2 waiting for ground, 3 refused calls, 4 negative dt, 5 events dropped,
 * 6 last refusal reason (EN_R_*), 7 entities over the body budget. Seeing, grounding, ticking,
 * events and bodies come in later tasks; until then 1, 2, 4, 5 and 7 read 0. */
static inline double en_stat(const EnWorld *w, int which) {
    switch (which) {
    case 0: return w->live;
    case 3: return w->refused;
    case 6: return w->reason;
    default: return 0;
    }
}

#endif

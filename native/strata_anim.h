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
 * a figure of capacity); a full pool refuses with AN_R_FULL.
 *
 * Attachments: a figure may hang from another's socket, up to AN_DEPTH links
 * deep; an update poses parents before children. Sockets and hit tests read
 * the pose before springs, and pose a stale figure (placed, played or changed
 * since its last pose, or hanging from one) before they answer; posing for a
 * query changes nothing an update carries forward. Hits find figures through
 * a coarse grid of their bounds (cells AN_CELL metres), rebuilt each update.
 *
 * Cost control: an_view sets the camera's planes, eye and far distance. A
 * figure outside the planes (with AN_MARGIN metres to spare) and not marked
 * always still advances and fires its marks every update but is posed at most
 * one update in AN_OUT_EVERY; past the far distance it is posed every other
 * update with its springs frozen. A skipped figure is stale, so a query poses
 * it first: the camera never changes a mark, a socket or a hit. Each figure
 * keeps its last two posed matrix sets; an_draw_matrices and an_draw_list
 * draw between them. */
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
#define AN_DEPTH 4                 /* links from a free figure down to the deepest one hanging from it */
#define AN_SOCKETS 32              /* sockets a rig */
#define AN_SIDE 128                /* a part's grid, voxels a side at most */
#define AN_SHAPES 4096             /* part grids over every rig */
#define AN_BRICKS 4096             /* occupied 8x8x8 bricks of part voxels over every rig (2 MB) */
#define AN_BRICK_INDEX (1 << 16)   /* brick table entries over every part grid */
#define AN_CELL 4.0f               /* the hit grid's cells, metres a side */
#define AN_CELLS_PER_FIG 8         /* a figure covering more cells is kept in a list every query tests */
#define AN_MARGIN 2.0f             /* metres a figure's bounds may lie outside the view planes and still count as in view */
#define AN_OUT_EVERY 4             /* a figure out of view poses one update in this many */
#define AN_FAR_EVERY 2             /* a figure past the far distance poses one update in this many */
#define AN_TRACED 64               /* figures drawn traced at once; more are counted in stat 9 and drawn plain */
#define AN_DRAW_MAX (1 << 14)      /* parts in one draw list (twice the arena's 7 680); more are counted in stat 10 */
#define AN_SHADOW_REACH 24.0f      /* metres along the ground a figure out of view may lie from the view and still be listed for its shadow */

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
    AN_R_PART = 13,     /* the procedural stage: a part outside the rig, a slot outside its range, or a chain not running down the rig */
    AN_R_SOCKET = 14,   /* no such socket on the rig */
    AN_R_ATTACH = 15    /* an attachment deeper than AN_DEPTH, or a cycle */
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
    float drop_goal, drop_rate;  /* where the drop is moving and how fast (metres a second; 0 holds) */
    int32_t att;                 /* the slot of the figure it hangs from, -1 for none */
    int16_t att_socket;          /* that figure's socket */
    uint8_t depth, pad;          /* links above it */
    float att_pos[3], att_q[4];  /* its placement in the socket's frame */
    float bmin[3], bmax[3];      /* world bounds of its part boxes before springs; bmin > bmax: none */
    /* cost control: set by the schedule in an_update, never by a query */
    uint8_t always, traced;      /* always posed; traced: 0 no, 1 drawn traced, 2 waiting past the cap */
    uint8_t cls, frozen;         /* AN_C_* as the last update found it; springs frozen (far, not always) */
    uint8_t wait, unposed;       /* updates skipped since its last scheduled pose (reset to a stagger on a class change); never posed by an update */
    uint8_t dspan, dsince;       /* drawing: updates between its last two poses, and since the last (the blend is stretched over the span) */
    float pend;                  /* seconds since its last scheduled pose (the springs' dt) */
    float dmove;                 /* drawing: updates of motion from the pose before last to the last */
    float vbmin[3], vbmax[3], vpos[3];   /* its bounds and placement at its last scheduled pose (culling reads them) */
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
enum { AN_C_OUT = 1, AN_C_FAR = 2 };   /* a figure's class: out of view, past the far distance */
typedef struct {
    int32_t next;                /* the figure's next record, -1 at the end */
    uint8_t kind, slot, n, fresh;
    int8_t chain[4];
    float v[9];
} AnProc;

/* a socket: a frame in a part's frame (metres from its origin) */
typedef struct { int32_t part; float pos[3], q[4]; } AnSocket;
/* a part's grid for hits: its voxels in occupied 8x8x8 bricks; the grid's
 * low corner sits at the part's box minimum in the part's frame */
typedef struct { int16_t sx, sy, sz, nbx, nby, nbz; int32_t index; float voxel; } AnShape;
/* a ray's answer: the figure's id, the part, the voxel in the part's grid, its
 * palette index, the distance in metres and the struck face's world normal */
typedef struct { int32_t fig, part, x, y, z, colour; float dist, nx, ny, nz; } AnHit;

struct AnWorld {
    int capacity;
    AnRig *rigs; int nrigs;
    uint8_t rig_live[AN_RIGS];   /* a rig slot in use (an_rig_free empties it) */
    int32_t rig_free[AN_RIGS]; int nrig_free;   /* freed rig slots, taken again before new ones */
    AnMask *masks; int nmasks;
    AnClip *clips; int nclips;
    AnKey *keys; uint8_t *key_interp; int nkeys;
    float *mark_t; int32_t *mark_tag; int nmarks_pool;
    AnFig *figs;
    float *mats;                 /* capacity x AN_PARTS x 12: column-major 3x4, world space */
    int32_t *ids; uint32_t ids_mask;   /* id -> slot: open addressing, linear probing, -1 empty; 2 x capacity rounded up to a power of two */
    int32_t *free_slots; int nfree;
    int32_t next_id;
    int32_t first_id;            /* the id the world's first figure took (or will): the hash counts ids from it */
    uint32_t generation;         /* bumped by each world opened in another's place (an_open_next, an_reopen) */
    AnEvent *events, *events_tmp; int nevents, fig_events;   /* capacity x AN_FIG_EVENTS */
    int live, posed;
    double refused, negative_dt, dropped; int reason;
    AnProc *procs; int32_t *proc_free; int nprocs, nproc_free;   /* the procedural pool and its free stack */
    AnSocket *sockets; int32_t nsockets[AN_RIGS];                /* AN_SOCKETS a rig */
    AnShape *shapes; int nshapes; int32_t *shape_of;             /* shape_of[rig * AN_PARTS + part]: a shape, -1 none */
    int32_t *brick_index; int nbrick_index;                      /* each grid's bricks: a brick, -1 empty */
    uint8_t *bricks; int nbricks;                                /* 512 palette indices a brick */
    int nattached, max_depth;
    int32_t *grid_head; uint32_t grid_mask;                      /* the hit grid: cell hash -> entries */
    int32_t *grid_next, *grid_slot; int ngrid;                   /* capacity x AN_CELLS_PER_FIG entries */
    int32_t *big; int nbig;                                      /* figures over more than AN_CELLS_PER_FIG cells */
    float ubmin[3], ubmax[3];                                    /* the union of the grid's figures' bounds */
    int grid_dirty;                                              /* a figure was posed or removed since the grid was built */
    uint32_t *stamp; uint32_t stamp_now;                         /* a figure seen by this query */
    float *scratch;                                              /* AN_PARTS x 12: a pose rebuilt for a query */
    int32_t *cand;                                               /* capacity: a box query's candidate slots */
    int nstale;                                                  /* live figures marked dirty: a query with none poses nothing */
    double scans;                                                /* passes a query made over the figures to pose stale ones (stat 8) */
    float *prev;                                                 /* capacity x AN_PARTS x 12: the pose before the last */
    float planes[24]; int nplanes;                               /* the view: 0 or 6 unit planes, n.p + d >= 0 inside */
    float eye[3], far_m; int has_far;                            /* the eye, and the distance past which a figure is far */
    float dsun[3]; int has_dsun;                                 /* the drawing's sun (an_draw_sun): toward it, unit, above the horizon */
    int ntraced, nwaiting;                                       /* figures drawn traced, and those waiting past the cap */
    int skipped;                                                 /* figures the last update did not pose (stat 11) */
    double draw_over;                                            /* parts left out of the last draw list (stat 10) */
    float *dscratch;                                             /* AN_PARTS x 12: an_draw_matrices */
    int32_t *dl_rp, *dl_flags; float *dl_xf;                     /* AN_DRAW_MAX: the draw list */
};
typedef struct AnWorld AnWorld;

static inline int an_refuse(AnWorld *w, int reason) { w->refused += 1; w->reason = reason; return -1; }
/* figure f is stale: changed since its last pose (counted, so a query on a clean world does nothing first) */
static inline void an_mark(AnWorld *w, AnFig *f) { if (!f->dirty) { f->dirty = 1; w->nstale++; } }

static inline void an_close(AnWorld *w) {
    if (!w) return;
    free(w->rigs); free(w->masks); free(w->clips); free(w->keys); free(w->key_interp);
    free(w->mark_t); free(w->mark_tag); free(w->figs); free(w->mats);
    free(w->ids); free(w->free_slots); free(w->events); free(w->events_tmp);
    free(w->procs); free(w->proc_free);
    free(w->sockets); free(w->shapes); free(w->shape_of); free(w->brick_index); free(w->bricks);
    free(w->grid_head); free(w->grid_next); free(w->grid_slot); free(w->big); free(w->stamp); free(w->scratch); free(w->cand);
    free(w->prev); free(w->dscratch); free(w->dl_rp); free(w->dl_flags); free(w->dl_xf);
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
    w->sockets = (AnSocket *)calloc((size_t)AN_RIGS * AN_SOCKETS, sizeof *w->sockets);
    w->shapes = (AnShape *)calloc(AN_SHAPES, sizeof *w->shapes);
    w->shape_of = (int32_t *)malloc((size_t)AN_RIGS * AN_PARTS * sizeof *w->shape_of);
    w->brick_index = (int32_t *)malloc(AN_BRICK_INDEX * sizeof *w->brick_index);
    w->bricks = (uint8_t *)calloc((size_t)AN_BRICKS * 512, 1);
    uint32_t gn = 2;
    while (gn < 4u * (uint32_t)capacity) gn *= 2;
    w->grid_mask = gn - 1;
    w->grid_head = (int32_t *)malloc(gn * sizeof *w->grid_head);
    w->grid_next = (int32_t *)malloc((size_t)capacity * AN_CELLS_PER_FIG * sizeof *w->grid_next);
    w->grid_slot = (int32_t *)malloc((size_t)capacity * AN_CELLS_PER_FIG * sizeof *w->grid_slot);
    w->big = (int32_t *)malloc((size_t)capacity * sizeof *w->big);
    w->stamp = (uint32_t *)calloc((size_t)capacity, sizeof *w->stamp);
    w->scratch = (float *)calloc(AN_PARTS * 12, sizeof *w->scratch);
    w->cand = (int32_t *)malloc((size_t)capacity * sizeof *w->cand);
    w->prev = (float *)calloc((size_t)capacity * AN_PARTS * 12, sizeof *w->prev);
    w->dscratch = (float *)calloc(AN_PARTS * 12, sizeof *w->dscratch);
    w->dl_rp = (int32_t *)malloc(AN_DRAW_MAX * sizeof *w->dl_rp);
    w->dl_flags = (int32_t *)malloc(AN_DRAW_MAX * sizeof *w->dl_flags);
    w->dl_xf = (float *)malloc((size_t)AN_DRAW_MAX * 12 * sizeof *w->dl_xf);
    if (!w->rigs || !w->masks || !w->clips || !w->keys || !w->key_interp || !w->mark_t || !w->mark_tag || !w->figs ||
        !w->mats || !w->ids || !w->free_slots || !w->events || !w->events_tmp || !w->procs || !w->proc_free ||
        !w->sockets || !w->shapes || !w->shape_of || !w->brick_index || !w->bricks || !w->grid_head || !w->grid_next ||
        !w->grid_slot || !w->big || !w->stamp || !w->scratch || !w->cand || !w->prev || !w->dscratch || !w->dl_rp ||
        !w->dl_flags || !w->dl_xf) { an_close(w); return NULL; }
    for (int i = 0; i < AN_RIGS * AN_PARTS; i++) w->shape_of[i] = -1;
    for (uint32_t i = 0; i < gn; i++) w->grid_head[i] = -1;
    for (int i = 0; i < w->nprocs; i++) w->proc_free[i] = w->nprocs - 1 - i;   /* record 0 handed out first */
    w->nproc_free = w->nprocs;
    for (uint32_t i = 0; i < hn; i++) w->ids[i] = -1;
    for (int i = 0; i < capacity; i++) { w->figs[i].id = -1; w->free_slots[i] = capacity - 1 - i; }   /* slot 0 handed out first */
    w->nfree = capacity;
    return w;
}

/* a world in place of one of [generation] whose next figure id was [next_id]:
 * its generation is one past, and its ids carry on from there, so an id held
 * from the last world does not name a figure of this one (until 2^20 ids
 * later). Rig, mask, clip and socket ids start again at 0. */
static inline AnWorld *an_open_next(int capacity, uint32_t generation, int32_t next_id) {
    AnWorld *w = an_open(capacity);
    if (w) { w->generation = generation + 1; w->next_id = w->first_id = next_id & (AN_IDS - 1); }
    return w;
}
/* close [old] (NULL for none) and open its successor */
static inline AnWorld *an_reopen(AnWorld *old, int capacity) {
    uint32_t gen = old ? old->generation : 0;
    int32_t nid = old ? old->next_id : 0;
    an_close(old);
    return an_open_next(capacity, gen, nid);
}

/* ---- rigs and masks ---- */
static inline int an_rig_ok(const AnWorld *w, int rig) { return rig >= 0 && rig < w->nrigs && w->rig_live[rig]; }
/* a rig's description checked: 0, or the refusal reason */
static inline int an_rig_check(int nparts, const int32_t *parent, const float *pivot, const float *rest_q, const float *box) {
    if (nparts < 1 || nparts > AN_PARTS) return AN_R_PARTS;
    if (!parent || !pivot || !rest_q || !box) return AN_R_VALUE;
    if (parent[0] != -1) return AN_R_PARENT;
    for (int i = 1; i < nparts; i++) if (parent[i] < 0 || parent[i] >= i) return AN_R_PARENT;
    if (!an_finite(pivot, 3 * nparts) || !an_finite(rest_q, 4 * nparts) || !an_finite(box, 6 * nparts)) return AN_R_VALUE;
    for (int i = 0; i < nparts; i++) {
        float q[4] = {rest_q[i * 4], rest_q[i * 4 + 1], rest_q[i * 4 + 2], rest_q[i * 4 + 3]};
        if (!an_qnorm(q)) return AN_R_VALUE;
        for (int k = 0; k < 3; k++) if (box[i * 6 + k] > box[i * 6 + 3 + k]) return AN_R_VALUE;
    }
    return 0;
}
static inline void an_rig_write(AnRig *r, int nparts, const int32_t *parent, const float *pivot, const float *rest_q, const float *box) {
    memset(r, 0, sizeof *r);
    r->nparts = nparts;
    for (int i = 0; i < nparts; i++) {
        r->parent[i] = (int8_t)parent[i];
        memcpy(r->pivot[i], pivot + i * 3, sizeof r->pivot[i]);
        memcpy(r->rest[i], rest_q + i * 4, sizeof r->rest[i]);
        an_qnorm(r->rest[i]);
        memcpy(r->box[i], box + i * 6, sizeof r->box[i]);
    }
}
static inline int an_rig(AnWorld *w, int nparts, const int32_t *parent, const float *pivot,
                         const float *rest_q, const float *box) {
    int bad = an_rig_check(nparts, parent, pivot, rest_q, box);
    if (bad) return an_refuse(w, bad);
    int slot;
    if (w->nrig_free > 0) slot = w->rig_free[--w->nrig_free];
    else if (w->nrigs < AN_RIGS) slot = w->nrigs++;
    else return an_refuse(w, AN_R_FULL);
    an_rig_write(&w->rigs[slot], nparts, parent, pivot, rest_q, box);
    w->rig_live[slot] = 1;
    w->nsockets[slot] = 0;
    return slot;
}

static inline int an_mask(AnWorld *w, int rig, int nroots, const int32_t *roots) {
    if (!an_rig_ok(w, rig)) return an_refuse(w, AN_R_RIG);
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
    if (!an_rig_ok(w, rig)) return an_refuse(w, AN_R_RIG);
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
static inline float *an_fig_prev(const AnWorld *w, const AnFig *f) { return w->prev + (size_t)(f - w->figs) * AN_PARTS * 12; }
static inline void an_pose(AnWorld *w, AnFig *f, float dt);
/* pose f as it stands, with nothing to draw between: both matrix sets, and the bounds culling reads */
static inline void an_settle(AnWorld *w, AnFig *f) {
    an_pose(w, f, 0);
    memcpy(an_fig_prev(w, f), an_fig_mats(w, f), (size_t)w->rigs[f->rig].nparts * 12 * sizeof(float));
    memcpy(f->vbmin, f->bmin, sizeof f->vbmin); memcpy(f->vbmax, f->bmax, sizeof f->vbmax); memcpy(f->vpos, f->pos, sizeof f->vpos);
}

static inline int an_spawn(AnWorld *w, int rig) {
    if (!an_rig_ok(w, rig)) return an_refuse(w, AN_R_RIG);
    if (w->nfree == 0) return an_refuse(w, AN_R_FULL);
    while (an_slot(w, w->next_id) >= 0) w->next_id = (w->next_id + 1) & (AN_IDS - 1);   /* at most capacity live ids */
    int id = w->next_id;
    w->next_id = (w->next_id + 1) & (AN_IDS - 1);
    int s = w->free_slots[--w->nfree];
    AnFig *f = &w->figs[s];
    memset(f, 0, sizeof *f);
    f->id = id; f->rig = rig; f->q[3] = 1; f->proc = -1;
    f->att = -1; f->att_q[3] = 1;
    f->unposed = 1; f->dspan = 1; f->dmove = 1;
    an_id_insert(w, id, s);
    w->live++;
    an_settle(w, f);                           /* the rest pose, at the origin */
    return id;
}

static inline void an_recount(AnWorld *w);
static inline void an_untrace(AnWorld *w, AnFig *f);
static inline void an_attach_place(AnWorld *w, AnFig *f);
static inline void an_place_chain(AnWorld *w, AnFig *f);
static inline void an_unhang(AnFig *f) {   /* no longer attached; it keeps its placement */
    f->att = -1; f->att_socket = 0;
    memset(f->att_pos, 0, sizeof f->att_pos);
    f->att_q[0] = f->att_q[1] = f->att_q[2] = 0; f->att_q[3] = 1;
}

/* a removed figure's children are detached at its socket as it stands now
 * (the chain above re-placed from the top: whether or not a query posed any
 * of them since the last update) */
static inline void an_remove(AnWorld *w, int fig) {
    AnFig *f = an_fig(w, fig);
    if (!f) { an_refuse(w, AN_R_FIGURE); return; }
    int slot = (int)(f - w->figs);
    if (w->nattached) {
        for (int s = 0; s < w->capacity; s++) {
            AnFig *c = &w->figs[s];
            if (c->id < 0 || c->att != slot) continue;
            an_place_chain(w, c);
            an_unhang(c);
        }
        an_unhang(f);
    }
    if (f->dirty) { f->dirty = 0; w->nstale--; }
    an_untrace(w, f);
    w->grid_dirty = 1;
    an_id_erase(w, fig);
    for (int i = f->proc; i >= 0; i = w->procs[i].next) w->proc_free[w->nproc_free++] = i;
    f->proc = -1;
    f->id = -1;
    w->free_slots[w->nfree++] = (int32_t)(f - w->figs);
    w->live--;
    if (w->nattached) an_recount(w);
}

static inline void an_place(AnWorld *w, int fig, const float pos[3], const float q[4]) {
    AnFig *f = an_fig(w, fig);
    if (!f) { an_refuse(w, AN_R_FIGURE); return; }
    float n[4] = {q[0], q[1], q[2], q[3]};
    if (!an_finite(pos, 3) || !an_finite(q, 4) || !an_qnorm(n)) { an_refuse(w, AN_R_VALUE); return; }
    memcpy(f->pos, pos, sizeof f->pos);
    memcpy(f->q, n, sizeof f->q);
    an_mark(w, f);
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
    an_mark(w, f);
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
    an_mark(w, f);
}

static inline void an_set(AnWorld *w, int fig, int layer, int what, float v, float fade) {
    AnFig *f = an_fig(w, fig);
    if (!f) { an_refuse(w, AN_R_FIGURE); return; }
    if (layer < 0 || layer >= AN_LAYERS) { an_refuse(w, AN_R_LAYER); return; }
    if (!isfinite(v) || !isfinite(fade)) { an_refuse(w, AN_R_VALUE); return; }
    AnLayer *L = &f->layer[layer];
    if (what == 1) { an_ramp_to(&L->weight, v < 0 ? 0 : (v > 1 ? 1 : v), fade); an_mark(w, f); return; }
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
    an_mark(w, f);
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
    /* the drop moving toward its goal at its speed */
    if (f->drop_rate > 0) {
        float step = f->drop_rate * dt, d = f->drop_goal - f->drop;
        if (fabsf(d) <= step) f->drop = f->drop_goal;
        else f->drop += d > 0 ? step : -step;
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
        memcpy(q, target, 4 * sizeof(float)); om[0] = om[1] = om[2] = 0;
        if (dt > 0) s->fresh = 0;              /* a pose without time (for a query) leaves it fresh */
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

/* a rotation's 3x3, column-major: o[0..2] is where it takes x */
static inline void an_qmat(const float q[4], float o[9]) {
    const float x = q[0], y = q[1], z = q[2], sw = q[3];
    o[0] = 1 - 2 * (y * y + z * z); o[1] = 2 * (x * y + sw * z);     o[2] = 2 * (x * z - sw * y);
    o[3] = 2 * (x * y - sw * z);     o[4] = 1 - 2 * (x * x + z * z); o[5] = 2 * (y * z + sw * x);
    o[6] = 2 * (x * z + sw * y);     o[7] = 2 * (y * z - sw * x);     o[8] = 1 - 2 * (x * x + y * y);
}
/* a part's 3x4: its rotation's columns, then its origin */
static inline void an_write_mat(const float q[4], const float p[3], float *o) {
    an_qmat(q, o);
    o[9] = p[0]; o[10] = p[1]; o[11] = p[2];
}
/* does part p's box hold any volume? (a pure joint's does not) */
static inline int an_has_box(const AnRig *r, int p) {
    const float *bx = r->box[p];
    return bx[3] > bx[0] && bx[4] > bx[1] && bx[5] > bx[2];
}
/* the figure's world bounds: every part box with volume, as the pose before springs places it */
static inline void an_bounds(const AnRig *r, const AnBody *b, AnFig *f) {
    float lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
    for (int p = 0; p < r->nparts; p++) {
        if (!an_has_box(r, p)) continue;
        const float *bx = r->box[p];
        float m[9], c[3], e[3];
        an_qmat(b->wq[p], m);
        for (int k = 0; k < 3; k++) { c[k] = (bx[k] + bx[3 + k]) * 0.5f; e[k] = (bx[3 + k] - bx[k]) * 0.5f; }
        for (int i = 0; i < 3; i++) {
            float wc = b->wp[p][i] + m[i] * c[0] + m[3 + i] * c[1] + m[6 + i] * c[2];
            float we = fabsf(m[i]) * e[0] + fabsf(m[3 + i]) * e[1] + fabsf(m[6 + i]) * e[2];
            if (wc - we < lo[i]) lo[i] = wc - we;
            if (wc + we > hi[i]) hi[i] = wc + we;
        }
    }
    if (lo[0] > hi[0]) { lo[0] = lo[1] = lo[2] = 1; hi[0] = hi[1] = hi[2] = 0; }
    memcpy(f->bmin, lo, sizeof lo); memcpy(f->bmax, hi, sizeof hi);
}

/* poses figure f: the pose up to the springs, then the hierarchy with the
 * springs advanced by dt (0 holds them), into its matrices; its bounds come
 * from the pose before springs. A frozen figure's springs are left as they
 * are and its parts drawn where the pose before springs puts them, as is a
 * fresh spring posed without time (for a query), so posing with dt 0 changes
 * no spring. */
static inline void an_pose(AnWorld *w, AnFig *f, float dt) {
    const AnRig *r = &w->rigs[f->rig];
    AnBody b;
    an_build(w, f, &b, 1);
    an_bounds(r, &b, f);
    w->grid_dirty = 1;
    int16_t spring[AN_PARTS];
    int any = 0;
    for (int p = 0; p < r->nparts; p++) spring[p] = -1;
    if (!f->frozen)
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
            if (spring[p] >= 0 && !(w->procs[spring[p]].fresh && !(dt > 0))) {
                AnProc *s = &w->procs[spring[p]];
                an_spring_step(s, b.wq[p], dt);
                memcpy(b.wq[p], s->v + 2, sizeof b.wq[p]);
            }
        }
        an_write_mat(b.wq[p], b.wp[p], out + p * 12);
    }
    if (f->dirty) { f->dirty = 0; w->nstale--; }
}

/* ---- attachments ---- */

/* socket s of figure f in world space, from the pose before springs (rebuilt
 * from the figure as it stands: the last pose's unless it changed since) */
static inline void an_socket_frame(const AnWorld *w, const AnFig *f, const AnSocket *s, float q[4], float p[3]) {
    AnBody b;
    float ro[3];
    an_build(w, f, &b, 1);
    an_qrot(b.wq[s->part], s->pos, ro);
    for (int i = 0; i < 3; i++) p[i] = b.wp[s->part][i] + ro[i];
    an_qmul(b.wq[s->part], s->q, q);
    an_qnorm(q);
}
/* an attached figure's placement: its parent's socket, then its offset */
static inline void an_attach_place(AnWorld *w, AnFig *f) {
    const AnFig *pf = &w->figs[f->att];
    float sq[4], sp[3], ro[3];
    an_socket_frame(w, pf, &w->sockets[pf->rig * AN_SOCKETS + f->att_socket], sq, sp);
    an_qrot(sq, f->att_pos, ro);
    for (int i = 0; i < 3; i++) f->pos[i] = sp[i] + ro[i];
    an_qmul(sq, f->att_q, f->q);
    an_qnorm(f->q);
}
/* f and every figure above it that hangs placed at its parent's socket from
 * the top down, so f's placement is a function of the chain's state alone
 * (not of which of them a query placed since the last update); each placed
 * one is marked stale. An update re-places them all the same way. */
static inline void an_place_chain(AnWorld *w, AnFig *f) {
    AnFig *chain[AN_DEPTH + 2];
    int n = 0;
    for (AnFig *g = f; n < AN_DEPTH + 2; g = &w->figs[g->att]) { chain[n++] = g; if (g->att < 0) break; }
    for (int i = n - 1; i >= 0; i--)
        if (chain[i]->att >= 0) { an_attach_place(w, chain[i]); an_mark(w, chain[i]); }
}
/* every figure's depth (links above it), the deepest, and how many hang */
static inline void an_recount(AnWorld *w) {
    w->nattached = 0; w->max_depth = 0;
    for (int s = 0; s < w->capacity; s++) {
        AnFig *f = &w->figs[s];
        if (f->id < 0) continue;
        int d = 0;
        for (const AnFig *g = f; g->att >= 0 && d <= AN_DEPTH; g = &w->figs[g->att]) d++;
        f->depth = (uint8_t)d;
        if (f->att >= 0) w->nattached++;
        if (d > w->max_depth) w->max_depth = d;
    }
}

/* ---- the hit grid ---- */
static inline uint32_t an_cell_hash(int x, int y, int z) { return (uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u ^ (uint32_t)z * 83492791u; }
static inline int an_cell_of(float v) {
    float c = floorf(v / AN_CELL);
    return c < -1e9f ? -1000000000 : (c > 1e9f ? 1000000000 : (int)c);
}
/* each figure with bounds goes into every cell its bounds touch (at most
 * AN_CELLS_PER_FIG; past that into the list every query tests) */
static inline void an_grid_build(AnWorld *w) {
    for (uint32_t i = 0; i <= w->grid_mask; i++) w->grid_head[i] = -1;
    w->ngrid = 0; w->nbig = 0;
    for (int k = 0; k < 3; k++) { w->ubmin[k] = INFINITY; w->ubmax[k] = -INFINITY; }
    for (int s = 0; s < w->capacity; s++) {
        const AnFig *f = &w->figs[s];
        if (f->id < 0 || !(f->bmin[0] <= f->bmax[0])) continue;
        int c0[3], c1[3];
        double n = 1;
        for (int k = 0; k < 3; k++) { c0[k] = an_cell_of(f->bmin[k]); c1[k] = an_cell_of(f->bmax[k]); n *= (double)c1[k] - c0[k] + 1; }
        if (n > AN_CELLS_PER_FIG) { w->big[w->nbig++] = s; continue; }
        for (int k = 0; k < 3; k++) {
            if (f->bmin[k] < w->ubmin[k]) w->ubmin[k] = f->bmin[k];
            if (f->bmax[k] > w->ubmax[k]) w->ubmax[k] = f->bmax[k];
        }
        for (int z = c0[2]; z <= c1[2]; z++)
            for (int y = c0[1]; y <= c1[1]; y++)
                for (int x = c0[0]; x <= c1[0]; x++) {
                    uint32_t h = an_cell_hash(x, y, z) & w->grid_mask;
                    w->grid_slot[w->ngrid] = s; w->grid_next[w->ngrid] = w->grid_head[h]; w->grid_head[h] = w->ngrid++;
                }
    }
    w->grid_dirty = 0;
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

/* a rotation's quaternion from its 3x3 (column-major, as an_qmat writes it) */
static inline void an_mat_q(const float *m, float q[4]) {
    float m00 = m[0], m10 = m[1], m20 = m[2], m01 = m[3], m11 = m[4], m21 = m[5], m02 = m[6], m12 = m[7], m22 = m[8];
    float tr = m00 + m11 + m22;
    if (tr > 0) {
        float s = sqrtf(tr + 1) * 2;
        q[3] = 0.25f * s; q[0] = (m21 - m12) / s; q[1] = (m02 - m20) / s; q[2] = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        float s = sqrtf(1 + m00 - m11 - m22) * 2;
        q[3] = (m21 - m12) / s; q[0] = 0.25f * s; q[1] = (m01 + m10) / s; q[2] = (m02 + m20) / s;
    } else if (m11 > m22) {
        float s = sqrtf(1 + m11 - m00 - m22) * 2;
        q[3] = (m02 - m20) / s; q[0] = (m01 + m10) / s; q[1] = 0.25f * s; q[2] = (m12 + m21) / s;
    } else {
        float s = sqrtf(1 + m22 - m00 - m11) * 2;
        q[3] = (m10 - m01) / s; q[0] = (m02 + m20) / s; q[1] = (m12 + m21) / s; q[2] = 0.25f * s;
    }
    an_qnorm(q);
}
/* a part between two 3x4s: the rotation slerped, the origin lerped; u 0 is a, 1 is b, exactly */
static inline void an_mat_mix(const float *a, const float *b, float u, float *o) {
    if (!(u > 0)) { memcpy(o, a, 12 * sizeof(float)); return; }
    if (u >= 1) { memcpy(o, b, 12 * sizeof(float)); return; }
    float qa[4], qb[4], q[4], p[3];
    an_mat_q(a, qa); an_mat_q(b, qb);
    an_slerp(qa, qb, u, q);
    for (int k = 0; k < 3; k++) p[k] = a[9 + k] + (b[9 + k] - a[9 + k]) * u;
    an_write_mat(q, p, o);
}
/* ---- cost control: the view, and which figures an update poses ---- */

/* f's bounds as culling sees them: as of its last scheduled pose, carried
 * along by any placement since; a figure of no volume is its placement */
static inline void an_cull_box(const AnFig *f, float lo[3], float hi[3]) {
    if (!(f->vbmin[0] <= f->vbmax[0])) { memcpy(lo, f->pos, 3 * sizeof(float)); memcpy(hi, f->pos, 3 * sizeof(float)); return; }
    for (int k = 0; k < 3; k++) { float m = f->pos[k] - f->vpos[k]; lo[k] = f->vbmin[k] + m; hi[k] = f->vbmax[k] + m; }
}
/* is f in view: no planes, or its bounds within AN_MARGIN of the inside of every plane */
static inline int an_in_view(const AnWorld *w, const AnFig *f) {
    if (!w->nplanes) return 1;
    float lo[3], hi[3];
    an_cull_box(f, lo, hi);
    for (int i = 0; i < w->nplanes; i++) {
        const float *pl = w->planes + 4 * i;
        float d = pl[3];
        for (int k = 0; k < 3; k++) d += pl[k] * (pl[k] > 0 ? hi[k] : lo[k]);   /* the corner furthest inside */
        if (d < -AN_MARGIN) return 0;
    }
    return 1;
}
/* f's class from the view: out of view, past the far distance (from the eye to its bounds' nearest point) */
static inline int an_classify(const AnWorld *w, const AnFig *f) {
    int c = an_in_view(w, f) ? 0 : AN_C_OUT;
    if (w->has_far) {
        float lo[3], hi[3], d2 = 0;
        an_cull_box(f, lo, hi);
        for (int k = 0; k < 3; k++) {
            float e = w->eye[k], g = e < lo[k] ? lo[k] - e : (e > hi[k] ? e - hi[k] : 0);
            d2 += g * g;
        }
        if (d2 > w->far_m * w->far_m) c |= AN_C_FAR;
    }
    return c;
}
/* does this update pose f (slot s)? A free figure by its class: always, or in
 * view and near, every update; out of view one in AN_OUT_EVERY; far one in
 * AN_FAR_EVERY. An attached figure poses when its parent does (it takes the
 * parent's class), unless it is always posed. A figure entering a class waits
 * a share of its period set by its slot, so a crowd entering at once spreads
 * over the updates. Springs freeze while far (not always) and start again at
 * rest when that ends. */
static inline int an_sched(AnWorld *w, AnFig *f, int s, float dt) {
    f->pend += dt;
    const AnFig *par = f->att >= 0 ? &w->figs[f->att] : NULL;
    int cls = f->always ? 0 : (par ? par->cls : an_classify(w, f));
    int frozen = (cls & AN_C_FAR) && !f->always;
    if (f->frozen && !frozen)
        for (int i = f->proc; i >= 0; i = w->procs[i].next) if (w->procs[i].kind == AN_K_SPRING) w->procs[i].fresh = 1;
    f->frozen = (uint8_t)frozen;
    int every = cls & AN_C_OUT ? AN_OUT_EVERY : (cls & AN_C_FAR ? AN_FAR_EVERY : 1);
    if (cls != f->cls) { f->cls = (uint8_t)cls; f->wait = (uint8_t)(s % every); }
    int pose = (par && !f->always) ? par->wait == 0 : f->wait + 1 >= every;
    if (pose) f->wait = 0;
    else if (f->wait < 255) f->wait++;
    return pose;
}

/* advances every figure (marks in slot order), then poses those the schedule
 * picks, parents first: depth 0, then 1, ... each attached figure placed at
 * its parent's socket; a figure not posed is left stale. Then rebuilds the hit
 * grid. A posed figure's last pose becomes the one before it. */
static inline void an_update(AnWorld *w, float dt) {
    if (!(dt >= 0) || !isfinite(dt)) { w->negative_dt += 1; return; }
    w->nevents = 0;
    w->posed = 0;
    w->skipped = 0;
    for (int s = 0; s < w->capacity; s++) if (w->figs[s].id >= 0) an_advance(w, &w->figs[s], dt);
    for (int d = 0; d <= w->max_depth; d++)
        for (int s = 0; s < w->capacity; s++) {
            AnFig *f = &w->figs[s];
            if (f->id < 0 || f->depth != d) continue;
            if (!an_sched(w, f, s, dt)) { an_mark(w, f); w->skipped++; if (f->dsince < 255) f->dsince++; continue; }
            if (f->att >= 0) an_attach_place(w, f);
            int np = w->rigs[f->rig].nparts;
            size_t len = (size_t)np * 12 * sizeof(float);
            /* the pose before last is where the drawing has got to: the last pose, unless
             * its span was longer than the updates since (a figure whose period shrank,
             * coming into view); then the motion left to draw (in updates) is drawn over
             * half as many updates, so the drawn lag halves each pose and nothing jumps */
            int span = f->dspan ? f->dspan : 1;
            float reached = (float)(f->dsince + 1) / (float)span;
            if (reached >= 1) {
                memcpy(an_fig_prev(w, f), an_fig_mats(w, f), len);
                f->dspan = (uint8_t)(f->dsince + 1); f->dmove = (float)f->dspan;
            } else {
                for (int p = 0; p < np; p++) an_mat_mix(an_fig_prev(w, f) + p * 12, an_fig_mats(w, f) + p * 12, reached, an_fig_prev(w, f) + p * 12);
                f->dmove = (1 - reached) * f->dmove + (float)(f->dsince + 1);
                int half = (int)ceilf(f->dmove * 0.5f - 1e-3f);
                f->dspan = (uint8_t)(half < 1 ? 1 : (half > 255 ? 255 : half));
            }
            f->dsince = 0;
            an_pose(w, f, f->pend);
            if (f->unposed) { memcpy(an_fig_prev(w, f), an_fig_mats(w, f), len); f->unposed = 0; f->dspan = 1; f->dmove = 1; }
            f->pend = 0;
            memcpy(f->vbmin, f->bmin, sizeof f->vbmin); memcpy(f->vbmax, f->bmax, sizeof f->vbmax); memcpy(f->vpos, f->pos, sizeof f->vpos);
            w->posed++;
        }
    an_grid_build(w);
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
    if (weight <= 0) { an_proc_clear(w, f, AN_K_OVERRIDE, part); an_mark(w, f); return; }
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
    an_mark(w, f);
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
    if (weight <= 0) { an_proc_clear(w, f, AN_K_AIM, slot); an_mark(w, f); return; }
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
    an_mark(w, f);
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
    if (seconds <= 0 && goal <= 0) { an_proc_clear(w, f, AN_K_REACH, slot); an_mark(w, f); return; }
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
    an_mark(w, f);
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
    if (stiffness == 0) { an_proc_clear(w, f, AN_K_SPRING, part); an_mark(w, f); return; }
    int made;
    AnProc *p = an_proc_get(w, f, AN_K_SPRING, part, &made);
    if (!p) return;
    if (made) { p->fresh = 1; p->v[5] = 1; }
    p->v[0] = stiffness; p->v[1] = damping;
    an_mark(w, f);
}

/* the root lowered by metres (world -y) after the layers, for foot placement */
static inline void an_drop(AnWorld *w, int fig, float metres) {
    AnFig *f = an_fig(w, fig);
    if (!f) { an_refuse(w, AN_R_FIGURE); return; }
    if (!isfinite(metres)) { an_refuse(w, AN_R_VALUE); return; }
    f->drop = f->drop_goal = metres;
    f->drop_rate = 0;
    an_mark(w, f);
}
/* the drop moving toward metres at speed metres a second through the
 * updates (speed 0 or less: at once) */
static inline void an_drop_to(AnWorld *w, int fig, float metres, float speed) {
    AnFig *f = an_fig(w, fig);
    if (!f) { an_refuse(w, AN_R_FIGURE); return; }
    if (!isfinite(metres) || !isfinite(speed)) { an_refuse(w, AN_R_VALUE); return; }
    if (speed <= 0) { an_drop(w, fig, metres); return; }
    f->drop_goal = metres;
    f->drop_rate = speed;
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

static inline uint64_t an_fnv(uint64_t h, const void *v, size_t n) {
    const unsigned char *p = (const unsigned char *)v;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}
/* a figure's layers, field by field (never padding) */
static inline uint64_t an_hash_layers(uint64_t h, const AnFig *f) {
    for (int l = 0; l < AN_LAYERS; l++) {
        const AnLayer *L = &f->layer[l];
        float lv[6] = {L->weight.from, L->weight.to, L->weight.t, L->weight.len, L->fade_t, L->fade_len};
        h = an_fnv(h, lv, sizeof lv);
        h = an_fnv(h, &L->nsrc, sizeof L->nsrc);
        for (int i = 0; i < L->nsrc; i++) {
            const AnSource *s = &L->src[i];
            float sv[8 + AN_BLEND] = {s->t, s->rate, s->w, s->base, s->value.from, s->value.to, s->value.t, s->value.len};
            int16_t si[AN_BLEND + 1];
            for (int k = 0; k < AN_BLEND; k++) { sv[8 + k] = k < s->n ? s->pos[k] : 0; si[k] = k < s->n ? s->clip[k] : -1; }
            si[AN_BLEND] = s->mask;
            uint8_t sf[6] = {s->n, s->blend, s->paused, s->fresh, s->loops, s->additive};
            h = an_fnv(h, sv, sizeof sv); h = an_fnv(h, si, sizeof si); h = an_fnv(h, sf, sizeof sf);
        }
    }
    return h;
}
/* every live figure's carried state: its layers; its matrices if the last
 * update posed it (a skipped figure's are its last pose, rewritten whole at
 * its next, and a query may have posed it since); its schedule; its
 * placement and culling bounds; the drop,
 * the procedural records and the attachment */
static inline uint64_t an_hash(const AnWorld *w) {
    uint64_t h = 1469598103934665603ull;
    for (int s = 0; s < w->capacity; s++) {
        const AnFig *f = &w->figs[s];
        if (f->id < 0) continue;
        /* ids counted from the world's first, so the same calls in a world
         * opened after another (whose ids carry on) hash the same */
        int32_t rid = (f->id - w->first_id) & (AN_IDS - 1);
        const unsigned char *p = (const unsigned char *)&rid;
        for (size_t i = 0; i < sizeof rid; i++) { h ^= p[i]; h *= 1099511628211ull; }
        h = an_hash_layers(h, f);
        uint8_t sched[4] = {f->always, f->cls, f->frozen, f->wait};
        h = an_fnv(h, sched, sizeof sched);
        h = an_fnv(h, &f->pend, sizeof f->pend);
        if (f->wait == 0) h = an_fnv(h, an_fig_mats(w, f), (size_t)w->rigs[f->rig].nparts * 12 * sizeof(float));
        /* where it stands, whatever the schedule: a free figure's placement (a
         * hanging one's is its parent's socket), and the bounds and placement
         * of its last scheduled pose, which decide its next class */
        if (f->att < 0) { h = an_fnv(h, f->pos, sizeof f->pos); h = an_fnv(h, f->q, sizeof f->q); }
        h = an_fnv(h, f->vbmin, sizeof f->vbmin); h = an_fnv(h, f->vbmax, sizeof f->vbmax); h = an_fnv(h, f->vpos, sizeof f->vpos);
        /* what the matrices do not show: the drop and its motion, and every
         * procedural record (spring state, reach fades, settings), in list order */
        float dr[3] = {f->drop, f->drop_goal, f->drop_rate};
        p = (const unsigned char *)dr;
        for (size_t i = 0; i < sizeof dr; i++) { h ^= p[i]; h *= 1099511628211ull; }
        for (int i = f->proc; i >= 0; i = w->procs[i].next) {
            const AnProc *q = &w->procs[i];
            unsigned char tag[8] = {q->kind, q->slot, q->n, q->fresh, (unsigned char)q->chain[0], (unsigned char)q->chain[1],
                                    (unsigned char)q->chain[2], (unsigned char)q->chain[3]};
            for (size_t k = 0; k < sizeof tag; k++) { h ^= tag[k]; h *= 1099511628211ull; }
            p = (const unsigned char *)q->v;
            for (size_t k = 0; k < sizeof q->v; k++) { h ^= p[k]; h *= 1099511628211ull; }
        }
        /* the attachment: the parent's id (-1 for none), the socket and the offset */
        int32_t link[2] = {f->att >= 0 ? (w->figs[f->att].id - w->first_id) & (AN_IDS - 1) : -1, f->att_socket};
        p = (const unsigned char *)link;
        for (size_t i = 0; i < sizeof link; i++) { h ^= p[i]; h *= 1099511628211ull; }
        float off[7] = {f->att_pos[0], f->att_pos[1], f->att_pos[2], f->att_q[0], f->att_q[1], f->att_q[2], f->att_q[3]};
        p = (const unsigned char *)off;
        for (size_t i = 0; i < sizeof off; i++) { h ^= p[i]; h *= 1099511628211ull; }
    }
    return h;
}

/* ---- sockets, part grids, attachments ---- */

/* a socket on rig's part at pos (metres in the part's frame) turned by q; its
 * id within the rig (0, 1, ...), or -1 */
static inline int an_socket_def(AnWorld *w, int rig, int part, const float pos[3], const float q[4]) {
    if (!an_rig_ok(w, rig)) return an_refuse(w, AN_R_RIG);
    if (part < 0 || part >= w->rigs[rig].nparts) return an_refuse(w, AN_R_PART);
    float n[4];
    if (!pos || !q || !an_finite(pos, 3) || !an_finite(q, 4)) return an_refuse(w, AN_R_VALUE);
    memcpy(n, q, sizeof n);
    if (!an_qnorm(n)) return an_refuse(w, AN_R_VALUE);
    if (w->nsockets[rig] >= AN_SOCKETS) return an_refuse(w, AN_R_FULL);
    AnSocket *k = &w->sockets[rig * AN_SOCKETS + w->nsockets[rig]];
    k->part = part;
    memcpy(k->pos, pos, sizeof k->pos);
    memcpy(k->q, n, sizeof k->q);
    return w->nsockets[rig]++;
}

/* rig's part's grid for hits: sx x sy x sz voxels of voxel metres, palette
 * indices at x + sx (y + sy z), 0 empty. The grid must fill the part's box
 * (its low corner is the box's minimum, so it lands on the drawn voxels).
 * Set once a part. Its shape number, or -1. */
static inline int an_shape(AnWorld *w, int rig, int part, int sx, int sy, int sz, float voxel, const uint8_t *palette_idx) {
    if (!an_rig_ok(w, rig)) return an_refuse(w, AN_R_RIG);
    const AnRig *r = &w->rigs[rig];
    if (part < 0 || part >= r->nparts) return an_refuse(w, AN_R_PART);
    int sz3[3] = {sx, sy, sz};
    if (!isfinite(voxel) || !(voxel > 0)) return an_refuse(w, AN_R_VALUE);
    for (int k = 0; k < 3; k++) {
        if (sz3[k] < 0 || sz3[k] > AN_SIDE) return an_refuse(w, AN_R_VALUE);
        float want = sz3[k] * voxel, have = r->box[part][3 + k] - r->box[part][k];
        if (fabsf(have - want) > 1e-4f * (1 + want)) return an_refuse(w, AN_R_VALUE);
    }
    if (w->shape_of[rig * AN_PARTS + part] >= 0) return an_refuse(w, AN_R_VALUE);
    int n = sx * sy * sz;
    if (n > 0 && !palette_idx) return an_refuse(w, AN_R_VALUE);
    int nbx = (sx + 7) / 8, nby = (sy + 7) / 8, nbz = (sz + 7) / 8, nb = nbx * nby * nbz, used = 0;
    for (int bz = 0; bz < nbz; bz++)
        for (int by = 0; by < nby; by++)
            for (int bx = 0; bx < nbx; bx++) {
                int any = 0;
                for (int z = bz * 8; z < bz * 8 + 8 && z < sz && !any; z++)
                    for (int y = by * 8; y < by * 8 + 8 && y < sy && !any; y++)
                        for (int x = bx * 8; x < bx * 8 + 8 && x < sx; x++)
                            if (palette_idx[x + sx * (y + sy * z)]) { any = 1; break; }
                used += any;
            }
    if (w->nshapes >= AN_SHAPES || w->nbrick_index + nb > AN_BRICK_INDEX || w->nbricks + used > AN_BRICKS) return an_refuse(w, AN_R_FULL);
    AnShape *sh = &w->shapes[w->nshapes];
    sh->sx = (int16_t)sx; sh->sy = (int16_t)sy; sh->sz = (int16_t)sz;
    sh->nbx = (int16_t)nbx; sh->nby = (int16_t)nby; sh->nbz = (int16_t)nbz;
    sh->index = w->nbrick_index; sh->voxel = voxel;
    for (int bz = 0, i = 0; bz < nbz; bz++)
        for (int by = 0; by < nby; by++)
            for (int bx = 0; bx < nbx; bx++, i++) {
                int b = -1;
                for (int z = bz * 8; z < bz * 8 + 8 && z < sz; z++)
                    for (int y = by * 8; y < by * 8 + 8 && y < sy; y++)
                        for (int x = bx * 8; x < bx * 8 + 8 && x < sx; x++) {
                            uint8_t c = palette_idx[x + sx * (y + sy * z)];
                            if (!c) continue;
                            if (b < 0) { b = w->nbricks++; memset(w->bricks + (size_t)b * 512, 0, 512); }
                            w->bricks[(size_t)b * 512 + (x & 7) + 8 * ((y & 7) + 8 * (z & 7))] = c;
                        }
                w->brick_index[sh->index + i] = b;
            }
    w->nbrick_index += nb;
    w->shape_of[rig * AN_PARTS + part] = w->nshapes;
    return w->nshapes++;
}
static inline int an_voxel(const AnWorld *w, const AnShape *sh, int x, int y, int z) {
    int b = w->brick_index[sh->index + (x >> 3) + sh->nbx * ((y >> 3) + sh->nby * (z >> 3))];
    return b < 0 ? 0 : w->bricks[(size_t)b * 512 + (x & 7) + 8 * ((y & 7) + 8 * (z & 7))];
}

/* hang child from parent's socket at pos, q in the socket's frame; posed after
 * the parent from the next update (or query). 1, or 0 refused: no such figure
 * or socket, a value not finite, a cycle, or the deepest figure hanging from
 * child landing more than AN_DEPTH links down. A child already hanging is moved. */
static inline int an_attach(AnWorld *w, int child, int parent, int socket, const float pos[3], const float q[4]) {
    AnFig *c = an_fig(w, child), *p = an_fig(w, parent);
    if (!c || !p) { an_refuse(w, AN_R_FIGURE); return 0; }
    if (socket < 0 || socket >= w->nsockets[p->rig]) { an_refuse(w, AN_R_SOCKET); return 0; }
    float n[4];
    if (!pos || !q || !an_finite(pos, 3) || !an_finite(q, 4)) { an_refuse(w, AN_R_VALUE); return 0; }
    memcpy(n, q, sizeof n);
    if (!an_qnorm(n)) { an_refuse(w, AN_R_VALUE); return 0; }
    int cs = (int)(c - w->figs), ps = (int)(p - w->figs), dp = 0;
    for (int g = ps; g >= 0; g = w->figs[g].att, dp++)
        if (g == cs) { an_refuse(w, AN_R_ATTACH); return 0; }   /* the parent is the child or hangs from it */
    dp--;                                      /* links above the parent */
    int below = 0;                             /* links from child down to the deepest figure hanging from it */
    for (int s = 0; s < w->capacity && w->nattached; s++) {
        if (w->figs[s].id < 0) continue;
        int k = 0;
        for (int g = s; g >= 0 && k <= AN_DEPTH + 1; g = w->figs[g].att, k++)
            if (g == cs) { if (k > below) below = k; break; }
    }
    if (dp + 1 + below > AN_DEPTH) { an_refuse(w, AN_R_ATTACH); return 0; }
    c->att = ps; c->att_socket = (int16_t)socket;
    memcpy(c->att_pos, pos, sizeof c->att_pos);
    memcpy(c->att_q, n, sizeof c->att_q);
    an_mark(w, c);
    an_recount(w);
    return 1;
}
/* child no longer hangs; it is left at its parent's socket as the chain
 * above it stands now, re-placed from the top (whether or not a query posed
 * any of them since the last update) */
static inline void an_detach(AnWorld *w, int child) {
    AnFig *c = an_fig(w, child);
    if (!c) { an_refuse(w, AN_R_FIGURE); return; }
    if (c->att < 0) return;
    an_place_chain(w, c);
    an_unhang(c);
    an_recount(w);
}

/* ---- queries: the pose before springs, stale figures posed first ---- */

/* pose g at its place (dt 0: springs held, nothing carried changes) and mark what hangs from it stale */
static inline void an_pose_now(AnWorld *w, AnFig *g) {
    if (g->att >= 0) an_attach_place(w, g);
    an_pose(w, g, 0);
    if (w->nattached) {
        int gs = (int)(g - w->figs);
        for (int s = 0; s < w->capacity; s++) if (w->figs[s].id >= 0 && w->figs[s].att == gs) an_mark(w, &w->figs[s]);
    }
}
/* f and the figures it hangs from: from the highest stale one down, posed */
static inline void an_freshen_one(AnWorld *w, AnFig *f) {
    AnFig *chain[AN_DEPTH + 2];
    int n = 0, top = -1;
    for (AnFig *g = f; n < AN_DEPTH + 2; g = &w->figs[g->att]) { chain[n++] = g; if (g->att < 0) break; }
    for (int i = n - 1; i >= 0 && top < 0; i--) if (chain[i]->dirty) top = i;
    if (top >= 0) w->scans += 1;
    for (int i = top; i >= 0; i--) an_pose_now(w, chain[i]);
}
/* every stale figure, and every figure hanging from one, posed; the grid rebuilt if anything moved */
static inline void an_freshen_all(AnWorld *w) {
    if (w->nstale == 0) { if (w->grid_dirty) an_grid_build(w); return; }
    w->scans += 1;
    for (int d = 1; d <= w->max_depth; d++)
        for (int s = 0; s < w->capacity; s++) {
            AnFig *f = &w->figs[s];
            if (f->id >= 0 && f->depth == d && w->figs[f->att].dirty) an_mark(w, f);
        }
    for (int d = 0; d <= w->max_depth; d++)
        for (int s = 0; s < w->capacity; s++) {
            AnFig *f = &w->figs[s];
            if (f->id < 0 || f->depth != d || !f->dirty) continue;
            if (f->att >= 0) an_attach_place(w, f);
            an_pose(w, f, 0);
        }
    if (w->grid_dirty) an_grid_build(w);
}

/* socket of figure fig in world space, from the pose before springs, posing
 * it first if it is stale. 1, or 0 if there is no such figure or socket. */
static inline int an_socket(AnWorld *w, int fig, int socket, float out_pos[3], float out_q[4]) {
    AnFig *f = an_fig(w, fig);
    if (!f) return 0;
    if (socket < 0 || socket >= w->nsockets[f->rig]) { an_refuse(w, AN_R_SOCKET); return 0; }
    an_freshen_one(w, f);
    float q[4], p[3];
    an_socket_frame(w, f, &w->sockets[f->rig * AN_SOCKETS + socket], q, p);
    if (out_pos) memcpy(out_pos, p, sizeof p);
    if (out_q) memcpy(out_q, q, sizeof q);
    return 1;
}

static inline int an_has_springs(const AnWorld *w, const AnFig *f) {
    for (int i = f->proc; i >= 0; i = w->procs[i].next) if (w->procs[i].kind == AN_K_SPRING) return 1;
    return 0;
}
/* f's pose before springs, 12 floats a part: its matrices when it has no
 * springs (they are that pose), else rebuilt into the world's scratch */
static inline const float *an_query_mats(AnWorld *w, const AnFig *f) {
    if (!an_has_springs(w, f)) return an_fig_mats(w, f);
    AnBody b;
    an_build(w, f, &b, 1);
    for (int p = 0; p < w->rigs[f->rig].nparts; p++) an_write_mat(b.wq[p], b.wp[p], w->scratch + p * 12);
    return w->scratch;
}
/* is slot s the ignored slot or hanging from it? */
static inline int an_ignored(const AnWorld *w, int s, int ign) {
    if (ign < 0) return 0;
    for (int k = 0; s >= 0 && k <= AN_DEPTH + 1; s = w->figs[s].att, k++) if (s == ign) return 1;
    return 0;
}
static inline void an_stamp_next(AnWorld *w) {
    if (++w->stamp_now == 0) { memset(w->stamp, 0, (size_t)w->capacity * sizeof *w->stamp); w->stamp_now = 1; }
}
/* the ray o + t d against the box lo..hi: entry tn (on axis), exit tf; 0 if the line misses it */
static inline int an_slab(const float o[3], const float d[3], const float lo[3], const float hi[3], float *tn, float *tf, int *axis) {
    float a = -INFINITY, b = INFINITY;
    int ax = -1;
    for (int k = 0; k < 3; k++) {
        if (d[k] == 0) { if (o[k] < lo[k] || o[k] > hi[k]) return 0; continue; }
        float t1 = (lo[k] - o[k]) / d[k], t2 = (hi[k] - o[k]) / d[k];
        if (t1 > t2) { float t = t1; t1 = t2; t2 = t; }
        if (t1 > a) { a = t1; ax = k; }
        if (t2 < b) b = t2;
    }
    *tn = a; *tf = b; *axis = ax;
    return b >= a;
}

typedef struct { float o[3], u[3], best; int found, ign; AnHit hit; } AnRay;

/* one part's grid: the box in the part's frame, then a 3D DDA voxel by voxel */
static inline void an_ray_part(const AnWorld *w, AnRay *R, int fig, int part, const AnShape *sh, const float *box, const float *m) {
    float rel[3] = {R->o[0] - m[9], R->o[1] - m[10], R->o[2] - m[11]}, g0[3], dg[3];
    int size[3] = {sh->sx, sh->sy, sh->sz};
    if (size[0] == 0 || size[1] == 0 || size[2] == 0) return;
    for (int k = 0; k < 3; k++) {
        g0[k] = (an_dot(m + 3 * k, rel) - box[k]) / sh->voxel;   /* in voxels from the grid's low corner */
        dg[k] = an_dot(m + 3 * k, R->u) / sh->voxel;              /* voxels a metre */
    }
    float zero[3] = {0, 0, 0}, hi[3] = {(float)size[0], (float)size[1], (float)size[2]}, tn, tf;
    int ax;
    if (!an_slab(g0, dg, zero, hi, &tn, &tf, &ax)) return;
    float t = tn > 0 ? tn : 0;
    if (tf < t || t > R->best) return;
    int i[3], step[3];
    float tmax[3], tdel[3];
    for (int k = 0; k < 3; k++) {
        int v = (int)floorf(g0[k] + dg[k] * t);
        i[k] = v < 0 ? 0 : (v >= size[k] ? size[k] - 1 : v);
        if (dg[k] > 0) { step[k] = 1; tmax[k] = (i[k] + 1 - g0[k]) / dg[k]; tdel[k] = 1 / dg[k]; }
        else if (dg[k] < 0) { step[k] = -1; tmax[k] = (i[k] - g0[k]) / dg[k]; tdel[k] = -1 / dg[k]; }
        else { step[k] = 0; tmax[k] = INFINITY; tdel[k] = INFINITY; }
    }
    for (int n = size[0] + size[1] + size[2] + 3; n > 0; n--) {
        int c = an_voxel(w, sh, i[0], i[1], i[2]);
        float vlo[3] = {(float)i[0], (float)i[1], (float)i[2]}, vhi[3] = {vlo[0] + 1, vlo[1] + 1, vlo[2] + 1}, vn, vf;
        int va;
        if (c && an_slab(g0, dg, vlo, vhi, &vn, &vf, &va) && vf >= 0) {
            /* the first filled voxel along the ray: its exact entry */
            float d = vn > 0 ? vn : 0;
            if (d <= R->best) {
                int a = va;
                if (!(vn > 0) || a < 0) {                       /* starting inside it: the face against the ray's main axis */
                    a = 0;
                    for (int k = 1; k < 3; k++) if (fabsf(dg[k]) > fabsf(dg[a])) a = k;
                }
                float sg = dg[a] > 0 ? -1.0f : 1.0f;
                R->best = d; R->found = 1;
                R->hit.fig = fig; R->hit.part = part; R->hit.x = i[0]; R->hit.y = i[1]; R->hit.z = i[2];
                R->hit.colour = c; R->hit.dist = d;
                R->hit.nx = m[3 * a] * sg; R->hit.ny = m[3 * a + 1] * sg; R->hit.nz = m[3 * a + 2] * sg;
            }
            return;
        }
        int a = tmax[0] < tmax[1] ? (tmax[0] < tmax[2] ? 0 : 2) : (tmax[1] < tmax[2] ? 1 : 2);
        if (tmax[a] > tf || tmax[a] > R->best) return;
        i[a] += step[a];
        if (i[a] < 0 || i[a] >= size[a]) return;
        tmax[a] += tdel[a];
    }
}
/* one figure: its bounds, then each part that has a grid */
static inline void an_ray_fig(AnWorld *w, AnRay *R, int s) {
    const AnFig *f = &w->figs[s];
    if (f->id < 0 || an_ignored(w, s, R->ign) || !(f->bmin[0] <= f->bmax[0])) return;
    float tn, tf;
    int ax;
    if (!an_slab(R->o, R->u, f->bmin, f->bmax, &tn, &tf, &ax) || tf < 0 || tn > R->best) return;
    const AnRig *r = &w->rigs[f->rig];
    const float *M = an_query_mats(w, f);
    for (int p = 0; p < r->nparts; p++) {
        int si = w->shape_of[f->rig * AN_PARTS + p];
        if (si >= 0) an_ray_part(w, R, f->id, p, &w->shapes[si], r->box[p], M + p * 12);
    }
}
static inline void an_ray_visit(AnWorld *w, AnRay *R, int s) {
    if (w->stamp[s] == w->stamp_now) return;
    w->stamp[s] = w->stamp_now;
    an_ray_fig(w, R, s);
}

/* the nearest voxel of any figure's parts along o + t d (d normalised; t in
 * metres) for t in [0, max], skipping figure ignore (-1 none) and every figure
 * hanging from it. 1 and *out, or 0 for a miss (or a refused call). */
static inline int an_hit_ray(AnWorld *w, const float o[3], const float d[3], float max, int ignore, AnHit *out) {
    if (!o || !d || !out || !an_finite(o, 3) || !an_finite(d, 3) || isnan(max) || max < 0) { an_refuse(w, AN_R_VALUE); return 0; }
    float dl = an_len(d);
    if (!(dl > 0) || !isfinite(dl)) { an_refuse(w, AN_R_VALUE); return 0; }
    AnRay R;
    memset(&R, 0, sizeof R);
    for (int k = 0; k < 3; k++) { R.o[k] = o[k]; R.u[k] = d[k] / dl; }
    R.best = max; R.ign = ignore >= 0 ? an_slot(w, ignore) : -1;
    an_freshen_all(w);
    an_stamp_next(w);
    for (int i = 0; i < w->nbig; i++) an_ray_visit(w, &R, w->big[i]);
    float tn, tf;
    int ax;
    if (w->ngrid > 0 && an_slab(R.o, R.u, w->ubmin, w->ubmax, &tn, &tf, &ax) && tf >= 0) {
        float t0 = tn > 0 ? tn : 0, t1 = tf < R.best ? tf : R.best;
        if (t0 <= t1) {
            float cells = (fabsf(R.u[0]) + fabsf(R.u[1]) + fabsf(R.u[2])) * (t1 - t0) / AN_CELL + 3;
            if (!(cells <= 4.0f * w->live + 64)) {          /* a long walk: every figure costs less */
                for (int s = 0; s < w->capacity; s++) if (w->figs[s].id >= 0) an_ray_visit(w, &R, s);
            } else {
                /* the cells along the ray, nearest first, until past the best hit */
                int c[3], step[3];
                float tmax[3], tdel[3];
                for (int k = 0; k < 3; k++) {
                    float p = R.o[k] + R.u[k] * t0;
                    c[k] = an_cell_of(p);
                    if (R.u[k] > 0) { step[k] = 1; tmax[k] = ((c[k] + 1) * AN_CELL - R.o[k]) / R.u[k]; tdel[k] = AN_CELL / R.u[k]; }
                    else if (R.u[k] < 0) { step[k] = -1; tmax[k] = (c[k] * AN_CELL - R.o[k]) / R.u[k]; tdel[k] = -AN_CELL / R.u[k]; }
                    else { step[k] = 0; tmax[k] = INFINITY; tdel[k] = INFINITY; }
                }
                for (int n = (int)cells + 3; n > 0; n--) {
                    for (int e = w->grid_head[an_cell_hash(c[0], c[1], c[2]) & w->grid_mask]; e >= 0; e = w->grid_next[e])
                        an_ray_visit(w, &R, w->grid_slot[e]);
                    int a = tmax[0] < tmax[1] ? (tmax[0] < tmax[2] ? 0 : 2) : (tmax[1] < tmax[2] ? 1 : 2);
                    if (tmax[a] > t1 || tmax[a] > R.best) break;
                    c[a] += step[a];
                    tmax[a] += tdel[a];
                }
            }
        }
    }
    if (R.found) *out = R.hit;
    return R.found;
}

/* do two boxes overlap? centres, axes (three columns, unit) and half sizes; separating axes */
static inline int an_obb_overlap(const float ca[3], const float *A, const float a[3], const float cb[3], const float *B, const float b[3]) {
    float R[3][3], AR[3][3], t[3], d[3] = {cb[0] - ca[0], cb[1] - ca[1], cb[2] - ca[2]};
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) { R[i][j] = an_dot(A + 3 * i, B + 3 * j); AR[i][j] = fabsf(R[i][j]) + 1e-6f; }
        t[i] = an_dot(d, A + 3 * i);
    }
    for (int i = 0; i < 3; i++)
        if (fabsf(t[i]) > a[i] + b[0] * AR[i][0] + b[1] * AR[i][1] + b[2] * AR[i][2]) return 0;
    for (int j = 0; j < 3; j++)
        if (fabsf(t[0] * R[0][j] + t[1] * R[1][j] + t[2] * R[2][j]) > a[0] * AR[0][j] + a[1] * AR[1][j] + a[2] * AR[2][j] + b[j]) return 0;
    for (int i = 0; i < 3; i++) {
        int i1 = (i + 1) % 3, i2 = (i + 2) % 3;
        for (int j = 0; j < 3; j++) {
            int j1 = (j + 1) % 3, j2 = (j + 2) % 3;
            float ra = a[i1] * AR[i2][j] + a[i2] * AR[i1][j], rb = b[j1] * AR[i][j2] + b[j2] * AR[i][j1];
            if (fabsf(t[i2] * R[i1][j] - t[i1] * R[i2][j]) > ra + rb) return 0;
        }
    }
    return 1;
}

/* every (figure id, part) whose part box (parts with volume) overlaps the box
 * at c of half sizes half turned by q, skipping figure ignore and what hangs
 * from it; in slot order, then part order. Writes at most max pairs into
 * figs_parts and answers how many there are (0 if refused). */
static inline int an_hit_box(AnWorld *w, const float c[3], const float half[3], const float q[4], int ignore, int max, int32_t *figs_parts) {
    if (!c || !half || !q || !an_finite(c, 3) || !an_finite(half, 3) || !an_finite(q, 4) || half[0] < 0 || half[1] < 0 || half[2] < 0 ||
        max < 0 || (max > 0 && !figs_parts)) { an_refuse(w, AN_R_VALUE); return 0; }
    float qn[4] = {q[0], q[1], q[2], q[3]}, Q[9], lo[3], hi[3];
    if (!an_qnorm(qn)) { an_refuse(w, AN_R_VALUE); return 0; }
    an_qmat(qn, Q);
    for (int i = 0; i < 3; i++) {
        float e = fabsf(Q[i]) * half[0] + fabsf(Q[3 + i]) * half[1] + fabsf(Q[6 + i]) * half[2];
        lo[i] = c[i] - e; hi[i] = c[i] + e;
    }
    int ign = ignore >= 0 ? an_slot(w, ignore) : -1;
    an_freshen_all(w);
    an_stamp_next(w);
    /* the candidates: the big list and the figures in the cells the box's bounds touch, in slot order */
    int nc = 0;
    for (int i = 0; i < w->nbig; i++) { w->stamp[w->big[i]] = w->stamp_now; w->cand[nc++] = w->big[i]; }
    int c0[3], c1[3];
    double cells = 1;
    for (int k = 0; k < 3; k++) { c0[k] = an_cell_of(lo[k]); c1[k] = an_cell_of(hi[k]); cells *= (double)c1[k] - c0[k] + 1; }
    if (cells > 64 && cells > w->live) {
        nc = 0;
        for (int s = 0; s < w->capacity; s++) if (w->figs[s].id >= 0) w->cand[nc++] = s;
    } else {
        for (int z = c0[2]; z <= c1[2]; z++)
            for (int y = c0[1]; y <= c1[1]; y++)
                for (int x = c0[0]; x <= c1[0]; x++)
                    for (int e = w->grid_head[an_cell_hash(x, y, z) & w->grid_mask]; e >= 0; e = w->grid_next[e]) {
                        int s = w->grid_slot[e];
                        if (w->stamp[s] != w->stamp_now) { w->stamp[s] = w->stamp_now; w->cand[nc++] = s; }
                    }
        for (int i = 1; i < nc; i++) {                  /* few candidates: insertion sort */
            int32_t v = w->cand[i];
            int j = i;
            for (; j > 0 && w->cand[j - 1] > v; j--) w->cand[j] = w->cand[j - 1];
            w->cand[j] = v;
        }
    }
    int n = 0;
    for (int ci = 0; ci < nc; ci++) {
        int s = w->cand[ci];
        const AnFig *f = &w->figs[s];
        if (f->id < 0 || an_ignored(w, s, ign)) continue;
        if (!(f->bmin[0] <= f->bmax[0]) || f->bmin[0] > hi[0] || f->bmin[1] > hi[1] || f->bmin[2] > hi[2] ||
            f->bmax[0] < lo[0] || f->bmax[1] < lo[1] || f->bmax[2] < lo[2]) continue;
        const AnRig *r = &w->rigs[f->rig];
        const float *M = an_query_mats(w, f);
        for (int p = 0; p < r->nparts; p++) {
            if (!an_has_box(r, p)) continue;
            const float *m = M + p * 12, *bx = r->box[p];
            float lc[3], h[3], wc[3];
            for (int k = 0; k < 3; k++) { lc[k] = (bx[k] + bx[3 + k]) * 0.5f; h[k] = (bx[3 + k] - bx[k]) * 0.5f; }
            for (int k = 0; k < 3; k++) wc[k] = m[9 + k] + m[k] * lc[0] + m[3 + k] * lc[1] + m[6 + k] * lc[2];
            if (!an_obb_overlap(c, Q, half, wc, m, h)) continue;
            if (n < max) { figs_parts[n * 2] = f->id; figs_parts[n * 2 + 1] = p; }
            n++;
        }
    }
    return n;
}

/* ---- the view, always, traced, and drawing between the last two poses ---- */

/* the camera: planes (6 of ax + by + cz + d >= 0 inside; NULL for none: all
 * in view), the eye (NULL for none) and the distance past which a figure is
 * far (0, below or infinite for none). Used from the next update; refused,
 * changing nothing, if a value is not finite or a plane has no normal. */
static inline void an_view(AnWorld *w, const float planes[24], const float eye[3], float far_metres) {
    if ((planes && !an_finite(planes, 24)) || (eye && !an_finite(eye, 3)) || isnan(far_metres)) { an_refuse(w, AN_R_VALUE); return; }
    float pn[24];
    if (planes)
        for (int i = 0; i < 6; i++) {
            float l = sqrtf(planes[4 * i] * planes[4 * i] + planes[4 * i + 1] * planes[4 * i + 1] + planes[4 * i + 2] * planes[4 * i + 2]);
            if (!(l > 1e-12f) || !isfinite(l)) { an_refuse(w, AN_R_VALUE); return; }
            for (int k = 0; k < 4; k++) pn[4 * i + k] = planes[4 * i + k] / l;
        }
    w->nplanes = planes ? 6 : 0;
    if (planes) memcpy(w->planes, pn, sizeof pn);
    w->has_far = eye && far_metres > 0 && isfinite(far_metres);
    if (eye) memcpy(w->eye, eye, sizeof w->eye);
    w->far_m = w->has_far ? far_metres : 0;
}
/* figure fig posed every update, in view or not, near or far (its springs run) */
static inline void an_always(AnWorld *w, int fig, int on) {
    AnFig *f = an_fig(w, fig);
    if (!f) { an_refuse(w, AN_R_FIGURE); return; }
    f->always = on != 0;
}
/* f no longer traced; a figure that held a place gives it to the waiting one in the lowest slot */
static inline void an_untrace(AnWorld *w, AnFig *f) {
    if (f->traced == 2) w->nwaiting--;
    if (f->traced == 1) {
        w->ntraced--;
        for (int s = 0; s < w->capacity && w->nwaiting > 0; s++)
            if (w->figs[s].id >= 0 && w->figs[s].traced == 2) { w->figs[s].traced = 1; w->nwaiting--; w->ntraced++; break; }
    }
    f->traced = 0;
}
/* figure fig drawn traced (a flag for the renderer: the draw list's bit 0).
 * Past AN_TRACED figures one waits, counted in stat 9 and drawn plain, until
 * a traced one is let go. */
static inline void an_traced(AnWorld *w, int fig, int on) {
    AnFig *f = an_fig(w, fig);
    if (!f) { an_refuse(w, AN_R_FIGURE); return; }
    if (!on) { an_untrace(w, f); return; }
    if (f->traced) return;
    if (w->ntraced < AN_TRACED) { f->traced = 1; w->ntraced++; }
    else { f->traced = 2; w->nwaiting++; }
}

/* how far along from the pose before last to the last pose f is drawn at
 * blend 0..1: a figure posed every update, blend itself; one posed every
 * span updates walks the span over that many updates, (since + blend) /
 * span, so its drawn motion runs on, never back, one period behind */
static inline float an_draw_u(const AnFig *f, float blend) {
    float b = blend > 0 ? (blend < 1 ? blend : 1) : 0;
    if (f->dspan <= 1) return f->dsince ? 1 : b;
    float u = ((float)f->dsince + b) / (float)f->dspan;
    return u < 1 ? u : 1;
}
/* figure fig drawn between the pose before its last and its last, at blend
 * 0..1 of the time since the last update; 12 floats a part, valid until the
 * next call. A figure posed every update draws its last two poses at blend
 * (0 and 1 exactly); one the schedule poses every few updates is stretched
 * over that period (an_draw_u). NULL if there is no such figure. */
static inline const float *an_draw_matrices(AnWorld *w, int fig, float blend) {
    AnFig *f = an_fig(w, fig);
    if (!f) return NULL;
    const float *a = an_fig_prev(w, f), *b = an_fig_mats(w, f);
    float u = an_draw_u(f, blend);
    for (int p = 0; p < w->rigs[f->rig].nparts; p++) an_mat_mix(a + p * 12, b + p * 12, u, w->dscratch + p * 12);
    return w->dscratch;
}
/* the sun the drawing casts shadows by (the direction toward it; NULL, or
 * one at or below 0.01 up, for none). Drawing only: it widens the draw list
 * to figures out of view whose shadows may fall in it, and changes no
 * answer, no hash and no figure's posing class. Refused, changing nothing,
 * if a value is not finite. */
static inline void an_draw_sun(AnWorld *w, const float sun[3]) {
    if (sun && !an_finite(sun, 3)) { an_refuse(w, AN_R_VALUE); return; }
    float l = sun ? sqrtf(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]) : 0;
    w->has_dsun = sun && l > 1e-12f && sun[1] / l > 0.01f;
    for (int k = 0; k < 3; k++) w->dsun[k] = w->has_dsun ? sun[k] / l : 0;
}
/* is f, out of view, near enough that its sun shadow may fall in view: its
 * bounds swept away from the sun until their top has come down by their
 * height, but at most AN_SHADOW_REACH metres along the ground, reach within
 * AN_MARGIN of the inside of every plane */
static inline int an_shadow_in_view(const AnWorld *w, const AnFig *f) {
    if (!w->nplanes || !w->has_dsun) return 0;
    float lo[3], hi[3];
    an_cull_box(f, lo, hi);
    const float *s = w->dsun;
    float flat = sqrtf(s[0] * s[0] + s[2] * s[2]);
    float t = (hi[1] - lo[1]) / s[1];                         /* along -sun, down by the height */
    if (flat * t > AN_SHADOW_REACH) t = AN_SHADOW_REACH / flat;
    float sweep[3] = { -s[0] * t, -s[1] * t, -s[2] * t };
    for (int i = 0; i < w->nplanes; i++) {
        const float *pl = w->planes + 4 * i;
        float d = pl[3], along = 0;
        for (int k = 0; k < 3; k++) { d += pl[k] * (pl[k] > 0 ? hi[k] : lo[k]); along += pl[k] * sweep[k]; }
        if (along > 0) d += along;                             /* the swept box's corner furthest inside */
        if (d < -AN_MARGIN) return 0;
    }
    return 1;
}
/* the renderer's source: every part with volume of every figure in view now
 * (or always posed), in slot then part order, drawn at blend as
 * an_draw_matrices draws it: rig x AN_PARTS + part, its 3x4 and its flags
 * (bit 0 traced; bit 1 out of view, listed only because its sun shadow may
 * fall in view (an_draw_sun): for the shadow and the rays, not to be drawn;
 * the figure's id from bit 8). Answers how many; the arrays are the world's,
 * valid until the next call. Past AN_DRAW_MAX parts the rest are left out,
 * counted in stat 10. */
static inline int an_draw_list(AnWorld *w, float blend, const int32_t **rig_part, const float **xf, const int32_t **flags) {
    int n = 0;
    w->draw_over = 0;
    for (int s = 0; s < w->capacity; s++) {
        const AnFig *f = &w->figs[s];
        if (f->id < 0) continue;
        int shade = 0;
        if (!(f->always || an_in_view(w, f))) { if (!an_shadow_in_view(w, f)) continue; shade = 2; }
        const AnRig *r = &w->rigs[f->rig];
        const float *a = an_fig_prev(w, f), *b = an_fig_mats(w, f);
        float u = an_draw_u(f, blend);
        for (int p = 0; p < r->nparts; p++) {
            if (!an_has_box(r, p)) continue;
            if (n >= AN_DRAW_MAX) { w->draw_over += 1; continue; }
            an_mat_mix(a + p * 12, b + p * 12, u, w->dl_xf + (size_t)n * 12);
            w->dl_rp[n] = f->rig * AN_PARTS + p;
            w->dl_flags[n] = (f->traced == 1 ? 1 : 0) | shade | (f->id << 8);
            n++;
        }
    }
    if (rig_part) *rig_part = w->dl_rp;
    if (xf) *xf = w->dl_xf;
    if (flags) *flags = w->dl_flags;
    return n;
}

/* ---- replacing and freeing rigs ---- */
/* the rig's part grids dropped and the pools closed up behind them: shapes and
 * their bricks are laid down in the order made, so one pass forward moves
 * every kept one down in place */
static inline void an_drop_shapes(AnWorld *w, int rig) {
    int32_t remap[AN_SHAPES];
    int ns = 0, nbi = 0, nb = 0;
    for (int s = 0; s < w->nshapes; s++) {
        int owned = 0;
        for (int p = 0; p < AN_PARTS && !owned; p++) owned = w->shape_of[rig * AN_PARTS + p] == s;
        AnShape sh = w->shapes[s];
        int n = sh.nbx * sh.nby * sh.nbz;
        if (owned) { remap[s] = -1; continue; }
        for (int i = 0; i < n; i++) {
            int b = w->brick_index[sh.index + i];
            if (b >= 0) {
                if (b != nb) memmove(w->bricks + (size_t)nb * 512, w->bricks + (size_t)b * 512, 512);
                b = nb++;
            }
            w->brick_index[nbi + i] = b;
        }
        sh.index = nbi;
        nbi += n;
        w->shapes[ns] = sh;
        remap[s] = ns++;
    }
    for (int p = 0; p < AN_PARTS; p++) w->shape_of[rig * AN_PARTS + p] = -1;
    for (int i = 0; i < AN_RIGS * AN_PARTS; i++) if (w->shape_of[i] >= 0) w->shape_of[i] = remap[w->shape_of[i]];
    w->nshapes = ns; w->nbrick_index = nbi; w->nbricks = nb;
    w->grid_dirty = 1;
}
static inline int an_rig_users(const AnWorld *w, int rig) {
    int n = 0;
    for (int s = 0; s < w->capacity; s++) if (w->figs[s].id >= 0 && w->figs[s].rig == rig) n++;
    return n;
}
/* rig [rig] replaced by a new description under the same id: its grids and
 * sockets are dropped (upload them again); each of its figures keeps its
 * placement and drop, loses its layers, procedural records and what hangs
 * from it (unhung where it stands), and is posed at rest. Answers the rig, or
 * a refusal. */
static inline int an_rig_replace(AnWorld *w, int rig, int nparts, const int32_t *parent, const float *pivot,
                                 const float *rest_q, const float *box) {
    if (!an_rig_ok(w, rig)) return an_refuse(w, AN_R_RIG);
    int bad = an_rig_check(nparts, parent, pivot, rest_q, box);
    if (bad) return an_refuse(w, bad);
    an_drop_shapes(w, rig);
    w->nsockets[rig] = 0;
    an_rig_write(&w->rigs[rig], nparts, parent, pivot, rest_q, box);
    for (int s = 0; s < w->capacity; s++) {
        AnFig *f = &w->figs[s];
        if (f->id < 0 || f->rig != rig) continue;
        if (w->nattached) {
            for (int c = 0; c < w->capacity; c++) {
                AnFig *ch = &w->figs[c];
                if (ch->id >= 0 && ch->att == s) { an_place_chain(w, ch); an_unhang(ch); }
            }
        }
        for (int i = f->proc; i >= 0; i = w->procs[i].next) w->proc_free[w->nproc_free++] = i;
        f->proc = -1;
        memset(f->layer, 0, sizeof f->layer);
        f->unposed = 1;
        an_settle(w, f);
    }
    if (w->nattached) an_recount(w);
    w->grid_dirty = 1;
    return rig;
}
/* rig [rig] freed: its grids and sockets dropped and its id free for the next
 * rig made. 1, or 0 refused (no such rig, or figures still use it). Its masks
 * and clips are not reclaimed. */
static inline int an_rig_free(AnWorld *w, int rig) {
    if (!an_rig_ok(w, rig)) { an_refuse(w, AN_R_RIG); return 0; }
    if (an_rig_users(w, rig) > 0) { an_refuse(w, AN_R_VALUE); return 0; }
    an_drop_shapes(w, rig);
    w->nsockets[rig] = 0;
    w->rig_live[rig] = 0;
    w->rig_free[w->nrig_free++] = rig;
    return 1;
}

/* 0 figures, 1 posed by the last update, 2 stale, 3 refused calls, 4 negative dt, 5 last refusal reason,
 * 6 marks dropped, 7 procedural records in use, 8 passes queries made over the figures to pose stale ones,
 * 9 figures traced past the cap of AN_TRACED (drawn plain), 10 parts left out of the last draw list,
 * 11 figures the last update did not pose (out of view or far), 12 live rigs, 13 part grids held, 14 bricks used */
static inline double an_stat(const AnWorld *w, int which) {
    switch (which) {
    case 0: return w->live;
    case 1: return w->posed;
    case 2: return w->nstale;
    case 3: return w->refused;
    case 4: return w->negative_dt;
    case 5: return w->reason;
    case 6: return w->dropped;
    case 7: return w->nprocs - w->nproc_free;
    case 8: return w->scans;
    case 9: return w->nwaiting;
    case 10: return w->draw_over;
    case 11: return w->skipped;
    case 12: { int n = 0; for (int i = 0; i < w->nrigs; i++) n += w->rig_live[i]; return n; }
    case 13: return w->nshapes;
    case 14: return w->nbricks;
    default: return 0;
    }
}

#endif

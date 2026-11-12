/* strata_ent.c — binds the entity kernel (strata_ent.h) to March. One
 * world per process, opened by en_x_open and freed by en_x_close. Knows
 * nothing of Vulkan, GLFW, physics or audio.
 *
 * March values: a NativeArray arrives as its heap pointer, element count at
 * +16, data at +32 (8 bytes an element for NativeIntArr and NativeFloatArr,
 * 4 for NativeI32Arr and NativeF32Arr, 1 for NativeU8Arr). Inputs are only
 * read; en_x_each, en_x_column, en_x_ray, en_x_events, en_x_near,
 * en_x_ray_entities, en_x_sweep, en_x_releases, en_x_seen_parts,
 * en_x_wanting, en_x_holding and en_x_snapshot fill a caller-owned
 * array, as strata_anim.c's matrices do. Reads of three numbers come back one
 * number a call (en_x_get's k). Calls March makes only for their effect
 * answer an Int it can use (GAPS S9).
 *
 * With no world open every call does nothing and answers its default
 * (0, -1 for a kind or an id, 0.0). */
/* Rewritten by stamp_ent.sh whenever strata_ent.h, strata_ent_grid.h, strata_ent_move.h, strata_ent_seen.h or strata_ent_snap.h
 * changes (GAPS S14). */
#define STRATA_ENT_STAMP "f72b732b76f1ee00"
#include <stdint.h>
#include <time.h>
#include "strata_ent.h"

static EnWorld *g_w;

static int32_t *en_x_i32(void *a) { return a ? (int32_t *)((char *)a + 32) : NULL; }
static double *en_x_f64(void *a) { return a ? (double *)((char *)a + 32) : NULL; }
static const uint8_t *en_x_u8(void *a) { return a ? (const uint8_t *)((char *)a + 32) : NULL; }
static float *en_x_f32(void *a) { return a ? (float *)((char *)a + 32) : NULL; }
static int64_t *en_x_i64(void *a) { return a ? (int64_t *)((char *)a + 32) : NULL; }
static int64_t en_x_len(void *a) { return a ? *(const int64_t *)((char *)a + 16) : 0; }
static int en_x_kind_ok(int64_t kind) { return g_w && kind >= 0 && kind < g_w->nkinds; }

int64_t en_x_open(int64_t capacity, int64_t route_points, int64_t bodies, double seen_cap, double chunk_metres, double voxel_metres) {
    if (g_w) en_close(g_w);
    g_w = NULL;
    if (capacity < 1 || capacity > EN_CAPACITY_MAX || route_points < 1 || route_points > EN_CAPACITY_MAX ||
        bodies < 1 || bodies > EN_CAPACITY_MAX) return 0;
    g_w = en_open((int)capacity, (int)route_points, (int)bodies, seen_cap, chunk_metres, voxel_metres);
    return g_w ? 1 : 0;
}
void en_x_close(void) { en_close(g_w); g_w = NULL; }

/* ints: 9 in en_kind's order; floats: 7 in en_kind's order. The kind's index, or -1. */
int64_t en_x_kind(void *ints, void *floats) {
    if (!g_w) return -1;
    if (en_x_len(ints) < EN_K_INTS || en_x_len(floats) < EN_K_FLOATS) return en_refuse(g_w, EN_R_VALUE);
    return en_kind(g_w, en_x_i32(ints), en_x_f32(floats));
}

int64_t en_x_add(int64_t kind, double x, double y, double z, double facing) {
    if (!g_w) return -1;
    if (kind < 0 || kind >= EN_KINDS) return en_refuse(g_w, EN_R_KIND);
    return en_add(g_w, (int)kind, x, y, z, facing);
}
void en_x_remove(int64_t id) { if (g_w) en_remove(g_w, id, 0); }
int64_t en_x_alive(int64_t id) { return g_w ? en_alive(g_w, id) : 0; }
int64_t en_x_kind_of(int64_t id) { return g_w ? en_kind_of(g_w, id) : -1; }

/* which: 0 position, 1 velocity, 2 facing, 3 age; component k (0..2; 0 for facing and age). 0 for a dead id. */
double en_x_get(int64_t id, int64_t which, int64_t k) {
    if (!g_w || which < 0 || which > 3 || k < 0 || k > 2) return 0;
    double v[3] = {0, 0, 0};
    en_get(g_w, id, (int)which, v);
    return v[k];
}
/* which: 0 position, 1 velocity (x, y, z); 2 facing (x) */
void en_x_set(int64_t id, int64_t which, double x, double y, double z) {
    if (!g_w) return;
    double v[3] = {x, y, z};
    en_set(g_w, id, which < 0 || which > 3 ? -1 : (int)which, v);
}

double en_x_float(int64_t id, int64_t n) { return g_w && n >= 0 && n < EN_SLOTS ? en_float(g_w, id, (int)n) : 0; }
void en_x_set_float(int64_t id, int64_t n, double v) { if (g_w) en_set_float(g_w, id, n < 0 || n >= EN_SLOTS ? -1 : (int)n, v); }
int64_t en_x_int(int64_t id, int64_t n) { return g_w && n >= 0 && n < EN_SLOTS ? en_int(g_w, id, (int)n) : 0; }
void en_x_set_int(int64_t id, int64_t n, int64_t v) { if (g_w) en_set_int(g_w, id, n < 0 || n >= EN_SLOTS ? -1 : (int)n, v); }

/* how many live entities kind has; 0 for no such kind */
int64_t en_x_count(int64_t kind) { return en_x_kind_ok(kind) ? g_w->kinds[kind].live : 0; }
/* kind's live ids in dense order into a caller-owned NativeIntArr; how many written */
int64_t en_x_each(int64_t kind, void *out) {
    if (!en_x_kind_ok(kind) || !out) return 0;
    int64_t n = en_x_len(out);
    return en_each(g_w, (int)kind, n > EN_CAPACITY_MAX ? EN_CAPACITY_MAX : (int)n, en_x_i64(out));
}
/* floats a row of column col of kind; 0 for no such kind or column */
int64_t en_x_column_width(int64_t kind, int64_t col) {
    if (!en_x_kind_ok(kind) || col < 0 || col > 10 + EN_SLOTS) return 0;
    return en_column_width(g_w, (int)kind, (int)col);
}
/* a whole column into a caller-owned NativeF32Arr of rows x width floats; how many rows written */
int64_t en_x_column(int64_t kind, int64_t col, void *out) {
    int64_t width = en_x_column_width(kind, col);
    if (width == 0 || !out) return 0;
    int64_t rows = en_x_len(out) / width;
    return en_column(g_w, (int)kind, (int)col, rows > EN_CAPACITY_MAX ? EN_CAPACITY_MAX : (int)rows, en_x_f32(out));
}
/* a whole column from a NativeF32Arr of exactly live x width floats, in en_each order */
void en_x_set_column(int64_t kind, int64_t col, void *in) {
    if (!g_w) return;
    if (!en_x_kind_ok(kind)) { en_refuse(g_w, EN_R_KIND); return; }
    int64_t width = en_x_column_width(kind, col), len = en_x_len(in);
    if (width == 0) { en_refuse(g_w, EN_R_WHICH); return; }
    if (len % width != 0 || len / width > EN_CAPACITY_MAX) { en_refuse(g_w, EN_R_ROWS); return; }
    en_set_column(g_w, (int)kind, (int)col, (int)(len / width), en_x_f32(in));
}

/* the hash, folded to 62 bits so it is the same number as a March Int */
int64_t en_x_hash(void) { return g_w ? (int64_t)(en_hash(g_w) & 0x3fffffffffffffffull) : 0; }
double en_x_stat(int64_t which) { return g_w ? en_stat(g_w, (int)which) : 0; }

/* ---- blocks ---- */
static int en_x_chunk_ok(int64_t cx, int64_t cy, int64_t cz) {
    return cx >= -EN_CHUNK_COORD_MAX && cx <= EN_CHUNK_COORD_MAX && cy >= -EN_CHUNK_COORD_MAX && cy <= EN_CHUNK_COORD_MAX &&
           cz >= -EN_CHUNK_COORD_MAX && cz <= EN_CHUNK_COORD_MAX;
}
/* which block ids stop, a byte an id (NativeU8Arr, at most 65,536); answers how many ids were taken, or 0 */
int64_t en_x_block_table(void *collides) {
    if (!g_w) return 0;
    int64_t n = en_x_len(collides);
    if (n > EN_BLOCK_IDS) { en_refuse(g_w, EN_R_VALUE); return 0; }
    en_block_table(g_w, (int)n, en_x_u8(collides));
    return n;
}
/* hold a chunk from its 32,768 ids (NativeI32Arr, Coord.index order); 1 if held, 0 if refused */
int64_t en_x_chunk(int64_t cx, int64_t cy, int64_t cz, void *ids) {
    if (!g_w) return 0;
    if (!en_x_chunk_ok(cx, cy, cz) || en_x_len(ids) != 32768) { en_refuse(g_w, EN_R_VALUE); return 0; }
    double before = g_w->refused;
    en_chunk(g_w, (int)cx, (int)cy, (int)cz, en_x_i32(ids));
    return g_w->refused == before;
}
int64_t en_x_chunk_uniform(int64_t cx, int64_t cy, int64_t cz, int64_t id) {
    if (!g_w) return 0;
    if (!en_x_chunk_ok(cx, cy, cz)) { en_refuse(g_w, EN_R_VALUE); return 0; }
    double before = g_w->refused;
    en_chunk_uniform(g_w, (int)cx, (int)cy, (int)cz, id < 0 || id >= EN_BLOCK_IDS ? -1 : (int)id);
    return g_w->refused == before;
}
/* answers 1 */
int64_t en_x_chunk_drop(int64_t cx, int64_t cy, int64_t cz) {
    if (g_w && en_x_chunk_ok(cx, cy, cz)) en_chunk_drop(g_w, (int)cx, (int)cy, (int)cz);
    return 1;
}
/* 1 solid, 0 clear, -1 not held (or no world) */
int64_t en_x_solid(int64_t vx, int64_t vy, int64_t vz) {
    int64_t lim = (int64_t)EN_CHUNK_COORD_MAX * 32;
    if (!g_w || vx < -lim || vx >= lim || vy < -lim || vy >= lim || vz < -lim || vz >= lim) return -1;
    return en_solid(g_w, (int)vx, (int)vy, (int)vz);
}
/* en_ray: the distance in metres, -1.0 a miss, -2.0 the ray left the held chunks; out (NativeI32Arr of at
 * least 4) gets the voxel and the face on a hit */
double en_x_ray(double ox, double oy, double oz, double dx, double dy, double dz, double max, void *out) {
    if (!g_w || en_x_len(out) < 4) return -1.0;
    double o[3] = {ox, oy, oz}, d[3] = {dx, dy, dz}, t = 0;
    int32_t hit[4];
    int r = en_ray(g_w, o, d, max, hit, &t);
    if (r == 1) { memcpy(en_x_i32(out), hit, sizeof hit); return t; }
    return r == 0 ? -1.0 : -2.0;
}

/* ---- the tick and events ---- */
/* en_tick with the focus points as a NativeFloatArr of 3 doubles each; answers the events written */
int64_t en_x_tick(void *focus, double dt) {
    if (!g_w) return 0;
    int64_t n = en_x_len(focus) / 3;
    en_tick(g_w, n > EN_CAPACITY_MAX ? EN_CAPACITY_MAX : (int)n, en_x_f64(focus), dt);
    return g_w->nev;
}
int64_t en_x_event_count(void) { return g_w ? g_w->nev : 0; }
/* the last tick's events into a NativeIntArr (7 each) and a NativeFloatArr (3 each); how many copied */
int64_t en_x_events(void *ints, void *floats) {
    if (!g_w || !ints || !floats) return 0;
    int64_t a = en_x_len(ints) / 7, b = en_x_len(floats) / 3, n = a < b ? a : b;
    return en_events(g_w, n > EN_EVENTS ? EN_EVENTS : (int)n, en_x_i64(ints), en_x_f64(floats));
}

/* ---- the grid: touch pairs, near, the entity ray ---- */
/* ask for touches between two kinds; 1 if taken, 0 if refused */
int64_t en_x_touching(int64_t kind_a, int64_t kind_b) {
    if (!g_w) return 0;
    if (!en_x_kind_ok(kind_a) || !en_x_kind_ok(kind_b)) { en_refuse(g_w, EN_R_KIND); return 0; }
    en_touching(g_w, (int)kind_a, (int)kind_b);
    return 1;
}
/* en_near into a caller-owned NativeIntArr, sorted by id: how many written; with an empty array, how many there are */
int64_t en_x_near(double x, double y, double z, double radius, int64_t mask, void *out) {
    if (!g_w) return 0;
    double p[3] = {x, y, z};
    int64_t n = en_x_len(out);
    if (n <= 0) return en_near(g_w, p, radius, (uint64_t)mask, 0, NULL);
    return en_near(g_w, p, radius, (uint64_t)mask, n > EN_CAPACITY_MAX ? EN_CAPACITY_MAX : (int)n, en_x_i64(out));
}
/* en_ray_entities: the id, or -1; on a hit the distance in metres into out[0] (a NativeFloatArr of at least 1) */
int64_t en_x_ray_entities(double ox, double oy, double oz, double dx, double dy, double dz, double max, int64_t mask, void *out) {
    if (!g_w || en_x_len(out) < 1) return -1;
    double o[3] = {ox, oy, oz}, d[3] = {dx, dy, dz}, t = 0;
    int64_t id = en_ray_entities(g_w, o, d, max, (uint64_t)mask, &t);
    if (id >= 0) en_x_f64(out)[0] = t;
    return id;
}
/* ---- the sweep, routes and walking ---- */
/* en_sweep: 0, or -1 (a chunk not held on the way, or a number refused: nothing moved); out (a NativeFloatArr of at
 * least 6) gets the end x, y, z and 1.0 or 0.0 for each axis blocked (x, y, z) */
int64_t en_x_sweep(double px, double py, double pz, double hx, double hy, double hz, double dx, double dy, double dz, void *out) {
    if (!g_w || en_x_len(out) < 6) return -1;
    double p[3] = {px, py, pz}, h[3] = {hx, hy, hz}, d[3] = {dx, dy, dz}, q[3];
    int b[3];
    int r = en_sweep(g_w, p, h, d, q, b);
    double *o = en_x_f64(out);
    for (int a = 0; a < 3; a++) { o[a] = q[a]; o[3 + a] = b[a] ? 1.0 : 0.0; }
    return r;
}
/* a route from a NativeFloatArr of 3 doubles a point: its id, or -1 */
int64_t en_x_route(void *points) {
    if (!g_w) return -1;
    int64_t len = en_x_len(points);
    if (len % 3 != 0 || len / 3 > EN_ROUTE_MAX) return en_refuse(g_w, EN_R_VALUE);
    return en_route(g_w, (int)(len / 3), en_x_f64(points));
}
static int en_x_route_id(int64_t route) { return route < -1 || route > EN_CAPACITY_MAX ? -2 : (int)route; }
/* answers 1 */
int64_t en_x_route_free(int64_t route) { if (g_w) en_route_free(g_w, en_x_route_id(route)); return 1; }
/* answers 1 */
int64_t en_x_follow(int64_t id, int64_t route, double speed, int64_t loops) {
    if (g_w) en_follow(g_w, id, en_x_route_id(route), speed, loops == 0 || loops == 1 ? (int)loops : 2);
    return 1;
}
int64_t en_x_grounded(int64_t id) { return g_w ? en_grounded(g_w, id) : 0; }
/* 1 if the entity was seen at the end of the last tick */
int64_t en_x_seen(int64_t id) { return g_w ? en_seen(g_w, id) : 0; }

/* ---- parts ---- */
/* part (0 figure, 1 body, 2 voice 0, 3 voice 1) of entity id; -1 for none */
int64_t en_x_part(int64_t id, int64_t part) { return g_w && part >= 0 && part < EN_PARTS ? en_part(g_w, id, (int)part) : -1; }
/* answers 1 */
int64_t en_x_set_part(int64_t id, int64_t part, int64_t v) {
    if (g_w) en_set_part(g_w, id, part < 0 || part >= EN_PARTS ? -1 : (int)part, v);
    return 1;
}
/* answers 1 */
int64_t en_x_figure_refused(int64_t kind) {
    if (g_w) en_figure_refused(g_w, kind < 0 || kind >= EN_KINDS ? -1 : (int)kind);
    return 1;
}
int64_t en_x_release_count(void) { return g_w ? g_w->nrel : 0; }
/* drains the release list into a NativeI32Arr of codes and a NativeIntArr of ids, as many as both hold; how many */
int64_t en_x_releases(void *codes, void *ids) {
    if (!g_w || !codes || !ids) return 0;
    int64_t a = en_x_len(codes), b = en_x_len(ids), n = a < b ? a : b;
    return en_releases(g_w, n > INT32_MAX ? INT32_MAX : (int)n, en_x_i32(codes), en_x_i64(ids));
}
/* en_seen_parts into a NativeI32Arr of part ids and a NativeF32Arr of 7 floats each: how many written; with an empty id
 * array, how many there are */
int64_t en_x_seen_parts(int64_t part, void *ids, void *poses) {
    if (!g_w || part < 0 || part >= EN_PARTS) return 0;
    int64_t a = en_x_len(ids), b = en_x_len(poses) / 7, n = a < b ? a : b;
    if (a <= 0) return en_seen_parts(g_w, (int)part, 0, NULL, NULL);
    return en_seen_parts(g_w, (int)part, n > EN_CAPACITY_MAX ? EN_CAPACITY_MAX : (int)n, en_x_i32(ids), en_x_f32(poses));
}
/* en_wanting into a NativeIntArr: how many written; with an empty array, how many there are */
int64_t en_x_wanting(int64_t part, void *ids) {
    if (!g_w || part < 0 || part >= EN_PARTS) return 0;
    int64_t n = en_x_len(ids);
    if (n <= 0) return en_wanting(g_w, (int)part, 0, NULL);
    return en_wanting(g_w, (int)part, n > EN_CAPACITY_MAX ? EN_CAPACITY_MAX : (int)n, en_x_i64(ids));
}
/* en_holding into a NativeIntArr: the seen entities holding part, how many written; with an empty array, how many there are */
int64_t en_x_holding(int64_t part, void *ids) {
    if (!g_w || part < 0 || part >= EN_PARTS) return 0;
    int64_t n = en_x_len(ids);
    if (n <= 0) return en_holding(g_w, (int)part, 0, NULL);
    return en_holding(g_w, (int)part, n > EN_CAPACITY_MAX ? EN_CAPACITY_MAX : (int)n, en_x_i64(ids));
}
/* answers 1 */
int64_t en_x_body_lost(int64_t id) { if (g_w) en_body_lost(g_w, id); return 1; }
/* answers 1 */
int64_t en_x_body_over(int64_t n) {
    if (g_w) en_body_over(g_w, n < 0 ? -1 : n > INT32_MAX ? INT32_MAX : (int)n);
    return 1;
}

/* ---- snapshot and restore ---- */
/* the ints (which 0), floats (1) or doubles (2) a snapshot needs; 0 with no world */
int64_t en_x_snapshot_size(int64_t which) {
    if (!g_w || which < 0 || which > 2) return 0;
    int sizes[3];
    en_snapshot_sizes(g_w, sizes);
    return sizes[which];
}
/* the snapshot into a NativeI32Arr, a NativeF32Arr and a NativeFloatArr each at least en_x_snapshot_size long; 1 if
 * written, 0 if an array is short (nothing written) */
int64_t en_x_snapshot(void *ints, void *floats, void *doubles) {
    if (!g_w || !ints || !floats || !doubles) return 0;
    int sizes[3];
    en_snapshot_sizes(g_w, sizes);
    if (en_x_len(ints) < sizes[0] || en_x_len(floats) < sizes[1] || en_x_len(doubles) < sizes[2]) return 0;
    return en_snapshot(g_w, en_x_i32(ints), en_x_f32(floats), en_x_f64(doubles));
}
static int en_x_snap_lens(void *ints, void *floats, void *doubles, int n[3]) {
    int64_t a = en_x_len(ints), b = en_x_len(floats), c = en_x_len(doubles);
    if (!ints || !floats || !doubles || a > INT32_MAX || b > INT32_MAX || c > INT32_MAX) return 0;
    n[0] = (int)a; n[1] = (int)b; n[2] = (int)c;
    return 1;
}
/* 1 if a restore from the three arrays (their whole lengths) would be accepted, parts held aside */
int64_t en_x_snapshot_fits(void *ints, void *floats, void *doubles) {
    int n[3];
    if (!g_w || !en_x_snap_lens(ints, floats, doubles, n)) return 0;
    return en_snapshot_fits(g_w, n[0], en_x_i32(ints), n[1], en_x_f32(floats), n[2], en_x_f64(doubles));
}
/* en_restore from the three arrays' whole lengths: 1, or 0 and nothing changed */
int64_t en_x_restore(void *ints, void *floats, void *doubles) {
    int n[3];
    if (!g_w) return 0;
    if (!en_x_snap_lens(ints, floats, doubles, n)) { en_refuse(g_w, EN_R_SNAPSHOT); return 0; }
    return en_restore(g_w, n[0], en_x_i32(ints), n[1], en_x_f32(floats), n[2], en_x_f64(doubles));
}
/* every held part onto the release list; answers 1 */
int64_t en_x_release_all(void) { if (g_w) en_release_all(g_w); return 1; }

/* seconds on a monotonic clock, for timing scenes; the kernel never reads it (its time is the dt it is given) */
double en_x_clock(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

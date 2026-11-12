/* strata_ent.c — binds the entity kernel (strata_ent.h) to March. One
 * world per process, opened by en_x_open and freed by en_x_close. Knows
 * nothing of Vulkan, GLFW, physics or audio.
 *
 * March values: a NativeArray arrives as its heap pointer, element count at
 * +16, data at +32 (8 bytes an element for NativeIntArr, 4 for NativeI32Arr
 * and NativeF32Arr). Inputs are only read; en_x_each and en_x_column fill a
 * caller-owned array, as strata_anim.c's matrices do. Reads of three numbers
 * come back one number a call (en_x_get's k).
 *
 * With no world open every call does nothing and answers its default
 * (0, -1 for a kind or an id, 0.0). */
/* Rewritten by stamp_ent.sh whenever strata_ent.h changes (GAPS S14). */
#define STRATA_ENT_STAMP "322fea8965c05bc0"
#include <stdint.h>
#include "strata_ent.h"

static EnWorld *g_w;

static const int32_t *en_x_i32(void *a) { return a ? (const int32_t *)((char *)a + 32) : NULL; }
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

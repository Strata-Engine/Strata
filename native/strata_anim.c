/* strata_anim.c — binds the pose kernel (strata_anim.h) to March. One
 * world per process, opened by an_x_open and freed by an_x_close. Knows
 * nothing of Vulkan or GLFW.
 *
 * March values: a NativeArray arrives as its heap pointer, element count at
 * +16, data at +32. Inputs are only read; an_x_matrices fills a
 * caller-owned NativeF32Arr, as strata_noise.c's fields do. Marks go back
 * one number per call (an_x_mark_int, an_x_mark_weight).
 *
 * Masks: March numbers a rig's masks 0, 1, ... in the order Strata.Rig made
 * them. an_x_masks uploads them all at once, right after the rig, so the
 * world's ids are contiguous and an_x_play / an_x_blend translate a rig's
 * mask number to the world's id. -1 means every part.
 *
 * Hits come back through caller-owned arrays: a ray's ints and floats, a
 * box's pairs (an_x_hit_box answers how many there are, so a caller whose
 * array was short can ask again with a longer one).
 *
 * The view (an_x_view) takes its 24 plane floats from a NativeF32Arr; one
 * shorter than 24 means no planes. The renderer reads the draw list straight
 * from strata_anim.h (an_draw_list), never through March.
 *
 * Timing, for measurements only: an_x_update and an_x_hit_ray time
 * themselves on the monotonic clock (an_x_timing). The kernel never reads a
 * clock, so no answer depends on it. */
/* Rewritten by stamp_anim.sh whenever strata_anim.h changes (GAPS S14). */
#define STRATA_ANIM_STAMP "c4c4020d7ec59a4b"
#include <stdint.h>
#include <time.h>
#include "strata_anim.h"

static AnWorld *g_w;
static int32_t g_mask_base[AN_RIGS], g_mask_n[AN_RIGS];
/* seconds: the last update, the worst, the sum, how many; rays' sum and count */
static double g_t_last, g_t_worst, g_t_sum, g_t_n, g_r_sum, g_r_n;
static double an_x_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static const int32_t *an_x_i32(void *a) { return a ? (const int32_t *)((char *)a + 32) : NULL; }
static const float *an_x_f32(void *a) { return a ? (const float *)((char *)a + 32) : NULL; }
static int64_t an_x_len(void *a) { return a ? *(const int64_t *)((char *)a + 16) : 0; }

/* the world's mask id for figure fig's rig mask number m; -1 stays -1, and
 * a number the rig does not have answers a value an_layer_for refuses */
static int an_x_mask_id(int64_t fig, int64_t m) {
    if (m < 0) return -1;
    AnFig *f = an_fig(g_w, (int)fig);
    if (!f) return -1;                         /* the call itself refuses the figure */
    if (m >= g_mask_n[f->rig]) return AN_MASKS;   /* no such mask: refused as AN_R_MASK */
    return g_mask_base[f->rig] + (int)m;
}

int64_t an_x_open(int64_t capacity) {
    if (g_w) an_close(g_w);
    g_w = an_open((int)capacity);
    for (int i = 0; i < AN_RIGS; i++) { g_mask_base[i] = 0; g_mask_n[i] = 0; }
    return g_w ? 1 : 0;
}
void an_x_close(void) { an_close(g_w); g_w = NULL; }

/* parent: n i32; floats: 13 a part (pivot 3, rest 4, box 6) */
int64_t an_x_rig(int64_t n, void *parent, void *floats) {
    if (!g_w) return -1;
    if (n < 1 || n > AN_PARTS || an_x_len(parent) < n || an_x_len(floats) < 13 * n) return an_refuse(g_w, AN_R_PARTS);
    const float *f = an_x_f32(floats);
    float pivot[AN_PARTS * 3], rest[AN_PARTS * 4], box[AN_PARTS * 6];
    for (int i = 0; i < n; i++) {
        memcpy(pivot + 3 * i, f + 13 * i, 3 * sizeof(float));
        memcpy(rest + 4 * i, f + 13 * i + 3, 4 * sizeof(float));
        memcpy(box + 6 * i, f + 13 * i + 7, 6 * sizeof(float));
    }
    return an_rig(g_w, (int)n, an_x_i32(parent), pivot, rest, box);
}

/* a rig's masks, all at once: counts[i] roots each, the roots one after another.
 * Answers how many were made; a refused one stops the upload there. */
int64_t an_x_masks(int64_t rig, int64_t n, void *counts, void *roots) {
    if (!g_w || rig < 0 || rig >= g_w->nrigs) { if (g_w) an_refuse(g_w, AN_R_RIG); return 0; }
    if (n <= 0) return 0;
    if (an_x_len(counts) < n) return an_refuse(g_w, AN_R_MASK) + 1;
    const int32_t *c = an_x_i32(counts), *r = an_x_i32(roots);
    int64_t have = an_x_len(roots), at = 0, made = 0;
    g_mask_base[rig] = g_w->nmasks;
    for (int64_t i = 0; i < n; i++) {
        if (c[i] < 0 || at + c[i] > have) { an_refuse(g_w, AN_R_MASK); break; }
        if (an_mask(g_w, (int)rig, c[i], r ? r + at : NULL) < 0) break;
        at += c[i]; made++;
    }
    g_mask_n[rig] = (int32_t)made;
    return made;
}

/* keys: ints 3 a key (part, channel, interp), floats 5 a key (time, x, y, z, w) */
int64_t an_x_clip(int64_t rig, double duration, int64_t loops, double stride, int64_t nkeys, void *key_ints, void *key_floats,
                  int64_t nmarks, void *mark_t, void *mark_tag) {
    if (!g_w) return -1;
    if (nkeys < 0 || nmarks < 0 || an_x_len(key_ints) < 3 * nkeys || an_x_len(key_floats) < 5 * nkeys ||
        an_x_len(mark_t) < nmarks || an_x_len(mark_tag) < nmarks) return an_refuse(g_w, AN_R_VALUE);
    return an_clip(g_w, (int)rig, (float)duration, loops != 0, (float)stride, (int)nkeys, an_x_i32(key_ints), an_x_f32(key_floats),
                   (int)nmarks, an_x_f32(mark_t), an_x_i32(mark_tag));
}

/* spawn and place in one: the rest pose at (x, y, z) turned by q, posed at once */
int64_t an_x_spawn(int64_t rig, double x, double y, double z, double qx, double qy, double qz, double qw) {
    if (!g_w) return -1;
    float pos[3] = {(float)x, (float)y, (float)z}, q[4] = {(float)qx, (float)qy, (float)qz, (float)qw}, n[4];
    memcpy(n, q, sizeof n);
    if (!an_finite(pos, 3) || !an_finite(q, 4) || !an_qnorm(n)) return an_refuse(g_w, AN_R_VALUE);
    int id = an_spawn(g_w, (int)rig);
    if (id < 0) return id;
    AnFig *f = an_fig(g_w, id);
    an_place(g_w, id, pos, q);
    an_settle(g_w, f);
    return id;
}
void an_x_remove(int64_t fig) { if (g_w) an_remove(g_w, (int)fig); }
void an_x_place(int64_t fig, double x, double y, double z, double qx, double qy, double qz, double qw) {
    if (!g_w) return;
    float pos[3] = {(float)x, (float)y, (float)z}, q[4] = {(float)qx, (float)qy, (float)qz, (float)qw};
    an_place(g_w, (int)fig, pos, q);
}
void an_x_play(int64_t fig, int64_t layer, int64_t clip, double fade, double rate, double weight, int64_t mask, int64_t additive) {
    if (!g_w) return;
    an_play(g_w, (int)fig, (int)layer, (int)clip, (float)fade, (float)rate, (float)weight, an_x_mask_id(fig, mask), additive != 0);
}
void an_x_blend(int64_t fig, int64_t layer, int64_t n, void *clips, void *positions, double value, double fade, double weight, int64_t mask) {
    if (!g_w) return;
    if (n < 1 || n > AN_BLEND || an_x_len(clips) < n || an_x_len(positions) < n) { an_refuse(g_w, AN_R_BLEND); return; }
    an_blend(g_w, (int)fig, (int)layer, (int)n, an_x_i32(clips), an_x_f32(positions), (float)value, (float)fade, (float)weight,
             an_x_mask_id(fig, mask));
}
void an_x_set(int64_t fig, int64_t layer, int64_t what, double v, double fade) {
    if (g_w) an_set(g_w, (int)fig, (int)layer, (int)what, (float)v, (float)fade);
}
void an_x_update(double dt) {
    if (!g_w) return;
    double t0 = an_x_now();
    an_update(g_w, (float)dt);
    double t = an_x_now() - t0;
    g_t_last = t; g_t_sum += t; g_t_n += 1;
    if (t > g_t_worst) g_t_worst = t;
}
/* the measurements: 0 the last update's seconds, 1 the worst since the reset, 2 their sum, 3 how many, 4 rays' seconds, 5 how many rays */
double an_x_timing(int64_t k) {
    switch (k) {
    case 0: return g_t_last;
    case 1: return g_t_worst;
    case 2: return g_t_sum;
    case 3: return g_t_n;
    case 4: return g_r_sum;
    case 5: return g_r_n;
    default: return 0;
    }
}
void an_x_timing_reset(void) { g_t_last = g_t_worst = g_t_sum = g_t_n = g_r_sum = g_r_n = 0; }

/* the camera: 24 plane floats (fewer: no planes, all in view), the eye, and the far distance (0: none) */
void an_x_view(void *planes, double ex, double ey, double ez, double far_m) {
    if (!g_w) return;
    float eye[3] = {(float)ex, (float)ey, (float)ez};
    an_view(g_w, an_x_len(planes) >= 24 ? an_x_f32(planes) : NULL, eye, (float)far_m);
}
void an_x_always(int64_t fig, int64_t on) { if (g_w) an_always(g_w, (int)fig, on != 0); }
void an_x_traced(int64_t fig, int64_t on) { if (g_w) an_traced(g_w, (int)fig, on != 0); }

/* how many parts figure fig has; 0 if there is no such figure */
int64_t an_x_parts(int64_t fig) {
    if (!g_w) return 0;
    AnFig *f = an_fig(g_w, (int)fig);
    return f ? g_w->rigs[f->rig].nparts : 0;
}
/* figure fig's matrices (12 floats a part, column-major 3x4, world space)
 * copied into a caller-owned NativeF32Arr of at least 12 x parts floats;
 * answers the parts copied (0 if no such figure or the array is too short). */
int64_t an_x_matrices(int64_t fig, void *out_f32) {
    if (!g_w || !out_f32) return 0;
    const float *m = an_matrices(g_w, (int)fig);
    int64_t parts = an_x_parts(fig);
    if (!m || an_x_len(out_f32) < parts * 12) return 0;
    memcpy((char *)out_f32 + 32, m, (size_t)parts * 12 * sizeof(float));
    return parts;
}
/* ---- the procedural stage ---- */
void an_x_override(int64_t fig, int64_t part, double qx, double qy, double qz, double qw, double x, double y, double z, double weight) {
    if (!g_w) return;
    float q[4] = {(float)qx, (float)qy, (float)qz, (float)qw}, m[3] = {(float)x, (float)y, (float)z};
    an_override(g_w, (int)fig, (int)part, q, m, (float)weight);
}
/* chain: n i32, the parts from the top of the chain down */
void an_x_aim(int64_t fig, int64_t slot, int64_t n, void *chain, double ax, double ay, double az, double tx, double ty, double tz,
              double limit, double weight) {
    if (!g_w) return;
    if (weight > 0 && (n < 1 || n > 4 || an_x_len(chain) < n)) { an_refuse(g_w, AN_R_PART); return; }
    float axis[3] = {(float)ax, (float)ay, (float)az}, t[3] = {(float)tx, (float)ty, (float)tz};
    an_aim(g_w, (int)fig, (int)slot, (int)n, an_x_i32(chain), axis, t, (float)limit, (float)weight);
}
/* fade 0 sets the weight at once; above 0 the weight moves there at 1/fade a second (a new reach starts at 0) */
void an_x_reach(int64_t fig, int64_t slot, int64_t upper, int64_t lower, int64_t end, double tx, double ty, double tz,
                double px, double py, double pz, double weight, double fade) {
    if (!g_w) return;
    int32_t chain[3] = {(int32_t)upper, (int32_t)lower, (int32_t)end};
    float t[3] = {(float)tx, (float)ty, (float)tz}, p[3] = {(float)px, (float)py, (float)pz};
    an_reach_to(g_w, (int)fig, (int)slot, chain, t, p, (float)weight, (float)fade);
}
/* only the weight: the slot keeps its chain, target and pole; nothing if the slot is empty */
void an_x_reach_fade(int64_t fig, int64_t slot, double weight, double fade) {
    if (g_w) an_reach_to(g_w, (int)fig, (int)slot, NULL, NULL, NULL, (float)weight, (float)fade);
}
void an_x_spring(int64_t fig, int64_t part, double stiffness, double damping) {
    if (g_w) an_spring(g_w, (int)fig, (int)part, (float)stiffness, (float)damping);
}
void an_x_drop(int64_t fig, double metres) { if (g_w) an_drop(g_w, (int)fig, (float)metres); }
/* the drop moving toward metres at speed metres a second (0: at once) */
void an_x_drop_to(int64_t fig, double metres, double speed) { if (g_w) an_drop_to(g_w, (int)fig, (float)metres, (float)speed); }
/* part's world origin into a caller-owned NativeF32Arr of 3: stage 0 the
 * animated pose (layers and overrides), 1 before the springs. 1, or 0 if no
 * such figure or part (or the array is short). */
int64_t an_x_origin(int64_t fig, int64_t part, int64_t stage, void *out_f32) {
    if (!g_w || an_x_len(out_f32) < 3) return 0;
    return an_part_origin_at(g_w, (int)fig, (int)part, stage > 0 ? 1 : 0, (float *)((char *)out_f32 + 32));
}
/* figure fig's rig id; -1 if no such figure */
int64_t an_x_fig_rig(int64_t fig) {
    if (!g_w) return -1;
    AnFig *f = an_fig(g_w, (int)fig);
    return f ? f->rig : -1;
}
/* figure fig's placement rotation (xyzw) into a NativeF32Arr of 4; 1, or 0 */
int64_t an_x_facing(int64_t fig, void *out_f32) {
    if (!g_w || an_x_len(out_f32) < 4) return 0;
    AnFig *f = an_fig(g_w, (int)fig);
    if (!f) return 0;
    memcpy((char *)out_f32 + 32, f->q, 4 * sizeof(float));
    return 1;
}

/* ---- sockets, part grids, attachments, hit tests ---- */
/* a socket on rig's part at (x, y, z) metres in the part's frame turned by q; its id within the rig, or -1 */
int64_t an_x_socket_def(int64_t rig, int64_t part, double x, double y, double z, double qx, double qy, double qz, double qw) {
    if (!g_w) return -1;
    float pos[3] = {(float)x, (float)y, (float)z}, q[4] = {(float)qx, (float)qy, (float)qz, (float)qw};
    return an_socket_def(g_w, (int)rig, (int)part, pos, q);
}
/* rig's part's grid: sx x sy x sz palette indices (x fastest) in a NativeU8Arr; its shape number, or -1 */
int64_t an_x_shape(int64_t rig, int64_t part, int64_t sx, int64_t sy, int64_t sz, double voxel, void *pal) {
    if (!g_w) return -1;
    if (sx < 0 || sy < 0 || sz < 0 || sx > AN_SIDE || sy > AN_SIDE || sz > AN_SIDE || an_x_len(pal) < sx * sy * sz)
        return an_refuse(g_w, AN_R_VALUE);
    return an_shape(g_w, (int)rig, (int)part, (int)sx, (int)sy, (int)sz, (float)voxel, pal ? (const uint8_t *)((char *)pal + 32) : NULL);
}
/* 1 attached, 0 refused */
int64_t an_x_attach(int64_t child, int64_t parent, int64_t socket, double x, double y, double z, double qx, double qy, double qz, double qw) {
    if (!g_w) return 0;
    float pos[3] = {(float)x, (float)y, (float)z}, q[4] = {(float)qx, (float)qy, (float)qz, (float)qw};
    return an_attach(g_w, (int)child, (int)parent, (int)socket, pos, q);
}
void an_x_detach(int64_t child) { if (g_w) an_detach(g_w, (int)child); }
/* socket's world position and rotation (x, y, z, qx, qy, qz, qw) into a NativeF32Arr of 7; 1, or 0 */
int64_t an_x_socket(int64_t fig, int64_t socket, void *out_f32) {
    if (!g_w || an_x_len(out_f32) < 7) return 0;
    float *o = (float *)((char *)out_f32 + 32);
    return an_socket(g_w, (int)fig, (int)socket, o, o + 3);
}
/* 1 hit: ints (fig, part, x, y, z, colour) into a NativeI32Arr of 6, floats (dist, nx, ny, nz) into a NativeF32Arr of 4; 0 a miss */
int64_t an_x_hit_ray(double ox, double oy, double oz, double dx, double dy, double dz, double max, int64_t ignore, void *ints, void *floats) {
    if (!g_w || an_x_len(ints) < 6 || an_x_len(floats) < 4) return 0;
    float o[3] = {(float)ox, (float)oy, (float)oz}, d[3] = {(float)dx, (float)dy, (float)dz};
    AnHit h;
    double t0 = an_x_now();
    int got = an_hit_ray(g_w, o, d, (float)max, (int)ignore, &h);
    g_r_sum += an_x_now() - t0; g_r_n += 1;
    if (!got) return 0;
    int32_t *i = (int32_t *)((char *)ints + 32);
    float *f = (float *)((char *)floats + 32);
    i[0] = h.fig; i[1] = h.part; i[2] = h.x; i[3] = h.y; i[4] = h.z; i[5] = h.colour;
    f[0] = h.dist; f[1] = h.nx; f[2] = h.ny; f[3] = h.nz;
    return 1;
}
/* (figure, part) pairs into a NativeI32Arr (two ints a pair, as many as fit); answers how many there are */
int64_t an_x_hit_box(double cx, double cy, double cz, double hx, double hy, double hz, double qx, double qy, double qz, double qw,
                     int64_t ignore, void *out) {
    if (!g_w) return 0;
    float c[3] = {(float)cx, (float)cy, (float)cz}, h[3] = {(float)hx, (float)hy, (float)hz}, q[4] = {(float)qx, (float)qy, (float)qz, (float)qw};
    int64_t room = an_x_len(out) / 2;
    return an_hit_box(g_w, c, h, q, (int)ignore, (int)(room > (1 << 30) ? (1 << 30) : room), room > 0 ? (int32_t *)((char *)out + 32) : NULL);
}

int64_t an_x_mark_count(void) { return g_w ? g_w->nevents : 0; }
/* mark i of the last update: k 0 figure, 1 layer, 2 tag */
int64_t an_x_mark_int(int64_t i, int64_t k) {
    if (!g_w || i < 0 || i >= g_w->nevents || k < 0 || k > 2) return -1;
    const AnEvent *e = &g_w->events[i];
    return k == 0 ? e->fig : (k == 1 ? e->layer : e->tag);
}
double an_x_mark_weight(int64_t i) {
    if (!g_w || i < 0 || i >= g_w->nevents) return 0;
    return g_w->events[i].weight;
}
/* the hash, folded to 62 bits so it is the same number as a March Int */
int64_t an_x_hash(void) { return g_w ? (int64_t)(an_hash(g_w) & 0x3fffffffffffffffull) : 0; }
double an_x_stat(int64_t which) { return g_w ? an_stat(g_w, (int)which) : 0; }

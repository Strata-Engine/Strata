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
 * mask number to the world's id. -1 means every part. */
/* Rewritten by stamp_anim.sh whenever strata_anim.h changes (GAPS S14). */
#define STRATA_ANIM_STAMP "9daac90c3091f8f3"
#include <stdint.h>
#include "strata_anim.h"

static AnWorld *g_w;
static int32_t g_mask_base[AN_RIGS], g_mask_n[AN_RIGS];

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
    an_pose(g_w, f);
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
void an_x_update(double dt) { if (g_w) an_update(g_w, (float)dt); }

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

/* strata_vox.cpp -- MagicaVoxel .vox files read through ogt_vox, with the
 * scene graph flattened into one grid of palette indices.
 *
 * C++ because ogt_vox.h is (references, templates, bool): it does not compile
 * as C. The entry points are extern "C" and touch nothing of C++ beyond what
 * the header uses; build with -fno-exceptions so no C++ runtime is linked.
 *
 * ogt_vox_read_scene flattens the nTRN/nGRP/nShp graph itself: each instance
 * comes back with its world transform (a signed permutation and a whole
 * translation). A model voxel v of a model of size s sits, by ogt_vox's
 * pivot rule, with its centre at R (v + 0.5 - floor(s / 2)) + t, and the cell
 * holding that centre is the voxel's world cell. Worked in doubled
 * coordinates, so it is integer throughout.
 *
 * The grid is Y up, as Strata is: grid x = vox x, grid y = vox z, grid z =
 * (box top in vox y) - vox y, a rotation (a right-handed Z-up frame to a
 * right-handed Y-up one), not a mirror. It spans the union of the instances'
 * model boxes; a cell no voxel reaches is 0. Where instances overlap the
 * first one wins (file version 200 order). Hidden instances, and instances
 * on hidden layers, are skipped.
 *
 * March values: a NativeArray arrives as its heap pointer; the element count
 * is at +16 and the data at +32. Every call is a pure function of its
 * arguments.
 */
#include <stdint.h>
#include <string.h>
#define OGT_VOX_IMPLEMENTATION
#include "ogt_vox.h"

#define NARR_HDR 32
static inline int64_t narr_len(void *a)  { return *(int64_t *)((char *)a + 16); }
static inline void   *narr_data(void *a) { return (char *)a + NARR_HDR; }

#define SV_MAX_SIDE 1023

static int64_t sv_floor_half(int64_t v) { return v >= 0 ? v / 2 : -((1 - v) / 2); }

static int sv_round(float f) { return (int)(f < 0.0f ? f - 0.5f : f + 0.5f); }

/* The world cell (vox axes) of voxel (x, y, z) of instance i. */
static void sv_cell(const ogt_vox_scene *s, uint32_t i, int64_t x, int64_t y, int64_t z, int64_t out[3]) {
    const ogt_vox_instance *in = &s->instances[i];
    const ogt_vox_model *m = s->models[in->model_index];
    const ogt_vox_transform *t = &in->transform;
    int64_t c[3] = {2 * x + 1 - 2 * (int64_t)(m->size_x / 2),
                    2 * y + 1 - 2 * (int64_t)(m->size_y / 2),
                    2 * z + 1 - 2 * (int64_t)(m->size_z / 2)};
    /* column-major: (m00, m01, m02) is where the model's x axis goes */
    int r[3][3] = {{sv_round(t->m00), sv_round(t->m10), sv_round(t->m20)},
                   {sv_round(t->m01), sv_round(t->m11), sv_round(t->m21)},
                   {sv_round(t->m02), sv_round(t->m12), sv_round(t->m22)}};
    int64_t tr[3] = {sv_round(t->m30), sv_round(t->m31), sv_round(t->m32)};
    for (int a = 0; a < 3; a++)
        out[a] = sv_floor_half(r[a][0] * c[0] + r[a][1] * c[1] + r[a][2] * c[2] + 2 * tr[a]);
}

static int sv_shown(const ogt_vox_scene *s, uint32_t i) {
    const ogt_vox_instance *in = &s->instances[i];
    if (in->hidden) return 0;
    if (in->layer_index < s->num_layers && s->layers[in->layer_index].hidden) return 0;
    return in->model_index < s->num_models;
}

/* The scene in the first n bytes, or NULL. */
static const ogt_vox_scene *sv_read(void *bytes, int64_t n) {
    if (n <= 0 || n > narr_len(bytes) || n > (int64_t)UINT32_MAX) return NULL;
    return ogt_vox_read_scene((const uint8_t *)narr_data(bytes), (uint32_t)n);
}

/* The union box (vox axes, inclusive); 0 when no shown instance. */
static int sv_box(const ogt_vox_scene *s, int64_t lo[3], int64_t hi[3]) {
    int any = 0;
    for (uint32_t i = 0; i < s->num_instances; i++) {
        if (!sv_shown(s, i)) continue;
        const ogt_vox_model *m = s->models[s->instances[i].model_index];
        int64_t a[3], b[3];
        sv_cell(s, i, 0, 0, 0, a);
        sv_cell(s, i, (int64_t)m->size_x - 1, (int64_t)m->size_y - 1, (int64_t)m->size_z - 1, b);
        for (int k = 0; k < 3; k++) {
            int64_t mn = a[k] < b[k] ? a[k] : b[k], mx = a[k] < b[k] ? b[k] : a[k];
            if (!any || mn < lo[k]) lo[k] = mn;
            if (!any || mx > hi[k]) hi[k] = mx;
        }
        any = 1;
    }
    return any;
}

/* Grid sides (Y up) of the scene, 0 when empty or a side passes the limit. */
static int sv_dims(const ogt_vox_scene *s, int64_t lo[3], int64_t hi[3], int64_t *sx, int64_t *sy, int64_t *sz) {
    if (!sv_box(s, lo, hi)) return 0;
    *sx = hi[0] - lo[0] + 1;
    *sy = hi[2] - lo[2] + 1;
    *sz = hi[1] - lo[1] + 1;
    return *sx <= SV_MAX_SIDE && *sy <= SV_MAX_SIDE && *sz <= SV_MAX_SIDE;
}

extern "C" int64_t sv_vox_size(void *bytes, int64_t n) {
    const ogt_vox_scene *s = sv_read(bytes, n);
    if (!s) return 0;
    int64_t lo[3], hi[3], sx = 0, sy = 0, sz = 0;
    int ok = sv_dims(s, lo, hi, &sx, &sy, &sz);
    ogt_vox_destroy_scene(s);
    return ok ? sx | (sy << 10) | (sz << 20) : 0;
}

extern "C" int64_t sv_vox_fill(void *bytes, int64_t n, void *out, int64_t cap) {
    const ogt_vox_scene *s = sv_read(bytes, n);
    if (!s) return 0;
    int64_t lo[3], hi[3], sx = 0, sy = 0, sz = 0;
    if (!sv_dims(s, lo, hi, &sx, &sy, &sz) || cap < sx * sy * sz || cap > narr_len(out)) {
        ogt_vox_destroy_scene(s);
        return 0;
    }
    uint8_t *g = (uint8_t *)narr_data(out);
    memset(g, 0, (size_t)(sx * sy * sz));
    for (uint32_t i = 0; i < s->num_instances; i++) {
        if (!sv_shown(s, i)) continue;
        const ogt_vox_model *m = s->models[s->instances[i].model_index];
        for (uint32_t z = 0; z < m->size_z; z++)
            for (uint32_t y = 0; y < m->size_y; y++)
                for (uint32_t x = 0; x < m->size_x; x++) {
                    uint8_t p = m->voxel_data[x + m->size_x * (y + m->size_y * z)];
                    if (p == 0) continue;
                    int64_t w[3];
                    sv_cell(s, i, x, y, z, w);
                    int64_t gi = (w[0] - lo[0]) + sx * ((w[2] - lo[2]) + sy * (hi[1] - w[1]));
                    if (g[gi] == 0) g[gi] = p;
                }
    }
    ogt_vox_destroy_scene(s);
    return sx * sy * sz;
}

/* The .vox reader: a two-model scene written with ogt_vox's writer comes back
 * flattened, each voxel at the cell the documented rule gives it; a buffer
 * that is not a .vox, or an output array that is too small, gets 0 and no
 * write. The test includes the implementation file so it compiles with the
 * verify.sh command line alone. It is C++ because ogt_vox.h is (see
 * strata_vox.cpp). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "strata_vox.cpp"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL: %s\n", what); fails++; } } while (0)

/* A fake March native u8 array: 32-byte header, count at +16. */
static void *make_arr(int64_t n, uint8_t fill) {
    char *a = (char *)calloc(1, 32 + (size_t)n);
    *(int64_t *)(a + 16) = n;
    memset(a + 32, fill, (size_t)n);
    return a;
}

static ogt_vox_transform translate(float x, float y, float z) {
    ogt_vox_transform t = ogt_vox_transform_get_identity();
    t.m30 = x; t.m31 = y; t.m32 = z;
    return t;
}

/* Two 2 x 2 x 2 models, every voxel its own palette index (model 0: 1..8,
 * model 1: 9..16, 1 + x + 2y + 4z within). Instance 0 puts model 0 at the
 * origin; instance 1 puts model 1 at (6, 3, 2), turned a quarter about vox z
 * (x -> y, y -> -x). Returns the file as a fake native u8 array. */
static void *two_model_file(void) {
    static uint8_t vox0[8], vox1[8];
    for (int i = 0; i < 8; i++) { vox0[i] = (uint8_t)(1 + i); vox1[i] = (uint8_t)(9 + i); }
    ogt_vox_model m0 = {2, 2, 2, 0, vox0}, m1 = {2, 2, 2, 0, vox1};
    const ogt_vox_model *models[2] = {&m0, &m1};

    ogt_vox_instance inst[2];
    memset(inst, 0, sizeof inst);
    inst[0].model_index = 0;
    inst[0].transform = translate(1, 1, 1);
    inst[1].model_index = 1;
    inst[1].transform = translate(6, 3, 2);
    inst[1].transform.m00 = 0; inst[1].transform.m01 = 1;   /* x axis -> +y */
    inst[1].transform.m10 = -1; inst[1].transform.m11 = 0;  /* y axis -> -x */

    ogt_vox_layer layer;
    memset(&layer, 0, sizeof layer);
    ogt_vox_group group;
    memset(&group, 0, sizeof group);
    group.transform = ogt_vox_transform_get_identity();
    group.parent_group_index = k_invalid_group_index;

    ogt_vox_scene scene;
    memset(&scene, 0, sizeof scene);
    scene.num_models = 2; scene.models = models;
    scene.num_instances = 2; scene.instances = inst;
    scene.num_layers = 1; scene.layers = &layer;
    scene.num_groups = 1; scene.groups = &group;
    for (int i = 0; i < 256; i++) scene.palette.color[i] = (ogt_vox_rgba){(uint8_t)i, (uint8_t)i, (uint8_t)i, 255};

    uint32_t size = 0;
    uint8_t *buf = ogt_vox_write_scene(&scene, &size);
    void *a = make_arr(size, 0);
    memcpy(narr_data(a), buf, size);
    ogt_vox_free(buf);
    return a;
}

/* A fake native u8 array holding the given 32-bit little-endian words. */
static void *words(const uint32_t *w, int n) {
    void *a = make_arr(4 * n, 0);
    memcpy(narr_data(a), w, 4 * (size_t)n);
    return a;
}
#define FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

static int at(void *grid, int sx, int sy, int x, int y, int z) {
    return ((uint8_t *)narr_data(grid))[x + sx * (y + sy * z)];
}

int main(void) {
    void *file = two_model_file();
    int64_t n = narr_len(file);

    /* vox x 0..6, vox y 0..3, vox z 0..2; Strata y is vox z and Strata z is
     * 3 - vox y, so the grid is 7 x 3 x 4 */
    int64_t packed = sv_vox_size(file, n);
    CHECK(packed == (7 | (3 << 10) | (4 << 20)), "size is the union box, Y up");
    int sx = 7, sy = 3, sz = 4, vol = sx * sy * sz;

    void *grid = make_arr(vol, 99);
    CHECK(sv_vox_fill(file, n, grid, vol) == vol, "fill returns the voxel count");
    /* instance 0: vox (x, y, z) -> grid (x, z, 3 - y) */
    CHECK(at(grid, sx, sy, 0, 0, 3) == 1, "model 0 voxel (0,0,0)");
    CHECK(at(grid, sx, sy, 1, 0, 3) == 2, "model 0 voxel (1,0,0)");
    CHECK(at(grid, sx, sy, 0, 1, 2) == 7, "model 0 voxel (0,1,1)");
    CHECK(at(grid, sx, sy, 1, 1, 2) == 8, "model 0 voxel (1,1,1)");
    /* instance 1: voxel v lands at vox (6 - vy, 2 + vx, 1 + vz) */
    CHECK(at(grid, sx, sy, 6, 1, 1) == 9, "model 1 voxel (0,0,0) at vox (6,2,1)");
    CHECK(at(grid, sx, sy, 5, 1, 0) == 12, "model 1 voxel (1,1,0) at vox (5,3,1)");
    CHECK(at(grid, sx, sy, 5, 2, 1) == 15, "model 1 voxel (0,1,1) at vox (5,2,2)");
    CHECK(at(grid, sx, sy, 6, 2, 0) == 14, "model 1 voxel (1,0,1) at vox (6,3,2)");
    int solid = 0, stray = 0;
    for (int i = 0; i < vol; i++) {
        uint8_t v = ((uint8_t *)narr_data(grid))[i];
        if (v != 0) solid++;
        if (v == 99) stray++;
    }
    CHECK(solid == 16, "sixteen solid voxels");
    CHECK(stray == 0, "every cell of the box written, empty ones 0");
    CHECK(at(grid, sx, sy, 3, 0, 0) == 0, "a cell between the two is empty");
    free(grid);

    /* too small: refused, nothing written */
    void *small = make_arr(vol, 99);
    CHECK(sv_vox_fill(file, n, small, vol - 1) == 0, "a short cap is refused");
    CHECK(sv_vox_fill(file, n, small, vol + 1) == 0, "a cap past the array is refused");
    CHECK(((uint8_t *)narr_data(small))[0] == 99, "and nothing is written");
    free(small);
    CHECK(sv_vox_size(file, n + 1) == 0, "a length past the array is refused");

    /* not a .vox */
    void *junk = make_arr(64, 7);
    void *out = make_arr(16, 99);
    CHECK(sv_vox_size(junk, 64) == 0, "junk has no size");
    CHECK(sv_vox_fill(junk, 64, out, 16) == 0, "junk fills nothing");
    CHECK(((uint8_t *)narr_data(out))[0] == 99, "and writes nothing");
    free(junk); free(out); free(file);

    /* corrupt files: 0, and the process carries on. A MAIN chunk whose
     * declared children run past the buffer (ogt_vox stops at the end and
     * finds no model); a SIZE chunk whose declared size is wrong (an
     * ogt_assert inside ogt_vox, which aborted before the hook) */
    {
        const uint32_t trunc[] = {FOURCC('V', 'O', 'X', ' '), 150, FOURCC('M', 'A', 'I', 'N'), 0, 1000};
        const uint32_t badsize[] = {FOURCC('V', 'O', 'X', ' '), 150, FOURCC('M', 'A', 'I', 'N'), 0, 32,
                                    FOURCC('S', 'I', 'Z', 'E'), 8, 0, 2, 2};
        void *t = words(trunc, 5), *b = words(badsize, 10), *o = make_arr(64, 99);
        CHECK(sv_vox_size(t, narr_len(t)) == 0, "a truncated MAIN chunk has no size");
        CHECK(sv_vox_fill(t, narr_len(t), o, 64) == 0, "and fills nothing");
        CHECK(sv_vox_size(b, narr_len(b)) == 0, "a SIZE chunk of the wrong size has no size");
        CHECK(sv_vox_fill(b, narr_len(b), o, 64) == 0, "and fills nothing");
        CHECK(((uint8_t *)narr_data(o))[0] == 99, "nor writes");
        void *good = two_model_file();
        CHECK(sv_vox_size(good, narr_len(good)) == (7 | (3 << 10) | (4 << 20)), "a good file still reads after a bad one");
        free(t); free(b); free(o); free(good);
    }

    if (fails) { printf("vox_test: %d failure(s)\n", fails); return 1; }
    printf("vox_test: ok\n");
    return 0;
}

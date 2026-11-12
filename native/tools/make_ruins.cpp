/* make_ruins -- writes the placeholder ruin templates, viewer/assets/ruins/
 * wall.vox, tower.vox and arch.vox, with ogt_vox's writer. Run from the
 * repository root:
 *
 *   cc -O1 -fno-exceptions -Inative native/tools/make_ruins.cpp -o native/tests/bin/make_ruins
 *   native/tests/bin/make_ruins
 *
 * C++ because ogt_vox.h is. The output is committed; this is how it was made.
 * MagicaVoxel is Z up: a model here is (x, depth y, height z). Palette 1 is
 * stone, 2 a darker stone, 3 rubble. Each file is one model, one instance.
 * Everything is a fixed pattern, so the files are the same on every run.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define OGT_VOX_IMPLEMENTATION
#include "ogt_vox.h"

enum { STONE = 1, DARK = 2, RUBBLE = 3 };

typedef struct { uint32_t sx, sy, sz; uint8_t *v; } grid;

static grid make(uint32_t sx, uint32_t sy, uint32_t sz) {
    grid g = {sx, sy, sz, (uint8_t *)calloc(sx * sy * sz, 1)};
    return g;
}
static void put(grid *g, int x, int y, int z, uint8_t p) {
    if (x < 0 || y < 0 || z < 0 || x >= (int)g->sx || y >= (int)g->sy || z >= (int)g->sz) return;
    g->v[x + g->sx * (y + g->sy * z)] = p;
}
/* a small fixed hash, for the stone's mottling and the broken edges */
static uint32_t hash3(int x, int y, int z) {
    uint32_t h = (uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u ^ (uint32_t)z * 83492791u;
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    return h;
}
static uint8_t stone(int x, int y, int z) { return hash3(x, y, z) % 5 == 0 ? DARK : STONE; }

static int write_vox(const char *path, grid *g) {
    ogt_vox_model model = {g->sx, g->sy, g->sz, 0, g->v};
    const ogt_vox_model *models[1] = {&model};
    ogt_vox_instance inst;
    memset(&inst, 0, sizeof inst);
    inst.transform = ogt_vox_transform_get_identity();
    ogt_vox_layer layer;
    memset(&layer, 0, sizeof layer);
    ogt_vox_group group;
    memset(&group, 0, sizeof group);
    group.transform = ogt_vox_transform_get_identity();
    group.parent_group_index = k_invalid_group_index;
    ogt_vox_scene scene;
    memset(&scene, 0, sizeof scene);
    scene.num_models = 1; scene.models = models;
    scene.num_instances = 1; scene.instances = &inst;
    scene.num_layers = 1; scene.layers = &layer;
    scene.num_groups = 1; scene.groups = &group;
    for (int i = 0; i < 256; i++) scene.palette.color[i] = (ogt_vox_rgba){200, 200, 200, 255};
    scene.palette.color[0] = (ogt_vox_rgba){0, 0, 0, 0};
    scene.palette.color[STONE] = (ogt_vox_rgba){132, 128, 120, 255};
    scene.palette.color[DARK] = (ogt_vox_rgba){92, 90, 86, 255};
    scene.palette.color[RUBBLE] = (ogt_vox_rgba){112, 102, 90, 255};
    uint32_t size = 0;
    uint8_t *buf = ogt_vox_write_scene(&scene, &size);
    FILE *f = fopen(path, "wb");
    int ok = f && buf && fwrite(buf, 1, size, f) == size;
    if (f) fclose(f);
    ogt_vox_free(buf);
    free(g->v);
    printf("%s %s: %u x %u x %u (x, depth, height), %u bytes\n", ok ? "wrote" : "FAILED", path, g->sx, g->sy, g->sz, size);
    return ok;
}

/* A broken wall, 12 long, 2 thick, 5 high: a jagged top, a slit window, and
 * a section near one end fallen to a stub with its stones at its foot. */
static grid wall(void) {
    static const int height[12] = {5, 5, 4, 5, 5, 4, 3, 1, 1, 2, 4, 3};
    grid g = make(12, 2, 5);
    for (int x = 0; x < 12; x++)
        for (int y = 0; y < 2; y++)
            for (int z = 0; z < height[x]; z++)
                put(&g, x, y, z, z == 0 ? STONE : stone(x, y, z));
    for (int y = 0; y < 2; y++) { put(&g, 3, y, 2, 0); put(&g, 3, y, 3, 0); }   /* the slit */
    put(&g, 7, 0, 1, RUBBLE); put(&g, 8, 1, 1, RUBBLE); put(&g, 9, 0, 2, RUBBLE);  /* the fall */
    put(&g, 2, 1, 3, 0);                                                          /* a lost stone */
    return g;
}

/* A round tower 9 across and 14 high, hollow, a rubble floor, a doorway on
 * one side, its top broken off unevenly and lowest on the side away from the
 * door. Darker courses every fourth row. */
static grid tower(void) {
    grid g = make(9, 9, 14);
    for (int x = 0; x < 9; x++)
        for (int y = 0; y < 9; y++) {
            int dx = x - 4, dy = y - 4, d2 = dx * dx + dy * dy;
            if (d2 > 20) continue;
            if (d2 < 10) {   /* the inside: a rubble floor, a heap against the far wall */
                put(&g, x, y, 0, RUBBLE);
                if (dy >= 1 && hash3(x, y, 1) % 2 == 0) put(&g, x, y, 1, RUBBLE);
                continue;
            }
            int top = 14 - (int)(hash3(x, y, 0) % 4);
            if (dy >= 2) top -= 4;                 /* the broken side */
            if (x == 0 && y == 4) top = 14;        /* the last full-height stone */
            for (int z = 0; z < top; z++) put(&g, x, y, z, z % 4 == 3 ? DARK : stone(x, y, z));
        }
    for (int x = 3; x <= 5; x++)                  /* the doorway, on the y = 0 side */
        for (int y = 0; y <= 1; y++)
            for (int z = 1; z <= 3; z++) put(&g, x, y, z, 0);
    return g;
}

/* An arch 11 wide, 3 deep, 9 high: two pillars two wide under a stepped arch
 * with a darker keystone course; the right pillar has crumbled to a stump,
 * the arch over it has fallen, and its stones lie in the opening. */
static grid arch(void) {
    grid g = make(11, 3, 9);
    for (int y = 0; y < 3; y++) {
        for (int z = 0; z < 9; z++)
            for (int x = 0; x < 11; x++) {
                int open = (z <= 5 && x >= 2 && x <= 8) || (z == 6 && x >= 3 && x <= 7) || (z == 7 && x >= 4 && x <= 6);
                if (!open) put(&g, x, y, z, z == 8 ? DARK : stone(x, y, z));
            }
        /* the crumbled pillar and the fallen half of the arch */
        for (int z = 0; z < 9; z++)
            for (int x = 7; x < 11; x++) {
                int keep = (x == 9 && z < 4 - (y == 2)) || (x == 10 && z < 3 - (y == 0));
                if (!keep) put(&g, x, y, z, 0);
            }
        put(&g, 6, y, 7, 0);
        put(&g, 6, y, 8, y == 1 ? DARK : 0);
    }
    put(&g, 7, 0, 0, RUBBLE); put(&g, 8, 0, 0, RUBBLE); put(&g, 8, 1, 0, RUBBLE);
    put(&g, 7, 2, 0, RUBBLE); put(&g, 8, 1, 1, RUBBLE); put(&g, 6, 1, 0, RUBBLE);
    return g;
}

int main(void) {
    grid w = wall(), t = tower(), a = arch();
    int ok = write_vox("viewer/assets/ruins/wall.vox", &w);
    ok &= write_vox("viewer/assets/ruins/tower.vox", &t);
    ok &= write_vox("viewer/assets/ruins/arch.vox", &a);
    return ok ? 0 : 1;
}

#include <stdint.h>
#include <stddef.h>

#define CHUNK_SIDE 32
#define CELL_COUNT (CHUNK_SIDE * CHUNK_SIDE * CHUNK_SIDE)
#define MESH_WORDS (CELL_COUNT * 6 * 2)

#ifdef __wasm__
#include "stream.h"
extern void march_start(void) __asm__("_start");
extern void strata_browser_begin_mesh(void);
extern void strata_browser_end_mesh(void);
void strata_browser_init_camera(void);

enum { CACHE_SIDE = 3, SLOT_COUNT = CACHE_SIDE * CACHE_SIDE, EDIT_LIMIT = 4096 };

typedef struct {
    int x, z, live, dirty;
    uint32_t revision, bytes;
    int32_t cells[CELL_COUNT];
    uint32_t mesh[MESH_WORDS];
} ChunkSlot;

typedef struct {
    int x, z, index;
    int32_t value;
} Edit;

static ChunkSlot slots[SLOT_COUNT];
static Edit edits[EDIT_LIMIT];
static size_t edit_count;
static ChunkSlot *building = &slots[0];
static uint32_t revision;
static int initialized;
static int editing;

static void remesh(ChunkSlot *slot, int reuse_cells) {
    building = slot;
    editing = reuse_cells;
    strata_browser_begin_mesh();
    march_start();
    strata_browser_end_mesh();
    slot->revision = ++revision;
    slot->live = 1;
    slot->dirty = 0;
}

int strata_browser_ready(void) { return initialized; }

static ChunkSlot *find_slot(int x, int z) {
    for (int i = 0; i < SLOT_COUNT; i++)
        if (slots[i].live && slots[i].x == x && slots[i].z == z) return &slots[i];
    return 0;
}

static void invalidate_neighbors(int x, int z, int min_x, int max_x, int min_z, int max_z) {
    for (int dz = min_z; dz <= max_z; dz++)
        for (int dx = min_x; dx <= max_x; dx++) {
            if (!dx && !dz) continue;
            ChunkSlot *slot = find_slot(x + dx, z + dz);
            if (slot) slot->dirty = 1;
        }
}

static int chunk_coord(int cell) {
    return (cell - (cell < 0 ? CHUNK_SIDE - 1 : 0)) / CHUNK_SIDE;
}

static ChunkSlot *voxel_slot(const int cell[3], int *index) {
    if (cell[1] < 0 || cell[1] >= CHUNK_SIDE) return 0;
    int x = chunk_coord(cell[0]), z = chunk_coord(cell[2]);
    int local_x = cell[0] - x * CHUNK_SIDE, local_z = cell[2] - z * CHUNK_SIDE;
    *index = local_x + CHUNK_SIDE * (local_z + CHUNK_SIDE * cell[1]);
    return find_slot(x, z);
}

int strata_browser_voxel(const int cell[3]) {
    int index;
    ChunkSlot *slot = voxel_slot(cell, &index);
    return slot ? slot->cells[index] : -1;
}

int strata_browser_edit(const int cell[3], int32_t value) {
    int index;
    ChunkSlot *slot = voxel_slot(cell, &index);
    if (!slot || slot->cells[index] == value) return 0;

    /* Keep edits when a chunk leaves the cache. Never silently forget them. */
    size_t i = 0;
    for (; i < edit_count; i++)
        if (edits[i].x == slot->x && edits[i].z == slot->z && edits[i].index == index) break;
    if (i == EDIT_LIMIT) return 0;
    if (i == edit_count) edit_count++;
    edits[i] = (Edit){slot->x, slot->z, index, value};
    slot->cells[index] = value;
    int x = index % 32, z = (index / 32) % 32;
    invalidate_neighbors(slot->x, slot->z, -(x == 0), x == 31, -(z == 0), z == 31);
    remesh(slot, 1);
    return 1;
}

static int outside(int coord, int center) {
    return coord < center - 1 || coord > center + 1;
}

__attribute__((export_name("stream_one")))
int stream_one(void) {
    if (!initialized) return 0;
    int cx = chunk_coord(strata_browser_eye_cell(0));
    int cz = chunk_coord(strata_browser_eye_cell(2));
    ChunkSlot *replace = 0;
    for (int i = 0; i < SLOT_COUNT; i++) {
        ChunkSlot *slot = &slots[i];
        if (!slot->live || outside(slot->x, cx) || outside(slot->z, cz)) {
            replace = slot;
            break;
        }
    }
    if (!replace) {
        for (int i = 0; i < SLOT_COUNT; i++) {
            if (slots[i].dirty) {
                remesh(&slots[i], 1);
                return 1;
            }
        }
        return 0;
    }

    /* Center, then the cardinal neighbors, then corners. */
    for (int distance = 0; distance <= 2; distance++) {
        for (int z = -1; z <= 1; z++) {
            for (int x = -1; x <= 1; x++) {
                if (x*x + z*z != distance || find_slot(cx+x, cz+z)) continue;
                if (replace->live) invalidate_neighbors(replace->x, replace->z, -1, 1, -1, 1);
                replace->x = cx+x;
                replace->z = cz+z;
                remesh(replace, 0);
                invalidate_neighbors(replace->x, replace->z, -1, 1, -1, 1);
                return 1;
            }
        }
    }
    return 0;
}
#else
#include <stdio.h>
#endif

static double samples[3];
static int64_t seed = INT64_C(4294979641);

int64_t strata_browser_seed(void) { return seed; }

int64_t strata_browser_origin_x(void) {
#ifdef __wasm__
    return building->x * CHUNK_SIDE;
#else
    return 0;
#endif
}

int64_t strata_browser_origin_z(void) {
#ifdef __wasm__
    return building->z * CHUNK_SIDE;
#else
    return 0;
#endif
}

int64_t strata_browser_editing(void) {
#ifdef __wasm__
    return editing;
#else
    return 0;
#endif
}

void strata_browser_sync_cells(void *array) {
#ifdef __wasm__
    int32_t *data = (int32_t *)((char *)array + 32);
    if (!editing) {
        for (size_t i = 0; i < edit_count; i++)
            if (edits[i].x == building->x && edits[i].z == building->z)
                data[edits[i].index] = edits[i].value;
    }
    for (size_t i = 0; i < CELL_COUNT; i++) {
        if (editing) data[i] = building->cells[i];
        else building->cells[i] = data[i];
    }
#else
    (void)array;
#endif
}

/* March supplies a zeroed 34³ NativeIntArr. Copy only the center and the
 * resident neighbors' one-cell shell, including diagonals for corner AO. */
int64_t strata_browser_padded(void *array) {
#ifdef __wasm__
    int64_t *ids = (int64_t *)((char *)array + 32);
    ChunkSlot *near[3][3];
    for (int dz = -1; dz <= 1; dz++)
        for (int dx = -1; dx <= 1; dx++)
            near[dz+1][dx+1] = dx || dz ? find_slot(building->x+dx, building->z+dz) : building;
    for (int z = -1; z <= 32; z++) {
        int dz = (z == 32) - (z == -1);
        for (int x = -1; x <= 32; x++) {
            int dx = (x == 32) - (x == -1);
            const ChunkSlot *slot = near[dz+1][dx+1];
            if (!slot) continue;
            int from = x - 32*dx + 32*(z - 32*dz);
            int to = x+1 + 34*(z+1) + 1156;
            for (int y = 0; y < 32; y++) ids[to + 1156*y] = slot->cells[from + 1024*y];
        }
    }
    return 1;
#else
    (void)array;
    return 0;
#endif
}

void strata_browser_publish(const void *quads, int64_t count,
                            double a, double b, double c) {
    if (count < 0 || count > MESH_WORDS / 2) __builtin_trap();
    const uint32_t *words = (const uint32_t *)((const char *)quads + 32);
    uint32_t bytes = (uint32_t)count * 8;
    samples[0] = a;
    samples[1] = b;
    samples[2] = c;
#ifdef __wasm__
    for (int64_t i = 0; i < count * 2; i++) building->mesh[i] = words[i];
    building->bytes = bytes;
#else
    if (fwrite(&bytes, sizeof bytes, 1, stdout) != 1 ||
        fwrite(samples, sizeof samples, 1, stdout) != 1 ||
        fwrite(words, 1, bytes, stdout) != bytes) __builtin_trap();
#endif
}

#ifdef __wasm__
__attribute__((export_name("init")))
int init(uint32_t lo, uint32_t hi) {
    if (hi > UINT32_C(0x3fffffff) || initialized) return 1;
    seed = (int64_t)(((uint64_t)hi << 32) | lo);
    remesh(&slots[0], 0);
    if (slots[0].bytes == 0) return 1;
    initialized = 1;
    strata_browser_init_camera();
    return 0;
}

__attribute__((export_name("mesh_revision")))
uint32_t mesh_revision(void) { return revision; }

/* First-slot access for the native parity fixture. */
__attribute__((export_name("mesh_ptr")))
uint32_t mesh_ptr(void) { return (uint32_t)(uintptr_t)slots[0].mesh; }

__attribute__((export_name("mesh_bytes")))
uint32_t mesh_bytes(void) { return slots[0].bytes; }

__attribute__((export_name("noise_sample")))
double noise_sample(uint32_t i) { return i < 3 ? samples[i] : 0; }

#define EXPORT(name) __attribute__((export_name(#name)))

EXPORT(chunk_capacity) int chunk_capacity(void) { return SLOT_COUNT; }
EXPORT(chunk_buffer_bytes) int chunk_buffer_bytes(void) { return MESH_WORDS * 4; }
EXPORT(chunk_revision) uint32_t chunk_revision(unsigned i) { return i < SLOT_COUNT ? slots[i].revision : 0; }
EXPORT(chunk_x) int chunk_x(unsigned i) { return i < SLOT_COUNT ? slots[i].x : 0; }
EXPORT(chunk_z) int chunk_z(unsigned i) { return i < SLOT_COUNT ? slots[i].z : 0; }
EXPORT(chunk_bytes) uint32_t chunk_bytes(unsigned i) { return i < SLOT_COUNT ? slots[i].bytes : 0; }
EXPORT(chunk_ptr) uint32_t chunk_ptr(unsigned i) {
    return i < SLOT_COUNT ? (uint32_t)(uintptr_t)slots[i].mesh : 0;
}
#endif

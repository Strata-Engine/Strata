#include <stdint.h>
#include <stddef.h>

#define CHUNK_SIDE 32
#define CELL_COUNT (CHUNK_SIDE * CHUNK_SIDE * CHUNK_SIDE)
#define EDIT_LIMIT 4096
/* Each column can generate two voxels, plus at most the full edit journal. */
#define MESH_WORDS ((2 * CHUNK_SIDE * CHUNK_SIDE + EDIT_LIMIT) * 6 * 2)

#ifdef __wasm__
#include "stream.h"
extern void march_start(void) __asm__("_start");
extern void strata_browser_begin_mesh(void);
extern void strata_browser_end_mesh(void);
void strata_browser_init_camera(void);

enum { CACHE_SIDE = 3, SLOT_COUNT = CACHE_SIDE * CACHE_SIDE * CACHE_SIDE };

typedef enum { CLEAN, STREAM_REFRESH, EDIT_REFRESH } Refresh;

typedef struct {
    int x, y, z, live;
    Refresh refresh;
    uint32_t revision, bytes;
    int32_t cells[CELL_COUNT];
    uint32_t mesh[MESH_WORDS];
} ChunkSlot;

typedef struct {
    int x, z, index;
    int32_t value;
} Edit;

static ChunkSlot slots[SLOT_COUNT];
_Static_assert(sizeof(Edit) == 4 * sizeof(int32_t), "Edit journal ABI");
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
    slot->refresh = CLEAN;
}

int strata_browser_ready(void) { return initialized; }

static ChunkSlot *find_slot(int x, int y, int z) {
    for (int i = 0; i < SLOT_COUNT; i++)
        if (slots[i].live && slots[i].x == x && slots[i].y == y && slots[i].z == z) return &slots[i];
    return 0;
}

static void invalidate_neighbors(const ChunkSlot *center, int min_x, int max_x,
                                 int min_y, int max_y, int min_z, int max_z, Refresh refresh) {
    for (int dy = min_y; dy <= max_y; dy++)
        for (int dz = min_z; dz <= max_z; dz++)
            for (int dx = min_x; dx <= max_x; dx++) {
                if (!dx && !dy && !dz) continue;
                ChunkSlot *slot = find_slot(center->x + dx, center->y + dy, center->z + dz);
                if (slot && slot->refresh < refresh) slot->refresh = refresh;
            }
}

static int chunk_coord(int cell) {
    return (cell - (cell < 0 ? CHUNK_SIDE - 1 : 0)) / CHUNK_SIDE;
}

static ChunkSlot *voxel_slot(const int cell[3], int *index) {
    int x = chunk_coord(cell[0]), y = chunk_coord(cell[1]), z = chunk_coord(cell[2]);
    int local_x = cell[0] - x * CHUNK_SIDE, local_z = cell[2] - z * CHUNK_SIDE;
    *index = local_x + CHUNK_SIDE * (local_z + CHUNK_SIDE * (cell[1] - y * CHUNK_SIDE));
    return find_slot(x, y, z);
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
    /* Layer zero uses the original journal encoding. */
    int journal_index = index + slot->y * CELL_COUNT;
    size_t i = 0;
    for (; i < edit_count; i++)
        if (edits[i].x == slot->x && edits[i].z == slot->z && edits[i].index == journal_index) break;
    if (i == EDIT_LIMIT) return 0;
    if (i == edit_count) edit_count++;
    edits[i] = (Edit){slot->x, slot->z, journal_index, value};
    slot->cells[index] = value;
    int x = index % 32, y = index / 1024, z = (index / 32) % 32;
    invalidate_neighbors(slot, -(x == 0), x == 31, -(y == 0), y == 31, -(z == 0), z == 31, EDIT_REFRESH);
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
    int cy = chunk_coord(strata_browser_eye_cell(1));
    int cz = chunk_coord(strata_browser_eye_cell(2));
    ChunkSlot *replace = 0;
    for (int i = 0; i < SLOT_COUNT; i++) {
        ChunkSlot *slot = &slots[i];
        if (!slot->live || outside(slot->x, cx) || outside(slot->y, cy) || outside(slot->z, cz)) {
            if (!replace) replace = slot;
        } else if (slot->refresh == EDIT_REFRESH) {
            remesh(slot, 1);
            return 1;
        }
    }
    if (!replace) {
        for (int i = 0; i < SLOT_COUNT; i++) {
            if (slots[i].refresh != CLEAN) {
                remesh(&slots[i], 1);
                return 1;
            }
        }
        return 0;
    }

    /* Center, then face, edge, and corner neighbors. */
    for (int distance = 0; distance <= 3; distance++)
        for (int y = -1; y <= 1; y++)
            for (int z = -1; z <= 1; z++)
                for (int x = -1; x <= 1; x++) {
                    if (x*x + y*y + z*z != distance || find_slot(cx+x, cy+y, cz+z)) continue;
                    if (replace->live) invalidate_neighbors(replace, -1, 1, -1, 1, -1, 1, STREAM_REFRESH);
                    replace->x = cx+x;
                    replace->y = cy+y;
                    replace->z = cz+z;
                    remesh(replace, 0);
                    invalidate_neighbors(replace, -1, 1, -1, 1, -1, 1, STREAM_REFRESH);
                    return 1;
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

int64_t strata_browser_origin_y(void) {
#ifdef __wasm__
    return building->y * CHUNK_SIDE;
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
        for (size_t i = 0; i < edit_count; i++) {
            int index = edits[i].index - building->y * CELL_COUNT;
            if (edits[i].x == building->x && edits[i].z == building->z && index >= 0 && index < CELL_COUNT)
                data[index] = edits[i].value;
        }
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
    const int first[] = {-1, 0, 32};
    const int last[] = {-1, 31, 32};
    for (int dy = -1; dy <= 1; dy++)
        for (int dz = -1; dz <= 1; dz++)
            for (int dx = -1; dx <= 1; dx++) {
                const ChunkSlot *slot = dx || dy || dz
                    ? find_slot(building->x+dx, building->y+dy, building->z+dz) : building;
                if (!slot) continue;
                for (int y = first[dy+1]; y <= last[dy+1]; y++)
                    for (int z = first[dz+1]; z <= last[dz+1]; z++)
                        for (int x = first[dx+1]; x <= last[dx+1]; x++) {
                            int from = x-32*dx + 32*(z-32*dz) + 1024*(y-32*dy);
                            ids[x+1 + 34*(z+1) + 1156*(y+1)] = slot->cells[from];
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

EXPORT(edit_words) uint32_t edit_words(void) { return (uint32_t)(uintptr_t)edits; }
EXPORT(edit_count) uint32_t saved_edit_count(void) { return (uint32_t)edit_count; }

EXPORT(restore_edit) int restore_edit(int x, int z, int index, int32_t value) {
    if (initialized || edit_count == EDIT_LIMIT || x < -17 || x > 17 ||
        z < -17 || z > 17 || index < -17 * CELL_COUNT || index >= 18 * CELL_COUNT ||
        (value != 0 && value != 1)) return 0;
    for (size_t i = 0; i < edit_count; i++) {
        if (edits[i].x == x && edits[i].z == z && edits[i].index == index) return 0;
    }
    edits[edit_count++] = (Edit){x, z, index, value};
    return 1;
}

EXPORT(chunk_capacity) int chunk_capacity(void) { return SLOT_COUNT; }
EXPORT(chunk_buffer_bytes) int chunk_buffer_bytes(void) { return MESH_WORDS * 4; }
EXPORT(chunk_revision) uint32_t chunk_revision(unsigned i) { return i < SLOT_COUNT ? slots[i].revision : 0; }
EXPORT(chunk_x) int chunk_x(unsigned i) { return i < SLOT_COUNT ? slots[i].x : 0; }
EXPORT(chunk_y) int chunk_y(unsigned i) { return i < SLOT_COUNT ? slots[i].y : 0; }
EXPORT(chunk_z) int chunk_z(unsigned i) { return i < SLOT_COUNT ? slots[i].z : 0; }
EXPORT(chunk_bytes) uint32_t chunk_bytes(unsigned i) { return i < SLOT_COUNT ? slots[i].bytes : 0; }
EXPORT(chunk_ptr) uint32_t chunk_ptr(unsigned i) {
    return i < SLOT_COUNT ? (uint32_t)(uintptr_t)slots[i].mesh : 0;
}
#endif

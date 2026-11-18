extern void *memset(void *dst, int value, __SIZE_TYPE__ size);
#include <stdint.h>

extern void march_entry(void) __asm__("_start");

extern void ur_collect(void *city, void *motion);

/* Roots retained between commands. */
static void *city, *motion;
static int mesh_tick, prior_mesh_tick;
static double clock_phase, prior_phase, traffic_time, traffic_dt;
void *ur_load_motion(void) { return motion; }
void ur_save_motion(void *value) { motion = value; }
int64_t ur_scene_tick(void) { return mesh_tick; }
int64_t ur_prior_scene_tick(void) { return prior_mesh_tick; }
double ur_prior_phase(void) { return prior_phase; }
double ur_phase(void) { return clock_phase; }
double ur_motion_time(void) { return traffic_time; }
double ur_motion_dt(void) { return traffic_dt; }
static int tool, selected, end_parcel, crossing_built;
static int32_t info[3];
static int initial_clock;
static int32_t parcels[2640 * 3], buildings[2640];
int64_t ur_command(void) { return tool; }
int64_t ur_parcel(void) { return selected; }
int64_t ur_end_parcel(void) { return end_parcel; }
void ur_crossing_built(int64_t built) { crossing_built = built != 0; }
void *ur_load(void) { return city; }

void ur_publish(void *next, const void *heights, const void *roads, const void *zones, const void *levels,
                int64_t harbour, int64_t month, int64_t cash, int64_t clock) {
    city = next;
    initial_clock = (int)clock;
    info[0] = (int32_t)harbour;
    info[1] = (int32_t)month;
    info[2] = (int32_t)cash;
    const int64_t *height = (const int64_t *)((const char *)heights + 32);
    const int64_t *road = (const int64_t *)((const char *)roads + 32);
    const int64_t *zone = (const int64_t *)((const char *)zones + 32);
    const int64_t *floors = (const int64_t *)((const char *)levels + 32);
    for (int i = 0; i < 2640; i++) {
        parcels[3*i] = (int32_t)height[i];
        parcels[3*i+1] = (int32_t)road[i];
        parcels[3*i+2] = (int32_t)zone[i];
        buildings[i] = (int32_t)floors[i];
    }
}

#define EXPORT(name) __attribute__((export_name(#name)))
EXPORT(ur_step) int ur_step(int action, int parcel) {
    if ((!city && (action < -3 || action >= 0)) || (city && action < 0) || action > 3 ||
        (action > 0 && (parcel < 0 || parcel >= 2640))) return 0;
    tool = action;
    selected = parcel;
    march_entry();
    ur_collect(city, motion);
    return 1;
}
EXPORT(ur_overpass) int ur_overpass(int start, int end) {
    if (!city || info[0] < 0 || start < 0 || start >= 2640 || end < 0 || end >= 2640) return 0;
    tool = 6;
    selected = start;
    end_parcel = end;
    crossing_built = 0;
    march_entry();
    ur_collect(city, motion);
    return crossing_built;
}

EXPORT(ur_info) uintptr_t ur_info(void) { return (uintptr_t)info; }
EXPORT(ur_parcels) uintptr_t ur_parcels(void) { return (uintptr_t)parcels; }
EXPORT(ur_buildings) uintptr_t ur_buildings(void) { return (uintptr_t)buildings; }
/* Each batch covers four parcels, four layers of at most 4096 quads each.
 * A GPU instance carries the original two words plus its chunk origin. */
enum { BATCH_SIZE = 4, SCENE_LIMIT = BATCH_SIZE * 4 * 4096 };
/* One bit per shadow voxel. Each column stores 32 vertical bits.
 * Each 8³ block and the layer publish their highest occupied voxel. */
enum { VOLUME_HEADER = 68, VOLUME_WORDS = VOLUME_HEADER + 1024,
       DECK_WORDS = 4 + 128 };
static uint32_t scene_volume[BATCH_SIZE * 4][VOLUME_WORDS];
static uint32_t scene_decks[BATCH_SIZE][DECK_WORDS];
static uint32_t *volume_layer;

void ur_scene_volume_begin(int64_t x, int64_t y, int64_t z, const void *heights);
void ur_scene_volume_box(int64_t x, int64_t low, int64_t z, int64_t w, int64_t high, int64_t d) {
    if (!volume_layer || x < 0 || z < 0 || w < 1 || d < 1 ||
        x + w > 32 || z + d > 32 || low < 0 || low >= high || high > 32) __builtin_trap();
    uint32_t mask = (UINT32_MAX >> (32 - (unsigned)high)) & (UINT32_MAX << (unsigned)low);
    if (volume_layer[0] < high) volume_layer[0] = (uint32_t)high;
    for (int zz = (int)z; zz < z + d; zz++) {
        for (int xx = (int)x; xx < x + w; xx++) {
            volume_layer[VOLUME_HEADER + zz * 32 + xx] |= mask;
        }
    }
    for (int cy = (int)low / 8; cy <= (high - 1) / 8; cy++) {
        uint32_t height = (uint32_t)(high - cy * 8);
        if (height > 8) height = 8;
        for (int cz = (int)z / 8; cz <= (z + d - 1) / 8; cz++) {
            for (int cx = (int)x / 8; cx <= (x + w - 1) / 8; cx++) {
                uint32_t *top = &volume_layer[4 + cy * 16 + cz * 4 + cx];
                if (*top < height) *top = height;
            }
        }
    }
}

void ur_scene_volume_begin(int64_t x, int64_t y, int64_t z, const void *heights) {
    int lot = (int)(x / 32 + z / 32 * 60) - selected;
    if (lot < 0 || lot >= BATCH_SIZE || y < 0 || y >= 128) __builtin_trap();
    volume_layer = scene_volume[lot * 4 + y / 32];
    volume_layer[1] = 1;
    const int64_t *hs = (const int64_t *)((const char *)heights + 32);
    for (int gz = 0; gz < 8; gz++) {
        for (int gx = 0; gx < 8; gx++) {
            int64_t high = hs[11 + gx + 10 * gz] - y;
            if (high > 0) ur_scene_volume_box(gx * 4, 0, gz * 4, 4, high > 32 ? 32 : high, 4);
        }
    }
}

void ur_scene_deck(int64_t slot, const void *shape, int64_t x, int64_t y, int64_t z) {
    int lot = (int)(slot / 4) - selected;
    if (lot < 0 || lot >= BATCH_SIZE || x < 1 || x > 32 ||
        y < 1 || y > 4 || z < 1 || z > 32) __builtin_trap();
    const uint8_t *bytes = (const uint8_t *)shape + 32;
    uint32_t *deck = scene_decks[lot];
    deck[0] = (uint32_t)slot + 1;
    deck[1] = (uint32_t)x; deck[2] = (uint32_t)y; deck[3] = (uint32_t)z;
    for (int zz = 0; zz < z; zz++) {
        for (int yy = 0; yy < y; yy++) {
            uint32_t row = 0;
            for (int xx = 0; xx < x; xx++) {
                if (bytes[xx + x * (yy + y * zz)]) row |= 1u << (unsigned)xx;
            }
            deck[4 + zz * 4 + yy] = row;
        }
    }
}

static uint8_t changed[(2640 + BATCH_SIZE - 1) / BATCH_SIZE];
static uint32_t scene_words[SCENE_LIMIT][8];
static uint32_t scene_count;
enum { POSE_LIMIT = 17000, DYNAMIC_FIRST = 10600, LAMP_LIMIT = 128 };
static float poses[POSE_LIMIT + 1][8];
static float light[12], lamps[LAMP_LIMIT][8];
static uint32_t lamp_count, static_lamp_count;
static float static_lamps[SCENE_LIMIT][4];

enum { FOOTPRINT_LIMIT = 512 };
typedef struct {
    int64_t slot;
    double x, y, z, dx, dz, half_length, half_width;
} Footprint;
static Footprint footprints[2][FOOTPRINT_LIMIT];
static unsigned footprint_frame, footprint_count[2];
static uint32_t capture_slot, capture_pose;
static int capture_origin[3];
static float emission[32][4];
static uint8_t scene_textures[32][1024];

void ur_scene_mesh(int64_t x, int64_t y, int64_t z, const void *mesh, int64_t count) {
    if (count < 0 || count > SCENE_LIMIT - scene_count) __builtin_trap();
    const uint32_t *words = (const uint32_t *)((const char *)mesh + 32);
    for (int64_t i = 0; i < count; ++i) {
        uint32_t *quad = scene_words[scene_count++];
        quad[0] = words[2*i]; quad[1] = words[2*i+1];
        quad[2] = quad[3] = quad[7] = 0;
        quad[4] = (uint32_t)x; quad[5] = (uint32_t)y; quad[6] = (uint32_t)z;
    }
}
void ur_scene_begin(int64_t slot, int64_t x, int64_t y, int64_t z, int64_t posed) {
    if (slot < 0 || slot >= POSE_LIMIT) __builtin_trap();
    capture_slot = (uint32_t)slot + 1;
    capture_pose = posed ? capture_slot : 0;
    capture_origin[0] = posed ? (int)(slot / 4 % 60 * 32) : (int)x;
    capture_origin[1] = (int)y;
    capture_origin[2] = posed ? (int)(slot / 4 / 60 * 32) : (int)z;
    if (posed) {
        float *pose = poses[capture_pose];
        for (int i = 0; i < 8; i++) pose[i] = 0;
        pose[3] = 0.25f;
        pose[7] = 1.0f;
    }
}

void ur_scene_part(const void *words, int64_t count) {
    uint32_t first = scene_count;
    ur_scene_mesh(capture_origin[0], capture_origin[1], capture_origin[2], words, count);
    for (uint32_t i = first; i < scene_count; i++) {
        scene_words[i][3] = capture_pose;
        scene_words[i][7] = capture_slot;
    }
}
void ur_scene_voxel(int64_t slot, double scale) { poses[slot + 1][3] = (float)scale; }
void ur_scene_pose(int64_t slot, double x, double y, double z,
                   double qx, double qy, double qz, double qw) {
    float *p = poses[slot + 1];
    p[0] = (float)x; p[1] = (float)y; p[2] = (float)z;
    p[4] = (float)qx; p[5] = (float)qy; p[6] = (float)qz; p[7] = (float)qw;
}
void ur_scene_fog(double near, double far) { light[8] = (float)near; light[9] = (float)far; }
void ur_scene_sun(double x, double y, double z, double r, double g, double b) {
    double length = __builtin_sqrt(x*x + y*y + z*z);
    light[0] = (float)(x / length); light[1] = (float)(y / length); light[2] = (float)(z / length);
    light[4] = (float)r; light[5] = (float)g; light[6] = (float)b;
}
void ur_scene_lamp(double x, double y, double z, double dx, double dz, int64_t material) {
    if (lamp_count >= LAMP_LIMIT) return;
    float *lamp = lamps[lamp_count++];
    lamp[0] = (float)(x * 0.25); lamp[1] = (float)(y * 0.25); lamp[2] = (float)(z * 0.25);
    lamp[4] = (float)dx; lamp[6] = (float)dz; lamp[7] = (float)material;
}
void ur_scene_static_lamp(int64_t x, int64_t y, int64_t z, int64_t material) {
    if (static_lamp_count >= SCENE_LIMIT) __builtin_trap();
    float *lamp = static_lamps[static_lamp_count++];
    lamp[0] = ((float)x + 0.5f) * 0.25f;
    lamp[1] = ((float)y + 0.5f) * 0.25f;
    lamp[2] = ((float)z + 0.5f) * 0.25f;
    lamp[3] = (float)material;
}

void ur_scene_footprint(int64_t slot, double x, double y, double z,
                        double dx, double dz, double length, double width) {
    unsigned index = footprint_count[footprint_frame];
    if (index >= FOOTPRINT_LIMIT) return;
    footprints[footprint_frame][index] = (Footprint){slot, x, y, z, dx, dz, length * 0.5 + 0.8, width * 0.5 + 0.8};
    footprint_count[footprint_frame] = index + 1;
}

static double min(double a, double b) { return a < b ? a : b; }
static double max(double a, double b) { return a > b ? a : b; }

/* Same swept footprint test as the desktop scene's pedestrian yield check. */
static int footprint_axis(double start, double end, double half, double *first, double *last) {
    double motion = end - start;
    if (__builtin_fabs(motion) < 1e-6) return __builtin_fabs(start) <= half;
    double a = (-half - start) / motion, b = (half - start) / motion;
    *first = max(*first, min(a, b));
    *last = min(*last, max(a, b));
    return *first <= *last;
}

int64_t ur_scene_blocked(double x0, double y0, double z0,
                         double x1, double y1, double z1, int64_t ignore) {
    for (unsigned frame = 0; frame < 2; frame++) {
        for (unsigned i = 0; i < footprint_count[frame]; i++) {
            const Footprint *v = &footprints[frame][i];
            if (v->slot == ignore || min(y0, y1) > v->y + 6 || max(y0, y1) < v->y - 4) continue;
            double ax = x0 - v->x, az = z0 - v->z, bx = x1 - v->x, bz = z1 - v->z;
            double a = ax * v->dx + az * v->dz, b = bx * v->dx + bz * v->dz;
            double side_a = az * v->dx - ax * v->dz, side_b = bz * v->dx - bx * v->dz;
            if (__builtin_fabs(a) <= v->half_length && __builtin_fabs(side_a) <= v->half_width &&
                bx * bx + bz * bz > ax * ax + az * az) continue;
            double first = 0, last = 1;
            if (footprint_axis(a, b, v->half_length, &first, &last) &&
                footprint_axis(side_a, side_b, v->half_width, &first, &last)) return 1;
        }
    }
    return 0;
}

int64_t ur_scene_night(void) { return clock_phase < 0.45 || clock_phase >= 1.35; }
void ur_scene_failed(void) { __builtin_trap(); }

void ur_scene_emission(int64_t id, int64_t r, int64_t g, int64_t b, int64_t strength) {
    if (id < 0 || id >= 32) __builtin_trap();
    float scale = (float)strength / (255.0f * 255.0f);
    emission[id][0] = (float)r * scale;
    emission[id][1] = (float)g * scale;
    emission[id][2] = (float)b * scale;
}
void ur_scene_texture(int64_t id, const void *array) {
    if (id < 0 || id >= 32) __builtin_trap();
    const uint8_t *bytes = (const uint8_t *)array + 32;
    for (int i = 0; i < 1024; ++i) scene_textures[id][i] = bytes[i];
}
EXPORT(ur_scene_batch) uint32_t ur_scene_batch(int first, double phase, int tick) {
    if (!city || first < 0 || first >= 2640) return 0;
    tool = 4;
    mesh_tick = tick;
    clock_phase = __builtin_isfinite(phase) ? phase : initial_clock / 1000.0;
    selected = first;
    scene_count = static_lamp_count = 0;
    volume_layer = 0;
    memset(scene_volume, 0, sizeof(scene_volume));
    memset(scene_decks, 0, sizeof(scene_decks));
    march_entry();
    ur_collect(city, motion);
    return scene_count;
}
EXPORT(ur_scene_volume_ptr) uintptr_t ur_scene_volume_ptr(void) { return (uintptr_t)scene_volume; }
EXPORT(ur_scene_volume_words) uint32_t ur_scene_volume_words(void) { return VOLUME_WORDS; }
EXPORT(ur_scene_decks_ptr) uintptr_t ur_scene_decks_ptr(void) { return (uintptr_t)scene_decks; }
EXPORT(ur_scene_deck_words) uint32_t ur_scene_deck_words(void) { return DECK_WORDS; }
EXPORT(ur_scene_ptr) uintptr_t ur_scene_ptr(void) { return (uintptr_t)scene_words; }
EXPORT(ur_scene_textures) uintptr_t ur_scene_textures(void) { return (uintptr_t)scene_textures; }
EXPORT(ur_scene_batch_size) uint32_t ur_scene_batch_size(void) { return BATCH_SIZE; }

void ur_scene_changed_lot(int64_t lot) { changed[lot / BATCH_SIZE] = 1; }

EXPORT(ur_scene_changes) uintptr_t ur_scene_changes(double before, double after, int before_tick, int after_tick) {
    if (!city) return 0;
    tool = 7;
    prior_mesh_tick = before_tick;
    mesh_tick = after_tick;
    prior_phase = before;
    clock_phase = after;
    for (unsigned i = 0; i < sizeof(changed); i++) changed[i] = 0;
    march_entry();
    ur_collect(city, motion);
    return (uintptr_t)changed;
}

EXPORT(ur_scene_frame) uint32_t ur_scene_frame(double time, double dt, double phase) {
    if (!city) return 0;
    tool = 5;
    scene_count = lamp_count = 0;
    footprint_frame ^= 1;
    footprint_count[footprint_frame] = 0;
    traffic_time = time;
    traffic_dt = dt;
    clock_phase = phase;
    march_entry();
    light[10] = (float)lamp_count;
    ur_collect(city, motion);
    return scene_count;
}
EXPORT(ur_scene_poses) uintptr_t ur_scene_poses(void) { return (uintptr_t)poses; }
EXPORT(ur_scene_static_count) uint32_t ur_scene_static_count(void) { return static_lamp_count; }
EXPORT(ur_scene_static_ptr) uintptr_t ur_scene_static_ptr(void) { return (uintptr_t)static_lamps; }
EXPORT(ur_scene_pose_count) uint32_t ur_scene_pose_count(void) { return POSE_LIMIT + 1; }
EXPORT(ur_scene_dynamic_first) uint32_t ur_scene_dynamic_first(void) { return DYNAMIC_FIRST + 1; }
EXPORT(ur_scene_light) uintptr_t ur_scene_light(void) { return (uintptr_t)light; }
EXPORT(ur_scene_emission_ptr) uintptr_t ur_scene_emission_ptr(void) { return (uintptr_t)emission; }
EXPORT(ur_initial_clock) int ur_initial_clock(void) { return initial_clock; }
EXPORT(ur_scene_lamps) uintptr_t ur_scene_lamps(void) { return (uintptr_t)lamps; }

/* The freestanding March runtime lowers its atan2 wrapper to libm. */
__attribute__((import_module("math"), import_name("atan2")))
double ur_host_atan2(double y, double x);
double atan2(double y, double x) { return ur_host_atan2(y, x); }

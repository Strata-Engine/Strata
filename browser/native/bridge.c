#include <stdint.h>
#include <stddef.h>
#ifdef __wasm__
extern void march_start(void) __asm__("_start");
extern void strata_browser_begin_mesh(void);
extern void strata_browser_end_mesh(void);
void strata_browser_init_camera(void);

static int32_t cells[32 * 32 * 32];
static uint32_t revision;
static int initialized;
static int editing;

void strata_browser_remesh(void) {
    strata_browser_begin_mesh();
    march_start();
    strata_browser_end_mesh();
    revision++;
}

int32_t *strata_browser_cells(void) { return cells; }
int strata_browser_ready(void) { return initialized; }
#else
#include <stdio.h>
#endif

/* A cube contributes at most six quads. The mesh array is borrowed by FFI. */
static uint32_t mesh[32 * 32 * 32 * 6 * 2];
static uint32_t bytes;
static double samples[3];
static int64_t seed = INT64_C(4294979641);

int64_t strata_browser_seed(void) { return seed; }

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
    for (size_t i = 0; i < sizeof cells / sizeof *cells; i++) {
        if (editing) data[i] = cells[i];
        else cells[i] = data[i];
    }
#else
    (void)array;
#endif
}

void strata_browser_publish(const void *quads, int64_t count,
                            double a, double b, double c) {
    if (count < 0 || count > (int64_t)(sizeof mesh / 8)) __builtin_trap();
    const uint32_t *words = (const uint32_t *)((const char *)quads + 32);
    for (int64_t i = 0; i < count * 2; i++) mesh[i] = words[i];
    bytes = (uint32_t)count * 8;
    samples[0] = a;
    samples[1] = b;
    samples[2] = c;
#ifndef __wasm__
    if (fwrite(&bytes, sizeof bytes, 1, stdout) != 1 ||
        fwrite(samples, sizeof samples, 1, stdout) != 1 ||
        fwrite(mesh, 1, bytes, stdout) != bytes) __builtin_trap();
#endif
}

#ifdef __wasm__
__attribute__((export_name("init")))
int init(uint32_t lo, uint32_t hi) {
    /* March Int is a signed 63-bit value. The fixture accepts nonnegative seeds. */
    if (hi > UINT32_C(0x3fffffff) || initialized) return 1;
    seed = (int64_t)(((uint64_t)hi << 32) | lo);
    strata_browser_remesh();
    if (bytes == 0) return 1;
    editing = initialized = 1;
    strata_browser_init_camera();
    return 0;
}

__attribute__((export_name("mesh_revision")))
uint32_t mesh_revision(void) { return revision; }

__attribute__((export_name("mesh_ptr")))
uint32_t mesh_ptr(void) { return (uint32_t)(uintptr_t)mesh; }

__attribute__((export_name("mesh_bytes")))
uint32_t mesh_bytes(void) { return bytes; }

__attribute__((export_name("noise_sample")))
double noise_sample(uint32_t i) { return i < 3 ? samples[i] : 0; }
#endif

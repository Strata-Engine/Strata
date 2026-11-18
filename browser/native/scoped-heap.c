/* Included after the pinned March WASM runtime. Only the chunk fixture
 * runs in this scope. Its FFI copies cells and mesh words to static
 * storage, and generated code does not cache heap-backed literals. */
static unsigned char *scope_base;
static uint32_t high_water;

void strata_browser_begin_mesh(void) {
    if (heap_ptr == 0) {
        heap_ptr = (unsigned char *)(__builtin_wasm_memory_size(0) * 65536);
        heap_end = heap_ptr;
    }
    scope_base = heap_ptr;
}

void strata_browser_end_mesh(void) {
    uint32_t used = (uint32_t)(heap_ptr - scope_base);
    if (used > high_water) high_water = used;
    heap_ptr = scope_base;
}

__attribute__((export_name("mesh_heap_high_water")))
uint32_t mesh_heap_high_water(void) { return high_water; }

#include <stdint.h>
#include <stddef.h>

extern void *memcpy(void *dst, const void *src, size_t count);
extern void *memset(void *dst, int byte, size_t count);

enum { FREE, LIVE, MARKED };
enum { PAGE = 65536, LIMIT = 256 * 1024 * 1024, BINS = 29 };

typedef struct Block {
    uint32_t size; /* Payload bytes, aligned to eight. */
    uint32_t state;
    struct Block *next;
    uint32_t reserved;
} Block;

_Static_assert(sizeof(Block) == 16, "WASM heap header");
static unsigned char starts[LIMIT / 64];
static Block *bins[BINS], *work;
static uintptr_t base, end, capacity;
static uint32_t live_bytes;

static void set_start(uintptr_t address, int used) {
    unsigned bit = 1u << ((address / 8) % 8);
    if (used) starts[address / 64] |= bit;
    else starts[address / 64] &= ~bit;
}

static unsigned bin(uint32_t size) { return 31u - __builtin_clz(size); }

static void insert_free(Block *block) {
    unsigned i = bin(block->size);
    block->next = bins[i];
    bins[i] = block;
}

void *march_alloc(int64_t bytes) {
    if (bytes < 0 || bytes > LIMIT - sizeof(Block) - 8) __builtin_trap();
    uint32_t size = bytes ? ((uint32_t)bytes + 7) & ~7u : 8;
    Block *block = NULL;
    for (unsigned i = bin(size); i < BINS && !block; i++) {
        Block **link = &bins[i];
        while (*link && (*link)->size < size) link = &(*link)->next;
        if (*link) {
            block = *link;
            *link = block->next;
        }
    }
    if (block) {
        if (block->size >= size + sizeof(Block) + 8) {
            Block *tail = (Block *)((char *)(block + 1) + size);
            tail->size = block->size - size - sizeof(Block);
            tail->state = FREE;
            insert_free(tail);
            block->size = size;
        }
    } else {
        if (!base) base = end = capacity = __builtin_wasm_memory_size(0) * PAGE;
        uintptr_t next = end + sizeof(Block) + size;
        if (next > LIMIT) __builtin_trap();
        if (next > capacity) {
            size_t pages = (next - capacity + PAGE - 1) / PAGE;
            if (__builtin_wasm_memory_grow(0, pages) == (size_t)-1) __builtin_trap();
            capacity += pages * PAGE;
        }
        block = (Block *)end;
        block->size = size;
        end = next;
    }
    block->state = LIVE;
    set_start((uintptr_t)(block + 1), 1);
    memset(block + 1, 0, block->size);
    return block + 1;
}

static void mark(uintptr_t address) {
    if (address < base + sizeof(Block) || address >= end || (address & 7)) return;
    if (!(starts[address / 64] & (1u << ((address / 8) % 8)))) return;
    Block *block = (Block *)address - 1;
    if (block->state != LIVE) return;
    block->state = MARKED;
    block->next = work;
    work = block;
}

/* Only called after March returns. The city and renderer state are the persistent heap
 * roots. The IR adapter disables cached heap literals. March object fields
 * and String data pointers in this build hold allocation starts. */
void ur_collect(void *city, void *motion) {
    mark((uintptr_t)city);
    mark((uintptr_t)motion);
    while (work) {
        Block *block = work;
        work = block->next;
        const char *data = (const char *)(block + 1);
        for (uint32_t offset = 0; offset < block->size; offset += sizeof(uint32_t)) {
            uint32_t candidate;
            memcpy(&candidate, data + offset, sizeof candidate);
            mark(candidate);
        }
    }

    memset(bins, 0, sizeof bins);
    live_bytes = 0;
    Block *free_run = NULL;
    for (uintptr_t addr = base; addr < end;) {
        Block *block = (Block *)addr;
        addr += sizeof(Block) + block->size;
        if (block->state == MARKED) {
            if (free_run) insert_free(free_run);
            free_run = NULL;
            block->state = LIVE;
            live_bytes += block->size;
        } else {
            if (block->state == LIVE) set_start((uintptr_t)(block + 1), 0);
            block->state = FREE;
            if (free_run) free_run->size += sizeof(Block) + block->size;
            else free_run = block;
        }
    }
    if (free_run) end = (uintptr_t)free_run;
}

__attribute__((export_name("ur_live_bytes")))
uint32_t ur_live_bytes(void) { return live_bytes; }

/* strata_lamps.h - which lamps a cell of the light volume lists.
 *
 * A cell (a chunk, 32 voxels a side, its first voxel at 32 * (cx, cy, cz))
 * lists at most `cap` of the glowing voxels whose light can reach it. When
 * more can, it keeps those nearest to it: a lamp's distance is from its
 * voxel to the cell's box (0 inside), so a lamp standing in the cell is
 * never dropped for one in a neighbouring chunk. A lamp no nearer than the
 * farthest one kept is not listed; of two at the same distance the one
 * listed first stays. Pure: no Vulkan, no globals. */
#ifndef STRATA_LAMPS_H
#define STRATA_LAMPS_H
#include <stdint.h>

/* the squared distance in voxels from voxel (x, y, z) to the cell box of chunk (cx, cy, cz) */
static inline int64_t lamp_cell_d2(const int32_t *l, int cx, int cy, int cz) {
    int c[3] = { cx, cy, cz };
    int64_t d2 = 0;
    for (int a = 0; a < 3; a++) {
        int64_t lo = (int64_t)c[a] * 32, hi = lo + 31, v = l[a];
        int64_t d = v < lo ? lo - v : (v > hi ? v - hi : 0);
        d2 += d * d;
    }
    return d2;
}

/* List lamp l (4 ints: x, y, z voxels, class) for the cell of chunk (cx, cy, cz)
 * whose list (4 ints an entry) holds *k of at most cap: appended while there
 * is room, else in place of the farthest kept when l is nearer. */
static inline void lamp_keep(int32_t *list, uint32_t *k, uint32_t cap, const int32_t *l, int cx, int cy, int cz) {
    if (*k < cap) {
        for (int a = 0; a < 4; a++) list[*k * 4 + a] = l[a];
        (*k)++;
        return;
    }
    if (cap == 0) return;
    uint32_t far = 0; int64_t fd = -1;
    for (uint32_t i = 0; i < cap; i++) {
        int64_t d = lamp_cell_d2(list + i * 4, cx, cy, cz);
        if (d > fd) { fd = d; far = i; }
    }
    if (lamp_cell_d2(l, cx, cy, cz) < fd)
        for (int a = 0; a < 4; a++) list[far * 4 + a] = l[a];
}
#endif

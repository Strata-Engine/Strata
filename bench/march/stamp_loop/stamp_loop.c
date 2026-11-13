/* The C of stamp_loop.march. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

typedef struct { int sx, sy, sz; int32_t *ids; } Stamp;

__attribute__((noinline)) static void blit(int32_t *c, const Stamp *t, int ox, int oy, int oz, int rot, int x0, int x1, int y0, int y1, int z0, int z1) {
    int r = rot & 3;
    for (int y = y0; y <= y1; y++)
        for (int z = z0; z <= z1; z++)
            for (int x = x0; x <= x1; x++) {
                int rx = x - ox, rz = z - oz, tx, tz;
                if (r == 0) { tx = rx; tz = rz; }
                else if (r == 1) { tx = rz; tz = t->sz - 1 - rx; }
                else if (r == 2) { tx = t->sx - 1 - rx; tz = t->sz - 1 - rz; }
                else { tx = t->sx - 1 - rz; tz = rx; }
                int32_t id = t->ids[tx + t->sx * ((y - oy) + t->sy * tz)];
                int i = x + 32 * (z + 32 * y);
                if (id != 0 && i >= 0 && i < 32768) c[i] = id;
            }
}

int main(void) {
    int reps = getenv("REPS") ? atoi(getenv("REPS")) : 1;
    Stamp t = { 9, 12, 9, calloc(972, sizeof(int32_t)) };
    for (int i = 0; i < 972; i++) {
        int x = i % 9, y = (i / 9) % 12, z = i / 108;
        t.ids[i] = (3 * x + y + 5 * z) % 4 == 0 ? 0 : 2 + (x + 2 * y + 3 * z) % 5;
    }
    int32_t *c = calloc(32768, sizeof(int32_t));
    for (int left = reps; left > 0; left--) blit(c, &t, 10, 11, 10, left, 10, 18, 11, 22, 10, 18);
    printf("%d\n", c[15 + 32 * (14 + 32 * 15)]);
    return 0;
}

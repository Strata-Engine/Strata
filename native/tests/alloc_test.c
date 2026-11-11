/* Tests for strata_alloc.h: a first-fit range allocator whose releases are
 * held for two frames before they can be reused. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "strata_alloc.h"

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

static void test_fill(void) {
    static SaAlloc a; sa_init(&a, 100);
    uint32_t f = 99;
    CHECK(sa_alloc(&a, 60, &f) && f == 0, "first alloc starts at 0");
    CHECK(sa_alloc(&a, 40, &f) && f == 60, "second follows it");
    CHECK(!sa_alloc(&a, 1, &f), "a full allocator refuses");
    CHECK(sa_live(&a) == 100, "live counts both");
    CHECK(!sa_alloc(&a, 0, &f) || 1, "a zero-length alloc does not crash");
}

static void test_deferred(void) {
    static SaAlloc a; sa_init(&a, 100);
    uint32_t f = 0;
    sa_alloc(&a, 100, &f);
    sa_release(&a, (SaRange){ 0, 100 }, 10);
    CHECK(sa_live(&a) == 0, "released is no longer live");
    sa_collect(&a, 10);
    CHECK(!sa_alloc(&a, 1, &f), "not reusable in the frame it was released");
    sa_collect(&a, 11);
    CHECK(!sa_alloc(&a, 1, &f), "nor the next frame");
    sa_collect(&a, 12);
    CHECK(sa_alloc(&a, 100, &f) && f == 0, "reusable two frames on");
}

static void test_merge(void) {
    static SaAlloc a; sa_init(&a, 90);
    uint32_t x, y, z, f;
    sa_alloc(&a, 30, &x); sa_alloc(&a, 30, &y); sa_alloc(&a, 30, &z);
    /* out of order, so merging must work whichever neighbour arrives first */
    sa_release(&a, (SaRange){ z, 30 }, 1);
    sa_release(&a, (SaRange){ x, 30 }, 1);
    sa_release(&a, (SaRange){ y, 30 }, 1);
    sa_collect(&a, 3);
    CHECK(sa_alloc(&a, 90, &f) && f == 0, "three adjacent releases merge into one range");
}

static void test_zero(void) {
    static SaAlloc a; sa_init(&a, 10);
    sa_release(&a, (SaRange){ 3, 0 }, 1);
    sa_collect(&a, 5);
    CHECK(sa_live(&a) == 0 && sa_free_total(&a) == 10 && sa_held_total(&a) == 0, "releasing nothing changes nothing");
}

static void test_random(void) {
    enum { TOTAL = 4096, MAXLIVE = 512 };
    static SaAlloc a; sa_init(&a, TOTAL);
    static SaRange live[MAXLIVE]; int nlive = 0;
    static unsigned char owner[TOTAL];           /* 1 where some live range covers the element */
    memset(owner, 0, sizeof owner);
    unsigned s = 12345u;
    int overlap = 0, broke = 0;
    for (uint64_t frame = 0; frame < 10000; frame++) {
        s = s * 1103515245u + 12345u;
        unsigned r = (s >> 16) & 0x7fff;
        if ((r % 3) != 0 && nlive < MAXLIVE) {
            uint32_t n = 1 + (r % 97), f = 0;
            if (sa_alloc(&a, n, &f)) {
                for (uint32_t i = f; i < f + n; i++) { if (owner[i]) overlap = 1; owner[i] = 1; }
                live[nlive++] = (SaRange){ f, n };
            }
        } else if (nlive > 0) {
            int k = (int)(r % (unsigned)nlive);
            for (uint32_t i = live[k].first; i < live[k].first + live[k].count; i++) owner[i] = 0;
            sa_release(&a, live[k], frame);
            live[k] = live[--nlive];
        }
        sa_collect(&a, frame);
        if (sa_live(&a) + sa_free_total(&a) + sa_held_total(&a) != TOTAL) broke = 1;
    }
    CHECK(!overlap, "no two live ranges ever overlap");
    CHECK(!broke, "live + free + held is the total at every step");
}

int main(void) {
    test_fill(); test_deferred(); test_merge(); test_zero(); test_random();
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("alloc_test: ok\n");
    return 0;
}

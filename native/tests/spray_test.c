/* The spray ring: appending wraps, the live span is what is younger than
 * the longest life, counts are clamped, a full ring overwrites its oldest. */
#include <stdio.h>
#include "strata_spray.h"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL: %s\n", what); fails++; } } while (0)

int main(void) {
    static float births[16];
    SprayRing r;
    sp_init(&r, births, 16);
    CHECK(sp_count(&r) == 0, "a new ring is empty");

    CHECK(sp_clamp(700) == 512, "a count over 512 is clamped");
    CHECK(sp_clamp(-3) == 0, "a negative count is nothing");
    CHECK(sp_clamp(40) == 40, "a small count is kept");

    /* six particles born at time 1, living 2 seconds */
    for (int i = 0; i < 6; i++) CHECK(sp_push(&r, 1.0f, 2.0f) == (uint32_t)i, "slots are handed out in order");
    sp_expire(&r, 1.5f);
    CHECK(sp_count(&r) == 6 && sp_first(&r) == 0, "all six live at 1.5");

    /* four more at time 2.5 with a shorter life: the longest life seen still decides */
    for (int i = 0; i < 4; i++) sp_push(&r, 2.5f, 0.5f);
    sp_expire(&r, 2.9f);
    CHECK(sp_count(&r) == 10, "nothing has outlived the longest life at 2.9");
    sp_expire(&r, 3.1f);
    CHECK(sp_count(&r) == 4 && sp_first(&r) == 6, "the first six have at 3.1");
    sp_expire(&r, 9.0f);
    CHECK(sp_count(&r) == 0, "and everything by 9");

    /* wrapping: twenty into a ring of sixteen */
    for (int i = 0; i < 20; i++) sp_push(&r, 10.0f, 1.0f);
    sp_expire(&r, 10.1f);
    CHECK(sp_count(&r) == 16, "a full ring holds its capacity, the oldest overwritten");
    CHECK(sp_first(&r) == (10 + 20) % 16, "its first live slot follows the newest");
    CHECK(sp_push(&r, 10.0f, 1.0f) == (10 + 20) % 16, "and the next push takes the oldest slot");

    if (fails == 0) printf("spray_test: ok\n");
    return fails == 0 ? 0 : 1;
}

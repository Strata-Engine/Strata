/* Tests for strata_lamps.h: a light-volume cell lists the lamps nearest it
 * when more can reach it than it has room for. */
#include <stdio.h>
#include <stdint.h>
#include "strata_lamps.h"

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

static int listed(const int32_t *list, uint32_t k, int x, int y, int z) {
    for (uint32_t i = 0; i < k; i++) if (list[i * 4] == x && list[i * 4 + 1] == y && list[i * 4 + 2] == z) return 1;
    return 0;
}

static void test_distance(void) {
    int32_t in[4] = { 40, 5, 70, 12 }, beside[4] = { 30, 5, 70, 12 }, corner[4] = { 66, 34, 50, 12 };
    CHECK(lamp_cell_d2(in, 1, 0, 2) == 0, "a lamp inside the cell is at 0");
    CHECK(lamp_cell_d2(beside, 1, 0, 2) == 4, "two voxels short of x 32: 4");
    CHECK(lamp_cell_d2(corner, 1, 0, 1) == 3 * 3 + 3 * 3, "past two faces (x 66 to 63, y 34 to 31): the sum of squares");
    int32_t neg[4] = { -40, -1, -3, 12 };
    CHECK(lamp_cell_d2(neg, -1, 0, -1) == 8 * 8 + 1, "negative chunks: (-40 to -32) and (-1 to 0)");
}

static void test_room(void) {
    int32_t list[4 * 4]; uint32_t k = 0;
    for (int i = 0; i < 3; i++) { int32_t l[4] = { 100 + i, 0, 0, 12 }; lamp_keep(list, &k, 4, l, 0, 0, 0); }
    CHECK(k == 3, "while there is room every lamp is listed");
    CHECK(listed(list, k, 100, 0, 0) && listed(list, k, 102, 0, 0), "in the order given");
}

/* the scene-17 case: the cell's own lamp comes after the cap is full of farther ones */
static void test_own_lamp_kept(void) {
    int32_t list[16 * 4]; uint32_t k = 0;
    for (int i = 0; i < 16; i++) { int32_t l[4] = { -10 - (i % 4), 40 + i, 5, 12 }; lamp_keep(list, &k, 16, l, 0, 1, 0); }
    CHECK(k == 16, "the cap is full");
    int32_t own[4] = { 16, 40, 16, 12 };
    lamp_keep(list, &k, 16, own, 0, 1, 0);
    CHECK(k == 16, "it stays at the cap");
    CHECK(listed(list, k, 16, 40, 16), "the cell's own lamp is listed though it came last");
    CHECK(!listed(list, k, -13, 43, 5), "in place of the farthest (13 voxels out)");
    int32_t farther[4] = { -15, 40, 5, 12 };
    lamp_keep(list, &k, 16, farther, 0, 1, 0);
    CHECK(!listed(list, k, -15, 40, 5), "a lamp farther than every kept one is not listed");
    int32_t tie[4] = { -13, 60, 5, 12 };
    lamp_keep(list, &k, 16, tie, 0, 1, 0);
    CHECK(!listed(list, k, -13, 60, 5) && listed(list, k, -13, 47, 5), "at the farthest kept distance the listed one stays");
}

int main(void) {
    test_distance();
    test_room();
    test_own_lamp_kept();
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("lamps: all passed\n");
    return 0;
}

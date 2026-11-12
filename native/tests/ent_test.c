/* The entity kernel, headless: kinds and their refusals, adding and reading,
 * slots, stale ids, bad numbers, a full world, each and columns, the hash's
 * determinism, blocks and the ray, the tick, the grid, the sweep, walking,
 * routes, seen and unseen, and setting on the ground. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "strata_ent.h"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL: %s\n", what); fails++; } } while (0)

/* ints: mover, unseen, divisor, float slots, int slots, gravity, figure, body, voices */
static void kind_ints(int32_t ki[9], int mover, int divisor, int fslots, int islots, int body) {
    ki[0] = mover; ki[1] = 0; ki[2] = divisor; ki[3] = fslots; ki[4] = islots;
    ki[5] = 0; ki[6] = 1; ki[7] = body; ki[8] = 2;
}
/* floats: hx, hy, hz, step height, lifetime, seen distance, gravity */
static const float kind_floats[7] = {0.25f, 0.5f, 0.25f, 0.5f, 0, 0, 9.8f};

static EnWorld *world(int capacity) { return en_open(capacity, 4096, 128, 160.0, 3.2, 0.1); }
static int plain_kind(EnWorld *w, int fslots, int islots) {
    int32_t ki[9];
    kind_ints(ki, 1, 1, fslots, islots, 0);
    return en_kind(w, ki, kind_floats);
}

static void test_kinds(void) {
    EnWorld *w = world(16);
    CHECK(w != NULL, "a world opens");
    CHECK(plain_kind(w, 3, 2) == 0, "a kind with 3 float and 2 int slots is index 0");
    CHECK(en_kind_int(w, 0, 3) == 3 && en_kind_int(w, 0, 4) == 2, "the kind's slot counts read back");
    CHECK(en_kind_int(w, 0, 6) == 1 && en_kind_int(w, 0, 7) == 0 && en_kind_int(w, 0, 8) == 2,
          "the kind's figure, body and voices read back");
    CHECK(en_kind_float(w, 0, 1) == 0.5f, "the kind's half height reads back");
    CHECK(en_kind_float(w, 0, 5) == 160.0f, "a seen distance of 0 reads as the seen cap");
    int32_t ki[9];
    kind_ints(ki, 1, 1, 9, 0, 0);
    CHECK(en_kind(w, ki, kind_floats) == -1, "a kind of 9 float slots is refused");
    CHECK(en_stat(w, 6) == EN_R_SLOTS, "... for its slots");
    kind_ints(ki, 1, 0, 0, 0, 0);
    CHECK(en_kind(w, ki, kind_floats) == -1, "a kind of divisor 0 is refused");
    CHECK(en_stat(w, 6) == EN_R_DIVISOR, "... for its divisor");
    kind_ints(ki, 4, 1, 0, 0, 0);
    CHECK(en_kind(w, ki, kind_floats) == -1, "a kind of mover 4 is refused");
    CHECK(en_stat(w, 6) == EN_R_MOVER, "... for its mover");
    kind_ints(ki, 2, 1, 0, 0, 1);
    CHECK(en_kind(w, ki, kind_floats) == -1, "a body on a walking kind is refused");
    CHECK(en_stat(w, 6) == EN_R_BODY, "... for its body");
    CHECK(en_stat(w, 3) == 4, "the four refusals are counted");
    for (int k = 1; k < EN_KINDS; k++) CHECK(plain_kind(w, 0, 0) == k, "kinds 1..61 are made in order");
    CHECK(plain_kind(w, 0, 0) == -1, "the 63rd kind is refused");
    CHECK(en_stat(w, 6) == EN_R_FULL, "... for a full table");
    en_close(w);
}

static void test_add_and_read(void) {
    EnWorld *w = world(16);
    int k = plain_kind(w, 3, 2);
    int64_t id = en_add(w, k, 1.5, 2.5, 3.5, 0.25);
    CHECK(id >= 0, "an entity is added");
    CHECK(en_alive(w, id) == 1, "it is alive");
    CHECK(en_stat(w, 0) == 1, "one is live");
    double v[3];
    CHECK(en_get(w, id, 0, v) == 1 && v[0] == 1.5 && v[1] == 2.5 && v[2] == 3.5, "its position reads (1.5, 2.5, 3.5)");
    CHECK(en_get(w, id, 2, v) == 1 && v[0] == 0.25, "its facing reads 0.25");
    CHECK(en_get(w, id, 1, v) == 1 && v[0] == 0 && v[1] == 0 && v[2] == 0, "its velocity reads 0");
    CHECK(en_get(w, id, 3, v) == 1 && v[0] == 0, "its age reads 0");
    CHECK(en_kind_of(w, id) == 0, "its kind is 0");
    for (int n = 0; n < EN_SLOTS; n++) {
        CHECK(en_float(w, id, n) == 0, "every float slot reads 0");
        CHECK(en_int(w, id, n) == 0, "every int slot reads 0");
    }
    for (int p = 0; p < 2 + EN_VOICES; p++) CHECK(en_part(w, id, p) == -1, "every part reads -1");
    en_close(w);
}

static void test_slots(void) {
    EnWorld *w = world(16);
    int k = plain_kind(w, 3, 2);
    int64_t id = en_add(w, k, 0, 0, 0, 0);
    en_set_float(w, id, 2, 7.5);
    CHECK(en_float(w, id, 2) == 7.5, "float slot 2 set to 7.5 reads 7.5");
    double before = en_stat(w, 3);
    en_set_float(w, id, 3, 1.0);
    CHECK(en_stat(w, 3) == before + 1 && en_stat(w, 6) == EN_R_SLOTS, "float slot 3 on a 3-slot kind is refused");
    CHECK(en_float(w, id, 3) == 0, "... and reads 0");
    en_set_int(w, id, 1, -42);
    CHECK(en_int(w, id, 1) == -42, "int slot 1 set to -42 reads -42");
    en_set_int(w, id, 2, 5);
    CHECK(en_int(w, id, 2) == 0 && en_stat(w, 6) == EN_R_SLOTS, "int slot 2 on a 2-slot kind is refused and reads 0");
    en_set_part(w, id, 0, 17);
    CHECK(en_part(w, id, 0) == 17, "the figure part set to 17 reads 17");
    en_set_part(w, id, 1, 3);
    CHECK(en_part(w, id, 1) == -1 && en_stat(w, 6) == EN_R_PART, "a body part on a kind without a body is refused");
    en_close(w);
}

static void test_stale_ids(void) {
    EnWorld *w = world(16);
    int k = plain_kind(w, 3, 2);
    int64_t a = en_add(w, k, 1, 2, 3, 0.5);
    en_set_float(w, a, 0, 4.0);
    en_set_int(w, a, 0, 9);
    en_set_part(w, a, 0, 11);
    en_remove(w, a, 0);
    int64_t b = en_add(w, k, 10, 20, 30, 0);
    CHECK(b >= 0 && b != a, "B is added with a new id");
    CHECK((b & EN_ROW_MASK) == (a & EN_ROW_MASK), "B takes A's row");
    CHECK(en_alive(w, a) == 0, "A is not alive");
    CHECK(en_alive(w, b) == 1, "B is alive");
    double before = en_stat(w, 3);
    double p[3] = {7, 7, 7};
    en_set(w, a, 0, p);
    double v[3];
    en_get(w, b, 0, v);
    CHECK(v[0] == 10 && v[1] == 20 && v[2] == 30, "a set through A leaves B's position unchanged");
    CHECK(en_stat(w, 3) == before + 1, "... and is counted once");
    CHECK(en_stat(w, 6) == EN_R_ENTITY, "... for a dead id");
    v[0] = v[1] = v[2] = 99;
    CHECK(en_get(w, a, 0, v) == 0 && v[0] == 0 && v[1] == 0 && v[2] == 0, "A's position reads 0");
    v[0] = 99;
    CHECK(en_get(w, a, 2, v) == 0 && v[0] == 0, "A's facing reads 0");
    CHECK(en_kind_of(w, a) == -1, "A's kind reads -1");
    CHECK(en_float(w, a, 0) == 0 && en_int(w, a, 0) == 0, "A's slots read 0");
    CHECK(en_part(w, a, 0) == -1, "A's parts read -1");
    before = en_stat(w, 3);
    en_set_float(w, a, 0, 1); en_set_int(w, a, 0, 1); en_set_part(w, a, 0, 1); en_remove(w, a, 0);
    CHECK(en_stat(w, 3) == before + 4, "slot, part and remove calls through A are each counted");
    CHECK(en_float(w, b, 0) == 0 && en_int(w, b, 0) == 0 && en_part(w, b, 0) == -1, "B starts with A's slots and parts cleared");
    CHECK(en_alive(w, -1) == 0 && en_alive(w, 0) == 0 && en_alive(w, (int64_t)1 << 62) == 0, "made-up ids are not alive");
    en_close(w);
}

static void test_bad_numbers(void) {
    EnWorld *w = world(16);
    int k = plain_kind(w, 3, 2);
    int64_t id = en_add(w, k, 1, 2, 3, 0);
    double before = en_stat(w, 3);
    double nan3[3] = {1, NAN, 3}, inf3[3] = {INFINITY, 0, 0}, v[3];
    en_set(w, id, 0, nan3);
    en_set(w, id, 0, inf3);
    CHECK(en_stat(w, 3) == before + 2 && en_stat(w, 6) == EN_R_VALUE, "a NaN or an infinite position is refused and counted");
    en_get(w, id, 0, v);
    CHECK(v[0] == 1 && v[1] == 2 && v[2] == 3, "... and the position is unchanged");
    double age[3] = {5, 0, 0};
    en_set(w, id, 3, age);
    CHECK(en_stat(w, 3) == before + 3 && en_stat(w, 6) == EN_R_WHICH, "setting the age is refused and counted");
    en_set_float(w, id, 0, NAN);
    CHECK(en_float(w, id, 0) == 0 && en_stat(w, 6) == EN_R_VALUE, "a NaN slot is refused");
    CHECK(en_add(w, k, NAN, 0, 0, 0) == -1, "an add at a NaN is refused");
    CHECK(en_add(w, k, 0, 0, 0, INFINITY) == -1, "an add facing infinity is refused");
    CHECK(en_add(w, 5, 0, 0, 0, 0) == -1 && en_stat(w, 6) == EN_R_KIND, "an add of an unknown kind is refused");
    CHECK(en_stat(w, 0) == 1, "nothing refused was added");
    CHECK(en_open(0, 4096, 128, 160.0, 3.2, 0.1) == NULL, "a world of capacity 0 is refused");
    CHECK(en_open(1048577, 4096, 128, 160.0, 3.2, 0.1) == NULL, "a world over 1,048,576 is refused");
    CHECK(en_open(16, 0, 128, 160.0, 3.2, 0.1) == NULL, "a world of no route points is refused");
    CHECK(en_open(16, 4096, 0, 160.0, 3.2, 0.1) == NULL, "a world of no body budget is refused");
    CHECK(en_open(16, 4096, 128, NAN, 3.2, 0.1) == NULL, "a NaN seen cap is refused");
    CHECK(en_open(16, 4096, 128, 160.0, 0, 0.1) == NULL, "a chunk of 0 metres is refused");
    CHECK(en_open(16, 4096, 128, 160.0, 3.2, -0.1) == NULL, "a negative voxel is refused");
    en_close(w);
}

static void test_full_world(void) {
    EnWorld *w = world(4);
    int k = plain_kind(w, 0, 0);
    int64_t ids[4];
    for (int i = 0; i < 4; i++) ids[i] = en_add(w, k, i, 0, 0, 0);
    CHECK(ids[3] >= 0, "four entities fit a capacity of 4");
    double before = en_stat(w, 3);
    CHECK(en_add(w, k, 9, 0, 0, 0) == -1, "a fifth add is refused");
    CHECK(en_stat(w, 3) == before + 1 && en_stat(w, 6) == EN_R_FULL, "... and counted, for a full world");
    en_remove(w, ids[1], 0);
    CHECK(en_stat(w, 0) == 3, "a remove leaves 3 live");
    CHECK(en_add(w, k, 9, 0, 0, 0) >= 0, "after one remove a further add succeeds");
    CHECK(en_stat(w, 0) == 4, "4 live again");
    en_close(w);
}

static void test_each_and_columns(void) {
    EnWorld *w = world(16);
    int k0 = plain_kind(w, 2, 0), k1 = plain_kind(w, 1, 0);
    int64_t of1[3]; int n1 = 0;
    for (int i = 0; i < 8; i++) {   /* kind 0 at rows 0, 2, 4, 6, 7; kind 1 at rows 1, 3, 5 */
        int k = (i % 2 == 1 && i < 6) ? k1 : k0;
        int64_t id = en_add(w, k, i, i + 0.5, -i, i * 0.1);
        if (k == k1) of1[n1++] = id;
    }
    int64_t ids[8];
    CHECK(en_each(w, k1, 8, ids) == 3, "en_each finds the 3 of kind 1");
    CHECK(ids[0] == of1[0] && ids[1] == of1[1] && ids[2] == of1[2], "... in dense order (add order, nothing removed)");
    CHECK(en_each(w, k0, 8, ids) == 5, "en_each finds the 5 of kind 0");
    CHECK(en_each(w, k0, 2, ids) == 2, "en_each writes at most max");
    float col[24];
    CHECK(en_column(w, k1, 0, 8, col) == 3, "the position column of kind 1 has 3 rows");
    int ok = 1;
    for (int r = 0; r < 3; r++) {
        int i = 2 * r + 1;
        if (col[r * 3] != (float)i || col[r * 3 + 1] != (float)(i + 0.5) || col[r * 3 + 2] != (float)-i) ok = 0;
    }
    CHECK(ok, "... their 9 position floats in en_each order");
    CHECK(en_column(w, k1, 2, 8, col) == 3 && col[1] == (float)0.3, "the facing column of kind 1 reads back");
    CHECK(en_column(w, k1, 11, 8, col) == 0, "a float slot past the kind's is no column");
    float in[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    double before = en_stat(w, 3);
    en_set_column(w, k1, 0, 2, in);
    CHECK(en_stat(w, 3) == before + 1 && en_stat(w, 6) == EN_R_ROWS, "en_set_column with 2 rows is refused");
    double v[3];
    en_get(w, of1[0], 0, v);
    CHECK(v[0] == 1, "... and nothing is written");
    en_set_column(w, k1, 0, 3, in);
    ok = 1;
    for (int r = 0; r < 3; r++) {
        en_get(w, of1[r], 0, v);
        if (v[0] != in[r * 3] || v[1] != in[r * 3 + 1] || v[2] != in[r * 3 + 2]) ok = 0;
    }
    CHECK(ok, "with 3 rows every position reads back");
    float slot[3] = {0.5f, 1.5f, 2.5f};
    en_set_column(w, k1, 10, 3, slot);
    CHECK(en_float(w, of1[2], 0) == 2.5, "a float slot column writes each row's slot");
    en_remove(w, of1[1], 0);
    CHECK(en_each(w, k1, 8, ids) == 2 && ids[0] == of1[0] && ids[1] == of1[2], "a removed row drops out of en_each");
    en_close(w);
}

static void test_order_after_removal(void) {
    EnWorld *w = world(16);
    int k = plain_kind(w, 0, 0);
    int64_t a = en_add(w, k, 1, 0, 0, 0), b = en_add(w, k, 2, 0, 0, 0), c = en_add(w, k, 3, 0, 0, 0);
    en_remove(w, a, 0);
    int64_t ids[4];
    CHECK(en_each(w, k, 4, ids) == 2 && ids[0] == c && ids[1] == b, "after removing A of A, B, C, en_each gives C, B");
    int64_t d = en_add(w, k, 4, 0, 0, 0);
    CHECK((d & EN_ROW_MASK) == (a & EN_ROW_MASK), "D reuses A's row");
    CHECK(en_each(w, k, 4, ids) == 3 && ids[0] == c && ids[1] == b && ids[2] == d, "after adding D, en_each gives C, B, D");
    float col[12];
    CHECK(en_column(w, k, 0, 4, col) == 3 && col[0] == 3 && col[3] == 2 && col[6] == 4,
          "the position column follows the same order");
    en_close(w);
}

static void test_seen_cap(void) {
    EnWorld *w = en_open(16, 4096, 128, 100.3, 3.2, 0.1);
    int32_t ki[9];
    kind_ints(ki, 1, 1, 0, 0, 0);
    float fl[7];
    memcpy(fl, kind_floats, sizeof fl);
    fl[5] = 100.3f;
    CHECK(en_kind(w, ki, fl) == 0, "a kind seen exactly at the cap is accepted");
    fl[5] = nextafterf(100.3f, 1000.0f);
    CHECK(en_kind(w, ki, fl) == -1 && en_stat(w, 6) == EN_R_VALUE, "a kind seen past the cap is refused");
    en_close(w);
}

static uint32_t rng_state;
static uint32_t rng(void) { rng_state = rng_state * 1664525u + 1013904223u; return rng_state >> 8; }

static EnWorld *scripted(void) {
    EnWorld *w = world(256);
    int k0 = plain_kind(w, 3, 2), k1 = plain_kind(w, 1, 1);
    int64_t ids[256]; int n = 0;
    rng_state = 12345;
    for (int step = 0; step < 1000; step++) {
        uint32_t r = rng() % 10;
        if (r < 5 || n == 0) {
            int64_t id = en_add(w, (rng() & 1) ? k1 : k0, rng() % 1000 * 0.1, rng() % 100 * 0.1, rng() % 1000 * 0.1, 0.01 * (rng() % 628));
            if (id >= 0) ids[n++] = id;
        } else if (r < 8) {
            int64_t id = ids[rng() % n];
            double v[3] = {rng() % 50 * 0.5, -1.0, 0.25 * (rng() % 8)};
            en_set(w, id, rng() % 3, v);
            en_set_float(w, id, 0, rng() % 100 * 0.01);
            en_set_int(w, id, 0, rng() % 1000);
        } else {
            int i = (int)(rng() % n);
            en_remove(w, ids[i], 0);
            ids[i] = ids[--n];
        }
    }
    return w;
}

static void test_hash(void) {
    EnWorld *a = scripted(), *b = scripted();
    CHECK(en_stat(a, 0) > 0, "the script leaves entities live");
    CHECK(en_hash(a) == en_hash(b), "two worlds given the same 1,000 scripted calls hash the same");
    int64_t ids[1];
    en_each(b, 0, 1, ids);
    en_set_float(b, ids[0], 1, en_float(b, ids[0], 1) + 1);
    CHECK(en_hash(a) != en_hash(b), "changing one slot changes the hash");
    en_close(a); en_close(b);
}

/* ---- blocks, the ray and the tick ---- */
static const uint8_t two_blocks[2] = {0, 1};   /* id 0 air passes, id 1 stops */

/* a chunk of air with one solid voxel at local (lx, ly, lz) */
static void one_voxel_chunk(EnWorld *w, int cx, int cy, int cz, int lx, int ly, int lz) {
    static int32_t ids[32768];
    memset(ids, 0, sizeof ids);
    ids[lx + 32 * (lz + 32 * ly)] = 1;
    en_chunk(w, cx, cy, cz, ids);
}

static void test_masks(void) {
    EnWorld *w = world(16);
    en_block_table(w, 2, two_blocks);
    one_voxel_chunk(w, 1, 0, -1, 3, 4, 5);
    CHECK(en_solid(w, 32 + 3, 4, -32 + 5) == 1, "the voxel at local (3, 4, 5) of chunk (1, 0, -1) is solid");
    CHECK(en_solid(w, 32 + 4, 4, -32 + 5) == 0, "the voxel beside it is clear");
    CHECK(en_solid(w, 32 + 3, 5, -32 + 5) == 0, "the voxel above it is clear");
    CHECK(en_solid(w, 9 * 32, 9 * 32, 9 * 32) == -1, "chunk (9, 9, 9) is not held");
    en_chunk_uniform(w, 0, 0, 0, 1);
    CHECK(en_solid(w, 0, 0, 0) == 1 && en_solid(w, 31, 31, 31) == 1, "a uniform solid chunk is solid throughout");
    en_chunk_uniform(w, 0, 0, 1, 0);
    CHECK(en_solid(w, 5, 5, 40) == 0, "a uniform air chunk is clear");
    en_chunk_drop(w, 0, 0, 0);
    CHECK(en_solid(w, 0, 0, 0) == -1, "a dropped chunk is not held");
    CHECK(en_solid(w, 32 + 3, 4, -32 + 5) == 1, "... and the others are kept");
    en_chunk_uniform(w, 0, 0, 1, 7);
    CHECK(en_solid(w, 5, 5, 40) == 0, "an id past the block table passes");
    for (int i = 0; i < 3000; i++) en_chunk_uniform(w, i, -5, i % 7, i & 1);   /* the chunk table grows */
    CHECK(en_solid(w, 2999 * 32, -5 * 32, (2999 % 7) * 32) == 1 && en_solid(w, 2998 * 32, -5 * 32, (2998 % 7) * 32) == 0,
          "3,000 more chunks are each held");
    CHECK(en_solid(w, 32 + 3, 4, -32 + 5) == 1, "... and the first still reads");
    en_close(w);
}

static void test_ray(void) {
    EnWorld *w = world(16);
    en_block_table(w, 2, two_blocks);
    en_chunk_uniform(w, 0, -1, 0, 1);    /* a floor: voxels y -32..-1 */
    en_chunk_uniform(w, 0, 0, 0, 0);
    one_voxel_chunk(w, -1, 0, 0, 31, 2, 7);   /* voxel (-1, 2, 7) */
    int32_t out[4]; double t = -9;
    double o1[3] = {1.65, 2.0, 1.65}, down[3] = {0, -1, 0};
    CHECK(en_ray(w, o1, down, 10.0, out, &t) == 1, "a ray straight down hits the floor");
    CHECK(out[0] == 16 && out[1] == -1 && out[2] == 16 && out[3] == 0, "... its top voxel (16, -1, 16), face 0");
    CHECK(fabs(t - 2.0) < 1e-9, "... 2 m away");
    CHECK(en_ray(w, o1, down, 1.5, out, &t) == 0, "the same ray of 1.5 m misses");
    double o2[3] = {0.55, 0.25, 0.75}, west[3] = {-3, 0, 0};
    CHECK(en_ray(w, o2, west, 5.0, out, &t) == 1, "a ray west across the chunk border hits");
    CHECK(out[0] == -1 && out[1] == 2 && out[2] == 7 && out[3] == 2, "... voxel (-1, 2, 7) of chunk (-1, 0, 0), face 2");
    CHECK(fabs(t - 0.55) < 1e-9, "... 0.55 m away");
    double o3[3] = {1.6, 1.6, 1.6}, east[3] = {1, 0, 0};
    CHECK(en_ray(w, o3, east, 10.0, out, &t) == -1, "a ray east out of the held chunks answers -1");
    double zero[3] = {0, 0, 0};
    CHECK(en_ray(w, o3, zero, 10.0, out, &t) == 0, "a zero direction is a miss");
    double o4[3] = {1.65, -0.05, 1.65};
    CHECK(en_ray(w, o4, east, 10.0, out, &t) == 1 && t == 0 && out[1] == -1 && out[3] == 3,
          "a ray starting inside a solid voxel hits it at 0, by the face facing back along it");
    en_close(w);
}

/* a flying kind seen at the cap: gravity flag, gravity m/s2, lifetime */
static int fly_kind(EnWorld *w, int gravity, float g, float lifetime) {
    int32_t ki[9];
    kind_ints(ki, 1, 1, 0, 0, 0);
    ki[5] = gravity;
    float fl[7] = {0.1f, 0.1f, 0.1f, 0, lifetime, 0, g};
    return en_kind(w, ki, fl);
}
static double pos_axis(EnWorld *w, int64_t id, int axis) { double v[3]; en_get(w, id, 0, v); return v[axis]; }

/* events of the last tick as (code, a, i0); how many */
static int events(EnWorld *w, int64_t *ints, double *floats) { return en_events(w, 64, ints, floats); }

static void test_flying(void) {
    EnWorld *w = world(16);
    int k = fly_kind(w, 0, 0, 0);
    int64_t a = en_add(w, k, 0, 0, 0, 0), b = en_add(w, k, 300, 0, 0, 0);
    double v10[3] = {10, 0, 0};
    en_set(w, a, 1, v10); en_set(w, b, 1, v10);
    double focus[3] = {300, 0, 0};
    en_tick(w, 1, focus, 0.1);
    CHECK(fabs(pos_axis(w, a, 0) - 1.0) < 1e-6, "unseen, velocity (10, 0, 0) and dt 0.1 advance x by 1.0");
    CHECK(fabs(pos_axis(w, b, 0) - 301.0) < 1e-6, "seen with no blocks held, the same");
    CHECK(en_stat(w, 1) == 1, "one of the two is seen");
    en_tick(w, 1, focus, 0.1); en_tick(w, 1, focus, 0.1);
    CHECK(fabs(pos_axis(w, a, 0) - 3.0) < 1e-6, "... 1.0 a tick");
    double age[3]; en_get(w, a, 3, age);
    CHECK(fabs(age[0] - 0.3) < 1e-9, "three ticks of 0.1 age it 0.3");
    en_close(w);

    w = world(16);
    k = fly_kind(w, 1, 10.0f, 0);
    a = en_add(w, k, 0, 50, 0, 0);
    for (int i = 0; i < 60; i++) en_tick(w, 0, NULL, 1.0 / 60);
    CHECK(fabs((50 - pos_axis(w, a, 1)) - 5.0) < 0.1, "gravity 10 from rest falls 5 m in 1 s of 60 ticks, within 0.1");
    en_close(w);
}

/* a world with a solid chunk (1, 0, 0) (x from 3.2 m) and air chunk (0, 0, 0), a flyer at x 0.25 going east */
static EnWorld *wall_world(int64_t *id, double speed) {
    EnWorld *w = world(16);
    en_block_table(w, 2, two_blocks);
    en_chunk_uniform(w, 0, 0, 0, 0);
    en_chunk_uniform(w, 1, 0, 0, 1);
    int k = fly_kind(w, 0, 0, 0);
    *id = en_add(w, k, 0.25, 1.65, 1.65, 0);
    double v[3] = {speed, 0, 0};
    en_set(w, *id, 1, v);
    return w;
}

static void test_wall(void) {
    int64_t id, ints[64 * 7]; double floats[64 * 3];
    EnWorld *w = wall_world(&id, 10);
    double focus[3] = {0, 0, 0};
    int hits = 0;
    for (int i = 0; i < 6; i++) {
        en_tick(w, 1, focus, 0.1);
        int n = events(w, ints, floats);
        for (int e = 0; e < n; e++) if (ints[e * 7] == 2) {
            hits++;
            CHECK(ints[e * 7 + 1] == id && ints[e * 7 + 2] == 0, "the block hit names the flyer");
            CHECK(ints[e * 7 + 3] == 32 && ints[e * 7 + 4] == 16 && ints[e * 7 + 5] == 16 && ints[e * 7 + 6] == 3,
                  "... the wall voxel (32, 16, 16) and face 3");
            CHECK(fabs(floats[e * 3] - 3.2) < 1e-6, "... and the point on the wall");
        }
    }
    CHECK(hits == 1, "a seen flyer aimed at a wall 3 m away reports one block hit");
    CHECK(fabs(pos_axis(w, id, 0) - 3.2) < 1e-6, "it stops at the wall");
    double v[3]; en_get(w, id, 1, v);
    CHECK(v[0] == 0 && v[1] == 0 && v[2] == 0, "... with velocity 0");
    en_close(w);

    w = wall_world(&id, 10);
    for (int i = 0; i < 6; i++) en_tick(w, 0, NULL, 0.1);
    CHECK(fabs(pos_axis(w, id, 0) - 6.25) < 1e-6, "the same flyer unseen passes through");
    CHECK(events(w, ints, floats) == 0, "... reporting nothing");
    en_close(w);

    w = wall_world(&id, 100);
    en_tick(w, 1, focus, 0);   /* a tick of dt 0 makes it seen: an entity becomes seen at the end of a tick */
    en_tick(w, 1, focus, 10.0);
    CHECK(fabs(pos_axis(w, id, 0) - 3.2) < 1e-6, "one dt of 10 s at 100 m/s still stops at the wall");
    CHECK(events(w, ints, floats) == 1 && ints[0] == 2, "... with one block hit");
    en_close(w);
}

static void test_lifetime_and_removal(void) {
    int64_t ints[64 * 7]; double floats[64 * 3];
    EnWorld *w = world(16);
    int k = fly_kind(w, 0, 0, 0.5f);
    int64_t a = en_add(w, k, 1, 2, 3, 0);
    for (int i = 0; i < 5; i++) en_tick(w, 0, NULL, 0.1);
    CHECK(en_alive(w, a) && events(w, ints, floats) == 0, "a lifetime of 0.5 s lives through an age of 0.5");
    en_tick(w, 0, NULL, 0.1);
    CHECK(!en_alive(w, a), "it is removed on the tick its age passes 0.5");
    CHECK(events(w, ints, floats) == 1 && ints[0] == 6 && ints[1] == a && ints[3] == 1, "... with event 6, reason 1");
    CHECK(floats[0] == 1 && floats[1] == 2 && floats[2] == 3, "... at its position");
    en_tick(w, 0, NULL, 0.1);
    CHECK(events(w, ints, floats) == 0, "the next tick clears the list");
    en_close(w);

    w = world(16);
    k = fly_kind(w, 0, 0, 0);
    a = en_add(w, k, 4, 5, 6, 0);
    en_tick(w, 0, NULL, 0.1);
    en_remove(w, a, 0);
    int64_t b = en_add(w, k, 0, 0, 0, 0);
    CHECK((b & EN_ROW_MASK) == (a & EN_ROW_MASK), "B takes removed A's row before the next tick");
    en_tick(w, 0, NULL, 0.1);
    CHECK(events(w, ints, floats) == 1 && ints[0] == 6 && ints[1] == a && ints[3] == 0,
          "the tick after the game removes A lists one event 6 for A, reason 0");
    CHECK(floats[0] == 4 && floats[1] == 5 && floats[2] == 6, "... at A's last position");
    CHECK(en_events(w, 0, NULL, NULL) == 1, "the count alone reads 1");
    en_tick(w, 0, NULL, 0.1);
    CHECK(events(w, ints, floats) == 0, "a third tick lists none");
    en_close(w);
}

static void test_dt_edges(void) {
    EnWorld *w = world(16);
    int k = fly_kind(w, 1, 10.0f, 0);
    int64_t a = en_add(w, k, 1, 2, 3, 0);
    double v[3] = {5, 5, 5}, p[3], age[3];
    en_set(w, a, 1, v);
    en_tick(w, 0, NULL, 0);
    en_get(w, a, 0, p); en_get(w, a, 3, age);
    CHECK(p[0] == 1 && p[1] == 2 && p[2] == 3 && age[0] == 0, "dt 0 moves nothing and ages nothing");
    CHECK(en_stat(w, 4) == 0, "... and is not a negative dt");
    en_tick(w, 0, NULL, -1);
    en_get(w, a, 0, p); en_get(w, a, 3, age);
    CHECK(p[0] == 1 && p[1] == 2 && p[2] == 3 && age[0] == 0, "dt -1 moves nothing and ages nothing");
    en_get(w, a, 1, v);
    CHECK(v[0] == 5 && v[1] == 5 && v[2] == 5, "... nor changes the velocity");
    CHECK(en_stat(w, 4) == 1, "... and stat 4 is 1");
    en_close(w);
}

/* ---- the grid: near, the entity ray, touches ---- */
#pragma STDC FP_CONTRACT OFF   /* the brute forces compute as the kernel does, unfused */

/* a kind of mover (0 placed, 1 flying), cube half extent h, lifetime (0 none), no gravity, seen at the cap */
static int box_kind(EnWorld *w, int mover, float h, float lifetime) {
    int32_t ki[9];
    kind_ints(ki, mover, 1, 0, 0, 0);
    float fl[7] = {h, h, h, 0, lifetime, 0, 0};
    return en_kind(w, ki, fl);
}
static double urand(double lo, double hi) { return lo + (hi - lo) * (rng() % 1000000) / 1000000.0; }

static int cmp_i64(const void *a, const void *b) {
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return x < y ? -1 : x > y;
}
/* every live id of the world, kind by kind */
static int all_ids(EnWorld *w, int64_t *ids, int max) {
    int n = 0;
    for (int k = 0; k < w->nkinds; k++) n += en_each(w, k, max - n, ids + n);
    return n;
}

static int64_t big_ids[200000], got_ids[200000], want_ids[200000];
static int64_t ev_i[EN_EVENTS * 7]; static double ev_f[EN_EVENTS * 3];

/* the ids within radius of p whose kind is in mask, sorted */
static int brute_near(EnWorld *w, const double p[3], double radius, uint64_t mask, int64_t *out) {
    int n = all_ids(w, big_ids, 200000), m = 0;
    for (int i = 0; i < n; i++) {
        if (!((mask >> en_kind_of(w, big_ids[i])) & 1)) continue;
        double q[3]; en_get(w, big_ids[i], 0, q);
        double dx = q[0] - p[0], dy = q[1] - p[1], dz = q[2] - p[2];
        if (dx * dx + dy * dy + dz * dz <= radius * radius) out[m++] = big_ids[i];
    }
    qsort(out, (size_t)m, sizeof *out, cmp_i64);
    return m;
}

static void test_near(void) {
    EnWorld *w = en_open(8192, 4096, 128, 160.0, 3.2, 0.1);
    for (int k = 0; k < 3; k++) box_kind(w, 0, 0.25f, 0);
    rng_state = 777;
    int64_t first = -1;
    for (int i = 0; i < 5000; i++) {
        int64_t id = en_add(w, i % 3, urand(-60, 60), urand(-60, 60), urand(-60, 60), 0);
        if (i == 0) first = id;
    }
    en_tick(w, 0, NULL, 0);
    static const double radii[3] = {0.5, 4, 37};
    int same = 1, found = 0, empty = 0;
    for (int q = 0; q < 100; q++) {
        double p[3];
        if (q == 0) { p[0] = -37.5; p[1] = -12.25; p[2] = -50.0; }
        else if (q % 4 == 0) { static int64_t ids[2000]; en_each(w, q % 3, 2000, ids); en_get(w, ids[q * 7], 0, p); p[0] += 0.1; }
        else { p[0] = urand(-70, 70); p[1] = urand(-70, 70); p[2] = urand(-70, 70); }
        for (int r = 0; r < 3; r++) for (int m = 0; m < 2; m++) {
            uint64_t mask = m == 0 ? 7 : 5;   /* every kind; all but kind 1 */
            int nw = brute_near(w, p, radii[r], mask, want_ids);
            int ng = en_near(w, p, radii[r], mask, 200000, got_ids);
            if (ng != nw || memcmp(got_ids, want_ids, (size_t)nw * sizeof *got_ids) != 0) same = 0;
            if (en_near(w, p, radii[r], mask, 0, NULL) != nw) same = 0;
            found += nw; empty += nw == 0;
        }
    }
    CHECK(same, "near equals a brute-force filter at 100 points, radii 0.5, 4 and 37, with and without kind 1");
    CHECK(found > 5000 && empty > 100, "... over both found and empty answers");
    double o[3] = {0, 0, 0};
    CHECK(en_near(w, o, 1000000, 7, 0, NULL) == 5000, "a radius of 1,000 km finds all 5,000 (the walk over the kind's list)");
    int nw = brute_near(w, o, 37, 7, want_ids);
    CHECK(nw > 3 && en_near(w, o, 37, 7, 3, got_ids) == 3 && got_ids[0] == want_ids[0] && got_ids[2] == want_ids[2],
          "near with max 3 writes the three lowest ids");
    double p0[3]; en_get(w, first, 0, p0);
    CHECK(en_near(w, p0, 0.01, 7, 16, got_ids) == 1 && got_ids[0] == first, "near finds an entity at its own position");
    en_remove(w, first, 0);
    int64_t late = en_add(w, 0, p0[0], p0[1], p0[2], 0);
    CHECK(en_near(w, p0, 0.01, 7, 16, got_ids) == 0, "between ticks, near names neither the removed entity nor the one just added");
    en_tick(w, 0, NULL, 0);
    CHECK(en_near(w, p0, 0.01, 7, 16, got_ids) == 1 && got_ids[0] == late, "after the next tick it names the new one");
    CHECK(en_near(w, p0, NAN, 7, 16, got_ids) == 0 && en_near(w, p0, -1, 7, 16, got_ids) == 0, "a NaN or negative radius finds nothing");
    en_close(w);
}

/* the ray's distance into the box lo..hi from o along unit u within max; 0 if it misses */
static int slab(const double o[3], const double u[3], double max, const double lo[3], const double hi[3], double *t) {
    double tn = 0, tf = max;
    for (int a = 0; a < 3; a++) {
        if (u[a] == 0) { if (o[a] < lo[a] || o[a] > hi[a]) return 0; continue; }
        double t1 = (lo[a] - o[a]) / u[a], t2 = (hi[a] - o[a]) / u[a];
        if (t1 > t2) { double s = t1; t1 = t2; t2 = s; }
        if (t1 > tn) tn = t1;
        if (t2 < tf) tf = t2;
    }
    if (tn > tf) return 0;
    *t = tn;
    return 1;
}

static void test_ray_entities(void) {
    EnWorld *w = en_open(4096, 4096, 128, 160.0, 3.2, 0.1);
    static const float halves[3] = {0.25f, 0.5f, 1.0f};
    for (int k = 0; k < 3; k++) box_kind(w, 0, halves[k], 0);
    rng_state = 4242;
    for (int i = 0; i < 2000; i++) en_add(w, i % 3, urand(-40, 40), urand(-10, 10), urand(-40, 40), 0);
    en_tick(w, 0, NULL, 0);
    int n = all_ids(w, big_ids, 200000), same = 1, hits = 0;
    for (int q = 0; q < 500; q++) {
        double o[3] = {urand(-50, 50), urand(-12, 12), urand(-50, 50)};
        double d[3] = {urand(-1, 1), urand(-0.3, 0.3), urand(-1, 1)};
        if (q % 50 == 0) { d[1] = 0; d[2] = 0; }   /* along an axis */
        double max = urand(1, 150);
        uint64_t mask = q % 2 ? 7 : 3;
        double len = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        double u[3] = {d[0] / len, d[1] / len, d[2] / len};
        int64_t want = -1; double wt = 0;
        for (int i = 0; i < n; i++) {
            int k = en_kind_of(w, big_ids[i]);
            if (!((mask >> k) & 1)) continue;
            double p[3], lo[3], hi[3], t;
            en_get(w, big_ids[i], 0, p);
            for (int a = 0; a < 3; a++) { lo[a] = p[a] - (double)halves[k]; hi[a] = p[a] + (double)halves[k]; }
            if (slab(o, u, max, lo, hi, &t) && (want < 0 || t < wt || (t == wt && big_ids[i] < want))) { want = big_ids[i]; wt = t; }
        }
        double t = -9;
        int64_t got = en_ray_entities(w, o, d, max, mask, &t);
        if (got != want || (want >= 0 && t != wt)) same = 0;
        hits += want >= 0;
    }
    CHECK(same, "the entity ray equals a brute-force nearest box hit for 500 rays");
    CHECK(hits >= 100 && hits <= 450, "... of which some hit and some miss");
    double o[3] = {0, 0, 0}, zero[3] = {0, 0, 0}, x[3] = {1, 0, 0}, t;
    int64_t twin_a = en_add(w, 0, 0, 100, 5, 0), twin_b = en_add(w, 0, 0, 100, 5, 0);
    en_tick(w, 0, NULL, 0);
    double above[3] = {0, 100, 0}, along_z[3] = {0, 0, 1};
    CHECK(en_ray_entities(w, above, along_z, 10, 7, &t) == (twin_a < twin_b ? twin_a : twin_b) && t == 4.75,
          "two boxes met at the same distance: the lower id, 4.75 m");
    CHECK(en_ray_entities(w, o, zero, 10, 7, &t) == -1, "a zero direction hits nothing");
    CHECK(en_ray_entities(w, o, x, 10, 0, &t) == -1, "an empty mask hits nothing");
    en_close(w);
}

static int cmp_pair(const void *a, const void *b) {
    const int64_t *x = (const int64_t *)a, *y = (const int64_t *)b;
    return x[0] != y[0] ? (x[0] < y[0] ? -1 : 1) : x[1] < y[1] ? -1 : x[1] > y[1];
}
/* the touch events of the last tick, as (a, b) pairs, sorted; how many */
static int touch_pairs(EnWorld *w, int64_t *pairs) {
    int n = en_events(w, EN_EVENTS, ev_i, ev_f), m = 0;
    for (int e = 0; e < n; e++) if (ev_i[e * 7] == EN_E_TOUCH) { pairs[2 * m] = ev_i[e * 7 + 1]; pairs[2 * m + 1] = ev_i[e * 7 + 2]; m++; }
    qsort(pairs, (size_t)m, 2 * sizeof *pairs, cmp_pair);
    return m;
}

static int64_t t_ids[2000]; static double t_from[2000 * 3];
/* the box an entity swept over the last tick: from t_from[i] to its position, grown by h */
static void swept(EnWorld *w, int i, double h, double lo[3], double hi[3]) {
    double p[3]; en_get(w, t_ids[i], 0, p);
    for (int a = 0; a < 3; a++) {
        double f = t_from[i * 3 + a];
        lo[a] = (f < p[a] ? f : p[a]) - h; hi[a] = (f > p[a] ? f : p[a]) + h;
    }
}
static int boxes_meet(const double alo[3], const double ahi[3], const double blo[3], const double bhi[3]) {
    for (int a = 0; a < 3; a++) if (!(alo[a] < bhi[a] && blo[a] < ahi[a])) return 0;
    return 1;
}
/* every touching pair among t_ids[0..n): kinds whose pair bit is set in pairs[k] (k <= j) */
static int brute_touches(EnWorld *w, int n, const double *half, uint64_t declared[2], int64_t *out) {
    int m = 0;
    for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++) {
        int ki = en_kind_of(w, t_ids[i]), kj = en_kind_of(w, t_ids[j]);
        int lo_k = ki < kj ? ki : kj, hi_k = ki < kj ? kj : ki;
        if (!((declared[lo_k] >> hi_k) & 1)) continue;
        double alo[3], ahi[3], blo[3], bhi[3];
        swept(w, i, half[ki], alo, ahi); swept(w, j, half[kj], blo, bhi);
        if (!boxes_meet(alo, ahi, blo, bhi)) continue;
        int64_t a = t_ids[i], b = t_ids[j];
        if (ki > kj || (ki == kj && a > b)) { int64_t s = a; a = b; b = s; }
        out[2 * m] = a; out[2 * m + 1] = b; m++;
    }
    qsort(out, (size_t)m, 2 * sizeof *out, cmp_pair);
    return m;
}

static void test_touches(void) {
    EnWorld *w = en_open(4096, 4096, 128, 160.0, 3.2, 0.1);
    double half[2] = {0.25, 0.5};
    box_kind(w, 1, 0.25f, 0); box_kind(w, 0, 0.5f, 0);
    rng_state = 99;
    for (int i = 0; i < 2000; i++) {
        int k = i % 4 == 0 ? 1 : 0;
        int64_t id = en_add(w, k, urand(-25, 25), urand(-6, 6), urand(-25, 25), 0);
        if (k == 0) { double v[3] = {urand(-60, 60), urand(-10, 10), urand(-60, 60)}; en_set(w, id, 1, v); }
    }
    en_touching(w, 0, 1);
    int n = all_ids(w, t_ids, 2000);
    for (int i = 0; i < n; i++) en_get(w, t_ids[i], 0, t_from + i * 3);
    en_tick(w, 0, NULL, 1.0 / 60);
    uint64_t declared[2] = {2, 0};
    int nw = brute_touches(w, n, half, declared, want_ids);
    int ng = touch_pairs(w, got_ids);
    CHECK(nw > 100, "the scattered 2,000 make over 100 touching pairs");
    CHECK(ng == nw && memcmp(got_ids, want_ids, (size_t)nw * 2 * sizeof *got_ids) == 0,
          "touches over 2,000 entities of two kinds equal a brute-force pair test of the swept boxes, a the flyer");
    /* the pair again in the other order, and kind 1 against itself; a tick of dt 0 sweeps nothing */
    en_touching(w, 1, 0); en_touching(w, 1, 1);
    for (int i = 0; i < n; i++) en_get(w, t_ids[i], 0, t_from + i * 3);
    en_tick(w, 0, NULL, 0);
    declared[0] = 2; declared[1] = 2;
    nw = brute_touches(w, n, half, declared, want_ids);
    ng = touch_pairs(w, got_ids);
    int same_kind = 0;
    for (int i = 0; i < nw; i++) same_kind += en_kind_of(w, want_ids[2 * i]) == 1;
    CHECK(same_kind > 10, "kind 1 touches itself in over 10 pairs");
    CHECK(ng == nw && memcmp(got_ids, want_ids, (size_t)nw * 2 * sizeof *got_ids) == 0,
          "with (0, 1) declared twice and (1, 1) added, each pair is reported once, a of the same kind the lower id");
    double before = en_stat(w, 3);
    en_touching(w, 0, 0);
    CHECK(en_stat(w, 3) == before, "a pair of kinds is taken");
    en_touching(w, 0, 2); en_touching(w, -1, 0);
    CHECK(en_stat(w, 3) == before + 2 && en_stat(w, 6) == EN_R_KIND, "a pair naming no such kind is refused and counted");
    en_close(w);
}

/* kind 0 a flyer of half 0.05 at the origin going east at speed, kind 1 a target of half 0.2 at x = 5 */
static EnWorld *range_world(int64_t *flyer, int64_t *target, double speed, int declare) {
    EnWorld *w = world(16);
    box_kind(w, 1, 0.05f, 0); box_kind(w, 0, 0.2f, 0);
    *flyer = en_add(w, 0, 0, 1, 0, 0);
    *target = en_add(w, 1, 5, 1, 0, 0);
    double v[3] = {speed, 0, 0};
    en_set(w, *flyer, 1, v);
    if (declare) en_touching(w, 0, 1);
    return w;
}

static void test_fast_and_undeclared(void) {
    int64_t f, g, pairs[16];
    EnWorld *w = range_world(&f, &g, 600, 1);
    en_tick(w, 0, NULL, 1.0 / 60);
    CHECK(fabs(pos_axis(w, f, 0) - 10.0) < 1e-9, "a flyer at 600 m/s with dt 1/60 steps from x 0 to 10");
    CHECK(touch_pairs(w, pairs) == 1 && pairs[0] == f && pairs[1] == g, "... and reports touching the 0.4 m target at x 5 it passed");
    en_tick(w, 0, NULL, 1.0 / 60);
    CHECK(touch_pairs(w, pairs) == 0, "the next step, past it, reports nothing");
    en_close(w);
    w = range_world(&f, &g, 600, 0);
    en_tick(w, 0, NULL, 1.0 / 60);
    CHECK(en_events(w, 0, NULL, NULL) == 0, "the same pass with the pair not declared reports nothing");
    en_close(w);
    w = range_world(&f, &g, 0, 0);
    en_touching(w, 0, 1); en_touching(w, 1, 0); en_touching(w, 0, 1);
    double at[3] = {5.1, 1, 0}; en_set(w, f, 0, at);
    box_kind(w, 0, 0.5f, 0);   /* kind 2: two boxes whose faces meet at x 20.5, which is not a touch */
    en_add(w, 2, 20, 1, 0, 0); en_add(w, 2, 21, 1, 0, 0);
    en_add(w, 2, 31, 1, 0, 0); en_add(w, 2, 30, 1, 0, 0);   /* and with the lower id on the right */
    en_touching(w, 2, 2);
    int once = 1;
    for (int t = 0; t < 3; t++) {
        en_tick(w, 0, NULL, 1.0 / 60);
        if (touch_pairs(w, pairs) != 1 || pairs[0] != f || pairs[1] != g) once = 0;
    }
    CHECK(once, "an overlapping pair declared three times reports once a tick, three ticks running; faces that only meet do not touch");
    en_close(w);
}

/* Two flyers of one kind (half 0.2, pair (0, 0) declared), A at a going va and B at b going vb, one step of 1/60 s;
 * how many touches, and in *pair the (a, b) of the first. Worked by hand, not by the brute force. */
static int two_flyers(const double a[3], const double va[3], const double b[3], const double vb[3], int64_t ids[2], int64_t pair[2]) {
    EnWorld *w = world(16);
    box_kind(w, 1, 0.2f, 0);
    ids[0] = en_add(w, 0, a[0], a[1], a[2], 0);
    ids[1] = en_add(w, 0, b[0], b[1], b[2], 0);
    en_set(w, ids[0], 1, va); en_set(w, ids[1], 1, vb);
    en_touching(w, 0, 0);
    en_tick(w, 0, NULL, 1.0 / 60);
    int64_t pairs[16];
    int n = touch_pairs(w, pairs);
    if (n > 0) { pair[0] = pairs[0]; pair[1] = pairs[1]; }
    en_close(w);
    return n;
}

static void test_flyer_against_flyer(void) {
    int64_t ids[2], pair[2] = {-1, -1};
    /* A from (-5, 0, 0) to (5, 0, 0) and B from (0, 0, -5) to (0, 0, 5) in one step: they cross at the origin */
    double a0[3] = {-5, 0, 0}, east[3] = {600, 0, 0}, b0[3] = {0, 0, -5}, north[3] = {0, 0, 600};
    CHECK(two_flyers(a0, east, b0, north, ids, pair) == 1 && pair[0] == ids[0] && pair[1] == ids[1],
          "two flyers crossing in one 10 m step report one touch, a the lower id");
    /* parallel paths 0.3 m apart, both stepping 2 m east, B 1.5 m ahead: never nearer than 1.5 m, but each swept box
     * (A x -0.2..2.2, B x 1.3..3.7; z -0.2..0.2 and 0.1..0.5) overlaps the other: the near miss the reading costs */
    double p0[3] = {0, 0, 0}, slow[3] = {120, 0, 0}, q0[3] = {1.5, 0, 0.3};
    CHECK(two_flyers(p0, slow, q0, slow, ids, pair) == 1, "parallel flyers 0.3 m apart, one 1.5 m ahead, report a near miss");
    double r0[3] = {1.5, 0, 0.5};
    CHECK(two_flyers(p0, slow, r0, slow, ids, pair) == 0, "the same pair 0.5 m apart reports none");
}

static void test_event_overflow(void) {
    EnWorld *w = en_open(1 << 17, 4096, 128, 160.0, 3.2, 0.1);
    box_kind(w, 0, 0.01f, 0); box_kind(w, 0, 100.0f, 0);
    for (int i = 0; i < 70000; i++) en_add(w, 0, (i % 50) * 1.5 - 37, (i / 50 % 40) * 1.5 - 30, (i / 2000) * 1.5 - 26, 0);
    en_add(w, 1, 0, 0, 0, 0);
    en_touching(w, 0, 1);
    en_tick(w, 0, NULL, 0);
    int n = en_events(w, EN_EVENTS, ev_i, ev_f), all_touch = 1;
    for (int e = 0; e < n; e++) if (ev_i[e * 7] != EN_E_TOUCH) all_touch = 0;
    CHECK(n == 65536 && all_touch, "70,000 touching pairs in one tick give 65,536 touch events");
    CHECK(en_stat(w, 5) == 4464, "... and stat 5 counts the 4,464 dropped");
    en_close(w);
}

static void test_removal_in_loop(void) {
    EnWorld *w = en_open(1024, 4096, 128, 160.0, 3.2, 0.1);
    box_kind(w, 1, 0.5f, 0); box_kind(w, 0, 0.5f, 0);
    for (int i = 0; i < 100; i++) { en_add(w, 0, i * 10.0, 0, 0, 0); en_add(w, 1, i * 10.0, 0, 0.25, 0); }
    en_touching(w, 0, 1);
    en_tick(w, 0, NULL, 1.0 / 60);
    int64_t pairs[512], ids[100], removed[50], added[10];
    CHECK(touch_pairs(w, pairs) == 100, "100 flyers each touch their target");
    int n = en_each(w, 0, 100, ids), nr = 0, na = 0;
    for (int i = 0; i < n; i++) {   /* walk the ids taken: remove every second, and add 10 while walking */
        if (i % 2 == 1) { en_remove(w, ids[i], 0); removed[nr++] = ids[i]; }
        if (i % 10 == 5) added[na++] = en_add(w, 0, (i + 0.5) * 10.0, 0, 0, 0);
    }
    for (int i = 0; i < na; i++) en_add(w, 1, (i * 10 + 5.5) * 10.0, 0, 0, 0);   /* a target for each new flyer */
    int defaults = 1;
    for (int i = 0; i < nr; i++) {
        double p[3] = {9, 9, 9};
        if (en_alive(w, removed[i]) || en_kind_of(w, removed[i]) != -1 || en_get(w, removed[i], 0, p) != 0 ||
            p[0] != 0 || p[1] != 0 || p[2] != 0) defaults = 0;
    }
    CHECK(defaults, "every removed id reads the defaults");
    double at[3] = {10, 0, 0};
    CHECK(en_near(w, at, 0.1, 1, 8, got_ids) == 0, "near between ticks does not name removed flyer 1");
    en_tick(w, 0, NULL, 1.0 / 60);
    int nev = en_events(w, EN_EVENTS, ev_i, ev_f), named = 0, sixes = 0, touches = 0, new_touch = 0;
    for (int e = 0; e < nev; e++) {
        int64_t *v = ev_i + e * 7;
        for (int i = 0; i < nr; i++) if (v[1] == removed[i] || v[2] == removed[i]) {
            if (v[0] == EN_E_REMOVED && v[3] == 0) sixes++; else named++;
        }
        if (v[0] == EN_E_TOUCH) { touches++; for (int i = 0; i < na; i++) new_touch += v[1] == added[i]; }
    }
    CHECK(sixes == 50, "the next tick lists each removed id once, as event 6 reason 0");
    CHECK(named == 0, "... and no other event names a removed id");
    CHECK(touches == 60 && new_touch == 10, "the 50 left and the 10 added each touch their target");
    en_close(w);

    w = world(16);
    box_kind(w, 1, 0.5f, 0.05f); box_kind(w, 0, 0.5f, 0);
    int64_t dying = en_add(w, 0, 0, 0, 0, 0);
    en_add(w, 1, 0.2, 0, 0, 0);
    en_touching(w, 0, 1);
    en_tick(w, 0, NULL, 0.1);
    nev = en_events(w, EN_EVENTS, ev_i, ev_f);
    CHECK(nev == 1 && ev_i[0] == EN_E_REMOVED && ev_i[1] == dying && ev_i[3] == 1,
          "an entity whose lifetime ends on the tick it would touch gives event 6 and no event 1");
    en_close(w);
}

/* ---- the sweep, walking and routes ---- */
static const uint8_t three_blocks[3] = {0, 1, 0};   /* 0 air, 1 stone, 2 a plant (a cross: the table says it passes) */

/* stone below y 0 (chunk y -6 to -1, from -19.2 m) and air above (chunk y 0 to 7, to 25.6 m), chunks x and z from c0
 * to c1: deep and tall enough that a walker near the floor has every chunk its grounding reads held */
static void flat_floor(EnWorld *w, int c0, int c1) {
    en_block_table(w, 3, three_blocks);
    for (int cx = c0; cx <= c1; cx++)
        for (int cz = c0; cz <= c1; cz++)
            for (int cy = -6; cy <= 7; cy++) en_chunk_uniform(w, cx, cy, cz, cy < 0 ? 1 : 0);
}
/* chunk (cx, 0, cz) of air with stone in local y below ly where local x is in x0..x1 */
static void ledge_chunk(EnWorld *w, int cx, int cz, int x0, int x1, int ly) {
    static int32_t ids[32768];
    memset(ids, 0, sizeof ids);
    for (int y = 0; y < ly; y++)
        for (int z = 0; z < 32; z++)
            for (int x = x0; x <= x1; x++) ids[x + 32 * (z + 32 * y)] = 1;
    en_chunk(w, cx, 0, cz, ids);
}

static void test_sweep(void) {
    EnWorld *w = world(16);
    flat_floor(w, 0, 0);
    en_chunk_uniform(w, 1, 0, 0, 1);   /* a wall from x 3.2 m */
    double h[3] = {0.3, 0.9, 0.3}, out[3]; int b[3];
    double p1[3] = {1.6, 2.0, 1.6}, down[3] = {0, -5, 0};
    CHECK(en_sweep(w, p1, h, down, out, b) == 0, "a box dropped onto the floor sweeps");
    CHECK(fabs(out[1] - 0.9 - 0.00001) < 1e-9, "... and rests with its base 1e-4 voxels (1e-5 m) above the floor");
    CHECK(out[0] == 1.6 && out[2] == 1.6, "... straight down");
    CHECK(b[0] == 0 && b[1] == 1 && b[2] == 0, "... blocked on y alone");
    double p2[3] = {2.0, 1.0, 1.6}, east[3] = {3, 0, 0};
    CHECK(en_sweep(w, p2, h, east, out, b) == 0, "a box pushed at the wall sweeps");
    CHECK(fabs(out[0] - (3.2 - 0.3 - 0.00001)) < 1e-9, "... and stops 1e-4 voxels short of it");
    CHECK(b[0] == 1 && b[1] == 0 && b[2] == 0, "... blocked on x alone");
    double small[3] = {0.5, 0, 0};
    CHECK(en_sweep(w, p2, h, small, out, b) == 0 && fabs(out[0] - 2.5) < 1e-12 && b[0] == 0, "a move short of the wall is not blocked");
    en_close(w);

    w = world(16);
    flat_floor(w, 0, 1);
    static int32_t ids[32768];
    for (int i = 0; i < 32768; i++) ids[i] = 2;   /* plants throughout chunk (1, 0, 0) */
    en_chunk(w, 1, 0, 0, ids);
    double p3[3] = {2.0, 1.0, 1.6};
    CHECK(en_sweep(w, p3, h, east, out, b) == 0 && fabs(out[0] - 5.0) < 1e-12 && b[0] == 0, "it passes through cross-style voxels");
    double far[3] = {4, 0, 0};
    double p4[3] = {5.0, 1.0, 1.6};
    out[0] = out[1] = out[2] = -7;
    CHECK(en_sweep(w, p4, h, far, out, b) == -1, "a box moved into a chunk not held answers -1");
    CHECK(out[0] == 5.0 && out[1] == 1.0 && out[2] == 1.6 && b[0] == 0 && b[1] == 0 && b[2] == 0, "... and does not move");
    double up_then_far[3] = {4, 0.5, 0};
    CHECK(en_sweep(w, p4, h, up_then_far, out, b) == -1 && out[1] == 1.0, "... not even on the axes swept before");
    en_close(w);
}

/* a walking kind: half extents 0.3 x 0.9 x 0.3, step height, gravity 9.8, divisor */
static int walk_kind(EnWorld *w, float step, int divisor) {
    int32_t ki[9];
    kind_ints(ki, 2, divisor, 0, 0, 0);
    ki[5] = 1;
    float fl[7] = {0.3f, 0.9f, 0.3f, step, 0, 0, 9.8f};
    return en_kind(w, ki, fl);
}
/* ticks of 1/60 with one focus at the entity's own position (so it is seen) */
static void seen_ticks(EnWorld *w, int64_t id, int n) {
    for (int i = 0; i < n; i++) {
        double p[3]; en_get(w, id, 0, p);
        en_tick(w, 1, p, 1.0 / 60);
    }
}

static void test_walking(void) {
    EnWorld *w = world(16);
    flat_floor(w, -1, 1);
    int k = walk_kind(w, 0.6f, 1);
    int64_t id = en_add(w, k, 1.6, 0.95, 1.6, 0);
    CHECK(en_grounded(w, id) == 0, "a walker just added is not grounded");
    seen_ticks(w, id, 2);
    double up[3] = {1.6, 5.9, 1.6};
    en_set(w, id, 0, up);
    seen_ticks(w, id, 1);
    CHECK(en_grounded(w, id) == 0, "a seen walker lifted 5 m is not grounded");
    seen_ticks(w, id, 120);
    CHECK(en_grounded(w, id) == 1, "a walker dropped 5 m lands grounded");
    CHECK(fabs(pos_axis(w, id, 1) - 0.9) < 0.02, "... within 0.02 m of the floor");
    double v[3]; en_get(w, id, 1, v);
    CHECK(v[1] == 0, "... its vertical velocity 0");
    CHECK(en_grounded(w, 12345) == 0, "an unknown id is not grounded");
    en_close(w);

    /* a step 0.5 m (5 voxels) high from x 2.0 m, and one 1.2 m high */
    for (int tall = 0; tall < 2; tall++) {
        w = world(16);
        flat_floor(w, -1, 1);
        int ly = tall ? 12 : 5;
        ledge_chunk(w, 0, 0, 20, 31, ly);
        ledge_chunk(w, 1, 0, 0, 31, ly);
        k = walk_kind(w, 0.6f, 1);
        id = en_add(w, k, 1.0, 0.95, 1.6, 0);
        seen_ticks(w, id, 10);
        double go[3] = {2, 0, 0};
        en_set(w, id, 1, go);
        seen_ticks(w, id, 120);
        if (!tall) {
            CHECK(pos_axis(w, id, 0) > 4.5, "a walker with step height 0.6 walks up a 0.5 m step");
            CHECK(fabs(pos_axis(w, id, 1) - 1.4) < 0.02 && en_grounded(w, id), "... and stands on it");
        } else {
            CHECK(fabs(pos_axis(w, id, 0) - (2.0 - 0.3 - 0.00001)) < 1e-6, "... and is stopped by a 1.2 m one");
            CHECK(fabs(pos_axis(w, id, 1) - 0.9) < 0.02 && en_grounded(w, id), "... still on the floor");
        }
        en_close(w);
    }

    /* a platform 0.5 m high for x below 2.0 m: walk off it */
    w = world(16);
    flat_floor(w, -1, 1);
    ledge_chunk(w, -1, 0, 0, 31, 5);
    ledge_chunk(w, 0, 0, 0, 19, 5);
    k = walk_kind(w, 0.6f, 1);
    id = en_add(w, k, 1.0, 1.45, 1.6, 0);
    seen_ticks(w, id, 10);
    CHECK(en_grounded(w, id) && fabs(pos_axis(w, id, 1) - 1.4) < 0.02, "a walker stands on the platform");
    double go[3] = {2, 0, 0};
    en_set(w, id, 1, go);
    int fell = 0;
    for (int i = 0; i < 120; i++) {
        seen_ticks(w, id, 1);
        double y = pos_axis(w, id, 1);
        if (!en_grounded(w, id) && y < 1.39 && y > 0.91) fell = 1;
    }
    CHECK(fell, "it walks off the ledge and falls");
    CHECK(fabs(pos_axis(w, id, 1) - 0.9) < 0.02 && en_grounded(w, id), "... landing on the floor below");
    en_close(w);
}

/* events of code in the last tick */
static int count_code(EnWorld *w, int code) {
    int n = en_events(w, EN_EVENTS, ev_i, ev_f), c = 0;
    for (int e = 0; e < n; e++) c += ev_i[e * 7] == code;
    return c;
}

static void test_routes(void) {
    double line[9] = {0, 0, 0, 10, 0, 0, 20, 0, 0};
    /* unseen, not looping */
    EnWorld *w = world(16);
    int k = walk_kind(w, 0.6f, 1);
    int r = en_route(w, 3, line);
    CHECK(r >= 0, "a route of 3 points is made");
    int64_t id = en_add(w, k, 0, 0, 0, 0);
    en_follow(w, id, r, 2.0, 0);
    for (int i = 0; i < 300; i++) en_tick(w, 0, NULL, 1.0 / 60);
    CHECK(fabs(pos_axis(w, id, 0) - 10.0) < 0.05, "an unseen follower at 2 m/s is 10 m along after 5 s");
    double f[3]; en_get(w, id, 2, f);
    CHECK(fabs(f[0] - 1.5707963) < 1e-5, "... facing its travel (+x: pi/2)");
    int fives = 0, at = -1;
    for (int i = 300; i < 900; i++) {
        en_tick(w, 0, NULL, 1.0 / 60);
        int c = count_code(w, EN_E_ROUTE);
        if (c && at < 0) at = i + 1;
        fives += c;
        if (c) CHECK(ev_i[1] == id && ev_i[3] == r, "event 5 names the follower and the route");
    }
    CHECK(fives == 1, "it reports event 5 once");
    CHECK(at >= 599 && at <= 601, "... at 10 s");
    CHECK(fabs(pos_axis(w, id, 0) - 20.0) < 1e-9, "... and stops at the end");
    double v[3]; en_get(w, id, 1, v);
    CHECK(v[0] == 0 && v[1] == 0 && v[2] == 0, "... still");
    en_close(w);

    /* unseen, looping */
    w = world(16);
    k = walk_kind(w, 0.6f, 1);
    r = en_route(w, 3, line);
    id = en_add(w, k, 0, 0, 0, 0);
    en_follow(w, id, r, 2.0, 1);
    fives = 0;
    for (int i = 0; i < 1200; i++) { en_tick(w, 0, NULL, 1.0 / 60); fives += count_code(w, EN_E_ROUTE); }
    CHECK(fives == 0, "looping, it reports no event 5");
    CHECK(fabs(pos_axis(w, id, 0)) < 0.05, "... and is back at the start at 20 s");
    en_close(w);

    /* seen, on flat ground */
    w = world(16);
    flat_floor(w, -1, 3);
    k = walk_kind(w, 0.6f, 1);
    double path[9] = {1.6, 0, 1.6, 6.4, 0, 1.6, 6.4, 0, 6.4};
    r = en_route(w, 3, path);
    id = en_add(w, k, 1.6, 0.91, 1.6, 0);
    en_follow(w, id, r, 2.0, 0);
    fives = 0;
    for (int i = 0; i < 600; i++) { seen_ticks(w, id, 1); fives += count_code(w, EN_E_ROUTE); }
    double p[3]; en_get(w, id, 0, p);
    CHECK(hypot(p[0] - 6.4, p[2] - 6.4) < 0.3, "a seen follower on flat ground reaches the end within 0.3 m");
    CHECK(fives == 1, "... reporting event 5 once");
    CHECK(en_grounded(w, id), "... on the ground");
    en_close(w);

    /* seen, against a wall */
    w = world(16);
    flat_floor(w, -1, 0);
    en_chunk_uniform(w, 1, 0, 0, 1);
    k = walk_kind(w, 0.6f, 1);
    double into[6] = {2.899, 0, 1.6, 6.0, 0, 1.6};
    r = en_route(w, 2, into);
    id = en_add(w, k, 2.899, 0.91, 1.6, 0);
    seen_ticks(w, id, 10);
    en_follow(w, id, r, 2.0, 0);
    int eights = 0; at = -1;
    for (int i = 0; i < 240; i++) {
        seen_ticks(w, id, 1);
        int c = count_code(w, EN_E_BLOCKED);
        if (c && at < 0) at = i + 1;
        eights += c;
    }
    CHECK(eights == 1, "a seen follower facing a wall reports event 8 once in 4 s");
    CHECK(at >= 59 && at <= 62, "... after 1 s");
    CHECK(fabs(pos_axis(w, id, 0) - (3.2 - 0.3 - 0.00001)) < 1e-6, "... pressed against the wall");
    en_get(w, id, 1, v);
    CHECK(v[0] > 1.9, "... still pushing toward its waypoint");
    en_close(w);
}

static void test_divisor(void) {
    double line[6] = {0, 0, 0, 100, 0, 0};
    double moved[2];
    for (int t = 0; t < 2; t++) {
        EnWorld *w = world(16);
        int k = walk_kind(w, 0.6f, t ? 4 : 1);
        int r = en_route(w, 2, line);
        int64_t a = en_add(w, k, 0, 0, 0, 0), b = en_add(w, k, 0, 0, 0, 0);
        en_follow(w, a, r, 2.0, 0); en_follow(w, b, r, 2.0, 0);
        en_tick(w, 0, NULL, 1.0 / 60);
        if (t) CHECK(pos_axis(w, a, 0) > 0 && pos_axis(w, b, 0) == 0, "divisor 4: row 0 steps on tick 0, row 1 not");
        for (int i = 1; i < 240; i++) en_tick(w, 0, NULL, 1.0 / 60);
        moved[t] = pos_axis(w, a, 0);
        if (t) CHECK(fabs(pos_axis(w, b, 0) - moved[t]) < 0.1, "... and row 1 catches up on its own ticks");
        en_close(w);
    }
    CHECK(fabs(moved[0] - 8.0) < 0.05, "divisor 1: 8 m in 240 ticks at 2 m/s");
    CHECK(fabs(moved[1] - moved[0]) < 0.1, "divisor 4 moves an unseen follower as far in 240 ticks within 0.1 m");
}

static void test_route_limits(void) {
    static double pts[300 * 3];
    for (int i = 0; i < 300; i++) { pts[i * 3] = i; pts[i * 3 + 1] = 0; pts[i * 3 + 2] = 0; }
    EnWorld *w = world(16);   /* 4,096 route points */
    int k = walk_kind(w, 0.6f, 1);
    double before = en_stat(w, 3);
    CHECK(en_route(w, 257, pts) == -1 && en_stat(w, 6) == EN_R_VALUE, "a route of 257 points is refused");
    CHECK(en_route(w, 0, pts) == -1, "... and one of 0");
    double bad[3] = {0, NAN, 0};
    CHECK(en_route(w, 1, bad) == -1 && en_stat(w, 6) == EN_R_VALUE, "... and one with a point not finite");
    CHECK(en_stat(w, 3) == before + 3, "... each counted");
    int r[32];
    for (int i = 0; i < 32; i++) r[i] = en_route(w, 128, pts);
    int ok = 1;
    for (int i = 0; i < 32; i++) ok &= r[i] >= 0;
    CHECK(ok, "32 routes of 128 points fill the 4,096-point pool");
    CHECK(en_stat(w, 8) == 4096 && en_stat(w, 9) == 32, "... stat 8 counts the points held and stat 9 the routes");
    CHECK(en_route(w, 1, pts) == -1 && en_stat(w, 6) == EN_R_FULL, "a route that would pass the pool is refused");
    en_route_free(w, r[3]); en_route_free(w, r[5]);
    CHECK(en_stat(w, 8) == 4096 - 256, "freeing two gives back their points");
    CHECK(en_route(w, 129, pts) == -1 && en_stat(w, 6) == EN_R_FULL,
          "a route of 129 points is refused with 256 free in two blocks of 128 (first fit, nothing compacted)");
    int r3 = en_route(w, 128, pts);
    CHECK(r3 >= 0, "... a route of 128 fits in a freed block");
    en_route_free(w, r[4]);
    CHECK(en_route(w, 256, pts) >= 0, "two neighbouring freed blocks join: a route of 256 fits");
    en_route_free(w, r[0]);
    before = en_stat(w, 3);
    en_route_free(w, r[0]);
    en_route_free(w, 9999);
    CHECK(en_stat(w, 3) == before + 2 && en_stat(w, 6) == EN_R_ROUTE, "freeing a freed or an unknown route is refused and counted");
    en_close(w);

    /* follow on a freed or unknown route */
    w = world(16);
    k = walk_kind(w, 0.6f, 1);
    double line[6] = {0, 0, 0, 100, 0, 0};
    int ra = en_route(w, 2, line);
    int64_t id = en_add(w, k, 0, 0, 0, 0);
    en_route_free(w, ra);
    before = en_stat(w, 3);
    en_follow(w, id, ra, 2.0, 0);
    en_follow(w, id, 77, 2.0, 0);
    en_follow(w, id, -5, 2.0, 0);
    CHECK(en_stat(w, 3) == before + 3 && en_stat(w, 6) == EN_R_ROUTE, "follow on a freed, an unknown or a negative route is refused and counted");
    int rb = en_route(w, 2, line);
    en_follow(w, id, rb, -1.0, 0);
    en_follow(w, id, rb, NAN, 0);
    en_follow(w, id, rb, 2.0, 2);
    CHECK(en_stat(w, 3) == before + 6, "... and a speed negative or not finite, or loops not 0/1");
    for (int i = 0; i < 60; i++) en_tick(w, 0, NULL, 1.0 / 60);
    CHECK(pos_axis(w, id, 0) == 0, "an entity whose follows were all refused stays");
    en_close(w);

    /* a route freed while followed lives until its last follower stops */
    w = world(16);
    k = walk_kind(w, 0.6f, 1);
    int rr = en_route(w, 2, line);
    int64_t a = en_add(w, k, 0, 0, 0, 0), b = en_add(w, k, 0, 0, 0, 0), c = en_add(w, k, 0, 0, 2, 0);
    double back[6] = {0, 0, 0, -100, 0, 0};
    en_follow(w, a, rr, 2.0, 0); en_follow(w, b, rr, 2.0, 0);
    en_route_free(w, rr);
    CHECK(en_stat(w, 9) == 1 && en_stat(w, 8) == 2, "a route freed while followed is still held");
    int other = en_route(w, 2, back);
    CHECK(other >= 0 && other != rr, "a route made meanwhile takes another id and other points");
    for (int i = 0; i < 60; i++) en_tick(w, 0, NULL, 1.0 / 60);
    CHECK(fabs(pos_axis(w, a, 0) - 2.0) < 0.01 && fabs(pos_axis(w, b, 0) - 2.0) < 0.01, "its followers still follow its points");
    before = en_stat(w, 3);
    en_follow(w, c, rr, 2.0, 0);
    CHECK(en_stat(w, 3) == before + 1, "... but a new follower is refused");
    en_follow(w, a, -1, 0, 0);
    CHECK(en_stat(w, 9) == 2, "one follower stopping leaves it held");
    for (int i = 0; i < 60; i++) en_tick(w, 0, NULL, 1.0 / 60);
    CHECK(fabs(pos_axis(w, a, 0) - 2.0) < 0.01 && fabs(pos_axis(w, b, 0) - 4.0) < 0.02, "the stopped one stays, the other goes on");
    en_remove(w, b, 0);
    CHECK(en_stat(w, 9) == 1 && en_stat(w, 8) == 2, "removing its last follower frees it");
    en_close(w);
}


/* ---- seen and unseen, setting on the ground ---- */
/* a kind: mover, unseen rule, seen distance (0 the cap), lifetime; a box of 0.3 x 0.9 x 0.3, and for a walker step
 * height 0.6 and gravity 9.8 */
static int seen_kind(EnWorld *w, int mover, int rule, float seen, float lifetime) {
    int32_t ki[9];
    kind_ints(ki, mover, 1, 0, 0, 0);
    ki[1] = rule; ki[5] = mover == 2;
    float fl[7] = {0.3f, 0.9f, 0.3f, 0.6f, lifetime, seen, mover == 2 ? 9.8f : 0};
    return en_kind(w, ki, fl);
}
static void tick_at(EnWorld *w, double x, double y, double z) { double f[3] = {x, y, z}; en_tick(w, 1, f, 1.0 / 60); }
/* where a walker set on ground whose top is at metres top stands: 1e-4 voxels above it */
static double stand_y(double top) { return top + 0.1 * 0.0001 + (double)0.9f; }

static void test_hysteresis(void) {
    EnWorld *w = world(16);
    int k = seen_kind(w, 0, 0, 40, 0);   /* d 40 m; the chunk c is 3.2 m */
    int64_t id = en_add(w, k, 0, 0, 0, 0);
    CHECK(en_seen(w, id) == 0, "an entity just added is unseen");
    tick_at(w, 39, 0, 0);
    CHECK(en_seen(w, id) == 1 && en_stat(w, 1) == 1, "with a focus 39 m away it becomes seen");
    CHECK(count_code(w, EN_E_ENTERED) == 1 && ev_i[1] == id && en_events(w, 0, NULL, NULL) == 1, "... with one event 3 naming it");
    tick_at(w, 42, 0, 0);
    CHECK(en_seen(w, id) == 1 && en_events(w, 0, NULL, NULL) == 0, "at 42 m (within d + c) it stays seen, with no event");
    tick_at(w, 44, 0, 0);
    CHECK(en_seen(w, id) == 0 && en_stat(w, 1) == 0, "at 44 m (past d + c = 43.2) it is unseen");
    CHECK(count_code(w, EN_E_LEFT) == 1 && ev_i[1] == id && en_events(w, 0, NULL, NULL) == 1, "... with one event 4 naming it");
    int quiet = 1;
    for (int i = 0; i < 20; i++) { tick_at(w, i % 2 ? 41 : 42, 0, 0); quiet &= en_events(w, 0, NULL, NULL) == 0 && !en_seen(w, id); }
    CHECK(quiet, "a focus pacing between 41 and 42 m leaves it unseen, with no event");
    CHECK(en_seen(w, 12345) == 0 && en_seen(w, -1) == 0, "an unknown id is not seen");
    en_close(w);

    w = world(16);
    k = seen_kind(w, 0, 0, 40, 0);
    id = en_add(w, k, 0, 0, 0, 0);
    tick_at(w, 39, 0, 0);
    int events = en_events(w, 0, NULL, NULL);
    for (int i = 0; i < 20; i++) { tick_at(w, i % 2 ? 41 : 42, 0, 0); events += en_events(w, 0, NULL, NULL); }
    CHECK(events == 1 && en_seen(w, id), "seen at 39 m, a focus pacing between 41 and 42 m causes no event after the first");
    en_close(w);
}

static void test_no_focus(void) {
    EnWorld *w = world(16);
    flat_floor(w, -1, 1);
    int kw = seen_kind(w, 2, 0, 0, 0), kf = seen_kind(w, 1, 0, 0, 0), kp = seen_kind(w, 0, 0, 0, 0);
    int64_t a = en_add(w, kw, 1.6, 0.95, 1.6, 0), b = en_add(w, kf, 1.6, 3, 1.6, 0), c = en_add(w, kp, 0, 0, 0, 0);
    int quiet = 1;
    for (int i = 0; i < 10; i++) { en_tick(w, 0, NULL, 1.0 / 60); quiet &= en_events(w, 0, NULL, NULL) == 0; }
    CHECK(!en_seen(w, a) && !en_seen(w, b) && !en_seen(w, c) && en_stat(w, 1) == 0, "with no focus nothing is seen");
    CHECK(quiet && en_stat(w, 2) == 0, "... nothing reported, nothing waiting for ground");
    en_close(w);
}

/* chunk (cx, cy, cz) of stone with a pit cleared for local x and z 8..24 and local y from ly up */
static void pit_chunk(EnWorld *w, int cx, int cy, int cz, int ly) {
    static int32_t ids[32768];
    for (int y = 0; y < 32; y++)
        for (int z = 0; z < 32; z++)
            for (int x = 0; x < 32; x++) ids[x + 32 * (z + 32 * y)] = (y >= ly && x >= 8 && x <= 24 && z >= 8 && z <= 24) ? 0 : 1;
    en_chunk(w, cx, cy, cz, ids);
}

static void test_grounding(void) {
    double p[3];
    /* inside a hill whose top is 2.0 m: set on the top */
    EnWorld *w = world(16);
    flat_floor(w, -1, 1);
    ledge_chunk(w, 0, 0, 0, 31, 20);
    int k = seen_kind(w, 2, 0, 0, 0);
    int64_t id = en_add(w, k, 1.6, 1.5, 1.6, 0);
    tick_at(w, 1.6, 0, 1.6);
    en_get(w, id, 0, p);
    CHECK(en_seen(w, id) && en_grounded(w, id), "a walker inside a hill becomes seen, grounded");
    CHECK(fabs(p[1] - stand_y(2.0)) < 1e-9 && p[0] == 1.6 && p[2] == 1.6, "... set on the hill's top (2.0 m), straight up");
    CHECK(count_code(w, EN_E_ENTERED) == 1 && ev_i[1] == id && ev_f[1] == p[1], "... with event 3 at where it was set");
    en_close(w);

    /* above a pit 2 m deep: set on the pit's floor */
    w = world(16);
    flat_floor(w, -1, 1);
    pit_chunk(w, 0, -1, 0, 12);
    k = seen_kind(w, 2, 0, 0, 0);
    id = en_add(w, k, 1.6, 0.9, 1.6, 0);
    tick_at(w, 1.6, 0, 1.6);
    CHECK(en_seen(w, id) && en_grounded(w, id) && fabs(pos_axis(w, id, 1) - stand_y(-2.0)) < 1e-9,
          "a walker above a pit becomes seen on the pit's floor (-2.0 m)");
    en_close(w);

    /* over a column of air 54 m tall: stays unseen, waiting */
    w = world(16);
    en_block_table(w, 3, three_blocks);
    for (int cy = -8; cy <= 8; cy++) en_chunk_uniform(w, 0, cy, 0, 0);
    k = seen_kind(w, 2, 0, 0, 0);
    id = en_add(w, k, 1.6, 1.6, 1.6, 0);
    int quiet = 1;
    for (int i = 0; i < 5; i++) { tick_at(w, 1.6, 0, 1.6); quiet &= en_events(w, 0, NULL, NULL) == 0; }
    CHECK(!en_seen(w, id) && quiet, "a walker over 40 m of air stays unseen, reporting nothing");
    CHECK(en_stat(w, 2) == 1, "... and stat 2 counts it waiting");
    CHECK(pos_axis(w, id, 1) == 1.6, "... where it was");
    en_close(w);

    /* its box straddling a column 1 m taller than the one under its centre: set on the taller one's edge */
    w = world(16);
    flat_floor(w, -1, 1);
    ledge_chunk(w, 0, 0, 0, 15, 10);   /* stone to 1.0 m for x below 1.6 m */
    k = seen_kind(w, 2, 0, 0, 0);
    id = en_add(w, k, 1.65, 0.95, 1.6, 0);   /* box x 1.35..1.95: its centre over the floor, its west side over the ledge */
    tick_at(w, 1.65, 0, 1.6);
    CHECK(en_seen(w, id) && en_grounded(w, id) && fabs(pos_axis(w, id, 1) - stand_y(1.0)) < 1e-9,
          "a walker whose box straddles a taller column stands on it: ground under its box, not only under its centre");
    en_close(w);

    /* its chunks not yet held: waits, then is set down on the tick after they arrive */
    w = world(16);
    en_block_table(w, 3, three_blocks);
    k = seen_kind(w, 2, 0, 0, 0);
    id = en_add(w, k, 1.6, 0.95, 1.6, 0);
    tick_at(w, 1.6, 0, 1.6);
    tick_at(w, 1.6, 0, 1.6);
    CHECK(!en_seen(w, id) && en_stat(w, 2) == 1, "a walker whose chunks are not held waits, unseen");
    flat_floor(w, -1, 1);
    tick_at(w, 1.6, 0, 1.6);
    CHECK(en_seen(w, id) && en_grounded(w, id) && fabs(pos_axis(w, id, 1) - stand_y(0)) < 1e-9 && en_stat(w, 2) == 0,
          "... and is set on the floor the tick after they arrive");
    en_close(w);

    /* flying and placed entities become seen at once, unmoved, even with no blocks held */
    w = world(16);
    int kf = seen_kind(w, 1, 0, 0, 0), kp = seen_kind(w, 0, 0, 0, 0);
    int64_t f = en_add(w, kf, 1, 50, 1, 0), q = en_add(w, kp, 2, -7, 2, 0);
    tick_at(w, 0, 0, 0);
    CHECK(en_seen(w, f) && en_seen(w, q) && pos_axis(w, f, 1) == 50 && pos_axis(w, q, 1) == -7,
          "a flyer and a placed entity become seen at once, unmoved");
    CHECK(count_code(w, EN_E_ENTERED) == 2, "... each with event 3");
    en_close(w);
}

static void test_unseen_rules(void) {
    EnWorld *w = world(16);
    int kl = seen_kind(w, 1, 0, 0, 0), kz = seen_kind(w, 1, 1, 0, 1.0f), kr = seen_kind(w, 1, 2, 0, 0);
    int64_t live = en_add(w, kl, 0, 0, 0, 0), frozen = en_add(w, kz, 0, 0, 5, 0), gone = en_add(w, kr, 0, 0, 10, 0);
    double v[3] = {1, 0, 0};
    en_set(w, live, 1, v); en_set(w, frozen, 1, v); en_set(w, gone, 1, v);
    tick_at(w, 0, 0, 0);
    CHECK(en_seen(w, live) && en_seen(w, frozen) && en_seen(w, gone), "three flyers near the focus are seen");
    tick_at(w, 1000, 0, 0);
    int n = en_events(w, EN_EVENTS, ev_i, ev_f), lefts = 0, at4 = -1, at6 = -1;
    for (int e = 0; e < n; e++) {
        if (ev_i[e * 7] == EN_E_LEFT) lefts++;
        if (ev_i[e * 7] == EN_E_LEFT && ev_i[e * 7 + 1] == gone) at4 = e;
        if (ev_i[e * 7] == EN_E_REMOVED && ev_i[e * 7 + 1] == gone && ev_i[e * 7 + 3] == 2) at6 = e;
    }
    CHECK(lefts == 3 && n == 4, "the focus jumps away: three events 4 and one event 6");
    CHECK(at4 >= 0 && at6 == at4 + 1, "... the removed kind's event 4, then its event 6 reason 2");
    CHECK(!en_alive(w, gone), "... and it is removed at once");
    double lx = pos_axis(w, live, 0), fp[3], fa[3], fa2[3], fp2[3];
    en_get(w, frozen, 0, fp); en_get(w, frozen, 3, fa);
    for (int i = 0; i < 120; i++) tick_at(w, 1000, 0, 0);
    en_get(w, frozen, 0, fp2); en_get(w, frozen, 3, fa2);
    CHECK(fabs(pos_axis(w, live, 0) - lx - 2.0) < 1e-6, "unseen, the live one flies on (2 m in 2 s)");
    CHECK(en_alive(w, frozen) && fa2[0] == fa[0] && fp2[0] == fp[0], "the frozen one neither moves nor ages, outliving its 1 s lifetime");
    CHECK(en_near(w, fp, 0.01, 1u << kz, 0, NULL) == 1, "... and is still in the grid");
    tick_at(w, fp[0], 0, fp[2]);
    int named = 0, n3 = en_events(w, EN_EVENTS, ev_i, ev_f);
    for (int e = 0; e < n3; e++) named += ev_i[e * 7] == EN_E_ENTERED && ev_i[e * 7 + 1] == frozen;
    CHECK(en_seen(w, frozen) && named == 1, "seen again, it reports event 3 (the live one, near too, also does)");
    tick_at(w, fp[0], 0, fp[2]);
    CHECK(pos_axis(w, frozen, 0) > fp[0], "... and moves again");
    int ticks = 0;
    while (en_alive(w, frozen) && ticks < 200) { tick_at(w, fp[0], 0, fp[2]); ticks++; }
    CHECK(ticks > 50 && ticks < 62, "... and ages again: its lifetime ends about 1 s after it was added, counting only seen time");
    en_close(w);
}

static void test_ground_goes(void) {
    /* the chunk under a seen walker dropped: held in place; it walks again when the chunk returns */
    EnWorld *w = world(16);
    flat_floor(w, -1, 1);
    int k = seen_kind(w, 2, 0, 0, 0);
    int64_t id = en_add(w, k, 1.6, 0.95, 1.6, 0);
    seen_ticks(w, id, 5);
    double go[3] = {1, 0, 0}, p0[3], p1[3];
    en_set(w, id, 1, go);
    seen_ticks(w, id, 1);
    en_chunk_drop(w, 0, -1, 0);
    en_get(w, id, 0, p0);
    seen_ticks(w, id, 30);
    en_get(w, id, 0, p1);
    CHECK(en_seen(w, id) && p1[0] == p0[0] && p1[1] == p0[1] && p1[2] == p0[2], "a seen walker whose floor chunk is dropped is held in place");
    en_chunk_uniform(w, 0, -1, 0, 1);
    seen_ticks(w, id, 30);
    CHECK(pos_axis(w, id, 0) > p0[0] + 0.45 && en_grounded(w, id), "... and walks on when the chunk returns");
    en_close(w);

    /* a walker far below everything never becomes seen, and stays where it is */
    w = world(16);
    flat_floor(w, -1, 1);
    k = seen_kind(w, 2, 0, 0, 0);
    double line[6] = {1.6, -1000000, 1.6, 30, -1000000, 1.6};
    int r = en_route(w, 2, line);
    id = en_add(w, k, 1.6, -1000000, 1.6, 0);
    en_follow(w, id, r, 2.0, 0);
    tick_at(w, 1.6, 0, 1.6);
    double x1 = pos_axis(w, id, 0);
    int never = 1, still = 1;
    for (int i = 1; i < 600; i++) {
        tick_at(w, 1.6, 0, 1.6);
        never &= !en_seen(w, id);
        still &= pos_axis(w, id, 1) == -1000000 && pos_axis(w, id, 0) == x1;
    }
    CHECK(never && pos_axis(w, id, 1) == -1000000, "a walker at y -1,000,000 never becomes seen and its y never changes in 600 ticks");
    CHECK(still && en_stat(w, 2) == 1, "... held in place while it waits, counted in stat 2");
    en_close(w);

    /* a seen walker's floor cleared by a new chunk: it falls and lands on the next floor down */
    w = world(16);
    flat_floor(w, -1, 1);
    k = seen_kind(w, 2, 0, 0, 0);
    id = en_add(w, k, 1.6, 0.95, 1.6, 0);
    seen_ticks(w, id, 5);
    static int32_t ids[32768];
    for (int i = 0; i < 32768; i++) ids[i] = i < 20 * 1024 ? 1 : 0;   /* stone in local y 0..19: top at -1.2 m */
    en_chunk(w, 0, -1, 0, ids);
    int fell = 0;
    for (int i = 0; i < 120; i++) { seen_ticks(w, id, 1); fell |= !en_grounded(w, id); }
    CHECK(fell && en_grounded(w, id) && fabs(pos_axis(w, id, 1) - stand_y(-1.2)) < 1e-6,
          "a seen walker whose floor voxels are cleared falls and lands on the next floor down (-1.2 m)");
    en_close(w);
}

/* a seen walker held by a chunk not held changes nothing that tick: not even its route progress or facing */
static void test_held_walker(void) {
    EnWorld *w = world(16);
    flat_floor(w, 0, 0);   /* chunks x 0 only: x 0..3.2 m */
    int k = seen_kind(w, 2, 0, 0, 0);
    int64_t id = en_add(w, k, 2.88, 0.95, 1.6, 1.0);
    seen_ticks(w, id, 1);
    CHECK(en_seen(w, id) && en_grounded(w, id), "a walker near the edge of the held chunks is seen, grounded");
    double route[6] = {2.9, 0.9, 1.6, 6.0, 0.9, 1.6};   /* its first point within 0.25 m: passed on its next seen step */
    int r = en_route(w, 2, route);
    en_follow(w, id, r, 6.0, 0);
    int row = (int)(id & EN_ROW_MASK);
    double p0[3], v0[3], p1[3], v1[3];
    en_get(w, id, 0, p0); en_get(w, id, 1, v0);
    int32_t next0 = w->rt_next[row]; float face0 = w->facing[row], best0 = w->rt_best[row], stall0 = w->rt_stall[row];
    uint8_t flags0 = w->flags[row];
    seen_ticks(w, id, 1);   /* a step of 0.1 m east would reach x 3.28: chunk (1, 0, 0), not held */
    en_get(w, id, 0, p1); en_get(w, id, 1, v1);
    CHECK(p1[0] == p0[0] && p1[1] == p0[1] && p1[2] == p0[2] && v1[0] == v0[0] && v1[1] == v0[1] && v1[2] == v0[2],
          "a seen walker that would step into a chunk not held keeps its position and velocity");
    CHECK(w->rt_next[row] == next0 && w->facing[row] == face0 && w->rt_best[row] == best0 && w->rt_stall[row] == stall0,
          "... its route progress and facing");
    CHECK(w->flags[row] == flags0 && en_events(w, 0, NULL, NULL) == 0, "... its grounded flag, and reports nothing");
    for (int cy = -6; cy <= 7; cy++) en_chunk_uniform(w, 1, cy, 0, cy < 0 ? 1 : 0);
    seen_ticks(w, id, 1);
    CHECK(pos_axis(w, id, 0) > p0[0] && w->rt_next[row] == 1 && fabs(w->facing[row] - 1.5707963) < 1e-5,
          "with the chunk held it walks: past its first point, facing east");
    en_close(w);
}

/* ---- parts: releases, seen parts, wanting, figure refusals ---- */
static int32_t rel_c[64]; static int64_t rel_i[64];

static void test_releases(void) {
    EnWorld *w = world(16);
    int k = seen_kind(w, 0, 0, 40, 0);   /* placed; a figure and two voices (kind_ints) */
    int64_t a = en_add(w, k, 0, 0, 0, 0), b = en_add(w, k, 1, 0, 0, 0);
    tick_at(w, 0, 0, 0);
    en_set_part(w, a, 0, 7); en_set_part(w, a, 2, 11); en_set_part(w, a, 3, 12); en_set_part(w, b, 0, 8);
    CHECK(en_releases(w, 0, NULL, NULL) == 0, "nothing to release before any part is cleared");
    en_remove(w, a, 0);
    CHECK(en_releases(w, 0, NULL, NULL) == 3, "a game's removal releases the entity's three parts at once");
    int n = en_releases(w, 64, rel_c, rel_i);
    CHECK(n == 3 && rel_c[0] == 0 && rel_i[0] == 7 && rel_c[1] == 2 && rel_i[1] == 11 && rel_c[2] == 3 && rel_i[2] == 12,
          "... as (code, id) pairs in part order: figure 7, voice 11, voice 12");
    CHECK(en_releases(w, 0, NULL, NULL) == 0, "... and draining empties the list");
    tick_at(w, 1000, 0, 0);
    n = en_releases(w, 64, rel_c, rel_i);
    CHECK(!en_seen(w, b) && n == 1 && rel_c[0] == 0 && rel_i[0] == 8, "becoming unseen releases its figure");
    CHECK(en_part(w, b, 0) == -1, "... and clears the part");
    tick_at(w, 1000, 0, 0);
    CHECK(en_releases(w, 0, NULL, NULL) == 0, "a part is released once");
    int64_t c = en_add(w, k, 0, 0, 0, 0);
    en_set_part(w, c, 0, 1); en_set_part(w, c, 2, 2); en_set_part(w, c, 3, 3);
    en_remove(w, c, 0);
    n = en_releases(w, 2, rel_c, rel_i);
    CHECK(n == 2 && rel_i[0] == 1 && rel_i[1] == 2 && en_releases(w, 0, NULL, NULL) == 1, "a drain of 2 takes the first two and leaves the third");
    n = en_releases(w, 64, rel_c, rel_i);
    CHECK(n == 1 && rel_c[0] == 3 && rel_i[0] == 3, "... which the next drain takes");
    en_close(w);

    /* the unseen rule's removal and a lifetime's release once each */
    w = world(16);
    int kr = seen_kind(w, 0, 2, 40, 0), kl = seen_kind(w, 0, 0, 40, 0.5f);
    int64_t g = en_add(w, kr, 0, 0, 0, 0), l = en_add(w, kl, 0, 0, 0, 0);
    tick_at(w, 0, 0, 0);
    en_set_part(w, g, 0, 21); en_set_part(w, l, 3, 22);
    tick_at(w, 1000, 0, 0);
    n = en_releases(w, 64, rel_c, rel_i);
    CHECK(!en_alive(w, g) && n == 2, "leaving view under the remove rule releases the figure once, the other kind's voice too");
    l = en_add(w, kl, 0, 0, 0, 0);
    tick_at(w, 0, 0, 0);
    en_set_part(w, l, 2, 23);
    for (int i = 0; i < 40 && en_alive(w, l); i++) tick_at(w, 0, 0, 0);
    n = en_releases(w, 64, rel_c, rel_i);
    CHECK(!en_alive(w, l) && n == 1 && rel_c[0] == 2 && rel_i[0] == 23, "a lifetime's end releases the voice");
    en_close(w);
}

static void test_seen_parts(void) {
    EnWorld *w = world(16);
    int k = seen_kind(w, 0, 0, 40, 0);
    int64_t e0 = en_add(w, k, 0, 0, 0, 0), e1 = en_add(w, k, 1, 0, 0, 0.5), e2 = en_add(w, k, 2, 3, 4, 1.0);
    int64_t far = en_add(w, k, 500, 0, 0, 0);
    tick_at(w, 0, 0, 0);
    en_set_part(w, e0, 0, 5); en_set_part(w, e2, 0, 6); en_set_part(w, far, 0, 9); en_set_part(w, e1, 2, 40);
    int32_t ids[8]; float poses[56];
    CHECK(en_seen_parts(w, 0, 0, NULL, NULL) == 2, "two seen entities hold a figure (the unseen one is not counted)");
    int n = en_seen_parts(w, 0, 8, ids, poses);
    CHECK(n == 2 && ids[0] == 5 && ids[1] == 6, "... their figures, in dense order");
    CHECK(poses[7] == 2.0f && poses[8] == 3.0f && poses[9] == 4.0f, "... each with its entity's position");
    CHECK(poses[10] == 0 && poses[12] == 0 && fabsf(poses[11] - (float)sin(0.5)) < 1e-6f && fabsf(poses[13] - (float)cos(0.5)) < 1e-6f,
          "... and its facing as a turn about +y (0, sin a/2, 0, cos a/2)");
    CHECK(poses[0] == 0 && poses[3] == 0 && poses[4] == 0 && poses[5] == 0 && poses[6] == 1.0f, "facing 0 is no turn");
    n = en_seen_parts(w, 2, 8, ids, poses);
    CHECK(n == 1 && ids[0] == 40 && poses[0] == 1.0f, "voice 0's holders");
    CHECK(en_seen_parts(w, 0, 1, ids, poses) == 1 && ids[0] == 5, "at most max are written");
    CHECK(en_seen_parts(w, 1, 8, ids, poses) == 0 && en_seen_parts(w, 4, 8, ids, poses) == 0 && en_seen_parts(w, -1, 8, ids, poses) == 0,
          "no body, and no part outside 0..3");
    en_close(w);
}

static void test_wanting(void) {
    EnWorld *w = world(16);
    int k = seen_kind(w, 0, 0, 40, 0);
    int32_t ki[9]; kind_ints(ki, 0, 1, 0, 0, 0); ki[6] = 0; ki[8] = 1;   /* no figure, one voice */
    float fl[7] = {0.3f, 0.9f, 0.3f, 0, 0, 40, 0};
    int kb = en_kind(w, ki, fl);
    int64_t d5 = en_add(w, k, 5, 0, 0, 0), d1 = en_add(w, k, 0, 7, -1, 0), d3a = en_add(w, k, 0, 0, 3, 0);
    int64_t d3b = en_add(w, k, -3, 0, 0, 0), plain = en_add(w, kb, 0.5, 0, 0, 0), far = en_add(w, k, 300, 0, 0, 0);
    double focus[6] = {0, 0, 0, 290, 0, 0};
    en_tick(w, 2, focus, 1.0 / 60);
    (void)far;
    int64_t ids[16];
    CHECK(en_wanting(w, 0, 0, NULL) == 5, "five seen entities want a figure (the plain kind has none; the far one is seen by the second focus)");
    int n = en_wanting(w, 0, 16, ids);
    CHECK(n == 5 && ids[0] == d1 && ids[1] == d3a && ids[2] == d3b && ids[3] == d5 && ids[4] == far,
          "... nearest a focus first, horizontally, ties by id; the far one 10 m from the second focus");
    CHECK(d3a < d3b, "(the tie's lower id is the one added first)");
    en_set_part(w, d1, 0, 3);
    n = en_wanting(w, 0, 2, ids);
    CHECK(n == 2 && ids[0] == d3a && ids[1] == d3b, "one given a figure no longer wants one; at most max are written");
    n = en_wanting(w, 2, 16, ids);
    CHECK(n == 6 && ids[0] == plain, "every seen entity wants voice 0, the plain kind's too");
    CHECK(en_wanting(w, 3, 0, NULL) == 5 && en_wanting(w, 1, 0, NULL) == 0 && en_wanting(w, 9, 0, NULL) == 0,
          "voice 1 only on the kind with two; no body; no part 9");
    en_close(w);
}

static void test_figure_refusals(void) {
    EnWorld *w = world(16);
    int k0 = seen_kind(w, 0, 0, 40, 0), k1 = seen_kind(w, 0, 0, 40, 0);
    en_figure_refused(w, k1); en_figure_refused(w, k1);
    CHECK(en_stat(w, 10 + k1) == 2 && en_stat(w, 10 + k0) == 0, "figure refusals are counted for their kind (stat 10 + kind)");
    CHECK(en_stat(w, 10 + 61) == 0 && en_stat(w, 3) == 0, "another kind reads 0; nothing refused");
    en_figure_refused(w, 7);
    CHECK(en_stat(w, 3) == 1 && en_stat(w, 6) == EN_R_KIND, "a kind not registered is refused");
    en_close(w);
}

int main(void) {
    test_kinds();
    test_add_and_read();
    test_slots();
    test_stale_ids();
    test_bad_numbers();
    test_full_world();
    test_each_and_columns();
    test_order_after_removal();
    test_seen_cap();
    test_hash();
    test_masks();
    test_ray();
    test_flying();
    test_wall();
    test_lifetime_and_removal();
    test_dt_edges();
    test_near();
    test_ray_entities();
    test_touches();
    test_fast_and_undeclared();
    test_flyer_against_flyer();
    test_event_overflow();
    test_removal_in_loop();
    test_sweep();
    test_walking();
    test_routes();
    test_divisor();
    test_route_limits();
    test_hysteresis();
    test_no_focus();
    test_grounding();
    test_unseen_rules();
    test_ground_goes();
    test_held_walker();
    test_releases();
    test_seen_parts();
    test_wanting();
    test_figure_refusals();
    if (fails == 0) printf("ent_test: ok\n");
    return fails == 0 ? 0 : 1;
}

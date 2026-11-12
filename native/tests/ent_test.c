/* The entity kernel, headless: kinds and their refusals, adding and reading,
 * slots, stale ids, bad numbers, a full world, each and columns, and the
 * hash's determinism. */
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
    if (fails == 0) printf("ent_test: ok\n");
    return fails == 0 ? 0 : 1;
}

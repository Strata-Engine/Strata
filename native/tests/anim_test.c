/* The pose kernel, headless: sampling, slerp, the hierarchy, masks and
 * additive layers, crossfades, blends in phase, marks, the dt edge cases,
 * degenerate clips, removed figures, refusals and determinism; then the
 * procedural stage: overrides, aim, reach (and its degenerate targets),
 * springs, the drop; then attachments, sockets, and ray and box hit tests
 * (against brute force). */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "strata_anim.h"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL: %s\n", what); fails++; } } while (0)

#define PI_F 3.14159265f
static void qz(float deg, float q[4]) {
    float h = deg * PI_F / 360.0f;
    q[0] = 0; q[1] = 0; q[2] = sinf(h); q[3] = cosf(h);
}

/* root; arm on the root at (0,1,0); hand on the arm at (0,1,0) */
static int chain(AnWorld *w, float arm_rest_deg) {
    int32_t parent[3] = {-1, 0, 1};
    float pivot[9] = {0, 0, 0, 0, 1, 0, 0, 1, 0};
    float rest[12] = {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1};
    qz(arm_rest_deg, rest + 4);
    float box[18];
    for (int i = 0; i < 3; i++) {
        box[i * 6 + 0] = -0.1f; box[i * 6 + 1] = 0; box[i * 6 + 2] = -0.1f;
        box[i * 6 + 3] = 0.1f; box[i * 6 + 4] = 1; box[i * 6 + 5] = 0.1f;
    }
    return an_rig(w, 3, parent, pivot, rest, box);
}

/* a clip of up to 8 turn keys about z: (part, interp, time, degrees) */
typedef struct { int part, interp; float t, deg; } TK;
static int turns(AnWorld *w, int rig, float dur, int loops, int n, const TK *k,
                 int nmarks, const float *mt, const int32_t *mtag) {
    int32_t ki[24]; float kf[40];
    for (int i = 0; i < n; i++) {
        ki[i * 3] = k[i].part; ki[i * 3 + 1] = 0; ki[i * 3 + 2] = k[i].interp;
        kf[i * 5] = k[i].t; qz(k[i].deg, kf + i * 5 + 1);
    }
    return an_clip(w, rig, dur, loops, 1.0f, n, ki, kf, nmarks, mt, mtag);
}

static float angle(const float *m, int part) {   /* the part's world turn about z, degrees */
    return atan2f(m[part * 12 + 1], m[part * 12 + 0]) * 180.0f / PI_F;
}
static int near(float a, float b, float eps) { return fabsf(a - b) <= eps; }
static int all_finite(const float *m, int parts) {
    for (int i = 0; i < parts * 12; i++) if (!isfinite(m[i])) return 0;
    return 1;
}

static void test_sampling(void) {
    AnWorld *w = an_open(16);
    int rig = chain(w, 0);
    CHECK(rig >= 0, "the chain rig is made");
    TK lin[2] = {{1, 1, 0, 0}, {1, 1, 1, 90}};
    int c = turns(w, rig, 1, 0, 2, lin, 0, NULL, NULL);
    CHECK(c >= 0, "a linear clip is made");
    int f = an_spawn(w, rig);
    an_play(w, f, 0, c, 0, 1, 1, -1, 0);
    an_update(w, 0.5f);
    const float *m = an_matrices(w, f);
    float s = sqrtf(0.5f);
    float want[12] = {s, s, 0, -s, s, 0, 0, 0, 1, 0, 1, 0};
    int ok = 1;
    for (int i = 0; i < 12; i++) if (!near(m[12 + i], want[i], 1e-5f)) ok = 0;
    CHECK(ok, "at 0.5 the arm's matrix is a 45-degree turn");

    TK step[2] = {{1, 0, 0, 0}, {1, 0, 1, 90}};
    int cs = turns(w, rig, 1, 0, 2, step, 0, NULL, NULL);
    int g = an_spawn(w, rig);
    an_play(w, g, 0, cs, 0, 1, 1, -1, 0);
    an_update(w, 0.5f);
    CHECK(near(angle(an_matrices(w, g), 1), 0, 1e-4f), "step holds 0 at 0.5");
    an_update(w, 0.49f);
    CHECK(near(angle(an_matrices(w, g), 1), 0, 1e-4f), "step holds 0 at 0.99");
    an_update(w, 0.01f);
    CHECK(near(angle(an_matrices(w, g), 1), 90, 1e-3f), "step reaches 90 at 1");

    TK smooth[2] = {{1, 2, 0, 0}, {1, 2, 1, 90}};
    int cm = turns(w, rig, 1, 0, 2, smooth, 0, NULL, NULL);
    int h = an_spawn(w, rig);
    an_play(w, h, 0, cm, 0, 1, 1, -1, 0);
    an_update(w, 0.25f);
    float a = angle(an_matrices(w, h), 1);
    CHECK(a > 0.0f && a < 22.5f - 0.5f, "smooth at 0.25 is below the linear value");
    an_close(w);
}

static void test_antipodal(void) {
    AnWorld *w = an_open(4);
    int rig = chain(w, 0);
    int32_t ki[6] = {1, 0, 1, 1, 0, 1};
    float kf[10];
    kf[0] = 0; qz(30, kf + 1);
    kf[5] = 1; qz(30, kf + 6);
    for (int i = 6; i < 10; i++) kf[i] = -kf[i];   /* -q: the same turn */
    int c = an_clip(w, rig, 1, 0, 1, 2, ki, kf, 0, NULL, NULL);
    int f = an_spawn(w, rig);
    an_play(w, f, 0, c, 0, 1, 1, -1, 0);
    an_update(w, 0.5f);
    const float *m = an_matrices(w, f);
    CHECK(near(angle(m, 1), 30, 1e-3f), "slerp between q and -q takes the short way");
    float len = sqrtf(m[12] * m[12] + m[13] * m[13] + m[14] * m[14]);
    CHECK(near(len, 1, 1e-5f), "and stays unit length");
    an_close(w);
}

static void test_hierarchy(void) {
    AnWorld *w = an_open(4);
    int rig = chain(w, 0);
    TK k[1] = {{1, 1, 0, 90}};
    int c = turns(w, rig, 1, 1, 1, k, 0, NULL, NULL);
    int f = an_spawn(w, rig);
    an_play(w, f, 0, c, 0, 1, 1, -1, 0);
    an_update(w, 0.1f);
    const float *m = an_matrices(w, f);
    CHECK(near(m[2 * 12 + 9], -1, 1e-5f) && near(m[2 * 12 + 10], 1, 1e-5f) && near(m[2 * 12 + 11], 0, 1e-5f),
          "turning the arm 90 degrees moves the hand's origin to (-1, 1, 0)");

    float pos[3] = {10, 0, 5}, q[4];
    qz(90, q);
    an_place(w, f, pos, q);
    CHECK(an_stat(w, 2) == 1, "a placed figure is stale until the next update");
    an_update(w, 0.1f);
    m = an_matrices(w, f);
    CHECK(near(m[2 * 12 + 9], 9, 1e-5f) && near(m[2 * 12 + 10], -1, 1e-5f) && near(m[2 * 12 + 11], 5, 1e-5f),
          "the placement carries the whole figure");
    CHECK(an_stat(w, 1) == 1 && an_stat(w, 2) == 0, "one figure posed, none stale");
    an_close(w);
}

static void test_masks_additive(void) {
    AnWorld *w = an_open(8);
    int rig = chain(w, 0);
    TK arm90[1] = {{1, 1, 0, 90}};
    int base = turns(w, rig, 1, 1, 1, arm90, 0, NULL, NULL);
    TK both45[2] = {{1, 1, 0, 45}, {2, 1, 0, 45}};
    int both = turns(w, rig, 1, 1, 2, both45, 0, NULL, NULL);
    int32_t hand = 2;
    int mask = an_mask(w, rig, 1, &hand);
    CHECK(mask >= 0, "a mask of the hand is made");

    int f = an_spawn(w, rig), ref = an_spawn(w, rig);
    an_play(w, f, 0, base, 0, 1, 1, -1, 0);
    an_play(w, ref, 0, base, 0, 1, 1, -1, 0);
    an_play(w, f, 1, both, 0, 1, 1, mask, 0);
    an_update(w, 0.1f);
    const float *m = an_matrices(w, f), *r = an_matrices(w, ref);
    CHECK(memcmp(m, r, 2 * 12 * sizeof(float)) == 0, "a layer masked to the hand leaves the root and arm alone");
    CHECK(near(angle(m, 2), 135, 1e-3f), "and turns the hand");

    TK add[2] = {{1, 1, 0, 10}, {1, 1, 1, 40}};
    int ca = turns(w, rig, 1, 0, 2, add, 0, NULL, NULL);
    int g = an_spawn(w, rig);
    an_play(w, g, 0, base, 0, 1, 1, -1, 0);
    an_play(w, g, 1, ca, 0, 1, 1, -1, 1);
    an_update(w, 1.0f);
    CHECK(near(angle(an_matrices(w, g), 1), 120, 1e-3f), "an additive layer adds its turn from its first key on top");
    an_set(w, g, 1, 1, 0.5f, 0);
    an_update(w, 0.0f);
    CHECK(near(angle(an_matrices(w, g), 1), 105, 1e-3f), "at weight 0.5 it adds half");
    an_close(w);
}

static float max_step(const float *a, const float *b) {
    float d = 0;
    for (int i = 0; i < 36; i++) { float e = fabsf(a[i] - b[i]); if (e > d) d = e; }
    return d;
}

static void test_crossfade(void) {
    AnWorld *w = an_open(4);
    int rig = chain(w, 0);
    TK swing[3] = {{1, 1, 0, 0}, {1, 1, 0.5f, 90}, {1, 1, 1, 0}};
    int a = turns(w, rig, 1, 1, 3, swing, 0, NULL, NULL);
    TK hold[2] = {{1, 1, 0, 30}, {2, 1, 0, 30}};
    int b = turns(w, rig, 1, 1, 2, hold, 0, NULL, NULL);
    int f = an_spawn(w, rig), s = an_spawn(w, rig);
    an_play(w, f, 0, a, 0, 1, 1, -1, 0);
    an_play(w, s, 0, a, 0, 1, 1, -1, 0);
    float prev[36], prev_s[36], worst = 0, snap = 0;
    for (int i = 0; i < 120; i++) {
        if (i == 15) { an_play(w, f, 0, b, 0.2f, 1, 1, -1, 0); an_play(w, s, 0, b, 0, 1, 1, -1, 0); }
        if (i > 0) { memcpy(prev, an_matrices(w, f), sizeof prev); memcpy(prev_s, an_matrices(w, s), sizeof prev_s); }
        an_update(w, 1.0f / 60);
        if (i > 0) {
            float d = max_step(prev, an_matrices(w, f));
            if (d > worst) worst = d;
            d = max_step(prev_s, an_matrices(w, s));
            if (d > snap) snap = d;
        }
    }
    CHECK(worst <= 0.1f, "a crossfade of 0.2 s changes no element by more than 0.1 an update");
    CHECK(snap > 0.1f, "(where a cut with no fade would)");
    CHECK(near(angle(an_matrices(w, f), 1), 30, 1e-3f), "and ends on the new clip");
    an_close(w);
}

static void test_blend(void) {
    AnWorld *w = an_open(4);
    int rig = chain(w, 0);
    /* walk turns the arm, run turns the hand, each 0 to 90 over its whole length */
    TK wk[2] = {{1, 1, 0, 0}, {1, 1, 1, 90}};
    TK rk[2] = {{2, 1, 0, 0}, {2, 1, 0.5f, 90}};
    int32_t ki[6]; float kf[10];
    for (int i = 0; i < 2; i++) { ki[i * 3] = wk[i].part; ki[i * 3 + 1] = 0; ki[i * 3 + 2] = 1; kf[i * 5] = wk[i].t; qz(wk[i].deg, kf + i * 5 + 1); }
    int walk = an_clip(w, rig, 1, 1, 1, 2, ki, kf, 0, NULL, NULL);
    for (int i = 0; i < 2; i++) { ki[i * 3] = rk[i].part; kf[i * 5] = rk[i].t; qz(rk[i].deg, kf + i * 5 + 1); }
    int run = an_clip(w, rig, 0.5f, 1, 2, 2, ki, kf, 0, NULL, NULL);
    int f = an_spawn(w, rig);
    int32_t clips[2] = {walk, run};
    float positions[2] = {0, 1};
    an_blend(w, f, 0, 2, clips, positions, 0.5f, 0, 1, -1);
    for (int i = 0; i < 100; i++) an_update(w, 1.0f / 60);
    const float *m = an_matrices(w, f);
    /* each part is half the way from rest to its clip: the hand's local turn is its world turn less the arm's */
    float arm = angle(m, 1), hand = angle(m, 2) - arm;
    float phase = (float)fmod((100.0 / 60.0) / 0.75, 1.0);
    CHECK(near(arm, hand, 1e-3f), "walk and run keep the same normalised time");
    CHECK(near(arm, 0.5f * 90 * phase, 1e-2f), "and that time advances at the blended length");
    an_close(w);
}

static void test_dt_and_marks(void) {
    AnWorld *w = an_open(4);
    int rig = chain(w, 0);
    TK swing[2] = {{1, 1, 0, 0}, {1, 1, 1, 90}};
    float mt[2] = {0.25f, 0.75f};
    int32_t tags[2] = {1, 2};
    int c = turns(w, rig, 1, 1, 2, swing, 2, mt, tags);
    int f = an_spawn(w, rig);
    an_play(w, f, 0, c, 0, 1, 1, -1, 0);
    an_update(w, 0.3f);
    float before[36];
    memcpy(before, an_matrices(w, f), sizeof before);
    uint64_t h = an_hash(w);
    an_update(w, 0);
    CHECK(memcmp(before, an_matrices(w, f), sizeof before) == 0, "dt 0 changes no matrix");
    an_update(w, -1);
    CHECK(memcmp(before, an_matrices(w, f), sizeof before) == 0 && an_hash(w) == h, "dt -1 changes nothing");
    CHECK(an_stat(w, 4) == 1, "and is counted in stat 4");
    an_close(w);

    w = an_open(4);
    rig = chain(w, 0);
    c = turns(w, rig, 1, 1, 2, swing, 2, mt, tags);
    f = an_spawn(w, rig);
    an_play(w, f, 0, c, 0, 1, 1, -1, 0);
    an_update(w, 2.6f);
    int32_t ints[30]; float wts[10];
    int n = an_marks(w, 10, ints, wts);
    int order[5] = {1, 2, 1, 2, 1}, ok = n == 5;
    for (int i = 0; ok && i < 5; i++) if (ints[i * 3 + 2] != order[i] || ints[i * 3] != f || ints[i * 3 + 1] != 0) ok = 0;
    CHECK(ok, "dt 2.6 over a 1 s loop reports the marks 0.25, 0.75, 0.25, 0.75, 0.25 in order");
    an_update(w, 0.1f);
    CHECK(an_marks(w, 10, ints, wts) == 0, "marks are only those since the last update");

    int g = an_spawn(w, rig);
    an_play(w, g, 2, c, 0, 1, 0, -1, 0);
    an_update(w, 0.5f);
    n = an_marks(w, 10, ints, wts);
    int seen = 0;
    for (int i = 0; i < n; i++) if (ints[i * 3] == g && ints[i * 3 + 1] == 2 && ints[i * 3 + 2] == 1 && wts[i] == 0) seen = 1;
    CHECK(seen, "a mark on a weight-0 layer is reported with weight 0");
    an_close(w);

    /* a mark at 0 on a loop: an update that ends exactly on the wrap leaves it to the next one, once */
    w = an_open(4);
    rig = chain(w, 0);
    float m0[1] = {0}; int32_t t0[1] = {5};
    c = turns(w, rig, 1, 1, 2, swing, 1, m0, t0);
    f = an_spawn(w, rig);
    an_play(w, f, 0, c, 0, 1, 1, -1, 0);
    int counts[3];
    float steps[3] = {0.5f, 0.5f, 0.5f};
    for (int k = 0; k < 3; k++) { an_update(w, steps[k]); counts[k] = an_marks(w, 10, ints, wts); }
    CHECK(counts[0] == 1 && counts[1] == 0 && counts[2] == 1, "a mark at 0 fires at the start and once after the wrap");
    an_close(w);

    /* dt 100 over a 1 s loop with two marks crosses 200 marks: past 64 loops they are skipped, past
     * a figure's share they are not reported; either way every one is counted */
    w = an_open(4);
    rig = chain(w, 0);
    c = turns(w, rig, 1, 1, 2, swing, 2, mt, tags);
    f = an_spawn(w, rig);
    an_play(w, f, 0, c, 0, 1, 1, -1, 0);
    an_update(w, 100.0f);
    n = an_marks(w, 10, ints, wts);
    CHECK(n == AN_FIG_EVENTS && n + an_stat(w, 6) == 200, "marks past 64 loops and past the cap are counted in stat 6");
    an_close(w);

    /* more marks than the cap: every figure keeps its share, whatever its slot */
    w = an_open(64);
    rig = chain(w, 0);
    c = turns(w, rig, 1, 1, 2, swing, 2, mt, tags);
    int per[64];
    for (int i = 0; i < 64; i++) {
        per[i] = an_spawn(w, rig);
        for (int l = 0; l < AN_LAYERS; l++) an_play(w, per[i], l, c, 0, 1, 0, -1, 0);   /* zero weight still fires */
    }
    an_update(w, 0.8f);   /* 8 layers x 2 marks = 16 a figure, twice the share */
    static int32_t many[64 * 16 * 3]; static float many_w[64 * 16];
    n = an_marks(w, 64 * 16, many, many_w);
    int each[64] = {0}, fair = n == 64 * AN_FIG_EVENTS;
    for (int i = 0; i < n; i++) for (int k = 0; k < 64; k++) if (many[i * 3] == per[k]) each[k]++;
    for (int k = 0; k < 64; k++) if (each[k] != AN_FIG_EVENTS) fair = 0;
    CHECK(fair, "past the cap every figure reports its share of marks, the last slot too");
    CHECK(an_stat(w, 6) == 64 * 8, "and the rest are counted in stat 6");
    an_close(w);
}

static void test_degenerate(void) {
    AnWorld *w = an_open(8);
    int rig = chain(w, 10);
    int c[4];
    c[0] = an_clip(w, rig, 1, 1, 1, 0, NULL, NULL, 0, NULL, NULL);
    TK at_end[2] = {{1, 1, 0, 0}, {1, 1, 1, 90}};
    c[1] = turns(w, rig, 1, 1, 2, at_end, 0, NULL, NULL);
    TK same[2] = {{1, 1, 0.5f, 0}, {1, 1, 0.5f, 90}};
    c[2] = turns(w, rig, 1, 1, 2, same, 0, NULL, NULL);
    TK zero[2] = {{1, 1, 0, 30}, {1, 2, 0, 60}};
    c[3] = turns(w, rig, 0, 1, 2, zero, 0, NULL, NULL);
    CHECK(c[0] >= 0 && c[1] >= 0 && c[2] >= 0 && c[3] >= 0, "degenerate clips are accepted");
    int f[4];
    for (int i = 0; i < 4; i++) { f[i] = an_spawn(w, rig); an_play(w, f[i], 0, c[i], 0, 1, 1, -1, 0); }
    float dts[6] = {0.3f, 0.7f, 1.0f, 0.5f, 0.0f, 0.25f};
    int finite = 1, rest = 1;
    for (int k = 0; k < 6; k++) {
        an_update(w, dts[k]);
        for (int i = 0; i < 4; i++) if (!all_finite(an_matrices(w, f[i]), 3)) finite = 0;
        const float *m = an_matrices(w, f[0]);
        if (!near(angle(m, 1), 10, 1e-3f) || !near(angle(m, 2), 10, 1e-3f) || !near(angle(m, 0), 0, 1e-5f)) rest = 0;
        if (!near(angle(an_matrices(w, f[1]), 0), 0, 1e-5f) || !near(angle(an_matrices(w, f[1]), 2), angle(an_matrices(w, f[1]), 1), 1e-3f)) rest = 0;
    }
    CHECK(finite, "every matrix of a degenerate clip stays finite");
    CHECK(rest, "and parts with no keys hold their rest pose");
    an_close(w);
}

static void test_removed(void) {
    AnWorld *w = an_open(4);
    int rig = chain(w, 0);
    TK k[1] = {{1, 1, 0, 90}};
    int c = turns(w, rig, 1, 1, 1, k, 0, NULL, NULL);
    int f = an_spawn(w, rig);
    an_remove(w, f);
    CHECK(an_matrices(w, f) == NULL, "a removed figure has no matrices");
    CHECK(an_stat(w, 0) == 0, "and is not counted");
    double before = an_stat(w, 3);
    float pos[3] = {0, 0, 0}, q[4] = {0, 0, 0, 1};
    int32_t clips[1] = {c}; float positions[1] = {0};
    an_place(w, f, pos, q);
    an_play(w, f, 0, c, 0, 1, 1, -1, 0);
    an_blend(w, f, 0, 1, clips, positions, 0, 0, 1, -1);
    an_set(w, f, 0, 1, 0.5f, 0);
    an_remove(w, f);
    CHECK(an_stat(w, 3) == before + 5, "every call on a removed figure is refused and counted");
    an_update(w, 0.1f);
    CHECK(an_matrices(w, f) == NULL, "and stays gone after an update");
    int g = an_spawn(w, rig);
    CHECK(g != f, "a new figure does not reuse the removed id");
    an_close(w);
}

static void test_refusals(void) {
    AnWorld *w = an_open(4);
    static int32_t parent[65]; static float pivot[195], rest[260], box[390];
    for (int i = 0; i < 65; i++) { parent[i] = i - 1; rest[i * 4 + 3] = 1; box[i * 6 + 3] = 1; box[i * 6 + 4] = 1; box[i * 6 + 5] = 1; }
    CHECK(an_rig(w, 65, parent, pivot, rest, box) == -1, "a 65-part rig is refused");
    CHECK(an_stat(w, 5) == AN_R_PARTS, "stat 5 says too many parts");
    CHECK(an_rig(w, 64, parent, pivot, rest, box) >= 0, "a 64-part rig is not");
    CHECK(an_clip(w, 99, 1, 1, 1, 0, NULL, NULL, 0, NULL, NULL) == -1, "a clip for a rig that does not exist is refused");
    CHECK(an_stat(w, 5) == AN_R_RIG, "stat 5 says no such rig");
    int rig = chain(w, 0);
    int c = an_clip(w, rig, 1, 1, 1, 0, NULL, NULL, 0, NULL, NULL);
    int f = an_spawn(w, rig);
    double before = an_stat(w, 3);
    an_play(w, f, 8, c, 0, 1, 1, -1, 0);
    CHECK(an_stat(w, 3) == before + 1, "layer 8 is refused");
    CHECK(an_stat(w, 5) == AN_R_LAYER, "stat 5 says no such layer");
    an_close(w);
}

static uint64_t script(void) {
    AnWorld *w = an_open(64);
    int rig = chain(w, 5);
    TK swing[3] = {{1, 1, 0, 0}, {1, 2, 0.4f, 70}, {2, 1, 0.8f, -40}};
    TK other[3] = {{1, 0, 0, 20}, {2, 2, 0.3f, 50}, {1, 1, 0.6f, -30}};
    float mt[1] = {0.5f}; int32_t tag[1] = {7};
    int a = turns(w, rig, 1, 1, 3, swing, 1, mt, tag);
    int b = turns(w, rig, 0.7f, 1, 3, other, 0, NULL, NULL);
    int32_t hand = 2;
    int mask = an_mask(w, rig, 1, &hand);
    int figs[50];
    for (int i = 0; i < 50; i++) {
        figs[i] = an_spawn(w, rig);
        float pos[3] = {(float)i, 0, (float)(i % 7)}, q[4];
        qz((float)(i * 13), q);
        an_place(w, figs[i], pos, q);
        an_play(w, figs[i], 0, i % 2 ? a : b, 0, 0.5f + 0.03f * i, 1, -1, 0);
    }
    int32_t clips[2] = {a, b}; float positions[2] = {0, 1};
    for (int u = 0; u < 600; u++) {
        int i = u % 50;
        if (u % 37 == 0) an_play(w, figs[i], 0, u % 2 ? b : a, 0.25f, 1, 1, -1, 0);
        if (u % 53 == 0) an_blend(w, figs[i], 1, 2, clips, positions, 0.3f, 0.1f, 0.8f, mask);
        if (u % 71 == 0) an_set(w, figs[i], 1, 3, 0.9f, 0.5f);
        if (u % 89 == 0) an_play(w, figs[i], 2, a, 0.1f, 1, 0.5f, -1, 1);
        an_update(w, 1.0f / 60);
    }
    uint64_t h = an_hash(w);
    an_close(w);
    return h;
}

static void test_determinism(void) {
    uint64_t a = script(), b = script();
    CHECK(a == b, "a scripted 600-update run of 50 figures hashes the same twice");
}

/* ---- the procedural stage ---- */

/* a rig of n parts from parents and pivots, rest identity, unit boxes */
static int rig_of(AnWorld *w, int n, const int32_t *parent, const float *pivot) {
    float rest[16 * 4], box[16 * 6];
    for (int i = 0; i < n; i++) {
        rest[i * 4] = rest[i * 4 + 1] = rest[i * 4 + 2] = 0; rest[i * 4 + 3] = 1;
        for (int k = 0; k < 3; k++) { box[i * 6 + k] = -0.1f; box[i * 6 + 3 + k] = 0.1f; }
    }
    return an_rig(w, n, parent, pivot, rest, box);
}
/* the turn between two parts' world rotations, degrees, from their matrices */
static float between(const float *m, int a, int b) {
    float tr = 0;
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < 3; k++) tr += m[a * 12 + i * 3 + k] * m[b * 12 + i * 3 + k];
    float c = (tr - 1) / 2;
    c = c > 1 ? 1 : (c < -1 ? -1 : c);
    return acosf(c) * 180.0f / PI_F;
}
static float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

static void test_override(void) {
    AnWorld *w = an_open(4);
    int rig = chain(w, 0);
    TK hold[1] = {{1, 0, 0, 60}};
    int c = turns(w, rig, 1, 1, 1, hold, 0, NULL, NULL);
    int f = an_spawn(w, rig);
    an_play(w, f, 0, c, 0, 1, 1, -1, 0);
    an_update(w, 0.1f);
    CHECK(near(angle(an_matrices(w, f), 1), 60, 1e-3f), "the clip turns the arm to 60");
    float q20[4], id[4] = {0, 0, 0, 1}, zero[3] = {0, 0, 0};
    qz(20, q20);
    an_override(w, f, 1, q20, zero, 1);
    an_update(w, 0.1f);
    CHECK(near(angle(an_matrices(w, f), 1), 20, 1e-3f), "an override at weight 1 replaces the clip's rotation exactly");
    CHECK(near(an_matrices(w, f)[12 + 9], 0, 1e-6f) && near(an_matrices(w, f)[12 + 10], 1, 1e-6f), "and its movement (none)");
    an_override(w, f, 1, id, zero, 0.5f);
    an_update(w, 0.1f);
    CHECK(near(angle(an_matrices(w, f), 1), 30, 1e-3f), "an override to rest at weight 0.5 halves the clip's 60");
    an_override(w, f, 1, id, zero, 0);
    an_update(w, 0.1f);
    CHECK(near(angle(an_matrices(w, f), 1), 60, 1e-3f), "weight 0 clears the override");
    CHECK(an_stat(w, 7) == 0, "a cleared override holds no record");
    double before = an_stat(w, 3);
    an_override(w, f, 3, q20, zero, 1);
    CHECK(an_stat(w, 3) == before + 1 && an_stat(w, 5) == AN_R_PART, "an override of a part the rig lacks is refused");
    an_close(w);
}

/* a root and two parts all turning about the origin: a, then b on a */
static void test_aim(void) {
    AnWorld *w = an_open(4);
    int32_t parent[3] = {-1, 0, 1};
    float pivot[9] = {0};
    int rig = rig_of(w, 3, parent, pivot);
    int f = an_spawn(w, rig);
    int32_t ch[2] = {1, 2};
    float ax[3] = {1, 0, 0}, t[3] = {10 * cosf(PI_F / 4), 10 * sinf(PI_F / 4), 0};
    an_aim(w, f, 0, 2, ch, ax, t, 30, 1);
    an_update(w, 0.1f);
    const float *m = an_matrices(w, f);
    CHECK(near(angle(m, 1), 22.5f, 1e-3f), "aim 45 off over 2 parts, limit 30: the first turns 22.5");
    CHECK(near(angle(m, 2), 45, 1e-3f), "and the second 22.5 more, pointing at the target");
    CHECK(near(between(m, 1, 2), 22.5f, 1e-2f), "the second's own turn is 22.5");
    an_aim(w, f, 0, 2, ch, ax, t, 10, 1);
    an_update(w, 0.1f);
    m = an_matrices(w, f);
    CHECK(near(angle(m, 1), 10, 1e-3f) && near(angle(m, 2), 20, 1e-3f), "with limit 10 a part it stops at 20");
    an_aim(w, f, 0, 2, ch, ax, t, 30, 0.5f);
    an_update(w, 0.1f);
    CHECK(near(angle(an_matrices(w, f), 2), 22.5f, 1e-3f), "at weight 0.5 it turns half as far");
    float behind[3] = {-10, 0, 0};
    an_aim(w, f, 0, 2, ch, ax, behind, 180, 1);
    an_update(w, 0.1f);
    CHECK(all_finite(an_matrices(w, f), 3), "a target straight behind gives finite matrices");
    CHECK(near(fabsf(angle(an_matrices(w, f), 2)), 180, 1e-2f) || between(an_matrices(w, f), 0, 2) > 179.9f, "and turns the axis onto it");
    an_aim(w, f, 0, 2, ch, ax, t, 30, 0);
    an_update(w, 0.1f);
    CHECK(near(angle(an_matrices(w, f), 2), 0, 1e-4f), "weight 0 clears the aim");
    double before = an_stat(w, 3);
    an_aim(w, f, 4, 2, ch, ax, t, 30, 1);
    int32_t back[2] = {2, 1};
    an_aim(w, f, 0, 2, back, ax, t, 30, 1);
    CHECK(an_stat(w, 3) == before + 2 && an_stat(w, 5) == AN_R_PART, "slot 4 and a chain not running down the rig are refused");
    an_close(w);
}

/* root; upper on it at the origin; lower 1 m below; foot 1 m below that.
 * The lower part rests bent 10 degrees about x so the knee has a side. */
static int leg(AnWorld *w) {
    int32_t parent[4] = {-1, 0, 1, 2};
    float pivot[12] = {0, 0, 0, 0, 0, 0, 0, -1, 0, 0, -1, 0};
    float rest[16] = {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1}, box[24];
    float h = 10 * PI_F / 360;
    rest[8] = sinf(h); rest[11] = cosf(h);
    for (int i = 0; i < 24; i++) box[i] = (i % 6) < 3 ? -0.1f : 0.1f;
    return an_rig(w, 4, parent, pivot, rest, box);
}
static void origin(const float *m, int p, float o[3]) { o[0] = m[p * 12 + 9]; o[1] = m[p * 12 + 10]; o[2] = m[p * 12 + 11]; }
static float dist3(const float *a, const float *b) {
    float d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    return sqrtf(dot3(d, d));
}
/* the knee's offset from the line root..target, made unit */
static void knee_side(const float *m, const float *t, float out[3]) {
    float a[3], k[3], u[3], v[3];
    origin(m, 1, a); origin(m, 2, k);
    for (int i = 0; i < 3; i++) { u[i] = t[i] - a[i]; v[i] = k[i] - a[i]; }
    float ul = sqrtf(dot3(u, u));
    if (ul > 1e-6f) for (int i = 0; i < 3; i++) u[i] /= ul;
    float d = ul > 1e-6f ? dot3(v, u) : 0;
    for (int i = 0; i < 3; i++) out[i] = v[i] - d * u[i];
    float l = sqrtf(dot3(out, out));
    if (l > 0) for (int i = 0; i < 3; i++) out[i] /= l;
}

static void test_reach(void) {
    AnWorld *w = an_open(4);
    int rig = leg(w);
    int f = an_spawn(w, rig);
    int32_t ch[3] = {1, 2, 3};
    float t[3] = {0.5f, -1.5f, 0.3f}, pole[3] = {0, -1, 5}, o[3];
    an_reach(w, f, 0, ch, t, pole, 1);
    an_update(w, 1.0f / 60);
    const float *m = an_matrices(w, f);
    origin(m, 3, o);
    CHECK(dist3(o, t) <= 1e-4f, "reach places the end within 1e-4 of a reachable target");
    float a[3], k[3];
    origin(m, 1, a); origin(m, 2, k);
    CHECK(near(dist3(a, k), 1, 1e-4f) && near(dist3(k, o), 1, 1e-4f), "the bones keep their lengths");
    float side[3], pp[3] = {pole[0] - a[0], pole[1] - a[1], pole[2] - a[2]};
    knee_side(m, t, side);
    CHECK(dot3(side, pp) > 0, "the knee is on the pole's side");
    an_drop(w, f, 0.3f);
    an_update(w, 1.0f / 60);
    origin(an_matrices(w, f), 0, o);
    CHECK(near(o[1], -0.3f, 1e-6f), "the drop lowers the root");
    origin(an_matrices(w, f), 3, o);
    CHECK(dist3(o, t) <= 1e-4f, "and the end still reaches the target");
    an_drop(w, f, 0);

    /* out of reach: straight toward the target */
    float far[3] = {0, -5, 1};
    an_reach(w, f, 0, ch, far, pole, 1);
    an_update(w, 1.0f / 60);
    m = an_matrices(w, f);
    origin(m, 3, o);
    float l = sqrtf(dot3(far, far)), want[3] = {2 * far[0] / l, 2 * far[1] / l, 2 * far[2] / l};
    CHECK(dist3(o, want) <= 1e-4f, "a target beyond reach straightens the chain toward it");
    origin(m, 2, k);
    float half[3] = {want[0] / 2, want[1] / 2, want[2] / 2};
    CHECK(dist3(k, half) <= 1e-3f, "with the knee on the line");

    /* the target at the root, moving 1 mm a step through it */
    float prev[3] = {0, 0, 0};
    int finite = 1, flips = 0;
    for (int i = 0; i < 100; i++) {
        float tr[3] = {-0.05f + 0.001f * i, 0, 0};
        an_reach(w, f, 0, ch, tr, pole, 1);
        an_update(w, 1.0f / 60);
        m = an_matrices(w, f);
        if (!all_finite(m, 4)) finite = 0;
        float kk[3];
        origin(m, 2, kk);
        if (i > 0 && dot3(kk, prev) <= 0) flips++;
        memcpy(prev, kk, sizeof prev);
    }
    CHECK(finite, "a target at the root gives finite matrices");
    CHECK(flips == 0, "and the knee does not flip as it moves through the root");

    /* the pole on the chain line, the target moving 1 mm a step */
    finite = 1; flips = 0;
    for (int i = 0; i < 100; i++) {
        float tr[3] = {0.3f + 0.001f * i, -1.2f, 0.2f}, pl[3] = {3 * tr[0], 3 * tr[1], 3 * tr[2]};
        an_reach(w, f, 0, ch, tr, pl, 1);
        an_update(w, 1.0f / 60);
        m = an_matrices(w, f);
        if (!all_finite(m, 4)) finite = 0;
        float s[3];
        knee_side(m, tr, s);
        if (i > 0 && dot3(s, prev) <= 0) flips++;
        memcpy(prev, s, sizeof prev);
        origin(m, 3, o);
        if (dist3(o, tr) > 1e-4f) finite = 0;
    }
    CHECK(finite, "a pole on the chain line gives finite matrices and still reaches");
    CHECK(flips == 0, "and the knee side does not change over 100 updates");

    /* weight: half a reach, then a fade to 0 */
    an_reach(w, f, 0, ch, t, pole, 0);
    an_update(w, 1.0f / 60);
    origin(an_matrices(w, f), 3, o);
    float rest_foot[3] = {0, -1 - cosf(10 * PI_F / 180), -sinf(10 * PI_F / 180)};
    CHECK(dist3(o, rest_foot) <= 1e-4f, "weight 0 clears the reach: the foot is back at rest");
    an_reach(w, f, 0, ch, t, pole, 1);
    an_reach_to(w, f, 0, NULL, NULL, NULL, 0, 0.15f);
    an_update(w, 0.075f);
    origin(an_matrices(w, f), 3, o);
    CHECK(dist3(o, t) > 1e-3f && dist3(o, rest_foot) > 1e-3f, "halfway through a 0.15 s fade the foot is between");
    an_update(w, 0.075f);
    origin(an_matrices(w, f), 3, o);
    CHECK(dist3(o, rest_foot) <= 1e-4f, "after 0.15 s the reach has faded to the clip");
    CHECK(an_stat(w, 7) == 0, "a reach faded out holds no record");

    double before = an_stat(w, 3);
    an_reach(w, f, 8, ch, t, pole, 1);
    int32_t bad[3] = {1, 3, 2};
    an_reach(w, f, 0, bad, t, pole, 1);
    CHECK(an_stat(w, 3) == before + 2 && an_stat(w, 5) == AN_R_PART, "slot 8 and a chain out of order are refused");
    an_close(w);
}

/* root; tail on it at the origin; tip 1 m along the tail's z */
static int tail(AnWorld *w) {
    int32_t parent[3] = {-1, 0, 1};
    float pivot[9] = {0, 0, 0, 0, 0, 0, 0, 0, 1};
    return rig_of(w, 3, parent, pivot);
}
static void qy(float deg, float q[4]) {
    float h = deg * PI_F / 360.0f;
    q[0] = 0; q[1] = sinf(h); q[2] = 0; q[3] = cosf(h);
}
static float yaw(const float *m, int part) {   /* a part's world turn about y, degrees, from its z axis */
    return atan2f(m[part * 12 + 6], m[part * 12 + 8]) * 180.0f / PI_F;
}

static void test_spring(void) {
    AnWorld *w = an_open(4);
    int rig = tail(w);
    int f = an_spawn(w, rig);
    an_spring(w, f, 1, 60, 8);
    an_update(w, 1.0f / 60);
    CHECK(near(yaw(an_matrices(w, f), 1), 0, 1e-4f), "a fresh spring starts at its animated rotation");
    float pos[3] = {0, 0, 0}, q[4];
    qy(90, q);
    an_place(w, f, pos, q);
    an_update(w, 1.0f / 60);
    const float *m = an_matrices(w, f);
    CHECK(near(yaw(m, 0), 90, 1e-3f), "the figure turned 90 in one update");
    CHECK(yaw(m, 1) < 45, "the spring part lags behind");
    float o[3], pre[3];
    origin(m, 2, o);
    CHECK(an_part_origin(w, f, 2, pre) == 1, "an_part_origin answers a live figure");
    CHECK(near(pre[0], 1, 1e-4f) && near(pre[2], 0, 1e-4f), "an_part_origin is before springs: the tip where the turn puts it");
    CHECK(dist3(o, pre) > 0.1f, "while the drawn tip lags");
    float most = 0, late = 0;
    for (int i = 1; i < 180; i++) {
        an_update(w, 1.0f / 60);
        float y = yaw(an_matrices(w, f), 1);
        if (y > most) most = y;
        if (i >= 119 && fabsf(y - 90) > late) late = fabsf(y - 90);
    }
    CHECK(most > 91, "it overshoots");
    CHECK(late < 1, "and settles within 2 s at stiffness 60 damping 8");
    an_spring(w, f, 1, 0, 0);
    CHECK(an_stat(w, 7) == 0, "stiffness 0 clears the spring");
    double before = an_stat(w, 3);
    an_spring(w, f, 1, -1, 0);
    CHECK(an_stat(w, 3) == before + 1 && an_stat(w, 5) == AN_R_VALUE, "a negative stiffness is refused");
    CHECK(an_part_origin(w, 999, 0, pre) == 0, "an_part_origin answers 0 for no such figure");
    an_close(w);
}

static uint64_t spring_script(int nudge) {
    AnWorld *w = an_open(8);
    int rig = tail(w);
    int f = an_spawn(w, rig);
    an_spring(w, f, 1, 40, 3);
    an_spring(w, f, 2, 90, 5);
    float pos[3] = {0, 0, 0}, q[4];
    for (int i = 0; i < 300; i++) {
        qy((float)(i % 50) * 3.0f, q);
        an_place(w, f, pos, q);
        an_update(w, 1.0f / 60);
    }
    if (nudge)   /* the same matrices, a different velocity */
        for (int i = w->figs[0].proc; i >= 0; i = w->procs[i].next)
            if (w->procs[i].kind == AN_K_SPRING) { w->procs[i].v[6] += 0.5f; break; }
    uint64_t h = an_hash(w);
    an_close(w);
    return h;
}

static void test_spring_hash(void) {
    uint64_t a = spring_script(0), b = spring_script(0), c = spring_script(1);
    CHECK(a == b, "springs repeat: two runs hash the same");
    CHECK(a != c, "spring state is in the hash");
}

static void test_proc_removed(void) {
    AnWorld *w = an_open(2);
    int rig = leg(w);
    int f = an_spawn(w, rig);
    int32_t ch[3] = {1, 2, 3};
    float t[3] = {0, -1.5f, 0}, pole[3] = {0, 0, 1}, id[4] = {0, 0, 0, 1};
    an_reach(w, f, 0, ch, t, pole, 1);
    an_spring(w, f, 3, 10, 1);
    an_override(w, f, 0, id, NULL, 1);
    CHECK(an_stat(w, 7) == 3, "three records in use");
    an_remove(w, f);
    CHECK(an_stat(w, 7) == 0, "removing the figure frees them");
    int g = an_spawn(w, rig);
    an_update(w, 0.1f);
    CHECK(near(an_matrices(w, g)[3 * 12 + 10], -1 - cosf(10 * PI_F / 180), 1e-5f), "a figure in a freed slot starts with no stage");
    an_close(w);
}

/* a reach fading out and a drop moving: nudge = 1 changes the fade speed, 2 the drop's speed, 3 the drop's goal */
static uint64_t carried_script(int nudge) {
    AnWorld *w = an_open(4);
    int rig = leg(w);
    int f = an_spawn(w, rig);
    int32_t ch[3] = {1, 2, 3};
    float t[3] = {0.5f, -1.5f, 0.3f}, pole[3] = {0, -1, 5};
    an_reach(w, f, 0, ch, t, pole, 1);
    an_drop_to(w, f, 0.3f, 2.0f);
    an_update(w, 1.0f / 60);
    an_reach_to(w, f, 0, NULL, NULL, NULL, 0, 0.15f);
    an_update(w, 1.0f / 60);
    for (int i = w->figs[0].proc; i >= 0; i = w->procs[i].next)
        if (w->procs[i].kind == AN_K_REACH && nudge == 1) w->procs[i].v[8] += 0.5f;   /* the same matrices, a different fade */
    if (nudge == 2) w->figs[0].drop_rate += 0.5f;
    if (nudge == 3) w->figs[0].drop_goal += 0.1f;
    uint64_t h = an_hash(w);
    an_close(w);
    return h;
}

static void test_carried_hash(void) {
    uint64_t a = carried_script(0), b = carried_script(0);
    CHECK(a == b, "a fading reach and a moving drop repeat");
    CHECK(a != carried_script(1), "a reach's fade progress is in the hash");
    CHECK(a != carried_script(2) && a != carried_script(3), "the drop's goal and speed are in the hash");
}

static void test_drop_to(void) {
    AnWorld *w = an_open(2);
    int rig = leg(w);
    int f = an_spawn(w, rig);
    an_drop_to(w, f, 0.3f, 2.0f);
    an_update(w, 1.0f / 60);
    CHECK(near(an_matrices(w, f)[10], -2.0f / 60, 1e-5f), "a drop toward 0.3 at 2 m/s moves 1/30 m in one update");
    an_update(w, 0.2f);
    CHECK(near(an_matrices(w, f)[10], -0.3f, 1e-6f), "and stops at its goal");
    an_drop_to(w, f, 0.1f, 0);
    an_update(w, 1.0f / 60);
    CHECK(near(an_matrices(w, f)[10], -0.1f, 1e-6f), "speed 0 sets it at once");
    an_close(w);
}

/* ---- attachments, sockets, hit tests ---- */

/* the chain's parts as 2 x 10 x 2 grids of 0.1 m voxels, every voxel filled
 * with the part's number + 1 (the chain's boxes are exactly that) */
static void shape_chain(AnWorld *w, int rig) {
    static uint8_t pal[40];
    for (int p = 0; p < 3; p++) {
        memset(pal, p + 1, sizeof pal);
        CHECK(an_shape(w, rig, p, 2, 10, 2, 0.1f, pal) >= 0, "the chain's grids are accepted");
    }
}
static float qdiff(const float a[4], const float b[4]) {   /* 0 when a and b are one rotation */
    float d = fabsf(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
    return 1 - (d > 1 ? 1 : d);
}

static void test_attach_follows(void) {
    AnWorld *w = an_open(8);
    int rig = chain(w, 0);
    float sp[3] = {0, 0.5f, 0};
    int s = an_socket_def(w, rig, 2, sp, an_qid);
    CHECK(s == 0, "a rig's first socket is 0");
    int grand = an_spawn(w, rig);   /* the lowest slots: posed after their parents all the same */
    int child = an_spawn(w, rig);
    int parent = an_spawn(w, rig);
    float off[3] = {0, 0, 0.2f};
    CHECK(an_attach(w, child, parent, s, off, an_qid) == 1, "a child hangs from the hand socket");
    CHECK(an_attach(w, grand, child, s, off, an_qid) == 1, "and a grandchild from the child's");
    an_update(w, 1.0f / 60);
    const float *m = an_matrices(w, child);
    CHECK(near(m[9], 0, 1e-5f) && near(m[10], 2.5f, 1e-5f) && near(m[11], 0.2f, 1e-5f), "the child sits at the socket plus its offset");
    float q90[4];
    qz(90, q90);
    an_override(w, parent, 1, q90, NULL, 1);
    an_update(w, 1.0f / 60);
    m = an_matrices(w, child);
    CHECK(near(m[9], -1.5f, 1e-5f) && near(m[10], 1, 1e-5f) && near(m[11], 0.2f, 1e-5f), "the child follows the hand through a 90-degree turn in the same update");
    CHECK(near(angle(m, 0), 90, 1e-3f), "and turns with it");
    m = an_matrices(w, grand);
    CHECK(near(m[9], -4, 1e-5f) && near(m[10], 1, 1e-5f) && near(m[11], 0.4f, 1e-5f), "the grandchild follows in the same update too");
    float pos[3], q[4];
    CHECK(an_socket(w, parent, s, pos, q) == 1, "the socket answers");
    CHECK(near(pos[0], -1.5f, 1e-5f) && near(pos[1], 1, 1e-5f) && near(pos[2], 0, 1e-5f), "at the turned hand");
    CHECK(qdiff(q, q90) < 1e-6f, "turned with it");
    double before = an_stat(w, 3);
    CHECK(an_socket(w, parent, 1, pos, q) == 0, "a socket the rig lacks answers 0");
    CHECK(an_stat(w, 3) == before + 1 && an_stat(w, 5) == AN_R_SOCKET, "and is refused as no such socket");
    an_detach(w, child);
    float far[3] = {9, 9, 9};
    an_place(w, parent, far, an_qid);
    an_update(w, 1.0f / 60);
    m = an_matrices(w, child);
    CHECK(near(m[9], -1.5f, 1e-5f) && near(m[10], 1, 1e-5f) && near(angle(m, 0), 90, 1e-3f), "a detached child stays where it was");
    an_close(w);
}

static void test_attach_limits(void) {
    AnWorld *w = an_open(16);
    int rig = chain(w, 0);
    float sp[3] = {0, 0.5f, 0};
    int s = an_socket_def(w, rig, 2, sp, an_qid);
    float off[3] = {0, 0, 0};
    int f[7];
    for (int i = 0; i < 7; i++) f[i] = an_spawn(w, rig);
    int ok = 1;
    for (int i = 1; i <= 4; i++) ok &= an_attach(w, f[i], f[i - 1], s, off, an_qid) == 1;
    CHECK(ok, "a chain four deep is allowed");
    CHECK(an_attach(w, f[5], f[4], s, off, an_qid) == 0, "depth 5 is refused");
    CHECK(an_stat(w, 5) == AN_R_ATTACH, "as too deep");
    CHECK(an_attach(w, f[5], f[6], s, off, an_qid) == 1, "f6 carries f5");
    CHECK(an_attach(w, f[6], f[3], s, off, an_qid) == 0, "f6 at depth 4 would put f5 at 5: refused");
    CHECK(an_attach(w, f[6], f[2], s, off, an_qid) == 1, "at depth 3 it is allowed");
    CHECK(an_attach(w, f[0], f[4], s, off, an_qid) == 0, "a cycle is refused");
    CHECK(an_stat(w, 5) == AN_R_ATTACH, "as a cycle");
    CHECK(an_attach(w, f[0], f[0], s, off, an_qid) == 0, "a figure on itself is refused");
    CHECK(an_attach(w, f[1], f[0], 7, off, an_qid) == 0 && an_stat(w, 5) == AN_R_SOCKET, "a socket the parent lacks is refused");
    an_update(w, 1.0f / 60);
    const float *m = an_matrices(w, f[4]);
    CHECK(all_finite(m, 3) && near(m[10], 10, 1e-4f), "four links up the hands: 2.5 m each");
    CHECK(an_stat(w, 3) == 5, "five refusals counted");
    an_close(w);
}

static void test_attach_removed(void) {
    AnWorld *w = an_open(8);
    int rig = chain(w, 0);
    float sp[3] = {0, 0.5f, 0};
    int s = an_socket_def(w, rig, 2, sp, an_qid);
    TK k[2] = {{1, 1, 0, 0}, {1, 1, 1, 360}};
    int clip = turns(w, rig, 1, 1, 2, k, 0, NULL, NULL);
    int c = an_spawn(w, rig), p = an_spawn(w, rig), g = an_spawn(w, rig);
    float at[3] = {3, 0, 0}, off[3] = {0, 0, 0.5f};
    an_place(w, p, at, an_qid);
    an_play(w, p, 0, clip, 0, 1, 1, -1, 0);
    CHECK(an_attach(w, c, p, s, off, an_qid) == 1, "the child hangs from a spinning arm");
    CHECK(an_attach(w, g, c, s, off, an_qid) == 1, "and carries a grandchild");
    for (int i = 0; i < 10; i++) an_update(w, 1.0f / 60);
    float keep[36], keep_g[36];
    memcpy(keep, an_matrices(w, c), sizeof keep);
    memcpy(keep_g, an_matrices(w, g), sizeof keep_g);
    an_remove(w, p);
    float pos[3], q[4];
    CHECK(an_socket(w, p, s, pos, q) == 0, "a socket on a removed figure answers 0");
    int p2 = an_spawn(w, rig);   /* takes the removed figure's slot */
    float there[3] = {-4, 1, 2};
    an_place(w, p2, there, an_qid);
    for (int i = 0; i < 5; i++) an_update(w, 1.0f / 60);
    const float *m = an_matrices(w, c);
    int same = 1;
    for (int i = 0; i < 36; i++) if (!near(m[i], keep[i], 1e-6f)) same = 0;
    CHECK(same, "removing the parent detaches the child, which keeps its last world placement");
    m = an_matrices(w, g);
    same = 1;
    for (int i = 0; i < 36; i++) if (!near(m[i], keep_g[i], 1e-6f)) same = 0;
    CHECK(same, "and the grandchild stays on the child");
    CHECK(an_attach(w, c, p, s, off, an_qid) == 0 && an_stat(w, 5) == AN_R_FIGURE, "attaching to a removed figure is refused");
    an_close(w);
}

/* a blocky rig for the hit tests: four parts with grids, one pure joint */
#define BK_PARTS 5
static const int bk_parent[BK_PARTS] = {-1, 0, 0, 2, 3};
static const int bk_size[BK_PARTS][3] = {{6, 4, 3}, {2, 5, 2}, {3, 3, 3}, {0, 0, 0}, {4, 1, 1}};
static const float bk_anchor[BK_PARTS][3] = {{3, 0, 1.5f}, {1, 5, 1}, {1.5f, 0, 1.5f}, {0, 0, 0}, {0, 0.5f, 0.5f}};
static const float bk_pivot[BK_PARTS][3] = {{0, 0, 0}, {1, 0, 1.5f}, {3, 4, 1.5f}, {3, 1.5f, 1.5f}, {0, 0, 0}};   /* in the parent's grid */
#define BK_V 0.25f
static uint8_t bk_grid[BK_PARTS][72];
static int blocky(AnWorld *w, unsigned *seed) {
    int32_t parent[BK_PARTS]; float pivot[BK_PARTS * 3], rest[BK_PARTS * 4], box[BK_PARTS * 6];
    for (int p = 0; p < BK_PARTS; p++) {
        parent[p] = bk_parent[p];
        for (int k = 0; k < 3; k++) {
            float pa = p == 0 ? 0 : bk_anchor[bk_parent[p]][k];
            pivot[p * 3 + k] = (bk_pivot[p][k] - pa) * BK_V;
            box[p * 6 + k] = -bk_anchor[p][k] * BK_V;
            box[p * 6 + 3 + k] = (bk_size[p][k] - bk_anchor[p][k]) * BK_V;
        }
        rest[p * 4] = rest[p * 4 + 1] = rest[p * 4 + 2] = 0; rest[p * 4 + 3] = 1;
    }
    int rig = an_rig(w, BK_PARTS, parent, pivot, rest, box);
    for (int p = 0; p < BK_PARTS; p++) {
        int n = bk_size[p][0] * bk_size[p][1] * bk_size[p][2];
        for (int i = 0; i < n; i++) {
            *seed = *seed * 1664525u + 1013904223u;
            unsigned r = (*seed >> 16) % 100;
            bk_grid[p][i] = r < 45 ? 0 : (uint8_t)(1 + r % 5);
        }
        CHECK(an_shape(w, rig, p, bk_size[p][0], bk_size[p][1], bk_size[p][2], BK_V, n ? bk_grid[p] : NULL) >= 0, "a blocky part's grid is accepted");
    }
    return rig;
}
static float frand(unsigned *s) { *s = *s * 1664525u + 1013904223u; return (float)(*s >> 8) / 16777216.0f; }
static void qrand(unsigned *s, float q[4]) {
    for (int i = 0; i < 4; i++) q[i] = frand(s) * 2 - 1;
    float l = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (int i = 0; i < 4; i++) q[i] /= l;
}

typedef struct { int fig, part, x, y, z, colour; float dist, n[3]; } Ref;
/* is figure i (of the test's figures) the ignored one or carried by it? par: the test's own attachment table */
static int ref_ignored(const int *par, int i, int ign) {
    for (; i >= 0; i = par[i]) if (i == ign) return 1;
    return 0;
}
/* every voxel of every part of every figure, by brute force: the nearest entry along unit d within max */
static int ref_ray(AnWorld *w, const int *figs, const int *par, int nf, const float o[3], const float d[3], float max, int ign, Ref *out) {
    int found = 0;
    float best = max;
    for (int i = 0; i < nf; i++) {
        if (ign >= 0 && ref_ignored(par, i, ign)) continue;
        const float *M = an_matrices(w, figs[i]);
        for (int p = 0; p < BK_PARTS; p++) {
            const float *m = M + p * 12;
            float rel[3] = {o[0] - m[9], o[1] - m[10], o[2] - m[11]}, lo[3], ld[3];
            for (int k = 0; k < 3; k++) { lo[k] = dot3(m + 3 * k, rel); ld[k] = dot3(m + 3 * k, d); }
            for (int z = 0; z < bk_size[p][2]; z++)
                for (int y = 0; y < bk_size[p][1]; y++)
                    for (int x = 0; x < bk_size[p][0]; x++) {
                        int c = bk_grid[p][x + bk_size[p][0] * (y + bk_size[p][1] * z)];
                        if (!c) continue;
                        int v[3] = {x, y, z}, ax = -1, miss = 0;
                        float tn = -INFINITY, tf = INFINITY;
                        for (int k = 0; k < 3; k++) {
                            float a = (v[k] - bk_anchor[p][k]) * BK_V, b = a + BK_V;
                            if (ld[k] == 0) { if (lo[k] < a || lo[k] > b) miss = 1; continue; }
                            float t1 = (a - lo[k]) / ld[k], t2 = (b - lo[k]) / ld[k];
                            if (t1 > t2) { float t = t1; t1 = t2; t2 = t; }
                            if (t1 > tn) { tn = t1; ax = k; }
                            if (t2 < tf) tf = t2;
                        }
                        float t = tn < 0 ? 0 : tn;
                        if (miss || tf < t || t > best || (found && t == best)) continue;
                        best = t; found = 1;
                        Ref r = {i, p, x, y, z, c, t, {0, 0, 0}};
                        if (ax >= 0 && tn >= 0) {
                            float sgn = ld[ax] > 0 ? -1.0f : 1.0f;
                            for (int k = 0; k < 3; k++) r.n[k] = m[3 * ax + k] * sgn;
                        }
                        *out = r;
                    }
        }
    }
    return found;
}

static void test_hit_ray(void) {
    unsigned seed = 12345;
    AnWorld *w = an_open(64);
    int rig = blocky(w, &seed);
    float sp[3] = {1, 0, 0};
    int s = an_socket_def(w, rig, 4, sp, an_qid);
    CHECK(rig >= 0 && s == 0, "the blocky rig and its socket");
    int figs[50], par[50];
    for (int i = 0; i < 50; i++) {
        figs[i] = an_spawn(w, rig);
        par[i] = -1;
        float pos[3] = {frand(&seed) * 20 - 10, frand(&seed) * 20 - 10, frand(&seed) * 20 - 10}, q[4];
        qrand(&seed, q);
        an_place(w, figs[i], pos, q);
        for (int p = 1; p < BK_PARTS; p++) { qrand(&seed, q); an_override(w, figs[i], p, q, NULL, 1); }
    }
    float off[3] = {0.3f, 0, 0};
    CHECK(an_attach(w, figs[1], figs[0], s, off, an_qid) == 1 && an_attach(w, figs[2], figs[1], s, off, an_qid) == 1, "two figures carried");
    par[1] = 0; par[2] = 1;
    an_update(w, 0);
    int agree = 0, hits = 0, ties = 0, normals = 1, colours = 1, ignored = 0;
    for (int r = 0; r < 1000; r++) {
        float o[3] = {frand(&seed) * 32 - 16, frand(&seed) * 32 - 16, frand(&seed) * 32 - 16};
        const float *tm = an_matrices(w, figs[(int)(frand(&seed) * 50) % 50]);
        float d[3], dl;
        for (int k = 0; k < 3; k++) d[k] = tm[9 + k] + frand(&seed) * 2 - 1 - o[k];
        dl = an_len(d);
        float u[3] = {d[0] / dl, d[1] / dl, d[2] / dl}, d3[3] = {u[0] * 3, u[1] * 3, u[2] * 3};
        float max = r % 10 == 9 ? 5.0f : 60.0f;
        int ign = r % 7 == 6 ? (int)(frand(&seed) * 50) % 50 : -1;
        if (r % 7 == 6) ign = r % 2 ? 0 : ign;   /* half of these ignore the carrier of two */
        Ref ref;
        AnHit h;
        int want = ref_ray(w, figs, par, 50, o, u, max, ign, &ref);
        int got = an_hit_ray(w, o, d3, max, ign >= 0 ? figs[ign] : -1, &h);
        if (ign >= 0) ignored++;
        if (want != got) continue;
        if (!want) { agree++; continue; }
        hits++;
        if (h.fig == figs[ref.fig] && h.part == ref.part && h.x == ref.x && h.y == ref.y && h.z == ref.z) {
            if (near(h.dist, ref.dist, 1e-4f)) agree++;
            if (h.colour != ref.colour) colours = 0;
            if (ref.dist > 0 && !(near(h.nx, ref.n[0], 1e-4f) && near(h.ny, ref.n[1], 1e-4f) && near(h.nz, ref.n[2], 1e-4f))) normals = 0;
        } else if (near(h.dist, ref.dist, 1e-5f)) { ties++; agree++; }
    }
    printf("  hit_ray: %d of 1000 agree, %d hits, %d ties, %d with an ignored figure\n", agree, hits, ties, ignored);
    CHECK(agree == 1000, "1000 random rays agree with a brute-force test of every voxel (figure, part, voxel, distance within 1e-4)");
    CHECK(hits > 300, "and most of them hit something");
    CHECK(colours, "the colour is the voxel's palette index");
    CHECK(normals, "the normal is the struck face's, in world space");
    CHECK(ties == 0, "no ties between voxels");
    an_close(w);
}

/* do two boxes overlap? corners projected on the 15 axes (independent of the kernel's test) */
static int ref_obb(const float ca[3], const float A[3][3], const float ha[3], const float cb[3], const float B[3][3], const float hb[3]) {
    float pa[8][3], pb[8][3], axes[15][3];
    for (int i = 0; i < 8; i++)
        for (int k = 0; k < 3; k++) {
            pa[i][k] = ca[k]; pb[i][k] = cb[k];
            for (int j = 0; j < 3; j++) {
                float sg = (i >> j & 1) ? 1.0f : -1.0f;
                pa[i][k] += sg * ha[j] * A[j][k];
                pb[i][k] += sg * hb[j] * B[j][k];
            }
        }
    int n = 0;
    for (int i = 0; i < 3; i++) { memcpy(axes[n++], A[i], sizeof axes[0]); memcpy(axes[n++], B[i], sizeof axes[0]); }
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            an_cross(A[i], B[j], axes[n]);
            if (an_len(axes[n]) > 1e-6f) n++;
        }
    for (int a = 0; a < n; a++) {
        float amin = INFINITY, amax = -INFINITY, bmin = INFINITY, bmax = -INFINITY;
        for (int i = 0; i < 8; i++) {
            float x = dot3(pa[i], axes[a]), y = dot3(pb[i], axes[a]);
            if (x < amin) amin = x; if (x > amax) amax = x;
            if (y < bmin) bmin = y; if (y > bmax) bmax = y;
        }
        if (amax < bmin || bmax < amin) return 0;
    }
    return 1;
}

static void test_hit_box(void) {
    unsigned seed = 777;
    AnWorld *w = an_open(64);
    int rig = blocky(w, &seed);
    float sp[3] = {1, 0, 0};
    int s = an_socket_def(w, rig, 4, sp, an_qid);
    int figs[50], par[50];
    for (int i = 0; i < 50; i++) {
        figs[i] = an_spawn(w, rig);
        par[i] = -1;
        float pos[3] = {frand(&seed) * 16 - 8, frand(&seed) * 16 - 8, frand(&seed) * 16 - 8}, q[4];
        qrand(&seed, q);
        an_place(w, figs[i], pos, q);
        for (int p = 1; p < BK_PARTS; p++) { qrand(&seed, q); an_override(w, figs[i], p, q, NULL, 1); }
    }
    float off[3] = {0, 0, 0};
    an_attach(w, figs[3], figs[4], s, off, an_qid);
    par[3] = 4;
    an_update(w, 1.0f / 60);
    static int32_t got[2 * 4096];
    int agree = 0, pairs = 0;
    for (int r = 0; r < 200; r++) {
        float c[3] = {frand(&seed) * 16 - 8, frand(&seed) * 16 - 8, frand(&seed) * 16 - 8};
        float half[3] = {0.05f + frand(&seed) * 2.5f, 0.05f + frand(&seed) * 2.5f, 0.05f + frand(&seed) * 2.5f}, q[4], Q[3][3];
        qrand(&seed, q);
        for (int k = 0; k < 3; k++) { float e[3] = {0, 0, 0}; e[k] = 1; an_qrot(q, e, Q[k]); }
        int ign = r % 5 == 4 ? (r % 2 ? 4 : (int)(frand(&seed) * 50) % 50) : -1;
        int want[2 * 4096], nw = 0;
        for (int i = 0; i < 50; i++) {
            if (ign >= 0 && ref_ignored(par, i, ign)) continue;
            const float *M = an_matrices(w, figs[i]);
            for (int p = 0; p < BK_PARTS; p++) {
                if (bk_size[p][0] * bk_size[p][1] * bk_size[p][2] == 0) continue;   /* a pure joint has no box */
                const float *m = M + p * 12;
                float lc[3], h[3], wc[3], A[3][3];
                for (int k = 0; k < 3; k++) { h[k] = bk_size[p][k] * BK_V / 2; lc[k] = h[k] - bk_anchor[p][k] * BK_V; }
                for (int k = 0; k < 3; k++) {
                    wc[k] = m[9 + k] + m[k] * lc[0] + m[3 + k] * lc[1] + m[6 + k] * lc[2];
                    for (int j = 0; j < 3; j++) A[j][k] = m[3 * j + k];
                }
                if (ref_obb(c, Q, half, wc, A, h)) { want[nw * 2] = figs[i]; want[nw * 2 + 1] = p; nw++; }
            }
        }
        int n = an_hit_box(w, c, half, q, ign >= 0 ? figs[ign] : -1, 4096, got);
        pairs += nw;
        if (n == nw && memcmp(got, want, (size_t)nw * 2 * sizeof(int32_t)) == 0) agree++;
    }
    printf("  hit_box: %d of 200 agree, %d pairs in all\n", agree, pairs);
    CHECK(agree == 200, "200 random boxes return exactly the pairs whose part boxes overlap (brute force)");
    CHECK(pairs > 200, "and they find many");
    int32_t few[4];
    float c0[3] = {0, 0, 0}, big[3] = {50, 50, 50};
    int all = an_hit_box(w, c0, big, an_qid, -1, 2, few);
    CHECK(all == 50 * 4, "a box over everything counts every part with a box, though it writes only max");
    CHECK(few[0] == figs[0] && few[1] == 0 && few[2] == figs[0] && few[3] == 1, "in figure then part order");
    an_close(w);
}

static void test_hit_ignore(void) {
    AnWorld *w = an_open(8);
    int rig = chain(w, 0);
    shape_chain(w, rig);
    float back[3] = {0, 0, -1};
    int s = an_socket_def(w, rig, 0, back, an_qid);
    int sh = an_spawn(w, rig), gun = an_spawn(w, rig), t = an_spawn(w, rig);
    float off[3] = {0, 0, 0}, tp[3] = {0, 0, -5};
    an_place(w, t, tp, an_qid);
    CHECK(an_attach(w, gun, sh, s, off, an_qid) == 1, "the shooter carries a gun 1 m behind");
    an_update(w, 1.0f / 60);
    float o[3] = {0, 0.5f, 5}, d[3] = {0, 0, -1};
    AnHit h;
    CHECK(an_hit_ray(w, o, d, 100, -1, &h) == 1 && h.fig == sh && near(h.dist, 4.9f, 1e-5f), "the shooter is hit first");
    CHECK(h.part == 0 && h.colour == 1 && near(h.nz, 1, 1e-5f), "on its root's +z face, colour 1");
    CHECK(an_hit_ray(w, o, d, 100, sh, &h) == 1 && h.fig == t && near(h.dist, 9.9f, 1e-5f), "ignoring the shooter skips it and the gun it carries");
    CHECK(an_hit_ray(w, o, d, 100, gun, &h) == 1 && h.fig == sh, "ignoring the gun still hits the shooter");
    CHECK(an_hit_ray(w, o, d, 4.8f, -1, &h) == 0, "nothing within 4.8 m");
    float side[3] = {0, 0.5f, 5}, sd[3] = {1, 0, 0};
    CHECK(an_hit_ray(w, side, sd, 100, -1, &h) == 0, "a ray past everything misses");
    an_close(w);
}

static void test_stale_query(void) {
    AnWorld *w = an_open(8);
    int rig = chain(w, 0);
    shape_chain(w, rig);
    float sp[3] = {0, 0.5f, 0};
    int s = an_socket_def(w, rig, 2, sp, an_qid);
    int f = an_spawn(w, rig), c = an_spawn(w, rig);
    float off[3] = {0, 0, 0};
    an_attach(w, c, f, s, off, an_qid);
    an_update(w, 1.0f / 60);
    float at[3] = {10, 0, 0};
    an_place(w, f, at, an_qid);   /* not updated since: stale */
    CHECK(an_stat(w, 2) == 1, "the moved figure is stale");
    AnHit h;
    float o[3] = {10, 0.5f, 5}, d[3] = {0, 0, -1};
    CHECK(an_hit_ray(w, o, d, 100, -1, &h) == 1 && h.fig == f && h.part == 0 && near(h.dist, 4.9f, 1e-5f), "a hit test poses the stale figure first");
    CHECK(an_stat(w, 2) == 0, "and it is stale no more");
    float oc[3] = {10, 3.2f, 5};
    CHECK(an_hit_ray(w, oc, d, 100, -1, &h) == 1 && h.fig == c, "the child it carries is posed with it");
    float at2[3] = {20, 0, 0}, pos[3], q[4];
    an_place(w, f, at2, an_qid);
    CHECK(an_socket(w, f, s, pos, q) == 1 && near(pos[0], 20, 1e-5f) && near(pos[1], 2.5f, 1e-5f), "a socket query poses the stale figure first");
    TK k[1] = {{1, 0, 0, 90}};
    int clip = turns(w, rig, 1, 0, 1, k, 0, NULL, NULL);
    an_play(w, f, 0, clip, 0, 1, 1, -1, 0);
    CHECK(an_socket(w, f, s, pos, q) == 1 && near(pos[0], 18.5f, 1e-5f) && near(pos[1], 1, 1e-5f), "a newly played clip is posed before the socket answers");
    float box_c[3] = {18.5f, 1, 0}, half[3] = {0.05f, 0.05f, 0.05f};
    int32_t pairs[8];
    int np = an_hit_box(w, box_c, half, an_qid, -1, 4, pairs), has_c = 0;
    for (int i = 0; i < np && i < 4; i++) if (pairs[i * 2] == c && pairs[i * 2 + 1] == 0) has_c = 1;
    CHECK(has_c, "a box finds the child at the moved socket");
    an_close(w);
}

static void test_presprings(void) {
    AnWorld *w = an_open(4);
    int rig = chain(w, 0);
    shape_chain(w, rig);
    float sp[3] = {0, 0.5f, 0};
    int s = an_socket_def(w, rig, 2, sp, an_qid);
    int f = an_spawn(w, rig);
    an_spring(w, f, 1, 20, 2);
    an_update(w, 1.0f / 60);
    float zero[3] = {0, 0, 0}, q90[4], pos[3], q[4];
    qz(90, q90);
    an_place(w, f, zero, q90);
    an_update(w, 1.0f / 60);
    const float *m = an_matrices(w, f);
    float drawn[3] = {m[24 + 9], m[24 + 10], m[24 + 11]}, pre[3] = {-2, 0, 0};
    CHECK(dist3(drawn, pre) > 0.5f, "the springy arm lags: the drawn hand is not where the pose puts it");
    CHECK(an_socket(w, f, s, pos, q) == 1 && near(pos[0], -2.5f, 1e-5f) && near(pos[1], 0, 1e-5f), "the socket answers the pose before springs");
    AnHit h;
    float o[3] = {-2.5f, 0, 5}, d[3] = {0, 0, -1};
    CHECK(an_hit_ray(w, o, d, 100, -1, &h) == 1 && h.part == 2 && near(h.dist, 4.9f, 1e-5f), "a ray hits the hand where the pose before springs has it");
    float od[3] = {drawn[0], drawn[1] + 0.05f, 5};
    CHECK(an_hit_ray(w, od, d, 100, -1, &h) == 0 || h.part != 2, "not where it is drawn");
    an_close(w);
}

static uint64_t attach_script(int socket) {
    AnWorld *w = an_open(4);
    int rig = chain(w, 0);
    float sp[3] = {0, 0.5f, 0};
    an_socket_def(w, rig, 2, sp, an_qid);
    an_socket_def(w, rig, 2, sp, an_qid);   /* the same place */
    int c = an_spawn(w, rig), p = an_spawn(w, rig);
    float off[3] = {0, 0.1f, 0};
    an_attach(w, c, p, socket, off, an_qid);
    for (int i = 0; i < 5; i++) an_update(w, 1.0f / 60);
    uint64_t h = an_hash(w);
    an_close(w);
    return h;
}
static void test_attach_hash(void) {
    CHECK(attach_script(0) == attach_script(0), "a run with attachments repeats");
    CHECK(attach_script(0) != attach_script(1), "the link is in the hash, though the matrices match");
}

/* a springy figure moved, then (or not) queried before the next update */
static uint64_t query_script(int query) {
    AnWorld *w = an_open(4);
    int rig = chain(w, 0);
    shape_chain(w, rig);
    float sp[3] = {0, 0.5f, 0};
    int s = an_socket_def(w, rig, 2, sp, an_qid);
    int f = an_spawn(w, rig), c = an_spawn(w, rig);
    float off[3] = {0, 0, 0};
    an_attach(w, c, f, s, off, an_qid);
    an_update(w, 1.0f / 60);
    an_spring(w, f, 1, 30, 3);   /* fresh until the next update */
    float at[3] = {1, 0, 0}, q90[4], pos[3], q[4], o[3] = {1, 0.5f, 5}, d[3] = {0, 0, -1};
    qz(90, q90);
    an_place(w, f, at, q90);
    if (query) {
        AnHit h;
        an_hit_ray(w, o, d, 100, -1, &h);
        an_socket(w, c, s, pos, q);
    }
    an_update(w, 1.0f / 60);
    an_update(w, 1.0f / 60);
    uint64_t hash = an_hash(w);
    an_close(w);
    return hash;
}
static void test_query_no_trace(void) {
    CHECK(query_script(0) == query_script(1), "posing stale figures for a query changes nothing the next update carries");
}

static void test_shape_refusals(void) {
    AnWorld *w = an_open(4);
    int rig = chain(w, 0);
    static uint8_t pal[129 * 10 * 2];
    memset(pal, 1, sizeof pal);
    CHECK(an_shape(w, rig, 0, 3, 10, 2, 0.1f, pal) == -1 && an_stat(w, 5) == AN_R_VALUE, "a grid that does not fill the part's box is refused");
    CHECK(an_shape(w, rig, 5, 2, 10, 2, 0.1f, pal) == -1 && an_stat(w, 5) == AN_R_PART, "a part the rig lacks is refused");
    CHECK(an_shape(w, rig, 0, 129, 10, 2, 0.1f, pal) == -1, "129 voxels a side is refused");
    CHECK(an_shape(w, 99, 0, 2, 10, 2, 0.1f, pal) == -1 && an_stat(w, 5) == AN_R_RIG, "a rig that does not exist is refused");
    CHECK(an_socket_def(w, rig, 3, an_qid, an_qid) == -1 && an_stat(w, 5) == AN_R_PART, "a socket on a part the rig lacks is refused");
    float zq[4] = {0, 0, 0, 0};
    CHECK(an_socket_def(w, rig, 0, an_qid, zq) == -1 && an_stat(w, 5) == AN_R_VALUE, "a socket with a zero rotation is refused");
    int n = 0;
    for (int i = 0; i < AN_SOCKETS + 1; i++) if (an_socket_def(w, rig, 0, an_qid, an_qid) >= 0) n++;
    CHECK(n == AN_SOCKETS && an_stat(w, 5) == AN_R_FULL, "a rig holds AN_SOCKETS sockets");
    an_close(w);
}

int main(void) {
    test_sampling();
    test_antipodal();
    test_hierarchy();
    test_masks_additive();
    test_crossfade();
    test_blend();
    test_dt_and_marks();
    test_degenerate();
    test_removed();
    test_refusals();
    test_determinism();
    test_override();
    test_aim();
    test_reach();
    test_spring();
    test_spring_hash();
    test_proc_removed();
    test_carried_hash();
    test_drop_to();
    test_attach_follows();
    test_attach_limits();
    test_attach_removed();
    test_hit_ray();
    test_hit_box();
    test_hit_ignore();
    test_stale_query();
    test_presprings();
    test_attach_hash();
    test_query_no_trace();
    test_shape_refusals();
    if (fails == 0) printf("anim_test: ok\n");
    return fails == 0 ? 0 : 1;
}

/* Particles: the ring (appending wraps, the live span is what is younger
 * than the longest life, counts are clamped, a full ring overwrites its
 * oldest) and the closed form the vertex shader computes. */
#include <stdio.h>
#include <math.h>
#include <string.h>
#include "strata_particles.h"

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
    CHECK(r.overwritten == 5, "the five pushed over live slots are counted as overwritten");

    /* the closed form: effect 0 is spray's path, ballistic under -9.8 m/s2 */
    {
        float fx[PT_EFFECT_FLOATS], rec[PT_FLOATS] = {0}, p[3];
        pt_effect_spray(fx);
        rec[0] = 1.0f; rec[1] = 2.0f; rec[2] = 3.0f; rec[5] = 7.0f; rec[7] = 2.0f;
        pt_position(rec, fx, 0.5f, p);
        CHECK(fabsf(p[0] - 1.0f) < 1e-5f && fabsf(p[1] - (2.0f + 3.5f - 1.225f)) < 1e-5f && fabsf(p[2] - 3.0f) < 1e-5f, "effect 0 flies as spray: start + v t - 4.9 t^2");
        CHECK(fx[0] == 0.0f && fx[2] == 0.0f, "effect 0 is shaded and lit per pixel");
    }
    /* drag: the speed tends to a / k */
    {
        float fx[PT_EFFECT_FLOATS] = {0}, rec[PT_FLOATS] = {0}, p0[3], p1[3];
        fx[33] = 2.0f; fx[37] = 3.0f;          /* drag 2, ay 3 */
        rec[7] = 20.0f;
        pt_position(rec, fx, 10.0f, p0);
        pt_position(rec, fx, 10.001f, p1);
        CHECK(fabsf((p1[1] - p0[1]) / 0.001f - 1.5f) < 1e-2f, "with drag the speed tends to a / k");
        pt_position(rec, fx, 0.0f, p0);
        CHECK(fabsf(p0[0]) < 1e-6f && fabsf(p0[1]) < 1e-6f && fabsf(p0[2]) < 1e-6f, "at age 0 a particle is at its start");
    }
    /* wobble: a sideways sway of the given metres, phased by the seed */
    {
        float fx[PT_EFFECT_FLOATS] = {0}, rec[PT_FLOATS] = {0}, p[3];
        fx[34] = 0.1f; fx[35] = 1.0f;          /* 0.1 m at 1 Hz */
        rec[7] = 5.0f; rec[10] = 0.25f;
        pt_position(rec, fx, 0.0f, p);
        CHECK(fabsf(p[0] - 0.1f) < 1e-6f, "a seed of 0.25 starts the sway at its full 0.1 m");
    }

    /* bursts written from a shape: the record writer */
    {
        static float recs[512 * PT_FLOATS], again[512 * PT_FLOATS];
        float seg[6] = { 5.0f, 6.0f, 7.0f, 5.0f, 6.0f, 7.0f }, abc[3] = { 0.5f, 0.0f, 0.0f }, vel[3] = { 0.0f, 1.0f, 0.0f };
        int inside = 1, vel_ok = 1, life_ok = 1;
        for (uint32_t i = 0; i < 512; i++) {
            float *r = recs + i * PT_FLOATS;
            pt_write(r, 3, 1, seg, abc, vel, 2.0f, 1.0f, 0.3f, 0.25f, 99u, i, 512, 4.0f);
            float dx = r[0] - 5.0f, dy = r[1] - 6.0f, dz = r[2] - 7.0f;
            if (dx * dx + dy * dy + dz * dz > 0.25f + 1e-5f) inside = 0;
            float ux = r[4], uy = r[5] - 1.0f, uz = r[6];
            if (ux * ux + uy * uy + uz * uz > 4.0f + 1e-4f) vel_ok = 0;
            if (r[7] < 0.7f - 1e-5f || r[7] > 1.3f + 1e-5f) life_ok = 0;
            if (r[3] != 4.0f || r[8] != 3.0f || r[12] != -1.0f) inside = 0;
        }
        CHECK(inside, "a sphere of 0.5 m: every start within it, born now, of its effect, no layer override");
        CHECK(vel_ok, "velocities within the spread's ball of the mean");
        CHECK(life_ok, "lives within life * (1 +- life_spread)");
        float bseg[6] = { 0, 0, 0, 0, 0, 0 }, box[3] = { 1.0f, 0.1f, 2.0f };
        int in_box = 1;
        for (uint32_t i = 0; i < 512; i++) {
            float *r = recs + i * PT_FLOATS;
            pt_write(r, 3, 2, bseg, box, vel, 0.0f, 1.0f, 0.0f, 0.0f, 7u, i, 512, 0.0f);
            if (fabsf(r[0]) > 1.0f + 1e-5f || fabsf(r[1]) > 0.1f + 1e-5f || fabsf(r[2]) > 2.0f + 1e-5f) in_box = 0;
        }
        CHECK(in_box, "a box of half sizes (1, 0.1, 2): every start inside it");
        float line[6] = { 0, 0, 0, 10.0f, 0, 0 }, none[3] = { 0, 0, 0 };
        int along = 1;
        for (uint32_t i = 0; i < 10; i++) {
            float *r = recs + i * PT_FLOATS;
            pt_write(r, 3, 0, line, none, vel, 0.0f, 1.0f, 0.0f, 0.0f, 7u, i, 10, 0.0f);
            if (fabsf(r[0] - (0.5f + (float)i)) > 1e-5f) along = 0;
        }
        CHECK(along, "a point moved along 10 m, ten particles: at 0.5, 1.5 .. 9.5");
        for (uint32_t i = 0; i < 64; i++) {
            pt_write(recs + i * PT_FLOATS, 3, 1, seg, abc, vel, 2.0f, 1.0f, 0.3f, 0.25f, 99u, i, 64, 4.0f);
            pt_write(again + i * PT_FLOATS, 3, 1, seg, abc, vel, 2.0f, 1.0f, 0.3f, 0.25f, 99u, i, 64, 4.0f);
        }
        CHECK(memcmp(recs, again, 64 * PT_FLOATS * sizeof(float)) == 0, "the same seed writes the same records");
        for (uint32_t i = 0; i < 64; i++) pt_write(again + i * PT_FLOATS, 3, 1, seg, abc, vel, 2.0f, 1.0f, 0.3f, 0.25f, 100u, i, 64, 4.0f);
        CHECK(memcmp(recs, again, 64 * PT_FLOATS * sizeof(float)) != 0, "another seed writes others");
    }

    if (fails == 0) printf("particles_test: ok\n");
    return fails == 0 ? 0 : 1;
}

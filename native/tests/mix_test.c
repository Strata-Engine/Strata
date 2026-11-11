/* The mixer, headless: the command ring, patches, envelopes, filters,
 * controls, the limiter, repeatable renders, WAV files, fingerprints. */
#include <stdio.h>
#include <stdlib.h>
#include "mix_util.h"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL: %s\n", what); fails++; } } while (0)

#define SEC(s) ((int)((s) * SM_RATE + 0.5))
static float buf[2 * SM_RATE * 4];

static void render(SmMixer *m, int frames) { sm_mix(m, buf, frames); }

static void test_ring(void) {
    static SmCmd store[16];
    SmRing r; sm_ring_init(&r, store, 16);
    SmCmd c; memset(&c, 0, sizeof c);
    for (int i = 0; i < 10; i++) { c.voice = i; CHECK(sm_ring_push(&r, &c) == 1, "a ring with room takes a push"); }
    int ordered = 1;
    for (int i = 0; i < 10; i++) { SmCmd o; if (!sm_ring_pop(&r, &o) || o.voice != i) ordered = 0; }
    CHECK(ordered, "ten pushes pop in order");
    SmCmd o; CHECK(sm_ring_pop(&r, &o) == 0, "an empty ring pops nothing");
    for (int i = 0; i < 16; i++) sm_ring_push(&r, &c);
    CHECK(sm_ring_push(&r, &c) == 0, "a ring of 16 refuses the 17th push");
}

static void test_sine_level(void) {
    SmMixer *m = sm_open();
    float p[SM_PATCH_FLOATS]; mu_patch_clear(p); mu_source(p, 0, SM_SRC_SINE, 1000.0f, 0.5f);
    mu_send_patch(m, 1, p);
    mu_play_flat(m, 1, 1, 1.0f, 0.0f);
    render(m, SEC(0.5));
    double rms = mu_rms(buf, SEC(0.1), SEC(0.5));
    CHECK(fabs(mu_db(rms) - mu_db(0.5 / sqrt(2.0))) < 0.5, "a sine of level 0.5 plays at its level");
    static float fp[SM_FP_VALUES * 8];
    int w = sm_fingerprint(buf, SEC(0.5), fp);
    CHECK(w == 5, "half a second is five 100 ms fingerprint windows");
    int loudest = 1;   /* window 2, left channel: band 5 (1 kHz) leads every other by 20 dB */
    for (int b = 0; b < SM_FP_BANDS; b++) if (b != 5 && fp[2 * SM_FP_VALUES + 5] - fp[2 * SM_FP_VALUES + b] < 20.0f) loudest = 0;
    CHECK(loudest, "the 1 kHz band is the loudest by 20 dB");
    sm_close(m);
}

static void test_envelope(void) {
    SmMixer *m = sm_open();
    float p[SM_PATCH_FLOATS]; mu_patch_clear(p); mu_source(p, 0, SM_SRC_SINE, 2000.0f, 0.8f);
    mu_env(p, 0, 0.1f, 0.0f, 1.0f, 0.2f, 0.0f);
    mu_send_patch(m, 1, p);
    mu_play_flat(m, 1, 1, 1.0f, -1.0f);   /* hard left: the left channel carries the whole source */
    render(m, SEC(0.5));
    int first = -1;
    for (int i = 0; i < SEC(0.5); i++) if (fabsf(buf[2 * i]) >= 0.72f) { first = i; break; }
    double t = (double)first / SM_RATE;
    CHECK(t >= 0.09 && t <= 0.11, "an attack of 0.1 s reaches 90% between 0.09 and 0.11 s");
    mu_cmd(m, SM_OP_STOP, 1, 0, -1.0f);   /* a negative fade: release by the patch's envelope */
    render(m, SEC(0.2) + SM_BLOCK);
    float tail = mu_peak(buf, SEC(0.2), SEC(0.2) + SM_BLOCK);
    CHECK(mu_db(tail) < -90.0, "the release ends in silence within release + one block");
    CHECK(sm_stats(m).playing == 0, "a released voice is freed");
    sm_close(m);
}

static void test_filter(void) {
    SmMixer *m = sm_open();
    float p[SM_PATCH_FLOATS]; mu_patch_clear(p); mu_source(p, 0, SM_SRC_PINK, 0.0f, 0.5f);
    mu_filter(p, 0, SM_FLT_LOW, 1000.0f, 0.707f);
    mu_send_patch(m, 1, p);
    mu_play_flat(m, 1, 1, 1.0f, 0.0f);
    render(m, SEC(1.0));
    static float fp[SM_FP_VALUES * 12];
    int w = sm_fingerprint(buf, SEC(1.0), fp);
    double lo = 0, hi = 0;
    for (int i = 2; i < w; i++) { lo += fp[i * SM_FP_VALUES + 3]; hi += fp[i * SM_FP_VALUES + 7]; }
    CHECK((lo - hi) / (w - 2) >= 20.0, "a 1 kHz low-pass puts the 4 kHz band 20 dB under the 250 Hz band");
    sm_close(m);
}

static void test_control(void) {
    SmMixer *m = sm_open();
    float p[SM_PATCH_FLOATS]; mu_patch_clear(p); mu_source(p, 0, SM_SRC_SINE, 50.0f, 0.8f);
    mu_control(p, 0, SM_TGT_GAIN, 0.0f, 1.0f, 0, 0.0f);
    mu_send_patch(m, 1, p);
    mu_play_flat(m, 1, 1, 1.0f, -1.0f);
    render(m, SEC(0.2));
    CHECK(mu_peak(buf, 0, SEC(0.2)) < 0.001f, "a gain control at 0 is silent");
    mu_cmd(m, SM_OP_CONTROL, 1, 0, 1.0f);
    render(m, SEC(0.2));
    int at = SM_RATE / 50;   /* 20 ms */
    CHECK(mu_peak(buf, at, at + SM_RATE / 50) > 0.72f, "the control has taken effect within 20 ms");
    float jump = 0.0f;
    for (int i = 1; i < SEC(0.2); i++) { float d = fabsf(buf[2 * i] - buf[2 * i - 2]); if (d > jump) jump = d; }
    CHECK(jump < 0.05f, "and it glides: no sample-to-sample jump over 0.05");
    sm_close(m);
}

static void test_limiter(void) {
    SmMixer *m = sm_open();
    float p[SM_PATCH_FLOATS]; mu_patch_clear(p); mu_source(p, 0, SM_SRC_WHITE, 0.0f, 1.0f);
    mu_send_patch(m, 1, p);
    for (int v = 1; v <= 20; v++) mu_play_flat(m, v, 1, 1.0f, 0.0f);
    render(m, SEC(1.0));
    CHECK(mu_peak(buf, 0, SEC(1.0)) <= 0.8913f, "twenty voices of full noise never pass -1 dBFS");
    CHECK(sm_stats(m).peak_db <= -0.99f, "and the meter says so");
    sm_close(m);
}

static void test_handles(void) {
    SmMixer *m = sm_open();
    float p[SM_PATCH_FLOATS]; mu_patch_clear(p); mu_source(p, 0, SM_SRC_SINE, 440.0f, 0.5f);
    mu_send_patch(m, 1, p);
    mu_cmd(m, SM_OP_STOP, 77, 0, 0.0f);
    mu_cmd(m, SM_OP_CONTROL, 77, 0, 1.0f);
    render(m, SEC(0.1));
    CHECK(sm_stats(m).playing == 0 && sm_stats(m).refused == 0, "commands for a voice never played change and count nothing");
    mu_play_flat(m, 5, 1, 1.0f, 0.0f);
    mu_cmd(m, SM_OP_STOP, 5, 0, 0.0f);
    render(m, SEC(0.1));
    mu_cmd(m, SM_OP_CONTROL, 5, 0, 1.0f);
    render(m, SEC(0.1));
    CHECK(sm_stats(m).playing == 0 && mu_peak(buf, 0, SEC(0.1)) == 0.0f, "a control on a stopped voice changes nothing");
    mu_play_flat(m, 6, 99, 1.0f, 0.0f);
    CHECK(sm_stats(m).refused == 1, "a play of an unknown source is refused and counted");
    sm_close(m);
}

static void test_repeatable(void) {
    static float a[2 * SM_RATE], b[2 * SM_RATE];
    for (int k = 0; k < 2; k++) {
        SmMixer *m = sm_open();
        float p[SM_PATCH_FLOATS]; mu_patch_clear(p); mu_source(p, 0, SM_SRC_BROWN, 0.0f, 0.5f); mu_source(p, 1, SM_SRC_WHITE, 0.0f, 0.2f);
        p[SM_P_MOD(0)] = SM_MOD_WALK; p[SM_P_MOD(0) + 1] = 2.0f; p[SM_P_MOD(0) + 2] = SM_TGT_GAIN; p[SM_P_MOD(0) + 3] = 0.5f;
        mu_send_patch(m, 1, p);
        mu_play_flat(m, 3, 1, 1.0f, 0.2f);
        sm_mix(m, k == 0 ? a : b, SEC(1.0));
        sm_close(m);
    }
    CHECK(memcmp(a, b, sizeof(float) * 2 * SEC(1.0)) == 0, "the same commands render the same samples");
}

static void test_wav(void) {
    const char *path = "native/tests/bin/mix_test.wav";
    for (int i = 0; i < 2 * 4800; i++) buf[i] = (float)sin(i * 0.01) * 0.5f;
    CHECK(sm_wav_write(path, buf, 4800) == 1, "a WAV is written");
    FILE *f = fopen(path, "rb");
    unsigned char h[44] = { 0 };
    int got = f ? (int)fread(h, 1, 44, f) : 0;
    if (f) fclose(f);
    int rate = h[24] | h[25] << 8 | h[26] << 16 | h[27] << 24;
    int chans = h[22] | h[23] << 8;
    int bytes = h[40] | h[41] << 8 | h[42] << 16 | h[43] << 24;
    CHECK(got == 44 && memcmp(h, "RIFF", 4) == 0 && memcmp(h + 8, "WAVE", 4) == 0, "it has a RIFF WAVE header");
    CHECK(rate == 48000 && chans == 2 && bytes == 4800 * 4, "48 000 Hz, two channels, every frame");
}

static void test_fp(void) {
    const char *a = "native/tests/bin/fp_a.fp", *b = "native/tests/bin/fp_b.fp", *c = "native/tests/bin/fp_c.fp";
    static float fp[SM_FP_VALUES * 3];
    for (int i = 0; i < SM_FP_VALUES * 3; i++) fp[i] = -40.0f + (float)(i % 7);
    CHECK(sm_fp_write(a, fp, 3) == 1, "a fingerprint is written");
    fp[SM_FP_VALUES + 4] += 2.0f;
    sm_fp_write(b, fp, 3);
    sm_fp_write(c, fp, 2);
    CHECK(sm_fp_compare(a, a, 1.5f, 0.5f) == 0, "a fingerprint matches itself");
    CHECK(sm_fp_compare(a, b, 1.5f, 0.5f) == 1, "a band 2 dB off fails at 1.5 dB");
    CHECK(sm_fp_compare(a, b, 2.5f, 0.5f) == 0, "and passes at 2.5 dB");
    CHECK(sm_fp_compare(a, c, 1.5f, 0.5f) == 2, "a different window count is a shape mismatch");
    CHECK(sm_fp_compare(a, "native/tests/bin/none.fp", 1.5f, 0.5f) == 2, "a missing file is a mismatch");
    for (int i = 0; i < SM_FP_VALUES * 3; i++) fp[i] = -40.0f + (float)(i % 7) + 1.0f;
    sm_fp_write(b, fp, 3);
    CHECK(sm_fp_compare(a, b, 1.5f, 0.5f) == 1, "everything 1 dB louder fails on the total at 0.5 dB");
}

int main(void) {
    test_fp();
    test_ring();
    test_sine_level();
    test_envelope();
    test_filter();
    test_control();
    test_limiter();
    test_handles();
    test_repeatable();
    test_wav();
    if (fails == 0) printf("mix_test: ok\n");
    return fails == 0 ? 0 : 1;
}

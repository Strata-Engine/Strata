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
    for (int i = 0; i < SM_FP_VALUES * 3; i++) fp[i] = -40.0f + (float)(i % 7);
    fp[3] = -120.0f; sm_fp_write(a, fp, 3);
    fp[3] = -95.0f; sm_fp_write(b, fp, 3);
    CHECK(sm_fp_compare(a, b, 1.5f, 0.5f) == 0, "bands below -80 dB on both sides are not compared: near silence is noise");
    fp[3] = -60.0f; sm_fp_write(b, fp, 3);
    CHECK(sm_fp_compare(a, b, 1.5f, 0.5f) == 1, "but silence against sound is a difference");
}

static SmRec *mk_rec(int frames, int channels, float hz) {
    SmRec *r = (SmRec *)calloc(1, sizeof(SmRec));
    r->data = (float *)calloc((size_t)frames * channels, sizeof(float));
    r->frames = frames; r->channels = channels;
    for (int i = 0; i < frames; i++) for (int c = 0; c < channels; c++) r->data[i * channels + c] = 0.5f * (float)sin(2.0 * SM_PI * hz * i / SM_RATE);
    return r;
}

static void test_recordings(void) {
    SmMixer *m = sm_open();
    /* a play of a recording that has not arrived is refused and counted */
    mu_play_flat(m, 1, SM_REC_FIRST, 1.0f, 0.0f);
    CHECK(sm_stats(m).refused == 1, "a recording not ready is refused and counted");
    /* half a second of a 1 kHz tone, mono */
    SmRec *r = mk_rec(SM_RATE / 2, 1, 1000.0f);
    sm_rec_publish(m, SM_REC_FIRST, r);
    mu_play_flat(m, 2, SM_REC_FIRST, 1.0f, 0.0f);
    CHECK(atomic_load(&r->refs) == 1, "a voice holds its recording");
    render(m, SEC(0.25));
    CHECK(fabs(mu_db(mu_rms(buf, SEC(0.05), SEC(0.25))) - mu_db(0.5 / sqrt(2.0))) < 0.5, "a recording plays at its level");
    /* withdrawn while playing: it keeps playing to its end, then lets go */
    sm_rec_withdraw(m, SM_REC_FIRST);
    render(m, SEC(0.2));
    CHECK(mu_peak(buf, SEC(0.1), SEC(0.2)) > 0.3f, "a recording withdrawn while it plays keeps playing");
    render(m, SEC(0.2));
    CHECK(sm_stats(m).playing == 0 && atomic_load(&r->refs) == 0, "and when it ends the voice lets go of it");
    mu_play_flat(m, 3, SM_REC_FIRST, 1.0f, 0.0f);
    CHECK(sm_stats(m).refused == 2, "a withdrawn recording cannot be played again");
    CHECK(sm_blocks(m) > 0, "the mixer counts its blocks, so a loader knows when a withdrawn recording is safe to free");
    free(r->data); free(r);
    /* a stereo recording played flat keeps its two channels */
    SmRec *st = mk_rec(SM_RATE / 4, 2, 500.0f);
    for (int i = 0; i < st->frames; i++) st->data[2 * i + 1] = 0.0f;   /* left only */
    sm_rec_publish(m, SM_REC_FIRST + 1, st);
    mu_play_flat(m, 4, SM_REC_FIRST + 1, 1.0f, 0.0f);
    render(m, SEC(0.2));
    CHECK(mu_peak(buf, 0, SEC(0.2)) > 0.3f, "a stereo recording plays");
    float right = 0.0f; for (int i = 0; i < SEC(0.2); i++) if (fabsf(buf[2 * i + 1]) > right) right = fabsf(buf[2 * i + 1]);
    CHECK(right < 0.001f, "and its silent right channel stays silent");
    /* a patch with a recording source, looping */
    SmRec *lp = mk_rec(SM_RATE / 10, 1, 440.0f);
    sm_rec_publish(m, SM_REC_FIRST + 2, lp);
    float p[SM_PATCH_FLOATS]; mu_patch_clear(p);
    p[SM_P_SRC(0)] = SM_SRC_RECORDING; p[SM_P_SRC(0) + 1] = 1.0f; p[SM_P_SRC(0) + 2] = 1.0f; p[SM_P_SRC(0) + 4] = (float)(SM_REC_FIRST + 2);
    p[SM_P_LOOP] = 1.0f;
    mu_send_patch(m, 1, p);
    mu_play_flat(m, 5, 1, 1.0f, 0.0f);
    render(m, SEC(0.5));
    CHECK(mu_peak(buf, SEC(0.4), SEC(0.5)) > 0.3f, "a looping recording source is still playing after five of its lengths");
    sm_close(m);
}

static void mu_listener(SmMixer *m, float x, float y, float z, float fx, float fy, float fz) {
    SmCmd c; memset(&c, 0, sizeof c);
    c.op = SM_OP_LISTENER; c.f[0] = x; c.f[1] = y; c.f[2] = z; c.f[3] = fx; c.f[4] = fy; c.f[5] = fz; c.f[6] = 0.0f; c.f[7] = 1.0f; c.f[8] = 0.0f;
    sm_command(m, &c);
}
static void mu_play_at(SmMixer *m, int voice, int src, float gain, float priority, float x, float y, float z) {
    SmCmd c; memset(&c, 0, sizeof c);
    c.op = SM_OP_PLAY; c.voice = voice; c.a = src;
    c.f[0] = gain; c.f[1] = 1.0f; c.f[2] = priority; c.f[3] = x; c.f[4] = y; c.f[5] = z; c.f[6] = 0.0f;
    sm_command(m, &c);
}
static void mu_output(SmMixer *m, int headphones) { mu_cmd(m, SM_OP_OUTPUT, 0, headphones, 0.0f); }
static double chan_db(int ch, int from, int to) {
    double s = 0; for (int i = from; i < to; i++) s += (double)buf[2 * i + ch] * buf[2 * i + ch];
    return mu_db(sqrt(s / (to - from)));
}
static int tone_patch(SmMixer *m, int id, int kind, float hz, float level) {
    float p[SM_PATCH_FLOATS]; mu_patch_clear(p); mu_source(p, 0, kind, hz, level); mu_send_patch(m, id, p); return id;
}

static void test_sides(int headphones) {
    for (int side = -1; side <= 1; side += 2) {
        SmMixer *m = sm_open();
        mu_output(m, headphones);
        mu_listener(m, 0, 0, 0, 0, 0, -1);
        tone_patch(m, 1, SM_SRC_PINK, 0.0f, 0.5f);
        mu_play_at(m, 1, 1, 1.0f, 1.0f, 10.0f * side, 0.0f, 0.0f);
        render(m, SEC(0.5));
        double l = chan_db(0, SEC(0.1), SEC(0.5)), r = chan_db(1, SEC(0.1), SEC(0.5));
        if (headphones) {   /* a head shadows the highs: compare the 4 kHz octave, ears left and right */
            static float fp[SM_FP_VALUES * 6];
            int w = sm_fingerprint(buf, SEC(0.5), fp); l = r = 0;
            for (int i = 1; i < w; i++) { l += fp[i * SM_FP_VALUES + 7]; r += fp[i * SM_FP_VALUES + SM_FP_BANDS + 7]; }
            l /= (w - 1); r /= (w - 1);
        }
        CHECK(side > 0 ? r - l >= 6.0 : l - r >= 6.0, headphones ? "binaural: a source to one side is 6 dB louder in that ear" : "speakers: a source to one side is 6 dB louder on that side");
        sm_close(m);
    }
}

static double band_balance(int front) {   /* the 4-8 kHz octave against 500 Hz-1 kHz, both ears */
    SmMixer *m = sm_open();
    mu_output(m, 1);
    mu_listener(m, 0, 0, 0, 0, 0, -1);
    tone_patch(m, 1, SM_SRC_PINK, 0.0f, 0.5f);
    mu_play_at(m, 1, 1, 1.0f, 1.0f, 0.0f, 0.0f, front ? -5.0f : 5.0f);
    render(m, SEC(1.0));
    static float fp[SM_FP_VALUES * 12];
    int w = sm_fingerprint(buf, SEC(1.0), fp);
    double hi = 0, lo = 0;
    for (int i = 2; i < w; i++) { hi += fp[i * SM_FP_VALUES + 7] + fp[i * SM_FP_VALUES + 17]; lo += fp[i * SM_FP_VALUES + 4] + fp[i * SM_FP_VALUES + 14]; }
    sm_close(m);
    return (hi - lo) / (2.0 * (w - 2));
}

static double diffuse_db(int mode) {      /* 0 flat mono, 1 speakers, 2 headphones: 40 noise sources all round */
    SmMixer *m = sm_open();
    if (mode) mu_output(m, mode == 2);
    mu_listener(m, 0, 0, 0, 0, 0, -1);
    tone_patch(m, 1, SM_SRC_PINK, 0.0f, 0.05f);
    for (int i = 0; i < 40; i++) {
        double zf = 1.0 - 2.0 * (i + 0.5) / 40.0, rr = sqrt(1.0 - zf * zf), a = i * 2.39996323;
        if (mode == 0) mu_play_flat(m, i + 1, 1, 1.0f, 0.0f);
        else mu_play_at(m, i + 1, 1, 1.0f, 1.0f, (float)(rr * cos(a)), (float)zf, (float)(rr * sin(a)));
    }
    render(m, SEC(1.0));
    double e = mu_rms(buf, SEC(0.2), SEC(1.0));
    sm_close(m);
    return mu_db(e);
}

static void test_space(void) {
    test_sides(0);
    test_sides(1);
    CHECK(fabs(band_balance(1) - band_balance(0)) >= 3.0, "binaural: a source behind has a 4-8 kHz balance 3 dB from one in front");
    double flat = diffuse_db(0);
    CHECK(fabs(diffuse_db(1) - flat) <= 1.0, "speakers keep a diffuse field's energy within 1 dB");
    CHECK(fabs(diffuse_db(2) - flat) <= 1.0, "headphones keep a diffuse field's energy within 1 dB");

    /* distance: twice as far beyond the reference is 6 dB quieter */
    double at[2];
    for (int k = 0; k < 2; k++) {
        SmMixer *m = sm_open();
        mu_listener(m, 0, 0, 0, 0, 0, -1);
        tone_patch(m, 1, SM_SRC_SINE, 200.0f, 0.5f);
        mu_play_at(m, 1, 1, 1.0f, 1.0f, 0.0f, 0.0f, k ? -8.0f : -4.0f);
        render(m, SEC(0.5));
        at[k] = mu_db(mu_rms(buf, SEC(0.1), SEC(0.5)));
        sm_close(m);
    }
    CHECK(fabs((at[0] - at[1]) - 6.02) <= 0.5, "twice as far is 6 dB quieter");

    /* degenerate: a source at the listener, a zero facing vector */
    for (int hp = 0; hp < 2; hp++) {
        SmMixer *m = sm_open();
        mu_output(m, hp);
        mu_listener(m, 1, 2, 3, 0, 0, 0);
        tone_patch(m, 1, SM_SRC_SINE, 300.0f, 0.5f);
        mu_play_at(m, 1, 1, 1.0f, 1.0f, 1.0f, 2.0f, 3.0f);
        render(m, SEC(0.3));
        int finite = 1; for (int i = 0; i < 2 * SEC(0.3); i++) if (!isfinite(buf[i])) finite = 0;
        double lvl = mu_db(mu_rms(buf, SEC(0.1), SEC(0.3)));
        CHECK(finite && fabs(lvl - mu_db(0.5 / sqrt(2.0))) <= 3.0, hp ? "binaural: a source at the listener, facing nowhere, plays finite at its gain" : "speakers: a source at the listener, facing nowhere, plays finite at its gain");
        sm_close(m);
    }

    /* ranking: of 100 voices the 64 loudest mix; the rest are virtual and keep time */
    {
        SmMixer *m = sm_open();
        mu_listener(m, 0, 0, 0, 0, 0, -1);
        tone_patch(m, 1, SM_SRC_SINE, 100.0f, 0.1f);
        for (int v = 1; v <= 100; v++) mu_play_at(m, v, 1, (float)v / 100.0f, 1.0f, 0.0f, 0.0f, -2.0f);
        render(m, SEC(0.1));
        CHECK(sm_stats(m).playing == 100 && sm_stats(m).virtual_ == 36, "of 100 voices 64 mix and 36 are virtual");
        CHECK(sm_is_mixed(m, 100) && sm_is_mixed(m, 37) && !sm_is_mixed(m, 36) && !sm_is_mixed(m, 1), "the loudest 64 are the ones mixed");
        float jump = 0.0f;
        for (int i = 1; i < SEC(0.1); i++) { float d = fabsf(buf[2 * i] - buf[2 * i - 2]); if (d > jump) jump = d; }
        sm_close(m);
        CHECK(jump < 0.3f, "voices entering the mix fade in: no jump");
    }
    {   /* a looping recording that goes virtual comes back where it would have been */
        SmMixer *m = sm_open();
        mu_listener(m, 0, 0, 0, 0, 0, -1);
        SmRec *r = mk_rec(SM_RATE, 1, 50.0f);   /* one second */
        for (int i = 0; i < r->frames; i++) r->data[i] = (float)i / r->frames;   /* a ramp: its value is its position */
        sm_rec_publish(m, SM_REC_FIRST, r);
        float p[SM_PATCH_FLOATS]; mu_patch_clear(p);
        p[SM_P_SRC(0)] = SM_SRC_RECORDING; p[SM_P_SRC(0) + 1] = 1.0f; p[SM_P_SRC(0) + 2] = 1.0f; p[SM_P_SRC(0) + 4] = (float)SM_REC_FIRST; p[SM_P_LOOP] = 1.0f;
        mu_send_patch(m, 1, p);
        tone_patch(m, 2, SM_SRC_SINE, 100.0f, 0.01f);
        mu_play_at(m, 1, 1, 0.001f, 1.0f, 0.0f, 0.0f, -1.0f);            /* quiet: it will be outranked */
        for (int v = 2; v <= 65; v++) mu_play_at(m, v, 2, 1.0f, 1.0f, 0.0f, 0.0f, -1.0f);
        render(m, SEC(0.3));
        CHECK(!sm_is_mixed(m, 1), "the quiet loop is virtual while 64 louder voices play");
        double pos = sm_voice_position(m, 1, 0);
        CHECK(fabs(pos - SEC(0.3)) < SM_BLOCK * 2, "and its place in the recording has moved on with time");
        free(r->data); free(r);
        sm_close(m);
    }
    {   /* past 1024 tracked, the lowest-ranked go and are counted */
        SmMixer *m = sm_open();
        mu_listener(m, 0, 0, 0, 0, 0, -1);
        tone_patch(m, 1, SM_SRC_SINE, 100.0f, 0.01f);
        for (int v = 1; v <= 1100; v++) mu_play_at(m, v, 1, 1.0f, 1.0f, 0.0f, 0.0f, -2.0f);
        render(m, SM_BLOCK);
        CHECK(sm_stats(m).refused == 76 && sm_stats(m).playing == 1024, "1100 voices: 1024 tracked, 76 dropped and counted");
        sm_close(m);
    }
}

static void mu_occlude(SmMixer *m, int voice, float gain, float cutoff) {
    SmCmd c; memset(&c, 0, sizeof c); c.op = SM_OP_OCCLUDE; c.voice = voice; c.f[0] = gain; c.f[1] = cutoff; sm_command(m, &c);
}
static void test_occlusion(void) {
    SmMixer *m = sm_open();
    mu_listener(m, 0, 0, 0, 0, 0, -1);
    tone_patch(m, 1, SM_SRC_SINE, 100.0f, 0.5f);
    mu_play_at(m, 1, 1, 1.0f, 1.0f, 0.0f, 0.0f, -1.0f);
    render(m, SEC(0.1));
    double before = mu_db(mu_rms(buf, SEC(0.05), SEC(0.1)));
    mu_occlude(m, 1, 0.01f, 400.0f);
    render(m, SEC(0.3));
    double soon = mu_db(mu_rms(buf, 0, SEC(0.02))), later = mu_db(mu_rms(buf, SEC(0.15), SEC(0.3)));
    CHECK(before - soon < 6.0, "muffling glides: 20 ms after it is set the level has not dropped 6 dB");
    CHECK(before - later > 35.0, "and after 150 ms it has dropped by the 40 dB asked");
    float jump = 0.0f; for (int i = 1; i < SEC(0.3); i++) { float d = fabsf(buf[2 * i] - buf[2 * i - 2]); if (d > jump) jump = d; }
    CHECK(jump < 0.02f, "with no jump on the way");
    sm_close(m);
    /* an occluded voice ranks below a clear one of the same level */
    m = sm_open();
    mu_listener(m, 0, 0, 0, 0, 0, -1);
    tone_patch(m, 1, SM_SRC_SINE, 100.0f, 0.01f);
    for (int v = 1; v <= 65; v++) mu_play_at(m, v, 1, 1.0f, 1.0f, 0.0f, 0.0f, -2.0f);
    mu_occlude(m, 1, 0.1f, 2000.0f);
    render(m, SM_BLOCK * 2);
    CHECK(!sm_is_mixed(m, 1) && sm_is_mixed(m, 65), "the muffled voice is the one left out");
    sm_close(m);
}

static void mu_environ(SmMixer *m, float decay, float damping, float wet, int nrefl, const float *refl) {
    SmCmd c; memset(&c, 0, sizeof c); c.op = SM_OP_ENVIRON; c.a = 0;
    c.f[0] = decay; c.f[1] = damping; c.f[2] = wet; c.f[3] = (float)nrefl;
    sm_command(m, &c);
    for (int r = 0; r * 2 < nrefl; r++) {
        memset(&c, 0, sizeof c); c.op = SM_OP_ENVIRON; c.a = r + 1;
        for (int k = 0; k < 10 && r * 10 + k < nrefl * 5; k++) c.f[k] = refl[r * 10 + k];
        sm_command(m, &c);
    }
}
/* RT60 from a Schroeder backward integration of the output's energy, between -5 and -25 dB */
static double rt60(const float *st, int from, int to) {
    static double e[SM_RATE * 5];
    double acc = 0.0;
    for (int i = to - 1; i >= from; i--) { acc += (double)st[2 * i] * st[2 * i] + (double)st[2 * i + 1] * st[2 * i + 1]; e[i - from] = acc; }
    double top = e[0]; int a = -1, b = -1;
    for (int i = 0; i < to - from; i++) {
        double db = 10.0 * log10(e[i] / top + 1e-30);
        if (a < 0 && db <= -5.0) a = i;
        if (b < 0 && db <= -25.0) { b = i; break; }
    }
    return a < 0 || b < 0 ? -1.0 : 3.0 * (double)(b - a) / SM_RATE;
}
static int click_patch(SmMixer *m, int id) {
    float p[SM_PATCH_FLOATS]; mu_patch_clear(p); mu_source(p, 0, SM_SRC_WHITE, 0.0f, 0.8f);
    p[SM_P_HOLD] = 0.004f; p[SM_P_ENV(0) + 3] = 0.001f; mu_send_patch(m, id, p); return id;
}
static void test_reverb(void) {
    static float tail[2 * SM_RATE * 4];
    /* a click into a space that rings for 2 s: the tail decays 60 dB in 2 s, within 10% */
    SmMixer *m = sm_open();
    mu_listener(m, 0, 0, 0, 0, 0, -1);
    mu_environ(m, 2.0f, 0.0f, 0.6f, 0, NULL);
    render(m, SEC(0.2));                                  /* let the space settle in */
    click_patch(m, 1);
    mu_play_at(m, 1, 1, 1.0f, 1.0f, 0.0f, 0.0f, -10.0f);
    sm_mix(m, tail, SEC(3.5));
    double t = rt60(tail, SEC(0.05), SEC(3.5));
    CHECK(t > 1.8 && t < 2.2, "an impulse into a 2.0 s space decays in 2.0 s, within 10%");
    sm_close(m);
    /* damping: the highs die sooner than the lows */
    m = sm_open();
    mu_listener(m, 0, 0, 0, 0, 0, -1);
    mu_environ(m, 2.0f, 0.8f, 0.6f, 0, NULL);
    render(m, SEC(0.2));
    click_patch(m, 1);
    mu_play_at(m, 1, 1, 1.0f, 1.0f, 0.0f, 0.0f, -10.0f);
    sm_mix(m, tail, SEC(2.0));
    static float fp[SM_FP_VALUES * 24];
    sm_fingerprint(tail, SEC(2.0), fp);
    double lo = fp[1 * SM_FP_VALUES + 4] - fp[5 * SM_FP_VALUES + 4], hi = fp[1 * SM_FP_VALUES + 8] - fp[5 * SM_FP_VALUES + 8];
    CHECK(hi > lo + 3.0, "a damped space's 8 kHz tail falls faster than its 500 Hz tail");
    sm_close(m);
    /* a change of space glides: no click */
    m = sm_open();
    mu_listener(m, 0, 0, 0, 0, 0, -1);
    tone_patch(m, 1, SM_SRC_SINE, 200.0f, 0.3f);
    mu_environ(m, 0.3f, 0.2f, 0.0f, 0, NULL);
    mu_play_at(m, 1, 1, 1.0f, 1.0f, 0.0f, 0.0f, -3.0f);
    render(m, SEC(0.3));
    float refl[5] = { 0.01f, -1.0f, 0.0f, 0.0f, 0.5f };
    mu_environ(m, 4.0f, 0.2f, 0.6f, 1, refl);
    render(m, SEC(0.5));
    float jump = 0.0f; for (int i = 1; i < SEC(0.5); i++) { float d = fabsf(buf[2 * i] - buf[2 * i - 2]); if (d > jump) jump = d; }
    CHECK(jump < 0.03f, "a new space glides in: no jump");
    sm_close(m);
    /* a reflection from the left arrives on the left, after its delay */
    m = sm_open();
    mu_listener(m, 0, 0, 0, 0, 0, -1);
    float left[5] = { 0.0117f, -1.0f, 0.0f, 0.0f, 0.8f };
    mu_environ(m, 0.1f, 0.0f, 0.6f, 1, left);
    render(m, SEC(0.2));
    click_patch(m, 1);
    mu_play_at(m, 1, 1, 1.0f, 1.0f, 0.0f, 0.0f, -10.0f);
    sm_mix(m, tail, SEC(0.1));
    double el = 0, er = 0;
    for (int i = SEC(0.0117) + 30; i < SEC(0.0117) + 260; i++) { el += tail[2 * i] * tail[2 * i]; er += tail[2 * i + 1] * tail[2 * i + 1]; }
    CHECK(el > 2.0 * er, "the reflection from the left wall is louder on the left");
    sm_close(m);
}

static int all_finite(const float *st, int frames) { for (int i = 0; i < 2 * frames; i++) if (!isfinite(st[i])) return 0; return 1; }
static void test_review(void) {
    /* a NaN gain, once, must not poison the echo or the output for good */
    SmMixer *m = sm_open();
    mu_listener(m, 0, 0, 0, 0, 0, -1);
    mu_environ(m, 2.0f, 0.2f, 0.5f, 0, NULL);
    tone_patch(m, 1, SM_SRC_SINE, 300.0f, 0.3f);
    mu_play_at(m, 1, 1, NAN, 1.0f, 0.0f, 0.0f, -3.0f);
    render(m, SEC(0.1));
    mu_cmd(m, SM_OP_STOP, 1, 0, 0.0f);
    mu_play_at(m, 2, 1, 1.0f, 1.0f, 0.0f, 0.0f, -3.0f);
    render(m, SEC(1.0));
    CHECK(all_finite(buf, SEC(1.0)), "a NaN gain once does not leave the output non-finite");
    float bad[5] = { NAN, NAN, NAN, NAN, NAN };
    mu_environ(m, NAN, NAN, NAN, 1, bad);
    render(m, SEC(0.2));
    mu_environ(m, 1.0f, 0.2f, 0.5f, 0, NULL);
    render(m, SEC(0.5));
    CHECK(all_finite(buf, SEC(0.5)), "a NaN space, then a good one: finite");
    mu_play_at(m, 3, 1, 1.0f, 1.0f, INFINITY, 0.0f, 0.0f);
    mu_cmd(m, SM_OP_GROUP, 0, 0, NAN);
    mu_occlude(m, 2, NAN, NAN);
    render(m, SEC(0.3));
    CHECK(all_finite(buf, SEC(0.3)), "an infinite position, a NaN group gain and a NaN occlusion: finite");
    sm_close(m);

    /* a placed voice starts at its level: no fade-in to blunt an attack */
    for (int hp = 0; hp < 2; hp++) {
        m = sm_open();
        mu_output(m, hp);
        mu_listener(m, 0, 0, 0, 0, 0, -1);
        tone_patch(m, 1, SM_SRC_SINE, 1000.0f, 0.5f);   /* inside the band a measured head shapes */
        mu_play_at(m, 1, 1, 1.0f, 1.0f, 0.0f, 0.0f, -1.0f);
        render(m, SEC(0.1));
        /* the head's filters delay sound by a few dozen samples: look just past that, still inside the first 3 ms */
        float early = hp ? mu_peak(buf, 48, 144) : mu_peak(buf, 0, 48), later = mu_peak(buf, SEC(0.05), SEC(0.1));
        CHECK(early > 0.6f * later, hp ? "binaural: a placed voice is at its level within its first millisecond" : "speakers: a placed voice is at its level within its first millisecond");
        sm_close(m);
    }

    /* a group's gain glides within a block: no step */
    m = sm_open();
    tone_patch(m, 1, SM_SRC_SINE, 50.0f, 0.8f);
    mu_play_flat(m, 1, 1, 1.0f, -1.0f);
    render(m, SEC(0.1));
    mu_cmd(m, SM_OP_GROUP, 0, 0, 0.3f);
    float jump = 0.0f, last = buf[2 * (SEC(0.1) - 1)];
    for (int b = 0; b < 40; b++) {   /* a device asks block by block: the step would fall between them */
        render(m, SM_BLOCK);
        for (int i = 0; i < SM_BLOCK; i++) { float d = fabsf(buf[2 * i] - last); if (d > jump) jump = d; last = buf[2 * i]; }
    }
    CHECK(jump < 0.02f, "turning a group down glides: no step between blocks");
    sm_close(m);

    /* a short looping recording played fast stays inside its buffer */
    m = sm_open();
    SmRec *r = mk_rec(10, 1, 1000.0f);
    sm_rec_publish(m, SM_REC_FIRST, r);
    float p[SM_PATCH_FLOATS]; mu_patch_clear(p);
    p[SM_P_SRC(0)] = SM_SRC_RECORDING; p[SM_P_SRC(0) + 1] = 30.0f; p[SM_P_SRC(0) + 2] = 1.0f; p[SM_P_SRC(0) + 4] = (float)SM_REC_FIRST; p[SM_P_LOOP] = 1.0f;
    mu_send_patch(m, 1, p);
    mu_play_flat(m, 1, 1, 1.0f, 0.0f);
    int inside = 1;
    for (int b = 0; b < 20; b++) { render(m, SM_BLOCK); double pos = sm_voice_position(m, 1, 0); if (pos < 0.0 || pos >= 10.0) inside = 0; }
    CHECK(inside && all_finite(buf, SM_BLOCK), "a 10-frame loop at rate 30 never reads outside its 10 frames");
    sm_close(m);
    free(r->data); free(r);

    /* an echo left behind when the space goes dry does not come back with the next one */
    m = sm_open();
    mu_listener(m, 0, 0, 0, 0, 0, -1);
    mu_environ(m, 4.0f, 0.0f, 0.6f, 0, NULL);
    render(m, SEC(0.2));
    click_patch(m, 1);
    mu_play_at(m, 1, 1, 1.0f, 1.0f, 0.0f, 0.0f, -10.0f);
    render(m, SEC(0.3));
    mu_environ(m, 4.0f, 0.0f, 0.0f, 0, NULL);
    render(m, SEC(1.0));
    mu_environ(m, 4.0f, 0.0f, 0.6f, 0, NULL);
    render(m, SEC(0.5));
    CHECK(mu_db(mu_rms(buf, 0, SEC(0.5))) < -80.0, "a new space starts silent: the old echo is gone");
    sm_close(m);

    /* a batch pushed at once is published at once, and a full ring takes what fits */
    static SmCmd store[16];
    SmRing rg; sm_ring_init(&rg, store, 16);
    SmCmd batch[20]; memset(batch, 0, sizeof batch);
    for (int i = 0; i < 20; i++) batch[i].voice = i;
    CHECK(sm_ring_push_n(&rg, batch, 20) == 16, "a ring of 16 takes 16 of a batch of 20");
    SmCmd o; int ord = 1; for (int i = 0; i < 16; i++) if (!sm_ring_pop(&rg, &o) || o.voice != i) ord = 0;
    CHECK(ord, "in order");
}

static void test_measured_ears(void) {
    CHECK(strcmp(SH_MODEL, "MIT KEMAR") == 0, "the binaural filters come from the MIT KEMAR measurements");
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
    test_recordings();
    test_space();
    test_occlusion();
    test_reverb();
    test_review();
    test_measured_ears();
    if (fails == 0) printf("mix_test: ok\n");
    return fails == 0 ? 0 : 1;
}

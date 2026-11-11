/* mix_demo DIR — one WAV per building block of a patch, for listening:
 *   cc -O2 -Inative -o /tmp/mix_demo native/tests/mix_demo.c && /tmp/mix_demo /tmp/strata-mix */
#include <stdio.h>
#include "tests/mix_util.h"

static float out[2 * SM_RATE * 4];

static void one(const char *dir, const char *name, const float *p, float seconds, float stop_at) {
    SmMixer *m = sm_open();
    mu_send_patch(m, 1, p);
    mu_play_flat(m, 1, 1, 0.8f, 0.0f);
    int frames = (int)(seconds * SM_RATE), at = (int)(stop_at * SM_RATE);
    sm_mix(m, out, at);
    mu_cmd(m, SM_OP_STOP, 1, 0, -1.0f);
    sm_mix(m, out + 2 * at, frames - at);
    char path[512]; snprintf(path, sizeof path, "%s/%s.wav", dir, name);
    printf("%s\n", sm_wav_write(path, out, frames) ? path : "(failed)");
    sm_close(m);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: mix_demo DIR\n"); return 2; }
    const char *dir = argv[1];
    char cmd[600]; snprintf(cmd, sizeof cmd, "mkdir -p '%s'", dir); if (system(cmd) != 0) return 1;
    static const char *names[] = { "", "sine", "triangle", "saw", "square", "white", "pink", "brown" };
    float p[SM_PATCH_FLOATS];
    for (int k = 1; k <= 7; k++) { mu_patch_clear(p); mu_source(p, 0, k, 220.0f, 0.5f); mu_env(p, 0, 0.02f, 0.0f, 1.0f, 0.3f, 0.0f); one(dir, names[k], p, 1.5f, 1.2f); }
    static const char *fn[] = { "", "lowpass", "highpass", "bandpass", "peak" };
    for (int k = 1; k <= 4; k++) {   /* a filter on saw + noise, its cutoff swept by an LFO */
        mu_patch_clear(p); mu_source(p, 0, SM_SRC_SAW, 110.0f, 0.4f); mu_source(p, 1, SM_SRC_PINK, 0.0f, 0.3f);
        mu_filter(p, 0, k, 800.0f, 2.0f);
        p[SM_P_MOD(0)] = SM_MOD_LFO; p[SM_P_MOD(0) + 1] = 0.5f; p[SM_P_MOD(0) + 2] = SM_TGT_CUTOFF; p[SM_P_MOD(0) + 3] = 2.5f;
        char nm[32]; snprintf(nm, sizeof nm, "filter-%s", fn[k]); one(dir, nm, p, 4.0f, 3.7f);
    }
    mu_patch_clear(p); mu_source(p, 0, SM_SRC_SINE, 330.0f, 0.5f);   /* vibrato */
    p[SM_P_MOD(0)] = SM_MOD_LFO; p[SM_P_MOD(0) + 1] = 5.0f; p[SM_P_MOD(0) + 2] = SM_TGT_PITCH; p[SM_P_MOD(0) + 3] = 0.5f;
    one(dir, "mod-lfo-pitch", p, 3.0f, 2.7f);
    mu_patch_clear(p); mu_source(p, 0, SM_SRC_BROWN, 0.0f, 0.8f);    /* gusting noise */
    mu_filter(p, 0, SM_FLT_BAND, 500.0f, 0.8f);
    p[SM_P_MOD(0)] = SM_MOD_WALK; p[SM_P_MOD(0) + 1] = 0.7f; p[SM_P_MOD(0) + 2] = SM_TGT_GAIN; p[SM_P_MOD(0) + 3] = 0.8f;
    p[SM_P_MOD(1)] = SM_MOD_WALK; p[SM_P_MOD(1) + 1] = 0.4f; p[SM_P_MOD(1) + 2] = SM_TGT_CUTOFF; p[SM_P_MOD(1) + 3] = 1.5f;
    one(dir, "mod-walk-gusts", p, 4.0f, 3.5f);
    mu_patch_clear(p); mu_source(p, 0, SM_SRC_SAW, 110.0f, 0.5f);    /* a plucked envelope with a filter sweep */
    mu_filter(p, 0, SM_FLT_LOW, 300.0f, 1.5f);
    mu_env(p, 0, 0.005f, 0.4f, 0.2f, 0.4f, 0.0f); mu_env(p, 1, 0.0f, 0.0f, 0.0f, 0.0f, 4.0f);
    one(dir, "envelope-pluck", p, 1.5f, 0.6f);
    return 0;
}

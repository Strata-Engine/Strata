/* wav_fp in.wav out.fp — the fingerprint of a 16-bit stereo 48 kHz WAV
 * (as strata_audio.c writes them), for scripts/audio_golden.sh. Prints the
 * peak and RMS. Exit 2 on a file it cannot read. */
#include "strata_mix.h"

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: wav_fp in.wav out.fp\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "wav_fp: cannot read %s\n", argv[1]); return 2; }
    unsigned char h[44];
    if (fread(h, 1, 44, f) != 44 || memcmp(h, "RIFF", 4) != 0 || h[22] != 2 || (h[24] | h[25] << 8 | h[26] << 16) != SM_RATE) { fprintf(stderr, "wav_fp: %s is not 48 kHz stereo\n", argv[1]); fclose(f); return 2; }
    uint32_t bytes = (uint32_t)h[40] | (uint32_t)h[41] << 8 | (uint32_t)h[42] << 16 | (uint32_t)h[43] << 24;
    int frames = (int)(bytes / 4);
    float *st = malloc(sizeof(float) * 2 * (size_t)(frames + 1));
    int16_t *raw = malloc(bytes + 4);
    if (!st || !raw || fread(raw, 1, bytes, f) != bytes) { fprintf(stderr, "wav_fp: short file\n"); fclose(f); return 2; }
    fclose(f);
    double sum = 0; float peak = 0;
    for (int i = 0; i < 2 * frames; i++) { st[i] = raw[i] / 32767.0f; sum += (double)st[i] * st[i]; if (fabsf(st[i]) > peak) peak = fabsf(st[i]); }
    int windows = frames / SM_FP_WINDOW;
    float *fp = malloc(sizeof(float) * SM_FP_VALUES * (size_t)(windows + 1));
    sm_fingerprint(st, frames, fp);
    if (!sm_fp_write(argv[2], fp, windows)) { fprintf(stderr, "wav_fp: cannot write %s\n", argv[2]); return 2; }
    printf("%s: %d frames, %d windows, peak %.1f dB, rms %.1f dB\n", argv[1], frames, windows,
           20.0 * log10(peak > 1e-6 ? peak : 1e-6), 10.0 * log10(frames ? sum / (2.0 * frames) + 1e-12 : 1e-12));
    return 0;
}

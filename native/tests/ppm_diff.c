/* ppm_diff A.ppm B.ppm [LEVEL [PERCENT]] -- compare two binary PPM (P6) images.
 * Exit 0 when at most PERCENT% (default 0.5) of pixels differ by more than
 * LEVEL (default 8) in any channel,
 * 1 when more do, 2 when the files cannot be read or their sizes differ.
 * Prints the differing fraction and the largest channel difference. */
#include <stdio.h>
#include <stdlib.h>

static unsigned char *load(const char *path, int *w, int *h) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    int maxv = 0;
    if (fscanf(f, "P6 %d %d %d", w, h, &maxv) != 3 || maxv != 255 || *w <= 0 || *h <= 0) { fclose(f); return NULL; }
    fgetc(f);                                   /* the single whitespace after the header */
    size_t n = (size_t)*w * (size_t)*h * 3;
    unsigned char *px = malloc(n);
    if (!px || fread(px, 1, n, f) != n) { fclose(f); free(px); return NULL; }
    fclose(f);
    return px;
}

int main(int argc, char **argv) {
    if (argc < 3 || argc > 5) { fprintf(stderr, "usage: ppm_diff A.ppm B.ppm [LEVEL [PERCENT]]\n"); return 2; }
    int level = argc > 3 ? atoi(argv[3]) : 8;
    double allowed = (argc > 4 ? atof(argv[4]) : 0.5) / 100.0;
    int wa, ha, wb, hb;
    unsigned char *a = load(argv[1], &wa, &ha), *b = load(argv[2], &wb, &hb);
    if (!a || !b) { fprintf(stderr, "ppm_diff: cannot read %s\n", a ? argv[2] : argv[1]); return 2; }
    if (wa != wb || ha != hb) { fprintf(stderr, "ppm_diff: sizes differ (%dx%d against %dx%d)\n", wa, ha, wb, hb); return 2; }
    size_t pixels = (size_t)wa * (size_t)ha, bad = 0;
    int worst = 0;
    for (size_t i = 0; i < pixels; i++) {
        int over = 0;
        for (int c = 0; c < 3; c++) {
            int d = abs((int)a[i * 3 + c] - (int)b[i * 3 + c]);
            if (d > worst) worst = d;
            if (d > level) over = 1;
        }
        bad += (size_t)over;
    }
    double frac = (double)bad / (double)pixels;
    printf("%.4f%% of pixels differ, largest difference %d\n", frac * 100.0, worst);
    free(a); free(b);
    return frac <= allowed ? 0 : 1;
}

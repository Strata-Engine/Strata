/* ppm_mean IMAGE.ppm X0 Y0 X1 Y1 MIN -- the mean channel value (0..255) of a
 * binary PPM (P6) over the box [X0, X1) x [Y0, Y1) (pixels, y down).
 * Exit 0 when it is at least MIN, 1 when below, 2 when the file cannot be
 * read or the box is not inside the image. Prints the mean. A region probe
 * for golden.sh: a light a scene must show, in numbers. */
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc != 7) { fprintf(stderr, "usage: ppm_mean IMAGE.ppm X0 Y0 X1 Y1 MIN\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "ppm_mean: cannot read %s\n", argv[1]); return 2; }
    int w = 0, h = 0, maxv = 0;
    if (fscanf(f, "P6 %d %d %d", &w, &h, &maxv) != 3 || maxv != 255 || w <= 0 || h <= 0) { fclose(f); fprintf(stderr, "ppm_mean: not a P6 image\n"); return 2; }
    fgetc(f);
    size_t n = (size_t)w * (size_t)h * 3;
    unsigned char *px = malloc(n);
    if (!px || fread(px, 1, n, f) != n) { fclose(f); free(px); fprintf(stderr, "ppm_mean: short image\n"); return 2; }
    fclose(f);
    int x0 = atoi(argv[2]), y0 = atoi(argv[3]), x1 = atoi(argv[4]), y1 = atoi(argv[5]);
    double min = atof(argv[6]);
    if (x0 < 0 || y0 < 0 || x1 > w || y1 > h || x0 >= x1 || y0 >= y1) { free(px); fprintf(stderr, "ppm_mean: box outside the image\n"); return 2; }
    double sum = 0;
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) for (int c = 0; c < 3; c++) sum += px[((size_t)y * (size_t)w + (size_t)x) * 3 + (size_t)c];
    free(px);
    double mean = sum / ((double)(x1 - x0) * (double)(y1 - y0) * 3.0);
    printf("mean %.1f over (%d, %d)-(%d, %d), at least %.1f wanted\n", mean, x0, y0, x1, y1, min);
    return mean >= min ? 0 : 1;
}

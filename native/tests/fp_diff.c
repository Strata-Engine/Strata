/* fp_diff stored.fp new.fp [band_db=1.5] [total_db=0.5]
 * Exit 0 within tolerance, 1 outside it, 2 on a shape mismatch or a file
 * that cannot be read. Used by scripts/audio_golden.sh. */
#include <stdlib.h>
#include "strata_mix.h"

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: fp_diff stored.fp new.fp [band_db] [total_db]\n"); return 2; }
    float band = argc > 3 ? (float)atof(argv[3]) : 1.5f, total = argc > 4 ? (float)atof(argv[4]) : 0.5f;
    return sm_fp_compare(argv[1], argv[2], band, total);
}

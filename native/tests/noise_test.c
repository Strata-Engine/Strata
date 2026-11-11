/* The noise substrate: a filled field equals single samples, the origin
 * matters, a too-small array is refused and left alone. The test includes the
 * implementation file so it compiles with the verify.sh command line alone. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "strata_noise.c"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL: %s\n", what); fails++; } } while (0)

/* A fake March native float array: 32-byte header, count at +16. */
static void *make_arr(int64_t n, float fill) {
    char *a = calloc(1, 32 + (size_t)n * sizeof(float));
    *(int64_t *)(a + 16) = n;
    for (int64_t i = 0; i < n; i++) ((float *)(a + 32))[i] = fill;
    return a;
}

int main(void) {
    for (int kind = 0; kind < 4; kind++) {
        void *a = make_arr(16, 9.0f);
        int64_t n = sn_field2(kind, 3, 2.0, 0.5, 0, 1, 0.0, 0.01, 10.0, -20.0, 7.0, 4, 4, a);
        CHECK(n == 16, "field2 returns w * d");
        int same = 1;
        for (int j = 0; j < 4; j++)
            for (int i = 0; i < 4; i++) {
                double s = sn_sample2(kind, 3, 2.0, 0.5, 0, 1, 0.0, 0.01, 10.0 + i * 7.0, -20.0 + j * 7.0);
                if (((float *)narr_data(a))[i + 4 * j] != (float)s) same = 0;
            }
        CHECK(same, "field2 equals sample2 at each point");
        free(a);
    }

    /* fractal and warp variants agree with their samples too */
    {
        void *a = make_arr(16, 0.0f);
        sn_field2(0, 4, 2.0, 0.5, 2, 5.0, 11, 0.02, 3.0, 4.0, 1.5, 4, 4, a);
        int same = 1, in = 1;
        for (int j = 0; j < 4; j++)
            for (int i = 0; i < 4; i++) {
                float v = ((float *)narr_data(a))[i + 4 * j];
                if (v != (float)sn_sample2(0, 4, 2.0, 0.5, 2, 5.0, 11, 0.02, 3.0 + i * 1.5, 4.0 + j * 1.5)) same = 0;
                if (v < 0.0f || v > 1.0f) in = 0;
            }
        CHECK(same, "ridged warped field equals samples");
        CHECK(in, "ridged output is in [0, 1]");
        free(a);
    }

    {
        void *a = make_arr(16, 0.0f), *b = make_arr(16, 0.0f);
        sn_field2(0, 1, 2.0, 0.5, 0, 0.0, 5, 0.01, -100000.0, 0.0, 1.0, 4, 4, a);
        sn_field2(0, 1, 2.0, 0.5, 0, 0.0, 5, 0.01, 0.0, 0.0, 1.0, 4, 4, b);
        CHECK(memcmp(narr_data(a), narr_data(b), 16 * sizeof(float)) != 0, "a far origin differs from the near one");
        free(a); free(b);
    }

    {
        void *a = make_arr(1000, 7.0f);
        int64_t n = sn_field3(0, 1, 2.0, 0.5, 0, 0.0, 5, 0.01, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 32, 32, 32, a);
        int untouched = 1;
        for (int i = 0; i < 1000; i++) if (((float *)narr_data(a))[i] != 7.0f) untouched = 0;
        CHECK(n == 0, "field3 with a short array returns 0");
        CHECK(untouched, "field3 with a short array writes nothing");
        free(a);

        a = make_arr(8, 7.0f);
        CHECK(sn_field2(0, 1, 2.0, 0.5, 0, 0.0, 5, 0.01, 0.0, 0.0, 1.0, 4, 4, a) == 0, "field2 with a short array returns 0");
        free(a);
    }

    {
        void *a = make_arr(24, 0.0f);
        int64_t n = sn_field3(1, 2, 2.0, 0.5, 1, 0.0, 5, 0.01, 1.0, 2.0, 3.0, 2.0, 3.0, 4.0, 2, 3, 4, a);
        CHECK(n == 24, "field3 returns w * h * d");
        int same = 1;
        for (int k = 0; k < 4; k++) for (int j = 0; j < 3; j++) for (int i = 0; i < 2; i++) {
            double s = sn_sample3(1, 2, 2.0, 0.5, 1, 0.0, 5, 0.01, 1.0 + i * 2.0, 2.0 + j * 3.0, 3.0 + k * 4.0);
            if (((float *)narr_data(a))[i + 2 * (j + 3 * k)] != (float)s) same = 0;
        }
        CHECK(same, "field3 equals sample3 at each point");
        free(a);
    }

    if (fails) { printf("%d failed\n", fails); return 1; }
    printf("noise_test: ok\n");
    return 0;
}

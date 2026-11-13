/* The C of stage_thermal.march: Strata.Thermal.relax on a 66 x 66 patch, 4
 * passes, the same order of float operations (doubles from float arrays). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define N 66
#define CNT (N * N)

static inline double excess(const float *h, int k, int q, double t) {
    double e = (double)h[k] - (double)h[q] - t;
    return e > 0.0 ? e : 0.0;
}
static inline double fmax2(double p, double q) { return p > q ? p : q; }

__attribute__((noinline)) static void relax(const float *h0, double step, const float *talus, int passes, float *h) {
    float drops[CNT], d[CNT];
    memcpy(h, h0, sizeof(float) * CNT);
    for (int k = 0; k < CNT; k++) drops[k] = (float)(step * tan((double)talus[k] * 0.017453292519943295));
    for (int p = 0; p < passes; p++) {
        memset(d, 0, sizeof d);
        for (int k = 0; k < CNT; k++) {
            int i = k % N, j = k / N;
            double t = drops[k];
            double el = i > 0 ? excess(h, k, k - 1, t) : 0.0, er = i < N - 1 ? excess(h, k, k + 1, t) : 0.0;
            double eu = j > 0 ? excess(h, k, k - N, t) : 0.0, ed = j < N - 1 ? excess(h, k, k + N, t) : 0.0;
            double total = el + er + eu + ed;
            if (total <= 0.0) continue;
            double most = fmax2(fmax2(el, er), fmax2(eu, ed));
            double share = 0.5 * most / total;
            d[k] = (float)((double)d[k] - 0.5 * most);
            if (share * el > 0.0) d[k - 1] = (float)((double)d[k - 1] + share * el);
            if (share * er > 0.0) d[k + 1] = (float)((double)d[k + 1] + share * er);
            if (share * eu > 0.0) d[k - N] = (float)((double)d[k - N] + share * eu);
            if (share * ed > 0.0) d[k + N] = (float)((double)d[k + N] + share * ed);
        }
        for (int k = 0; k < CNT; k++) h[k] = (float)((double)h[k] + (double)d[k]);
    }
}

int main(void) {
    int reps = getenv("REPS") ? atoi(getenv("REPS")) : 1;
    static float h[CNT], tal[CNT], r[CNT];
    for (int k = 0; k < CNT; k++) {
        h[k] = (float)(50.0 + 3.0 * sin((double)(k % N) * 0.35) * cos((double)(k / N) * 0.27));
        tal[k] = (float)(40.0 + 15.0 * sin((double)(k % N) * 0.11 + (double)(k / N) * 0.07));
    }
    double acc = 0.0;
    for (int n = reps; n > 0; n--) { relax(h, 1.0, tal, 4, r); acc += r[2000 + n % 7]; }
    printf("%f\n", acc);
    relax(h, 1.0, tal, 4, r);
    double s = 0.0;
    for (int k = 0; k < CNT; k++) s += (double)r[k] * (double)(k % 7);
    printf("%f\n", s);
    return 0;
}

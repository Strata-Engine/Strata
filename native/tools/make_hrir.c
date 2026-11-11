/* make_hrir — write native/strata_hrir.h: the binaural decoder's filters for
 * a third-order ambisonic bus (16 channels, ACN order, SN3D), 128 taps at
 * 48 kHz, for the LEFT ear; the right ear is the mirror (channels with m < 0
 * change sign), so the mixer computes both ears from one set.
 *
 * Given a directory, the head-related responses are the MIT KEMAR
 * measurements in it (native/hrir/kemar: Gardner and Martin, MIT Media Lab,
 * 1994; see native/hrir/README.md): each virtual speaker takes the nearest
 * measured direction, resampled from 44.1 to 48 kHz. Without one they come
 * from a computed spherical-head model: an interaural delay round a sphere
 * (Woodworth), a head shadow (Brown and Duda's one-pole one-zero), a pinna
 * notch that rises with elevation, and a dulling of the highs from behind.
 *
 *   cc -O2 -o /tmp/make_hrir native/tools/make_hrir.c -lm && /tmp/make_hrir native/hrir/kemar > native/strata_hrir.h
  * then native/stamp_audio.sh, so forge rebuilds the shim (GAPS S14). */
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>

#define RATE 48000.0
#define TAPS 128
#define NFFT 256
#define DIRS 512
#define CH 16
static const double PI = 3.14159265358979323846;

/* SN3D real spherical harmonics to order 3, ACN order; X front, Y left, Z up */
static void sh(double X, double Y, double Z, double *o) {
    const double s3 = sqrt(3.0), s15 = sqrt(15.0), s58 = sqrt(5.0 / 8.0), s38 = sqrt(3.0 / 8.0);
    o[0] = 1.0; o[1] = Y; o[2] = Z; o[3] = X;
    o[4] = s3 * X * Y; o[5] = s3 * Y * Z; o[6] = 0.5 * (3 * Z * Z - 1); o[7] = s3 * X * Z; o[8] = 0.5 * s3 * (X * X - Y * Y);
    o[9] = s58 * Y * (3 * X * X - Y * Y); o[10] = s15 * X * Y * Z; o[11] = s38 * Y * (5 * Z * Z - 1); o[12] = 0.5 * Z * (5 * Z * Z - 3);
    o[13] = s38 * X * (5 * Z * Z - 1); o[14] = 0.5 * s15 * Z * (X * X - Y * Y); o[15] = s58 * X * (X * X - 3 * Y * Y);
}
static int order_of(int k) { return k < 1 ? 0 : k < 4 ? 1 : k < 9 ? 2 : 3; }

/* the left ear's response to a source in direction (X, Y, Z), as TAPS taps */
static void model(double X, double Y, double Z, double *h) {
    const double a = 0.0875, c = 343.0, w0 = c / a;
    double cth = Y > 1 ? 1 : (Y < -1 ? -1 : Y), th = acos(cth);          /* angle from the left ear's axis */
    double path = th < PI / 2 ? a * (1.0 - cth) : a * (th - PI / 2 + 1.0);
    double tau = path / c + 24.0 / RATE;   /* headroom before the onset for the equaliser's zero-phase spread */
    double alpha = 1.05 + 0.95 * cos(th * 180.0 / 150.0);
    double back = X < 0 ? -X : 0.0;
    double fn = 7000.0 + 3500.0 * Z, depth = 0.55 * (1.0 - 0.4 * fabs(Z));
    double re[NFFT], im[NFFT];
    for (int b = 0; b <= NFFT / 2; b++) {
        double f = b * RATE / NFFT, w = 2 * PI * f;
        /* head shadow: (1 + j alpha w / 2w0) / (1 + j w / 2w0) */
        double nr = 1.0, ni = alpha * w / (2 * w0), dr = 1.0, di = w / (2 * w0);
        double den = dr * dr + di * di, hr = (nr * dr + ni * di) / den, hi = (ni * dr - nr * di) / den;
        double g = 1.0 - depth * exp(-pow((f - fn) / (fn / 7.0), 2.0));        /* pinna notch */
        g *= 1.0 - 0.8 * back * (f * f / (f * f + 2500.0 * 2500.0));           /* behind: duller */
        double front = X > 0 ? X : 0.0;                                        /* in front: the concha's lift, 3 to 6 kHz */
        g *= 1.0 + 0.8 * front * exp(-pow((f - 4200.0) / 1600.0, 2.0));
        double ph = -w * tau;
        re[b] = g * (hr * cos(ph) - hi * sin(ph));
        im[b] = g * (hr * sin(ph) + hi * cos(ph));
    }
    for (int t = 0; t < TAPS; t++) {                                          /* a real inverse DFT */
        double s = re[0] + re[NFFT / 2] * cos(PI * t);
        for (int b = 1; b < NFFT / 2; b++) { double ang = 2 * PI * b * t / NFFT; s += 2.0 * (re[b] * cos(ang) - im[b] * sin(ang)); }
        double win = t < TAPS - 32 ? 1.0 : 0.5 * (1.0 + cos(PI * (t - (TAPS - 32)) / 32.0));
        h[t] = s / NFFT * win;
    }
}

/* ---- the KEMAR compact set ---- */
#define KN 128                 /* samples a measured response, at 44.1 kHz */
#define KMAX 400
typedef struct { double el, az, x, y, z; double l[KN], r[KN]; } Meas;
static Meas meas[KMAX];
static int nmeas;

static int read_wav(const char *path, double *l, double *r) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char buf[2048];
    size_t n = fread(buf, 1, sizeof buf, f); fclose(f);
    if (n < 44 || memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4)) return 0;
    size_t at = 12;
    while (at + 8 <= n) {
        uint32_t len = buf[at + 4] | buf[at + 5] << 8 | buf[at + 6] << 16 | (uint32_t)buf[at + 7] << 24;
        if (!memcmp(buf + at, "data", 4)) {
            if (at + 8 + len > n || len < KN * 4) return 0;
            for (int i = 0; i < KN; i++) {
                const unsigned char *p = buf + at + 8 + 4 * i;
                l[i] = (int16_t)(p[0] | p[1] << 8) / 32768.0; r[i] = (int16_t)(p[2] | p[3] << 8) / 32768.0;
            }
            return 1;
        }
        at += 8 + len + (len & 1);
    }
    return 0;
}
static void load_kemar(const char *dir) {
    for (int el = -40; el <= 90; el += 10) {
        char sub[1024]; snprintf(sub, sizeof sub, "%s/elev%d", dir, el);
        DIR *d = opendir(sub);
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d)) && nmeas < KMAX) {
            int fe, fa;
            if (sscanf(e->d_name, "H%de%da.wav", &fe, &fa) != 2) continue;
            char path[2048]; snprintf(path, sizeof path, "%s/%s", sub, e->d_name);
            Meas *m = &meas[nmeas];
            if (!read_wav(path, m->l, m->r)) { fprintf(stderr, "make_hrir: cannot read %s\n", path); continue; }
            m->el = fe; m->az = fa;
            double ce = cos(fe * PI / 180.0);
            m->x = ce * cos(fa * PI / 180.0); m->y = -ce * sin(fa * PI / 180.0); m->z = sin(fe * PI / 180.0);   /* azimuth runs clockwise: 90 is right, -Y */
            nmeas++;
        }
        closedir(d);
    }
}
/* the left ear's response to a source in direction (X, Y, Z), from the nearest measurement */
static void measured(double X, double Y, double Z, double *h) {
    int left_side = Y > 0.0;               /* measured only on the right: a source on the left is the mirror, ears swapped */
    double my = left_side ? -Y : Y, best = -2.0; int bi = 0;
    for (int i = 0; i < nmeas; i++) { double d = meas[i].x * X + meas[i].y * my + meas[i].z * Z; if (d > best) { best = d; bi = i; } }
    const double *src = left_side ? meas[bi].r : meas[bi].l;
    /* 44.1 to 48 kHz by a windowed sinc, after 16 samples of headroom for the equaliser's zero-phase spread */
    for (int t = 0; t < TAPS; t++) {
        double u = (t - 16) * 44100.0 / RATE, s = 0.0;
        for (int k = (int)floor(u) - 8; k <= (int)floor(u) + 8; k++) {
            if (k < 0 || k >= KN) continue;
            double x = u - k, sinc = fabs(x) < 1e-9 ? 1.0 : sin(PI * x) / (PI * x), w = fabs(x) < 8.0 ? 0.5 * (1.0 + cos(PI * x / 8.0)) : 0.0;
            s += src[k] * sinc * w;
        }
        double win = t < TAPS - 16 ? 1.0 : 0.5 * (1.0 + cos(PI * (t - (TAPS - 16)) / 16.0));
        h[t] = s * win;
    }
}

int main(int argc, char **argv) {
    static double filt[CH][TAPS];
    if (argc > 1) { load_kemar(argv[1]); if (nmeas == 0) { fprintf(stderr, "make_hrir: no measurements in %s\n", argv[1]); return 1; } fprintf(stderr, "make_hrir: %d measured directions\n", nmeas); }
    /* max-rE weights for order 3 */
    double re3 = cos(137.9 / (3 + 1.51) * PI / 180.0), wn[4];
    wn[0] = 1.0; wn[1] = re3; wn[2] = 0.5 * (3 * re3 * re3 - 1); wn[3] = 0.5 * (5 * re3 * re3 * re3 - 3 * re3);
    for (int j = 0; j < DIRS; j++) {                                          /* virtual speakers on a Fibonacci sphere */
        double Z = 1.0 - 2.0 * (j + 0.5) / DIRS, r = sqrt(1 - Z * Z), az = j * 2.39996322972865332;
        double X = r * cos(az), Y = r * sin(az), y[CH], h[TAPS];
        sh(X, Y, Z, y);
        if (nmeas) measured(X, Y, Z, h); else model(X, Y, Z, h);
        for (int k = 0; k < CH; k++) {
            int n = order_of(k);
            double coef = wn[n] * (2 * n + 1) / DIRS * y[k];
            for (int t = 0; t < TAPS; t++) filt[k][t] += coef * h[t];
        }
    }
    /* Diffuse-field equalisation: at every frequency, a field from all round
     * must reach the two ears together with the power one flat voice has.
     * Without it the decoder's low end adds up coherently and its highs
     * cancel, so it is loud and dull. */
    static double fr[CH][NFFT], fi[CH][NFFT];
    for (int k = 0; k < CH; k++)
        for (int b = 0; b <= NFFT / 2; b++) {
            double sr = 0, si = 0;
            for (int t = 0; t < TAPS; t++) { double ang = -2 * PI * b * t / NFFT; sr += filt[k][t] * cos(ang); si += filt[k][t] * sin(ang); }
            fr[k][b] = sr; fi[k][b] = si;
        }
    static double pw[NFFT / 2 + 1]; int tests = 600;
    for (int i = 0; i < tests; i++) {
        double Z = 1.0 - 2.0 * (i + 0.5) / tests, r = sqrt(1 - Z * Z), az = i * 2.39996322972865332;
        double y[CH]; sh(r * cos(az), r * sin(az), Z, y);
        for (int ear = 0; ear < 2; ear++)
            for (int b = 0; b <= NFFT / 2; b++) {
                double sr = 0, si = 0;
                for (int k = 0; k < CH; k++) { int m = k - order_of(k) * (order_of(k) + 1); double sg = (ear && m < 0 ? -1 : 1) * y[k]; sr += sg * fr[k][b]; si += sg * fi[k][b]; }
                pw[b] += (sr * sr + si * si) / tests;
            }
    }
    /* the gain each bin needs; above 20 kHz, where a 44.1 kHz
     * measurement holds nothing, hold the gain at its edge rather than
     * boost noise */
    double gb[NFFT / 2 + 1];
    int lo = 0, hi = (int)floor(20000.0 * NFFT / RATE);
    for (int b = 0; b <= NFFT / 2; b++) {
        int bb = b < lo ? lo : (b > hi ? hi : b);
        double g = 1.0 / sqrt(pw[bb] > 1e-12 ? pw[bb] : 1e-12);
        gb[b] = g > 16.0 ? 16.0 : g;                                           /* at most +24 dB */
    }
    for (int b = 0; b <= NFFT / 2; b++) for (int k = 0; k < CH; k++) { fr[k][b] *= gb[b]; fi[k][b] *= gb[b]; }
    for (int k = 0; k < CH; k++)
        for (int t = 0; t < TAPS; t++) {
            double sv = fr[k][0] + fr[k][NFFT / 2] * cos(PI * t);
            for (int b = 1; b < NFFT / 2; b++) { double ang = 2 * PI * b * t / NFFT; sv += 2.0 * (fr[k][b] * cos(ang) - fi[k][b] * sin(ang)); }
            double win = t < TAPS - 32 ? 1.0 : 0.5 * (1.0 + cos(PI * (t - (TAPS - 32)) / 32.0));
            filt[k][t] = sv / NFFT * win;
        }
    /* truncating to TAPS after equalising loses a little: one overall gain,
     * measured on the finished filters, puts a diffuse field back at unity */
    double e = 0.0; int probes = 1000;
    for (int i = 0; i < probes; i++) {
        double Z = 1.0 - 2.0 * (i + 0.5) / probes, r = sqrt(1 - Z * Z), az = i * 2.39996322972865332;
        double y[CH]; sh(r * cos(az), r * sin(az), Z, y);
        for (int ear = 0; ear < 2; ear++)
            for (int t = 0; t < TAPS; t++) {
                double sv = 0;
                for (int k = 0; k < CH; k++) { int m = k - order_of(k) * (order_of(k) + 1); sv += (ear && m < 0 ? -1 : 1) * y[k] * filt[k][t]; }
                e += sv * sv;
            }
    }
    double scale = sqrt(probes / e);
    uint64_t hash = 1469598103934665603ull;
    printf("/* strata_hrir.h -- generated by native/tools/make_hrir.c from %s; do not edit.\n", nmeas ? "the MIT KEMAR measurements (Gardner and Martin, MIT Media Lab, 1994: see native/hrir/README.md)" : "a spherical-head model");
    printf(" * The left ear's filters for a third-order SN3D/ACN bus, %d taps at 48 kHz; the right ear\n * is the mirror: channels with m < 0 change sign. */\n", TAPS);
    printf("#ifndef STRATA_HRIR_H\n#define STRATA_HRIR_H\n#define SH_TAPS %d\n#define SH_MODEL \"%s\"\n", TAPS, nmeas ? "MIT KEMAR" : "spherical head");
    printf("static const float strata_hrir[16][%d] = {\n", TAPS);
    for (int k = 0; k < CH; k++) {
        printf("  {");
        for (int t = 0; t < TAPS; t++) {
            float v = (float)(filt[k][t] * scale);
            uint32_t bits; memcpy(&bits, &v, 4); hash = (hash ^ bits) * 1099511628211ull;
            printf("%s%.8ef", t ? "," : "", v);
        }
        printf("}%s\n", k < CH - 1 ? "," : "");
    }
    printf("};\n/* hash %016llx */\n#endif\n", (unsigned long long)hash);
    fprintf(stderr, "make_hrir: hash %016llx\n", (unsigned long long)hash);
    return 0;
}

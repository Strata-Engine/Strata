/* strata_audio.c — Strata's audio shim: the mixer (strata_mix.h) on a
 * miniaudio device, on miniaudio's silent null device, or rendered in game
 * time to a WAV with no device at all. Knows nothing of Vulkan or GLFW.
 *
 * Threads. March calls in on its main thread: sa_send pushes command records
 * into a wait-free ring and never waits. The device thread (miniaudio's
 * callback) drains the ring and mixes; it never calls March, never locks,
 * never allocates. Back to March go only numbers, in atomics. In game time
 * there is no device thread: sa_advance drains and mixes on the caller's
 * thread.
 *
 * March values: a NativeArray arrives as its heap pointer, element count at
 * +16, data at +32; a String through march_str_borrow. */
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include "march_ffi.h"
#include "strata_mix.h"
#include <time.h>
#include <pthread.h>

#define SA_RING 4096
#define SA_STATS 12
#define SA_PERIOD 256
#define SA_PERIODS 3

static SmMixer *g_mix;
static SmRing g_ring;
static SmCmd g_ring_buf[SA_RING];
static int g_mode;                 /* 0 closed, 1 device, 2 null device, 3 game time */
static ma_context g_ctx; static int g_ctx_ok;
static ma_device g_dev; static int g_dev_ok;
static FILE *g_wav; static uint32_t g_wav_frames;
static double g_carry;
static float g_buf[2 * 4096];
static _Atomic double g_stat[SA_STATS];
static _Atomic int64_t g_refused_here;     /* records refused on the main-thread side */
static _Atomic double g_play_sent;         /* when the last batch with a PLAY was sent */
static _Atomic int g_play_pending;

/* ---- recordings: decoded by one loader thread, published by pointer ---- */
typedef struct { int op; int id; char path[512]; } SaReq;    /* op 1 load, 2 unload */
#define SA_REQS 256
static SaReq g_req[SA_REQS];
static _Atomic uint32_t g_req_head, g_req_tail;
static pthread_t g_loader; static int g_loader_on;
static _Atomic int g_loader_stop;
static _Atomic int g_rec_state[SM_RECS];      /* 0 none, 1 loading, 2 ready, 3 failed, 4 unloaded */
static SmRec *g_rec_owned[SM_RECS];           /* the loader's: what it decoded */
static _Atomic int64_t g_budget = 64ll * 1024 * 1024, g_used;
static _Atomic int64_t g_budget_refused;
static int g_next_rec = SM_REC_FIRST;
typedef struct { SmRec *rec; uint64_t after; int64_t bytes; } SaFree;
static SaFree g_free[SA_REQS]; static int g_nfree;

static void sa_sleep_ms(int ms) { struct timespec t = { 0, ms * 1000000L }; nanosleep(&t, NULL); }

static int64_t sa_rec_bytes(const SmRec *r) { return (int64_t)r->frames * r->channels * (int64_t)sizeof(float); }

static void sa_decode(int id, const char *path) {
    int slot = id - SM_REC_FIRST;
    ma_decoder_config dc = ma_decoder_config_init(ma_format_f32, 0, SM_RATE);
    ma_decoder dec;
    if (ma_decoder_init_file(path, &dc, &dec) != MA_SUCCESS) { fprintf(stderr, "strata_audio: cannot decode %s\n", path); atomic_store(&g_rec_state[slot], 3); return; }
    if (dec.outputChannels > 2) {   /* more than stereo: fold to two */
        ma_decoder_uninit(&dec); dc.channels = 2;
        if (ma_decoder_init_file(path, &dc, &dec) != MA_SUCCESS) { atomic_store(&g_rec_state[slot], 3); return; }
    }
    int ch = (int)dec.outputChannels;
    size_t cap = 1 << 16, n = 0; float *data = malloc(cap * ch * sizeof(float));
    int over = 0;
    while (data) {
        if ((int64_t)((n + 4096) * ch * sizeof(float)) + atomic_load(&g_used) > atomic_load(&g_budget)) { over = 1; break; }
        if (n + 4096 > cap) { float *d2 = realloc(data, cap * 2 * ch * sizeof(float)); if (!d2) { free(data); data = NULL; break; } data = d2; cap *= 2; }
        ma_uint64 got = 0;
        if (ma_decoder_read_pcm_frames(&dec, data + n * ch, 4096, &got) != MA_SUCCESS || got == 0) break;
        n += (size_t)got;
    }
    ma_decoder_uninit(&dec);
    if (over || !data || n == 0) {
        free(data);
        if (over) { atomic_fetch_add(&g_budget_refused, 1); fprintf(stderr, "strata_audio: %s would pass the decoded budget\n", path); }
        atomic_store(&g_rec_state[slot], 3);
        return;
    }
    SmRec *r = calloc(1, sizeof(SmRec));
    if (!r) { free(data); atomic_store(&g_rec_state[slot], 3); return; }
    r->data = data; r->frames = (int)n; r->channels = ch;
    atomic_fetch_add(&g_used, sa_rec_bytes(r));
    g_rec_owned[slot] = r;
    sm_rec_publish(g_mix, id, r);
    atomic_store(&g_rec_state[slot], 2);
}

static void sa_reap(int all) {
    int k = 0;
    for (int i = 0; i < g_nfree; i++) {
        SaFree f = g_free[i];
        if (all || (sm_blocks(g_mix) > f.after && atomic_load(&f.rec->refs) == 0)) { free(f.rec->data); free(f.rec); atomic_fetch_sub(&g_used, f.bytes); }
        else g_free[k++] = f;
    }
    g_nfree = k;
}

static void *sa_loader_main(void *arg) {
    (void)arg;
    while (!atomic_load(&g_loader_stop)) {
        uint32_t t = atomic_load_explicit(&g_req_tail, memory_order_relaxed), h = atomic_load_explicit(&g_req_head, memory_order_acquire);
        if (t == h) { sa_reap(0); sa_sleep_ms(2); continue; }
        SaReq q = g_req[t % SA_REQS];
        atomic_store_explicit(&g_req_tail, t + 1, memory_order_release);
        int slot = q.id - SM_REC_FIRST;
        if (q.op == 1) sa_decode(q.id, q.path);
        else if (q.op == 2 && g_rec_owned[slot]) {
            sm_rec_withdraw(g_mix, q.id);
            if (g_nfree < SA_REQS) { g_free[g_nfree++] = (SaFree){ g_rec_owned[slot], sm_blocks(g_mix), sa_rec_bytes(g_rec_owned[slot]) }; g_rec_owned[slot] = NULL; }
            atomic_store(&g_rec_state[slot], 4);
        }
    }
    return NULL;
}

static int sa_request(int op, int id, const char *path) {
    uint32_t h = atomic_load_explicit(&g_req_head, memory_order_relaxed), t = atomic_load_explicit(&g_req_tail, memory_order_acquire);
    if (h - t >= SA_REQS) return 0;
    SaReq *q = &g_req[h % SA_REQS];
    q->op = op; q->id = id; q->path[0] = 0;
    if (path) { strncpy(q->path, path, sizeof q->path - 1); q->path[sizeof q->path - 1] = 0; }
    atomic_store_explicit(&g_req_head, h + 1, memory_order_release);
    return 1;
}

static void sa_loader_start(void) {
    atomic_store(&g_loader_stop, 0);
    atomic_store(&g_req_head, 0); atomic_store(&g_req_tail, 0);
    atomic_store(&g_used, 0); atomic_store(&g_budget_refused, 0);
    for (int i = 0; i < SM_RECS; i++) { atomic_store(&g_rec_state[i], 0); g_rec_owned[i] = NULL; }
    g_next_rec = SM_REC_FIRST; g_nfree = 0;
    g_loader_on = pthread_create(&g_loader, NULL, sa_loader_main, NULL) == 0;
}

static void sa_loader_stop(void) {
    if (!g_loader_on) return;
    atomic_store(&g_loader_stop, 1);
    pthread_join(g_loader, NULL); g_loader_on = 0;
    sa_reap(1);
    for (int i = 0; i < SM_RECS; i++) if (g_rec_owned[i]) { free(g_rec_owned[i]->data); free(g_rec_owned[i]); g_rec_owned[i] = NULL; }
}

static double sa_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + t.tv_nsec * 1e-9; }

static void sa_publish(void) {
    SmStats s = sm_stats(g_mix);
    atomic_store(&g_stat[0], s.playing); atomic_store(&g_stat[1], s.virtual_); atomic_store(&g_stat[2], s.refused);
    atomic_store(&g_stat[4], s.underruns); atomic_store(&g_stat[5], s.cpu);
    atomic_store(&g_stat[6], s.peak_db); atomic_store(&g_stat[7], s.rms_db);
}

/* drain the ring into the mixer; on the mixing thread only */
static void sa_drain(void) {
    SmCmd c; int played = 0;
    while (sm_ring_pop(&g_ring, &c)) { if (c.op == SM_OP_PLAY) played = 1; sm_command(g_mix, &c); }
    if (played && atomic_exchange(&g_play_pending, 0)) {
        double buffered = g_mode == 3 ? 0.0 : (double)(SA_PERIOD * SA_PERIODS) / SM_RATE;
        atomic_store(&g_stat[10], (sa_now() - atomic_load(&g_play_sent) + buffered) * 1000.0);
    }
}

static void sa_callback(ma_device *d, void *out, const void *in, ma_uint32 frames) {
    (void)d; (void)in;
    double t0 = sa_now();
    sa_drain();
    sm_mix(g_mix, (float *)out, (int)frames);
    double spent = sa_now() - t0, period = (double)frames / SM_RATE;
    sm_set_cpu(g_mix, (float)(spent / period));
    if (spent > period) sm_note_underrun(g_mix);
    sa_publish();
}

static void sa_reset(void) {
    for (int i = 0; i < SA_STATS; i++) atomic_store(&g_stat[i], 0.0);
    atomic_store(&g_refused_here, 0); atomic_store(&g_play_pending, 0);
    sm_ring_init(&g_ring, g_ring_buf, SA_RING);
    g_carry = 0.0;
}

static int sa_forced_fail(void) { const char *e = getenv("STRATA_AUDIO_FAIL"); return e && e[0] == '1'; }

static int sa_start_device(int null_backend) {
    if (sa_forced_fail()) { fprintf(stderr, "strata_audio: STRATA_AUDIO_FAIL=1, no device\n"); return 0; }
    ma_backend nb[1] = { ma_backend_null };
    if (ma_context_init(null_backend ? nb : NULL, null_backend ? 1 : 0, NULL, &g_ctx) != MA_SUCCESS) { fprintf(stderr, "strata_audio: no audio context\n"); return 0; }
    g_ctx_ok = 1;
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32; cfg.playback.channels = 2;
    cfg.sampleRate = SM_RATE; cfg.periodSizeInFrames = SA_PERIOD; cfg.periods = SA_PERIODS;
    cfg.dataCallback = sa_callback;
    if (ma_device_init(&g_ctx, &cfg, &g_dev) != MA_SUCCESS) { fprintf(stderr, "strata_audio: no playback device\n"); return 0; }
    g_dev_ok = 1;
    if (ma_device_start(&g_dev) != MA_SUCCESS) { fprintf(stderr, "strata_audio: the device would not start\n"); return 0; }
    return 1;
}

void sa_close(void);

static int64_t sa_open_mode(int mode) {
    if (g_mode) sa_close();
    g_mix = sm_open();
    if (!g_mix) return 0;
    sa_reset();
    atomic_store(&g_stat[11], (double)sizeof(SmMixer));
    g_mode = mode;
    sa_loader_start();
    if (!sa_start_device(mode == 2)) { sa_close(); return 0; }
    return 1;
}

int64_t sa_open(int64_t headphones) {
    int64_t ok = sa_open_mode(1);
    if (ok && headphones) { SmCmd c; memset(&c, 0, sizeof c); c.op = SM_OP_OUTPUT; c.a = 1; sm_ring_push(&g_ring, &c); }
    return ok;
}
int64_t sa_open_null(void) { return sa_open_mode(2); }

static void sa_wav_header(FILE *f, uint32_t frames) {
    unsigned char h[44]; uint32_t bytes = frames * 4u;
    memcpy(h, "RIFF", 4); sm_put32(h + 4, 36 + bytes); memcpy(h + 8, "WAVEfmt ", 8);
    sm_put32(h + 16, 16); h[20] = 1; h[21] = 0; h[22] = 2; h[23] = 0;
    sm_put32(h + 24, SM_RATE); sm_put32(h + 28, SM_RATE * 4); h[32] = 4; h[33] = 0; h[34] = 16; h[35] = 0;
    memcpy(h + 36, "data", 4); sm_put32(h + 40, bytes);
    fseek(f, 0, SEEK_SET); fwrite(h, 1, 44, f); fseek(f, 0, SEEK_END);
}

int64_t sa_render_open(march_value path) {
    if (g_mode) sa_close();
    march_slice t = march_str_borrow(path);
    char name[1024]; size_t n = t.len < sizeof name - 1 ? t.len : sizeof name - 1;
    memcpy(name, t.ptr, n); name[n] = 0;
    g_wav = fopen(name, "wb");
    if (!g_wav) { fprintf(stderr, "strata_audio: cannot write %s\n", name); return 0; }
    g_wav_frames = 0; sa_wav_header(g_wav, 0);
    g_mix = sm_open();
    if (!g_mix) { fclose(g_wav); g_wav = NULL; return 0; }
    sa_reset();
    atomic_store(&g_stat[11], (double)sizeof(SmMixer));
    g_mode = 3;
    sa_loader_start();
    return 1;
}

void sa_close(void) {
    if (g_dev_ok) { ma_device_uninit(&g_dev); g_dev_ok = 0; }
    if (g_ctx_ok) { ma_context_uninit(&g_ctx); g_ctx_ok = 0; }
    if (g_wav) { sa_wav_header(g_wav, g_wav_frames); fclose(g_wav); g_wav = NULL; }
    sa_loader_stop();
    if (g_mix) { sm_close(g_mix); g_mix = NULL; }
    g_mode = 0;
}

int64_t sa_send(void *ints, void *floats, int64_t n) {
    if (!g_mode || n <= 0) { if (n > 0) atomic_fetch_add(&g_refused_here, n); return 0; }
    const int32_t *ia = (const int32_t *)((char *)ints + 32);
    const float *fa = (const float *)((char *)floats + 32);
    int64_t took = 0; int played = 0;
    for (int64_t i = 0; i < n; i++) {
        SmCmd c;
        c.op = ia[4 * i]; c.voice = ia[4 * i + 1]; c.a = ia[4 * i + 2]; c.b = ia[4 * i + 3];
        memcpy(c.f, fa + 12 * i, sizeof c.f);
        if (sm_ring_push(&g_ring, &c)) { took++; if (c.op == SM_OP_PLAY) played = 1; }
    }
    if (played) { atomic_store(&g_play_sent, sa_now()); atomic_store(&g_play_pending, 1); }
    if (took < n) atomic_fetch_add(&g_refused_here, n - took);
    return took;
}

void sa_advance(double seconds) {
    if (g_mode != 3 || seconds <= 0.0) return;
    g_carry += seconds * SM_RATE;
    int frames = (int)g_carry;
    g_carry -= frames;
    sa_drain();
    while (frames > 0) {
        int k = frames < 4096 ? frames : 4096;
        double t0 = sa_now();
        sm_mix(g_mix, g_buf, k);
        sm_set_cpu(g_mix, (float)((sa_now() - t0) / ((double)k / SM_RATE)));
        for (int i = 0; i < 2 * k; i++) {
            int16_t q = (int16_t)lrintf(sm_clampf(g_buf[i], -1.0f, 1.0f) * 32767.0f);
            unsigned char b[2] = { (unsigned char)(q & 255), (unsigned char)((q >> 8) & 255) };
            fwrite(b, 1, 2, g_wav);
        }
        g_wav_frames += (uint32_t)k;
        frames -= k;
    }
    sa_publish();
}

/* 0 playing, 1 virtual, 2 refused, 3 ring refused, 4 underruns, 5 callback CPU share,
 * 6 peak dB, 7 RMS dB, 8 decoded bytes held, 9 loads refused by the budget,
 * 10 latency ms of the last play, 11 bytes the mixer holds */
double sa_stat(int64_t which) {
    if (which == 3) return (double)atomic_load(&g_refused_here);
    if (which == 8) return (double)atomic_load(&g_used);
    if (which == 9) return (double)atomic_load(&g_budget_refused);
    if (which < 0 || which >= SA_STATS) return 0.0;
    return atomic_load(&g_stat[which]);
}

/* ---- recordings, from March ---- */
static void sa_cstr(march_value v, char *out, size_t cap) {
    march_slice t = march_str_borrow(v);
    size_t n = t.len < cap - 1 ? t.len : cap - 1;
    memcpy(out, t.ptr, n); out[n] = 0;
}
/* Start decoding [path]; answers its id at once (0: closed, or no ids or room left). */
int64_t sa_load(march_value path) {
    if (!g_mode || !g_loader_on || g_next_rec >= SM_REC_FIRST + SM_RECS) return 0;
    char p[512]; sa_cstr(path, p, sizeof p);
    int id = g_next_rec;
    atomic_store(&g_rec_state[id - SM_REC_FIRST], 1);
    if (!sa_request(1, id, p)) { atomic_store(&g_rec_state[id - SM_REC_FIRST], 0); return 0; }
    g_next_rec++;
    return id;
}
/* 1 ready, 0 still loading, -1 failed or unloaded or unknown */
int64_t sa_ready(int64_t id) {
    if (id < SM_REC_FIRST || id >= SM_REC_FIRST + SM_RECS) return -1;
    int st = atomic_load(&g_rec_state[id - SM_REC_FIRST]);
    return st == 2 ? 1 : (st == 1 ? 0 : -1);
}
/* For tools and tests only: wait up to [ms] for a recording to settle. Never call it from a frame. */
int64_t sa_ready_wait(int64_t id, int64_t ms) {
    for (int64_t t = 0; t < ms; t += 2) { int64_t r = sa_ready(id); if (r != 0) return r; sa_sleep_ms(2); }
    return sa_ready(id);
}
void sa_unload(int64_t id) {
    if (id < SM_REC_FIRST || id >= SM_REC_FIRST + SM_RECS || !g_loader_on) return;
    sa_request(2, (int)id, NULL);
}
void sa_set_budget(int64_t bytes) { atomic_store(&g_budget, bytes < 0 ? 0 : bytes); }

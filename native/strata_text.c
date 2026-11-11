/* Strata text: fonts (FreeType), a distance-field glyph atlas that fills on
 * demand, and layout into quads for the 2D layer. See strata_text.h.
 *
 * A glyph is rasterised once, as a signed distance field at ST_EM pixels to
 * the em, and that one picture serves every size. FreeType draws the glyph's
 * coverage; the field is worked out here from that (FreeType's own
 * distance-field renderer takes 1 to 4 ms a glyph, this about a twentieth). The atlas is pages of
 * 8-bit pixels; a glyph's cell comes from a free cell, then a shelf, then
 * from the glyph unused for longest (never one used this frame), then from a
 * new page. Rasterising is budgeted per frame, so a screenful of new text is
 * spread over a few frames instead of being one long one.
 *
 * Nothing here is thread-safe: text is the main thread's. */
#include "strata_text.h"
#include "strata_font.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_ADVANCES_H
#include FT_MODULE_H
#include <hb.h>
#include <hb-ot.h>
/* SheenBidi (native/sheenbidi, Apache 2.0) is compiled into this file: see the end. */
#define SB_CONFIG_UNITY
#include <SheenBidi/SheenBidi.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>

/* ── UTF-8 ────────────────────────────────────────────────────────────────── */
/* Decode one code point at s[i]; returns its length in bytes, 0 if invalid. */
static int utf8_next(const unsigned char *s, int64_t n, int64_t i, uint32_t *cp) {
    unsigned char c = s[i];
    if (c < 0x80) { *cp = c; return 1; }
    int len = (c >= 0xc2 && c <= 0xdf) ? 2 : (c >= 0xe0 && c <= 0xef) ? 3 : (c >= 0xf0 && c <= 0xf4) ? 4 : 0;
    if (!len || i + len > n) return 0;
    uint32_t v = c & (0xff >> (len + 1));
    for (int k = 1; k < len; k++) {
        if ((s[i + k] & 0xc0) != 0x80) return 0;
        v = (v << 6) | (s[i + k] & 0x3f);
    }
    if ((len == 3 && v < 0x800) || (len == 4 && v < 0x10000) || v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff)) return 0;
    *cp = v;
    return len;
}

int64_t st_utf8_count(const char *s, int64_t n) {
    if (!s || n < 0) return -1;
    int64_t count = 0;
    for (int64_t i = 0; i < n; count++) {
        uint32_t cp; int len = utf8_next((const unsigned char *)s, n, i, &cp);
        if (!len) return -1;
        i += len;
    }
    return count;
}

/* ── State ────────────────────────────────────────────────────────────────── */
#define MAX_FONTS 64
#define MAX_FALLBACKS 8
#define MAX_PAGES 8
#define GL_CAP 65536           /* glyph table slots; a power of two */
#define DIRTY_CAP 8192
#define CELL 8                 /* cells are multiples of this many pixels */
#define LC_CAP 1024            /* layouts remembered */

typedef struct {
    FT_Face face;
    hb_font_t *hb;             /* the same file, for shaping; positions come back in font units */
    unsigned char *owned;      /* the file's bytes, when loaded from a path */
    char *path; int64_t face_index;   /* where it came from: loading the same file again gives the same handle */
    int nfb; int fb[MAX_FALLBACKS];
    float asc, desc, lineh;    /* in ems */
} Font;

typedef struct {
    uint32_t font, gid;           /* gid's top bit: the fine picture, for large text */
    uint8_t state;             /* 0 never seen, 1 in the atlas (or inkless), 2 evicted */
    int16_t page, x, y, w, h, cw, ch, left, top;
    uint64_t used;             /* the frame it was last asked for */
} Glyph;

typedef struct { int16_t page, x, y, cw, ch; } Cell;
typedef struct { int16_t page, y, h, x; } Shelf;
typedef struct { int32_t font; uint32_t gid; float x, y; int32_t line, cluster; float adv; } LGlyph;   /* x, y, adv in ems; cluster: the byte its characters start at */
typedef struct {
    uint64_t hash, used; int font, n, align; float wrap_em;
    char *bytes; LGlyph *g; int ng, nlines; float width;
} Layout;

static struct {
    int init;
    FT_Library lib;
    Font fonts[MAX_FONTS]; int nfonts;          /* a handle is an index; 0 is unused */
    int page_px, max_pages, npages;
    uint8_t *pages[MAX_PAGES]; int page_y[MAX_PAGES];
    Shelf *shelves; int nshelves, shelf_cap;
    Cell *free_cells; int nfree, free_cap;
    Glyph *glyphs; int nglyphs_seen, nlive;
    struct { int16_t page, x, y, w, h; } dirty[DIRTY_CAP]; uint32_t dirty_head, dirty_n;
    uint64_t frame;
    int64_t budget_us; double spent_us;
    uint64_t waited, evictions, rasterised, layouts, shapes;
    hb_buffer_t *hb;
    Layout cache[LC_CAP]; uint64_t tick;
} T;

static double now_us(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (double)ts.tv_sec * 1.0e6 + (double)ts.tv_nsec / 1.0e3; }

static void atlas_reset(void) {
    for (int p = 0; p < MAX_PAGES; p++) { free(T.pages[p]); T.pages[p] = NULL; T.page_y[p] = 0; }
    T.npages = 0; T.nshelves = 0; T.nfree = 0; T.dirty_head = 0; T.dirty_n = 0; T.nlive = 0; T.nglyphs_seen = 0;
    if (T.glyphs) memset(T.glyphs, 0, sizeof(Glyph) * GL_CAP);
}

static int64_t font_from(const unsigned char *bytes, int64_t n, int64_t face_index, unsigned char *owned);

static int init(void) {
    if (T.init) return T.init > 0;
    T.init = -1;
    if (FT_Init_FreeType(&T.lib)) return 0;
    T.glyphs = calloc(GL_CAP, sizeof(Glyph));
    T.hb = hb_buffer_create();
    if (!T.glyphs || !T.hb || !hb_buffer_allocation_successful(T.hb)) return 0;
    T.page_px = 2048; T.max_pages = 4; T.budget_us = 1000; T.nfonts = 1; T.frame = 1;
    T.init = 1;
    if (font_from(strata_default_font, strata_default_font_len, 0, NULL) != 1) { T.init = -1; return 0; }
    return 1;
}

/* ── Fonts ────────────────────────────────────────────────────────────────── */
static uint32_t be32(const unsigned char *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

/* A single-font file's table directory must lie wholly inside the file:
 * FreeType opens a file cut short and fails only when a glyph is asked for. */
static int tables_fit(const unsigned char *b, int64_t n) {
    if (n < 12) return 0;
    uint32_t tag = be32(b);
    if (tag == 0x74746366u) return 1;                                  /* a collection: FreeType checks the face asked for */
    if (tag != 0x00010000u && tag != 0x4f54544fu && tag != 0x74727565u) return 1;   /* not sfnt: let FreeType judge */
    int tables = (b[4] << 8) | b[5];
    if (12 + (int64_t)tables * 16 > n) return 0;
    for (int t = 0; t < tables; t++) {
        const unsigned char *e = b + 12 + t * 16;
        if ((uint64_t)be32(e + 8) + be32(e + 12) > (uint64_t)n) return 0;
    }
    return 1;
}

static int64_t font_from(const unsigned char *bytes, int64_t n, int64_t face_index, unsigned char *owned) {
    if (!init() || T.nfonts >= MAX_FONTS || !bytes || n <= 0 || !tables_fit(bytes, n)) return 0;
    FT_Face face;
    if (FT_New_Memory_Face(T.lib, bytes, (FT_Long)n, (FT_Long)(face_index < 0 ? 0 : face_index), &face)) return 0;
    if (!FT_IS_SCALABLE(face) || face->units_per_EM == 0 || FT_Set_Pixel_Sizes(face, 0, ST_EM)) { FT_Done_Face(face); return 0; }
    hb_blob_t *blob = hb_blob_create((const char *)bytes, (unsigned)n, HB_MEMORY_MODE_READONLY, NULL, NULL);
    hb_face_t *hface = hb_face_create(blob, (unsigned)(face_index < 0 ? 0 : face_index));
    hb_font_t *hfont = hb_font_create(hface);
    hb_face_destroy(hface); hb_blob_destroy(blob);
    if (!hfont || hfont == hb_font_get_empty()) { FT_Done_Face(face); return 0; }
    hb_ot_font_set_funcs(hfont);
    Font *f = &T.fonts[T.nfonts];
    memset(f, 0, sizeof *f);
    f->face = face; f->hb = hfont; f->owned = owned;
    float upem = (float)face->units_per_EM;
    f->asc = (float)face->ascender / upem; f->desc = -(float)face->descender / upem; f->lineh = (float)face->height / upem;
    if (f->lineh < f->asc + f->desc) f->lineh = f->asc + f->desc;
    if (f->lineh <= 0.0f) { f->asc = 0.8f; f->desc = 0.2f; f->lineh = 1.2f; }
    return T.nfonts++;
}

int64_t st_font_default(void) { return init() ? 1 : 0; }

int64_t st_font_load_mem(const unsigned char *bytes, int64_t n, int64_t face_index) { return font_from(bytes, n, face_index, NULL); }

int64_t st_font_load(const char *path, int64_t nbytes, int64_t face_index) {
    if (!init() || !path || nbytes <= 0 || nbytes > 4095) return 0;
    char p[4096]; memcpy(p, path, (size_t)nbytes); p[nbytes] = 0;
    for (int i = 2; i < T.nfonts; i++) if (T.fonts[i].path && T.fonts[i].face_index == face_index && strcmp(T.fonts[i].path, p) == 0) return i;
    FILE *fp = fopen(p, "rb");
    if (!fp) return 0;
    unsigned char *bytes = NULL; long size = 0;
    if (fseek(fp, 0, SEEK_END) == 0 && (size = ftell(fp)) > 0 && size < (64L << 20) && fseek(fp, 0, SEEK_SET) == 0) {
        bytes = malloc((size_t)size);
        if (bytes && fread(bytes, 1, (size_t)size, fp) != (size_t)size) { free(bytes); bytes = NULL; }
    }
    fclose(fp);
    if (!bytes) return 0;
    int64_t h = font_from(bytes, size, face_index, bytes);
    if (!h) free(bytes);
    else { T.fonts[h].path = strdup(p); T.fonts[h].face_index = face_index; }
    return h;
}

/* A bad handle draws with the built-in font. */
static int font_ok(int64_t font) { return (init() && font >= 1 && font < T.nfonts) ? (int)font : 1; }

int64_t st_font_set_fallbacks(int64_t font, const int64_t *handles, int64_t n) {
    if (!init() || font < 1 || font >= T.nfonts || n < 0 || (n > 0 && !handles)) return 0;
    Font *f = &T.fonts[font];
    f->nfb = 0;
    for (int64_t i = 0; i < n && f->nfb < MAX_FALLBACKS; i++)
        if (handles[i] >= 1 && handles[i] < T.nfonts && handles[i] != font) f->fb[f->nfb++] = (int)handles[i];
    for (int i = 0; i < LC_CAP; i++) T.cache[i].hash = 0;      /* layouts made under the old list are stale */
    return 1;
}

int64_t st_font_glyph(int64_t font, int64_t codepoint) {
    if (!init() || codepoint < 0) return 0;
    return (int64_t)FT_Get_Char_Index(T.fonts[font_ok(font)].face, (FT_ULong)codepoint);
}

/* ── The atlas ────────────────────────────────────────────────────────────── */
void st_atlas_config(int64_t page_px, int64_t max_pages) {
    if (!init()) return;
    atlas_reset();
    T.page_px = (page_px >= 64 && page_px <= 8192) ? (int)page_px : 2048;
    T.max_pages = (max_pages >= 1 && max_pages <= MAX_PAGES) ? (int)max_pages : 4;
}
int64_t st_atlas_page_px(void) { return init() ? T.page_px : 0; }
int64_t st_atlas_max_pages(void) { return init() ? T.max_pages : 0; }

void st_frame_begin(void) { if (!init()) return; T.frame++; T.spent_us = 0.0; T.waited = 0; }
void st_set_raster_budget(int64_t us) { if (init()) T.budget_us = us < 0 ? 0 : us; }

double st_stat(int64_t which) {
    if (!init()) return -1.0;
    switch (which) {
        case 0: return (double)T.nlive;
        case 1: return (double)T.waited;
        case 2: return (double)T.evictions;
        case 3: return (double)T.npages * T.page_px * T.page_px;
        case 4: return (double)T.rasterised;
        case 5: return T.spent_us;
        case 6: return (double)T.layouts;
        case 7: return (double)T.shapes;
        default: return -1.0;
    }
}

static uint32_t gl_hash(uint32_t font, uint32_t gid) { uint32_t h = font * 0x9e3779b1u ^ gid * 0x85ebca6bu; h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12; return h; }

/* The glyph's slot in the table, made if it is not there. NULL if the table is full. */
static Glyph *gl_find(uint32_t font, uint32_t gid) {
    uint32_t i = gl_hash(font, gid) & (GL_CAP - 1);
    for (int probe = 0; probe < GL_CAP; probe++, i = (i + 1) & (GL_CAP - 1)) {
        Glyph *g = &T.glyphs[i];
        if (g->state == 0) {
            if (T.nglyphs_seen >= GL_CAP * 3 / 4) return NULL;
            g->font = font; g->gid = gid; T.nglyphs_seen++;
            return g;
        }
        if (g->font == font && g->gid == gid) return g;
    }
    return NULL;
}

static int page_make(void) {
    if (T.npages >= T.max_pages) return 0;
    uint8_t *px = calloc((size_t)T.page_px, (size_t)T.page_px);
    if (!px) return 0;
    T.pages[T.npages] = px; T.page_y[T.npages] = 0; T.npages++;
    return 1;
}

static int shelf_take(int cw, int ch, Cell *out) {
    for (int i = 0; i < T.nshelves; i++) {
        Shelf *s = &T.shelves[i];
        if (s->h == ch && s->x + cw <= T.page_px) { *out = (Cell){ s->page, s->x, s->y, (int16_t)cw, (int16_t)ch }; s->x = (int16_t)(s->x + cw); return 1; }
    }
    for (int p = 0; p < T.npages; p++) {
        if (T.page_y[p] + ch > T.page_px) continue;
        if (T.nshelves == T.shelf_cap) {
            int cap = T.shelf_cap ? T.shelf_cap * 2 : 64;
            Shelf *grown = realloc(T.shelves, sizeof(Shelf) * (size_t)cap);
            if (!grown) return 0;
            T.shelves = grown; T.shelf_cap = cap;
        }
        T.shelves[T.nshelves++] = (Shelf){ (int16_t)p, (int16_t)T.page_y[p], (int16_t)ch, (int16_t)cw };
        *out = (Cell){ (int16_t)p, 0, (int16_t)T.page_y[p], (int16_t)cw, (int16_t)ch };
        T.page_y[p] += ch;
        return 1;
    }
    return 0;
}

static int free_take(int cw, int ch, Cell *out) {
    int best = -1;
    for (int i = 0; i < T.nfree; i++) {
        Cell *c = &T.free_cells[i];
        if (c->cw < cw || c->ch < ch) continue;
        if (best < 0 || (int)c->cw * c->ch < (int)T.free_cells[best].cw * T.free_cells[best].ch) best = i;
    }
    if (best < 0) return 0;
    *out = T.free_cells[best];
    T.free_cells[best] = T.free_cells[--T.nfree];
    return 1;
}

/* Drop the glyph unused for longest whose cell is big enough, and take its
 * cell. Never one asked for this frame. */
static int evict_take(int cw, int ch, Cell *out) {
    Glyph *old = NULL;
    for (int i = 0; i < GL_CAP; i++) {
        Glyph *g = &T.glyphs[i];
        if (g->state != 1 || g->cw < cw || g->ch < ch || g->used >= T.frame) continue;
        if (!old || g->used < old->used) old = g;
    }
    if (!old) return 0;
    *out = (Cell){ old->page, old->x, old->y, old->cw, old->ch };
    old->state = 2; old->cw = old->ch = 0;
    T.nlive--; T.evictions++;
    return 1;
}

/* The last resort: empty the page whose glyphs have gone unused for longest,
 * if it holds none asked for this frame. Cells of many sizes can leave no
 * single one big enough; a fresh page always is. */
static int page_wipe(void) {
    uint64_t newest[MAX_PAGES] = { 0 };
    for (int i = 0; i < GL_CAP; i++) {
        Glyph *g = &T.glyphs[i];
        if (g->state == 1 && g->cw > 0 && g->used > newest[g->page]) newest[g->page] = g->used;
    }
    int pick = -1;
    for (int p = 0; p < T.npages; p++) if (newest[p] < T.frame && (pick < 0 || newest[p] < newest[pick])) pick = p;
    if (pick < 0) return 0;
    for (int i = 0; i < GL_CAP; i++) {
        Glyph *g = &T.glyphs[i];
        if (g->state == 1 && g->cw > 0 && g->page == pick) { g->state = 2; g->cw = g->ch = 0; T.nlive--; T.evictions++; }
    }
    int keep = 0;
    for (int i = 0; i < T.nshelves; i++) if (T.shelves[i].page != pick) T.shelves[keep++] = T.shelves[i];
    T.nshelves = keep;
    keep = 0;
    for (int i = 0; i < T.nfree; i++) if (T.free_cells[i].page != pick) T.free_cells[keep++] = T.free_cells[i];
    T.nfree = keep;
    T.page_y[pick] = 0;
    return 1;
}

static int cell_take(int cw, int ch, Cell *out) {
    if (cw > T.page_px || ch > T.page_px) return 0;
    if (T.npages == 0 && !page_make()) return 0;
    if (free_take(cw, ch, out) || shelf_take(cw, ch, out) || evict_take(cw, ch, out)) return 1;
    if (page_make() && shelf_take(cw, ch, out)) return 1;
    return page_wipe() && shelf_take(cw, ch, out);
}

static void dirty_push(const Cell *c) {
    if (T.dirty_n == DIRTY_CAP) { T.dirty_head = (T.dirty_head + 1) % DIRTY_CAP; T.dirty_n--; }     /* nobody is listening: forget the oldest */
    uint32_t at = (T.dirty_head + T.dirty_n++) % DIRTY_CAP;
    T.dirty[at].page = c->page; T.dirty[at].x = c->x; T.dirty[at].y = c->y; T.dirty[at].w = c->cw; T.dirty[at].h = c->ch;
}

int64_t st_atlas_dirty(int *page, int *x, int *y, int *w, int *h, const uint8_t **pixels, int *stride) {
    if (!init() || T.dirty_n == 0) return 0;
    uint32_t at = T.dirty_head;
    T.dirty_head = (T.dirty_head + 1) % DIRTY_CAP; T.dirty_n--;
    *page = T.dirty[at].page; *x = T.dirty[at].x; *y = T.dirty[at].y; *w = T.dirty[at].w; *h = T.dirty[at].h;
    *pixels = T.pages[T.dirty[at].page]; *stride = T.page_px;
    return 1;
}

/* ── Coverage to a distance field ─────────────────────────────────────────── */
/* One pass of the exact squared-distance transform along a line (Felzenszwalb
 * and Huttenlocher): f in, d out, both [n] long, [stride] apart. */
#define SDF_INF 1.0e20f
static void edt_line(float *grid, int offset, int stride, int n, float *f, float *z, int *v) {
    int k = 0;
    v[0] = 0; z[0] = -SDF_INF; z[1] = SDF_INF; f[0] = grid[offset];
    for (int q = 1; q < n; q++) {
        f[q] = grid[offset + q * stride];
        float s;
        do {
            int r = v[k];
            s = (f[q] - f[r] + (float)q * (float)q - (float)r * (float)r) / (float)(q - r) / 2.0f;
        } while (s <= z[k] && --k > -1);
        k++;
        v[k] = q; z[k] = s; z[k + 1] = SDF_INF;
    }
    k = 0;
    for (int q = 0; q < n; q++) {
        while (z[k + 1] < (float)q) k++;
        int r = v[k];
        grid[offset + q * stride] = f[r] + (float)(q - r) * (float)(q - r);
    }
}
static void edt(float *grid, int w, int h, float *f, float *z, int *v) {
    for (int x = 0; x < w; x++) edt_line(grid, x, w, h, f, z, v);
    for (int y = 0; y < h; y++) edt_line(grid, y * w, 1, w, f, z, v);
}

/* A glyph's coverage ([w] x [h], rows [pitch] apart) to a distance field
 * with a margin of [spread] on every side: 128 on the outline, more inside,
 * 127 a spread. A part-covered pixel places the outline within it. Returns
 * the field (malloc), (w + 2 spread) x (h + 2 spread), or NULL. */
static uint8_t *field_from(const uint8_t *cov, int w, int h, int pitch, int spread) {
    int W = w + 2 * spread, H = h + 2 * spread, big = W > H ? W : H;
    float *outer = malloc(sizeof(float) * (size_t)W * H * 2), *f = malloc(sizeof(float) * (size_t)big * 2 + sizeof(float) * 2);
    int *v = malloc(sizeof(int) * (size_t)big);
    uint8_t *out = malloc((size_t)W * H);
    if (!outer || !f || !v || !out) { free(outer); free(f); free(v); free(out); return NULL; }
    float *inner = outer + (size_t)W * H, *z = f + big;
    for (int i = 0; i < W * H; i++) { outer[i] = SDF_INF; inner[i] = 0.0f; }
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        float a = (float)cov[y * pitch + x] / 255.0f;
        int i = (y + spread) * W + x + spread;
        if (a >= 1.0f) { outer[i] = 0.0f; inner[i] = SDF_INF; }
        else if (a > 0.0f) { float d = 0.5f - a; outer[i] = d > 0.0f ? d * d : 0.0f; inner[i] = d < 0.0f ? d * d : 0.0f; }
    }
    edt(outer, W, H, f, z, v);
    edt(inner, W, H, f, z, v);
    for (int i = 0; i < W * H; i++) {
        float d = sqrtf(inner[i]) - sqrtf(outer[i]);
        float val = 128.0f + d * (127.0f / (float)spread);
        out[i] = (uint8_t)(val < 0.0f ? 0.0f : val > 255.0f ? 255.0f : val + 0.5f);
    }
    free(outer); free(f); free(v);
    return out;
}

int64_t st_atlas_dirty_peek(int *w, int *h) {
    if (!init() || T.dirty_n == 0) return 0;
    *w = T.dirty[T.dirty_head].w; *h = T.dirty[T.dirty_head].h;
    return 1;
}

static void gl_out(const Glyph *g, float out[8]) {
    out[0] = g->page; out[1] = g->x; out[2] = g->y; out[3] = g->w; out[4] = g->h; out[5] = g->left; out[6] = g->top; out[7] = 0.0f;
}

int64_t st_glyph(int64_t font, int64_t glyph, int64_t big, float out[8]) {
    if (!init() || !out) return 0;
    int fi = font_ok(font);
    int fine = big ? ST_BIG : 1, spread = ST_SPREAD * fine;
    uint32_t gid = (uint32_t)(glyph < 0 ? 0 : glyph) & 0x7fffffffu;
    Glyph *g = gl_find((uint32_t)fi, gid | (big ? 0x80000000u : 0u));
    memset(out, 0, 8 * sizeof(float));
    if (!g) { T.waited++; return 0; }
    if (g->state == 1) { g->used = T.frame; gl_out(g, out); return 1; }
    if (T.spent_us >= (double)T.budget_us) { T.waited++; return 0; }
    double t0 = now_us();
    FT_Face face = T.fonts[fi].face;
    int inked = 0;
    if (big) FT_Set_Pixel_Sizes(face, 0, ST_EM * ST_BIG);
    if (!FT_Load_Glyph(face, gid, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) && face->glyph->format == FT_GLYPH_FORMAT_OUTLINE && face->glyph->outline.n_points > 0)
        inked = !FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) && face->glyph->bitmap.width > 0 && face->glyph->bitmap.rows > 0
             && face->glyph->bitmap.pixel_mode == FT_PIXEL_MODE_GRAY;
    int ok = 1;
    uint8_t *field = NULL;
    if (inked) {
        FT_Bitmap *cov = &face->glyph->bitmap;
        field = field_from(cov->buffer, (int)cov->width, (int)cov->rows, cov->pitch < 0 ? -cov->pitch : cov->pitch, spread);
        if (!field) inked = 0;
    }
    if (!inked) { g->page = g->x = g->y = g->w = g->h = g->cw = g->ch = g->left = g->top = 0; }
    else {
        int w = (int)face->glyph->bitmap.width + 2 * spread, h = (int)face->glyph->bitmap.rows + 2 * spread;
        int cw = (w + 1 + CELL - 1) / CELL * CELL, ch = (h + 1 + CELL - 1) / CELL * CELL;     /* a clear pixel between neighbours */
        Cell c;
        if (!cell_take(cw, ch, &c)) ok = 0;
        else {
            uint8_t *page = T.pages[c.page];
            for (int row = 0; row < c.ch; row++) memset(page + (size_t)(c.y + row) * T.page_px + c.x, 0, (size_t)c.cw);
            for (int row = 0; row < h; row++) memcpy(page + (size_t)(c.y + row) * T.page_px + c.x, field + (size_t)row * (size_t)w, (size_t)w);
            g->page = c.page; g->x = c.x; g->y = c.y; g->w = (int16_t)w; g->h = (int16_t)h; g->cw = c.cw; g->ch = c.ch;
            g->left = (int16_t)(face->glyph->bitmap_left - spread); g->top = (int16_t)(face->glyph->bitmap_top + spread);
            dirty_push(&c);
        }
    }
    free(field);
    if (big) FT_Set_Pixel_Sizes(face, 0, ST_EM);
    T.spent_us += now_us() - t0;
    if (!ok) { T.waited++; return 0; }
    g->state = 1; g->used = T.frame; T.nlive++; T.rasterised++;
    gl_out(g, out);
    return 1;
}

/* ── Layout ───────────────────────────────────────────────────────────────── */
static int is_cjk(uint32_t c) {
    return (c >= 0x2e80 && c <= 0x9fff) || (c >= 0xac00 && c <= 0xd7af) || (c >= 0xf900 && c <= 0xfaff) || (c >= 0xff00 && c <= 0xffef) || (c >= 0x20000 && c <= 0x3ffff);
}
static int is_space(uint32_t c) { return c == ' ' || c == '\t' || c == 0x3000; }
static int is_mark(uint32_t c) {
    SBGeneralCategory k = SBCodepointGetGeneralCategory(c);
    return k == SBGeneralCategoryMN || k == SBGeneralCategoryMC || k == SBGeneralCategoryME;
}

/* The font of [font]'s list that has [cp]: the one the run is already in if
 * it has it and [stay] (a space or a mark belongs with what it follows),
 * then the font itself, then its fallbacks. The font itself if none has:
 * its missing-glyph box is drawn. */
static int font_for(int font, int running, int stay, uint32_t cp) {
    Font *f = &T.fonts[font];
    if (stay && running && FT_Get_Char_Index(T.fonts[running].face, cp)) return running;
    if (FT_Get_Char_Index(f->face, cp)) return font;
    for (int i = 0; i < f->nfb; i++)
        if (FT_Get_Char_Index(T.fonts[f->fb[i]].face, cp)) return f->fb[i];
    return font;
}

typedef struct { LGlyph *g; int n, cap; float *line_w; int nlines, line_cap; int failed; } Build;

static void build_glyph(Build *b, int font, uint32_t gid, float x, float y, int cluster, float adv) {
    if (b->n == b->cap) {
        int cap = b->cap ? b->cap * 2 : 64;
        LGlyph *grown = realloc(b->g, sizeof(LGlyph) * (size_t)cap);
        if (!grown) { b->failed = 1; return; }
        b->g = grown; b->cap = cap;
    }
    b->g[b->n++] = (LGlyph){ font, gid, x, y, b->nlines, cluster, adv };
}

static void build_line_end(Build *b, float width) {
    if (b->nlines == b->line_cap) {
        int cap = b->line_cap ? b->line_cap * 2 : 8;
        float *grown = realloc(b->line_w, sizeof(float) * (size_t)cap);
        if (!grown) { b->failed = 1; return; }
        b->line_w = grown; b->line_cap = cap;
    }
    b->line_w[b->nlines++] = width;
}

/* A paragraph being laid out: its bytes, and per byte the font and script of
 * the character it belongs to. */
typedef struct {
    const char *s; int n;          /* the paragraph, no newline in it */
    int base;                      /* its offset in the whole text: what clusters are counted from */
    uint8_t *font, *script;        /* per byte */
    SBParagraphRef para;
} Para;

/* Shape bytes [a, e) of the paragraph, one font, one script, one direction.
 * The buffer holds the glyphs in visual order, left to right. */
static void shape_run(const Para *p, int a, int e, int rtl) {
    hb_buffer_reset(T.hb);
    hb_buffer_add_utf8(T.hb, p->s, p->n, (unsigned)a, e - a);
    hb_buffer_set_direction(T.hb, rtl ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
    hb_buffer_set_script(T.hb, hb_script_from_iso15924_tag(SBScriptGetUnicodeTag(p->script[a])));
    hb_buffer_set_language(T.hb, hb_language_get_default());
    hb_shape(T.fonts[p->font[a]].hb, T.hb, NULL, 0);
    T.shapes++;
}

/* Walk bytes [ls, le) of the paragraph in visual order, as runs of one
 * direction, script and font, shaping each. With [b], place the glyphs as a
 * line, leaving out everything from byte [trim] on (the spaces at its end);
 * with [adv], add each glyph's advance to its cluster's first byte. */
static void visit_line(const Para *p, int ls, int le, int trim, Build *b, float *adv) {
    SBLineRef line = le > ls ? SBParagraphCreateLine(p->para, (SBUInteger)ls, (SBUInteger)(le - ls)) : NULL;
    float x = 0.0f;
    if (line) {
        const SBRun *runs = SBLineGetRunsPtr(line);
        SBUInteger nruns = SBLineGetRunCount(line);
        for (SBUInteger r = 0; r < nruns; r++) {
            int ra = (int)runs[r].offset, re = ra + (int)runs[r].length, rtl = runs[r].level & 1;
            /* pieces of one font and script; a right-to-left run's pieces are met last first */
            int at = rtl ? re : ra;
            while (rtl ? at > ra : at < re) {
                int a, e;
                if (rtl) { e = at; a = e - 1; while (a > ra && p->font[a - 1] == p->font[e - 1] && p->script[a - 1] == p->script[e - 1]) a--; at = a; }
                else { a = at; e = a + 1; while (e < re && p->font[e] == p->font[a] && p->script[e] == p->script[a]) e++; at = e; }
                shape_run(p, a, e, rtl);
                unsigned count = 0;
                hb_glyph_info_t *info = hb_buffer_get_glyph_infos(T.hb, &count);
                hb_glyph_position_t *pos = hb_buffer_get_glyph_positions(T.hb, &count);
                float upem = (float)T.fonts[p->font[a]].face->units_per_EM;
                for (unsigned i = 0; i < count; i++) {
                    int cl = (int)info[i].cluster;
                    float ga = (float)pos[i].x_advance / upem;
                    if (adv && cl >= 0 && cl < p->n) adv[cl] += ga;
                    if (b && cl < trim) {
                        build_glyph(b, p->font[a], info[i].codepoint, x + (float)pos[i].x_offset / upem, (float)pos[i].y_offset / upem, p->base + cl, ga);
                        x += ga;
                    }
                }
            }
        }
        SBLineRelease(line);
    }
    if (b) build_line_end(b, x);
}

/* Lay out one paragraph: mark fonts, scripts and directions, break it into
 * lines no wider than [wrap_em] (at spaces and around CJK characters; a
 * word that is too long breaks at a cluster), and place each line. */
static void place_paragraph(Build *b, int font, const char *s, int n, int base, float wrap_em) {
    if (n == 0) { build_line_end(b, 0.0f); return; }
    Para p = { s, n, base, malloc((size_t)n), malloc((size_t)n), NULL };
    uint32_t *cps = malloc(sizeof(uint32_t) * (size_t)n);
    float *adv = wrap_em > 0.0f ? calloc((size_t)n, sizeof(float)) : NULL;
    SBCodepointSequence seq = { SBStringEncodingUTF8, (void *)s, (SBUInteger)n };
    SBAlgorithmRef alg = SBAlgorithmCreate(&seq);
    p.para = alg ? SBAlgorithmCreateParagraph(alg, 0, (SBUInteger)n, SBLevelDefaultLTR) : NULL;
    SBScriptLocatorRef loc = SBScriptLocatorCreate();
    if (!p.font || !p.script || !cps || (wrap_em > 0.0f && !adv) || !p.para || !loc) b->failed = 1;
    else {
        /* per byte: the character's code point (at its first byte, 0 after), font and script */
        int running = 0;
        for (int i = 0; i < n;) {
            uint32_t cp = 0; int len = utf8_next((const unsigned char *)s, n, i, &cp);
            if (!len) { len = 1; cp = 0xfffd; }
            int f = font_for(font, running, is_space(cp) || is_mark(cp), cp);
            if (!is_space(cp)) running = f;
            for (int k = 0; k < len; k++) { p.font[i + k] = (uint8_t)f; cps[i + k] = k == 0 ? cp : 0; }
            i += len;
        }
        memset(p.script, SBScriptZYYY, (size_t)n);
        SBScriptLocatorLoadCodepoints(loc, &seq);
        const SBScriptAgent *agent = SBScriptLocatorGetAgent(loc);
        while (SBScriptLocatorMoveNext(loc))
            for (SBUInteger k = 0; k < agent->length && agent->offset + k < (SBUInteger)n; k++) p.script[agent->offset + k] = (uint8_t)agent->script;
        if (wrap_em <= 0.0f) {
            int trim = n;
            while (trim > 0 && (cps[trim - 1] == 0 ? 0 : is_space(cps[trim - 1]))) trim--;
            visit_line(&p, 0, n, trim, b, NULL);
        } else {
            visit_line(&p, 0, n, n, NULL, adv);             /* how wide each cluster is */
            int start = 0;
            while (start < n) {
                float w = 0.0f;
                int brk = -1, end = n;
                for (int i = start; i < n; i++) {
                    if (cps[i] == 0) continue;
                    int space = is_space(cps[i]);
                    /* a break is allowed before a character that follows a space, or with CJK on either side of the gap */
                    if (i > start && !space && adv[i] > 0.0f) {
                        int prev = i - 1; while (prev > start && cps[prev] == 0) prev--;
                        if (is_space(cps[prev]) || is_cjk(cps[prev]) || is_cjk(cps[i])) brk = i;
                    }
                    if (!space && adv[i] > 0.0f && w + adv[i] > wrap_em && i > start) { end = brk > start ? brk : i; break; }
                    w += adv[i];
                }
                int trim = end;
                while (trim > start && (cps[trim - 1] == 0 ? 0 : is_space(cps[trim - 1]))) trim--;
                visit_line(&p, start, end, trim, b, NULL);
                start = end;
                while (start < n && end < n && cps[start] != 0 && is_space(cps[start])) start++;    /* a wrapped line does not begin with the space it broke at */
            }
        }
    }
    if (loc) SBScriptLocatorRelease(loc);
    if (p.para) SBParagraphRelease(p.para);
    if (alg) SBAlgorithmRelease(alg);
    free(p.font); free(p.script); free(cps); free(adv);
}

static uint64_t fnv(const char *s, int64_t n, uint64_t h) { for (int64_t i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 0x100000001b3ull; } return h; }

/* The layout of [n] bytes in [font], lines wrapped at [wrap_em] ems and
 * aligned: glyph positions in ems from the block's top-left. Remembered. */
static const Layout *layout(int font, const char *s, int64_t n, float wrap_em, int align) {
    uint64_t h = fnv(s, n, 0xcbf29ce484222325ull ^ ((uint64_t)font << 40) ^ ((uint64_t)align << 34));
    h = fnv((const char *)&wrap_em, sizeof wrap_em, h);
    if (h == 0) h = 1;
    Layout *slot = NULL;
    for (int probe = 0; probe < 4; probe++) {
        Layout *l = &T.cache[(h + (uint64_t)probe) % LC_CAP];
        if (l->hash == h && l->font == font && l->n == (int)n && l->align == align && l->wrap_em == wrap_em && (n == 0 || memcmp(l->bytes, s, (size_t)n) == 0)) { l->used = ++T.tick; return l; }
        if (!slot || (slot->hash != 0 && (l->hash == 0 || l->used < slot->used))) slot = l;
    }
    T.layouts++;
    Build b = { 0 };
    int valid = n < (1 << 24) && st_utf8_count(s, n) >= 0;
    if (valid) {
        int64_t from = 0;
        for (int64_t i = 0; i <= n; i++) {
            if (i < n && s[i] != '\n') continue;
            int64_t to = i;
            if (to > from && s[to - 1] == '\r') to--;
            place_paragraph(&b, font, s + from, (int)(to - from), (int)from, wrap_em);
            from = i + 1;
        }
    }
    if (!valid || b.failed) { b.n = 0; b.nlines = 0; }
    if (b.nlines == 0) { free(b.line_w); b.line_w = NULL; b.line_cap = 0; build_line_end(&b, 0.0f); }
    float width = 0.0f;
    for (int i = 0; i < b.nlines; i++) if (b.line_w[i] > width) width = b.line_w[i];
    if (align > 0) for (int i = 0; i < b.n; i++) b.g[i].x += (width - b.line_w[b.g[i].line]) * (align == 1 ? 0.5f : 1.0f);
    free(slot->bytes); free(slot->g);
    slot->bytes = n > 0 ? malloc((size_t)n) : NULL;
    if (n > 0 && !slot->bytes) { slot->hash = 0; slot->g = NULL; free(b.g); free(b.line_w); return NULL; }
    if (n > 0) memcpy(slot->bytes, s, (size_t)n);
    slot->hash = h; slot->font = font; slot->n = (int)n; slot->align = align; slot->wrap_em = wrap_em;
    slot->g = b.g; slot->ng = b.n; slot->nlines = b.nlines ? b.nlines : 1; slot->width = width; slot->used = ++T.tick;
    free(b.line_w);
    return slot;
}

int64_t st_shape(int64_t font, const char *s, int64_t n, double size, float *out, int64_t cap) {
    if (!init() || !out || cap <= 0 || n <= 0 || !s || !(size > 0.0)) return 0;
    const Layout *l = layout(font_ok(font), s, n, 0.0f, 0);
    if (!l) return 0;
    int64_t count = l->ng < cap ? l->ng : cap;
    for (int64_t i = 0; i < count; i++) {
        float *o = out + i * 6;
        o[0] = (float)l->g[i].font; o[1] = (float)l->g[i].gid; o[2] = l->g[i].x * (float)size; o[3] = l->g[i].y * (float)size;
        o[4] = (float)l->g[i].cluster; o[5] = l->g[i].adv * (float)size;
    }
    return count;
}

int64_t st_text_bound(int64_t n) { return n > 0 ? 3 * n : 0; }

/* The block's size and where it sits: left and the first baseline, in pixels. */
typedef struct { float left, base, width, height, lh, asc; } Placed;

static Placed place(const Layout *l, int font, const float *p) {
    Placed r;
    float size = p[2];
    Font *f = &T.fonts[font];
    r.lh = floorf(f->lineh * size + 0.5f); if (r.lh < 1.0f) r.lh = 1.0f;
    r.asc = f->asc * size;
    r.width = l->width * size;
    r.height = r.lh * (float)l->nlines;
    float top = p[22] > 0.5f ? p[1] - r.asc : p[1] - p[21] * 0.5f * r.height;
    r.left = floorf(p[0] - p[20] * 0.5f * r.width + 0.5f);
    r.base = floorf(top + r.asc + 0.5f);
    return r;
}

int64_t st_measure(int64_t font, const char *s, int64_t n, const float *params, float out[4]) {
    if (!out) return 0;
    out[0] = out[1] = out[2] = out[3] = 0.0f;
    if (!init() || !params || n < 0 || (n > 0 && !s) || !(params[2] > 0.0f)) return 0;
    int fi = font_ok(font);
    const Layout *l = layout(fi, s, n, params[3] > 0.0f ? params[3] / params[2] : 0.0f, (int)(params[4] + 0.5f));
    if (!l) return 0;
    Placed pl = place(l, fi, params);
    out[0] = pl.width; out[1] = pl.height; out[2] = (float)l->nlines; out[3] = pl.asc;
    return 1;
}

int64_t st_text(int64_t font, const char *s, int64_t n, const float *params, float *out, int64_t cap) {
    if (!init() || !params || !out || cap <= 0 || n <= 0 || !s || !(params[2] > 0.0f)) return 0;
    int fi = font_ok(font);
    float size = params[2];
    const Layout *l = layout(fi, s, n, params[3] > 0.0f ? params[3] / size : 0.0f, (int)(params[4] + 0.5f));
    if (!l || l->ng == 0) return 0;
    Placed pl = place(l, fi, params);
    int big = size > ST_BIG_FROM;
    float scale = size / (float)(big ? ST_EM * ST_BIG : ST_EM);
    float reach = 2.0f * ST_SPREAD * size / (float)ST_EM;                /* pixels the field's whole range spans at this size */
    float outline = params[9] > 0.0f && params[13] > 0.0f ? params[9] : 0.0f;
    if (outline > 0.4f * reach) outline = 0.4f * reach;    /* the field reaches no further */
    int shadow = (params[14] != 0.0f || params[15] != 0.0f) && params[19] > 0.0f;
    int64_t written = 0;
    for (int pass = 0; pass < 3; pass++) {
        if ((pass == 0 && !shadow) || (pass == 1 && outline <= 0.0f)) continue;
        const float *rgba = pass == 0 ? params + 16 : pass == 1 ? params + 10 : params + 5;
        float dx = pass == 0 ? params[14] : 0.0f, dy = pass == 0 ? params[15] : 0.0f;
        float widen = pass == 1 ? outline : (pass == 0 ? outline : 0.0f);       /* an outlined text's shadow is the outline's shadow */
        for (int i = 0; i < l->ng && written < cap; i++) {
            float g[8];
            if (st_glyph(l->g[i].font, l->g[i].gid, big, g) != 1 || g[3] <= 0.0f) continue;
            float *q = out + written * ST_QUAD_FLOATS;
            q[0] = pl.left + l->g[i].x * size + g[5] * scale + dx;
            q[1] = pl.base + (float)l->g[i].line * pl.lh - l->g[i].y * size - g[6] * scale + dy;
            q[2] = q[0] + g[3] * scale; q[3] = q[1] + g[4] * scale;
            q[4] = g[1]; q[5] = g[2]; q[6] = g[1] + g[3]; q[7] = g[2] + g[4];
            q[8] = rgba[0]; q[9] = rgba[1]; q[10] = rgba[2]; q[11] = rgba[3];
            q[12] = 3.0f; q[13] = g[0]; q[14] = widen; q[15] = reach;
            written++;
        }
    }
    return written;
}

/* ── What March calls ─────────────────────────────────────────────────────── */
/* A March String reaches C as its runtime value: a pointer to a counted heap
 * string, or, for seven bytes or fewer, the bytes packed into the pointer
 * itself (march_runtime.h: march_string, MARCH_SSO_BIT). GAPS S16. */
#define NARR_HDR 32
static int64_t narr_len(void *a) { return *(int64_t *)((char *)a + 16); }
static void *narr_data(void *a) { return (char *)a + NARR_HDR; }

static const char *mstr(const void *p, char scratch[8], int64_t *len) {
    uintptr_t v = (uintptr_t)p;
    if ((v & ((uintptr_t)1 << 63)) && !(v & 1u)) {
        int64_t n = (int64_t)((v >> 59) & 0xF);
        uint64_t bits = (uint64_t)v >> 3;
        if (n > 7) n = 7;
        for (int64_t i = 0; i < n; i++) scratch[i] = (char)((bits >> (i * 8)) & 0xFF);
        scratch[n] = 0; *len = n;
        return scratch;
    }
    if (!p) { *len = 0; scratch[0] = 0; return scratch; }
    *len = *(const int64_t *)((const char *)p + 16);
    return (const char *)p + 24;
}

int64_t stm_utf8_count(void *s) { char sc[8]; int64_t n; const char *b = mstr(s, sc, &n); return st_utf8_count(b, n); }
int64_t stm_byte_count(void *s) { char sc[8]; int64_t n; mstr(s, sc, &n); return n; }
int64_t stm_font_default(void) { return st_font_default(); }
int64_t stm_font_load(void *path, int64_t face) { char sc[8]; int64_t n; const char *b = mstr(path, sc, &n); return st_font_load(b, n, face); }
/* [handles] is a NativeI32Arr of [n] font handles. */
int64_t stm_font_set_fallbacks(int64_t font, void *handles, int64_t n) {
    int64_t h[MAX_FALLBACKS];
    if (n < 0 || (n > 0 && (!handles || narr_len(handles) < n))) return 0;
    if (n > MAX_FALLBACKS) n = MAX_FALLBACKS;
    for (int64_t i = 0; i < n; i++) h[i] = ((int32_t *)narr_data(handles))[i];
    return st_font_set_fallbacks(font, h, n);
}
/* Write a text's quads into [quads] (a NativeF32Arr) from quad [at] on;
 * [params] is a NativeF32Arr of ST_PARAMS floats. Returns the quads written. */
int64_t stm_text(void *quads, int64_t at, int64_t font, void *s, void *params) {
    if (!quads || !params || at < 0 || narr_len(params) < ST_PARAMS) return 0;
    int64_t cap = narr_len(quads) / ST_QUAD_FLOATS - at;
    if (cap <= 0) return 0;
    char sc[8]; int64_t n; const char *b = mstr(s, sc, &n);
    return st_text(font, b, n, narr_data(params), (float *)narr_data(quads) + at * ST_QUAD_FLOATS, cap);
}
/* Measure into [out] (a NativeF32Arr of 4): width, height, lines, ascent. */
int64_t stm_measure(int64_t font, void *s, void *params, void *out) {
    if (!params || !out || narr_len(params) < ST_PARAMS || narr_len(out) < 4) return 0;
    char sc[8]; int64_t n; const char *b = mstr(s, sc, &n);
    return st_measure(font, b, n, narr_data(params), narr_data(out));
}
double stm_stat(int64_t which) { return st_stat(which); }
void stm_set_raster_budget(int64_t us) { st_set_raster_budget(us); }

/* ── SheenBidi, compiled in ───────────────────────────────────────────────── */
#include "sheenbidi/Source/SheenBidi.c"

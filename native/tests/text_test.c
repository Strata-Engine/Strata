/* Text without a GPU: UTF-8, fonts, the glyph atlas, layout.
 * Run from the repository root (it reads test/fonts). */
#include "../strata_text.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

static int fails = 0, checks = 0;
#define CHECK(c, what) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, what); } } while (0)

static int64_t count(const char *s) { return st_utf8_count(s, (int64_t)strlen(s)); }
static int64_t load(const char *p) { return st_font_load(p, (int64_t)strlen(p), 0); }
static void params(float *p, float size, float wrap) {
    memset(p, 0, ST_PARAMS * sizeof(float));
    p[2] = size; p[3] = wrap; p[5] = p[6] = p[7] = p[8] = 1.0f;
}
static int64_t text(int64_t font, const char *s, const float *p, float *out, int64_t cap) { return st_text(font, s, (int64_t)strlen(s), p, out, cap); }
static void measure(int64_t font, const char *s, const float *p, float out[4]) { st_measure(font, s, (int64_t)strlen(s), p, out); }
/* let every glyph of a string into the atlas, however many frames it takes */
static void warm(int64_t font, const char *s) {
    float p[ST_PARAMS], q[ST_QUAD_FLOATS * 256]; params(p, 48, 0);
    for (int i = 0; i < 200; i++) { st_frame_begin(); text(font, s, p, q, 256); if (st_stat(1) == 0) break; }
}

static void test_utf8(void) {
    CHECK(count("hello") == 5, "ascii counts bytes");
    CHECK(count("\xc3\xa9!") == 2, "a two-byte character is one");
    CHECK(count("\xf0\x9f\x98\x80") == 1, "a four-byte character is one");
    CHECK(count("") == 0, "empty is zero");
    CHECK(st_utf8_count("\xc3\x28", 2) == -1, "a bad continuation byte is invalid");
    CHECK(st_utf8_count("\xe4\xbd", 2) == -1, "a truncated sequence is invalid");
    CHECK(st_utf8_count("\xc0\xaf", 2) == -1, "an overlong form is invalid");
    CHECK(st_utf8_count("\xed\xa0\x80", 3) == -1, "a surrogate is invalid");
}

static void test_fonts(void) {
    CHECK(st_font_default() == 1, "the built-in font is handle 1");
    int64_t f = load("test/fonts/NotoSans-Latin.ttf");
    CHECK(f > 1, "a font file loads");
    CHECK(load("test/fonts/no-such-font.ttf") == 0, "a missing file fails");
    CHECK(load("test/fonts/NotoSans-Latin.ttf") == f, "loading the same file again gives the same handle");
    CHECK(load("test/fonts/OFL.txt") == 0, "a text file is not a font");
    /* the first 3000 bytes of a font: its table directory with most tables cut off */
    FILE *in = fopen("test/fonts/NotoSans-Latin.ttf", "rb"), *out = fopen("native/tests/bin/truncated.ttf", "wb");
    CHECK(in && out, "the truncated copy can be written");
    if (in && out) { char buf[3000]; size_t n = fread(buf, 1, sizeof buf, in); fwrite(buf, 1, n, out); }
    if (in) fclose(in);
    if (out) fclose(out);
    CHECK(load("native/tests/bin/truncated.ttf") == 0, "a truncated font fails");
    CHECK(st_font_glyph(f, 'A') > 0, "the font has an A");
    CHECK(st_font_glyph(f, 0x4f60) == 0, "the Latin font has no Chinese");
    CHECK(st_font_glyph(999, 'A') == st_font_glyph(1, 'A'), "a bad handle is the built-in font");
}

static void test_atlas(void) {
    st_atlas_config(2048, 4);
    st_set_raster_budget(1000000);
    st_frame_begin();
    int64_t f = st_font_default();
    float a[8], v[8], sp[8];
    double before = st_stat(4);
    CHECK(st_glyph(f, st_font_glyph(f, 'A'), 0, a) == 1, "A is drawn at once within the budget");
    CHECK(a[3] > 8 && a[4] > 8 && a[1] >= 0 && a[2] >= 0 && a[1] + a[3] <= 2048 && a[2] + a[4] <= 2048, "A has a rect inside the atlas");
    CHECK(st_glyph(f, st_font_glyph(f, 'V'), 0, v) == 1, "V is drawn");
    int apart = a[0] != v[0] || a[1] + a[3] <= v[1] || v[1] + v[3] <= a[1] || a[2] + a[4] <= v[2] || v[2] + v[4] <= a[2];
    CHECK(apart, "two glyphs' rects do not overlap");
    CHECK(st_stat(4) == before + 2, "two glyphs were rasterised");
    CHECK(st_glyph(f, st_font_glyph(f, 'A'), 0, a) == 1 && st_stat(4) == before + 2, "asking again rasterises nothing");
    CHECK(st_glyph(f, st_font_glyph(f, ' '), 0, sp) == 1 && sp[3] == 0 && sp[4] == 0, "a space is ready and has no ink");
    /* the field: an I is a plain stem. Its middle is inside, its corner outside. */
    float g[8];
    CHECK(st_glyph(f, st_font_glyph(f, 'I'), 0, g) == 1, "I is drawn");
    int page, x, y, w, h, stride; const uint8_t *px = NULL; int found = 0;
    while (st_atlas_dirty(&page, &x, &y, &w, &h, &px, &stride)) {
        if (x == (int)g[1] && y == (int)g[2]) {
            found = 1;
            int best = 0;
            for (int c = 0; c < (int)g[3]; c++) if (px[(y + (int)g[4] / 2) * stride + x + c] > best) best = px[(y + (int)g[4] / 2) * stride + x + c];
            CHECK(best > 140, "the field is above the edge value inside the stem");
            CHECK(px[(y + (int)g[4] / 2) * stride + x] < 96, "and below it beside the stem");
            CHECK(px[y * stride + x] < 96, "the field is below the edge value at the cell's corner");
        }
    }
    CHECK(found, "the new glyph is reported as changed");
    CHECK(st_atlas_dirty(&page, &x, &y, &w, &h, &px, &stride) == 0, "nothing is reported twice");
    /* large text has a finer picture of its own */
    float big[8];
    CHECK(st_glyph(f, st_font_glyph(f, 'A'), 1, big) == 1 && big[3] > 2.5f * a[3] && big[3] < 3.5f * a[3], "the fine picture is ST_BIG times the ordinary one");
    CHECK(st_glyph(f, st_font_glyph(f, 'A'), 0, v) == 1 && v[3] == a[3], "and the ordinary one is still there");
}

static void test_budget_and_pressure(void) {
    int64_t f = st_font_default();
    float g[8];
    st_atlas_config(2048, 4);
    st_set_raster_budget(0);
    st_frame_begin();
    CHECK(st_glyph(f, st_font_glyph(f, 'Q'), 0, g) == 0, "with no budget a new glyph waits");
    CHECK(st_stat(1) == 1, "and is counted as waiting");
    st_set_raster_budget(1000000);
    st_frame_begin();
    CHECK(st_glyph(f, st_font_glyph(f, 'Q'), 0, g) == 1, "it appears once a frame has a budget");
    CHECK(st_stat(1) == 0, "and nothing waits");

    /* a small atlas of one page: fill it over many frames, so old glyphs are dropped */
    st_atlas_config(256, 1);
    int ok = 1;
    for (int c = 0x21; c < 0x7f; c++) { st_frame_begin(); if (st_glyph(f, st_font_glyph(f, c), 0, g) != 1) ok = 0; }
    CHECK(ok, "a full page makes room by dropping glyphs not used this frame");
    CHECK(st_stat(2) > 0, "glyphs were evicted");
    /* no rect is ever held by two glyphs: every glyph still in the atlas, pairwise */
    st_frame_begin();
    float r[96][8]; int have[96], n = 0, clash = 0;
    double ras = st_stat(4);
    for (int c = 0x7e; c >= 0x21 && st_stat(4) == ras; c--) { have[n] = st_glyph(f, st_font_glyph(f, c), 0, r[n]) == 1 && st_stat(4) == ras; if (have[n]) n++; }
    for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++) {
        if (r[i][3] == 0 || r[j][3] == 0) continue;
        if (!(r[i][0] != r[j][0] || r[i][1] + r[i][3] <= r[j][1] || r[j][1] + r[j][3] <= r[i][1] || r[i][2] + r[i][4] <= r[j][2] || r[j][2] + r[j][4] <= r[i][2])) clash = 1;
    }
    CHECK(n >= 3 && !clash, "no rect is held by two glyphs");
    /* all in use in one frame: nothing may be dropped; with one page the newest wait */
    st_atlas_config(256, 1);
    st_frame_begin();
    int waited = 0; double ev = st_stat(2);
    for (int c = 0x21; c < 0x7f; c++) if (st_glyph(f, st_font_glyph(f, c), 0, g) != 1) waited++;
    CHECK(waited > 0 && st_stat(2) == ev, "glyphs used this frame are never evicted: the newest wait");
    /* with a second page allowed, the atlas grows instead */
    st_atlas_config(256, 2);
    st_frame_begin();
    waited = 0; int second = 0;
    for (int c = 0x21; c < 0x60; c++) { if (st_glyph(f, st_font_glyph(f, c), 0, g) != 1) waited++; else if (g[0] == 1) second = 1; }
    CHECK(second, "when every glyph is in use the atlas grows by a page");
    CHECK(st_stat(3) == 2 * 256 * 256, "and holds two pages' bytes");
    st_atlas_config(2048, 4);
}

static float right_edge(const float *q, int64_t n) { float m = -1e9f; for (int64_t i = 0; i < n; i++) if (q[i * 16 + 2] > m) m = q[i * 16 + 2]; return m; }
static float left_edge(const float *q, int64_t n) { float m = 1e9f; for (int64_t i = 0; i < n; i++) if (q[i * 16] < m) m = q[i * 16]; return m; }

static void test_layout(void) {
    int64_t f = st_font_default();
    float p[ST_PARAMS], q[ST_QUAD_FLOATS * 256], m[4], m2[4];
    st_atlas_config(2048, 4); st_set_raster_budget(1000000);
    warm(f, "Hello, world! a b The quick brown fox AVfi");
    st_frame_begin();
    params(p, 32, 0);
    CHECK(text(f, "Hello", p, q, 256) == 5, "five letters are five quads");
    CHECK(q[12] == 3.0f, "a glyph quad is kind 3");
    CHECK(q[16] > q[0] && q[32] > q[16], "glyphs run left to right");
    CHECK(text(f, "", p, q, 256) == 0, "an empty string draws nothing");
    CHECK(text(f, "   ", p, q, 256) == 0, "spaces draw nothing");
    CHECK(text(f, "\n\n", p, q, 256) == 0, "newlines draw nothing");
    CHECK(st_text(f, "ab\xc3\x28", 4, p, q, 256) == 0, "invalid UTF-8 draws nothing");
    CHECK(text(f, "Hello", p, q, 3) == 3, "never more quads than there is room for");
    measure(f, "", p, m);
    CHECK(m[0] == 0 && m[2] == 1 && m[1] > 30 && m[1] < 60, "an empty string is zero wide and one line high");
    st_measure(f, "ab\xc3\x28", 4, p, m);
    CHECK(m[0] == 0, "invalid UTF-8 measures zero wide");
    measure(f, "a b", p, m);
    params(p, 32, m[0] * 0.6f);
    measure(f, "a b", p, m2);
    CHECK(m2[2] == 2 && fabsf(m2[1] - 2 * m[1]) < 1.5f, "a width that fits one word gives two lines");
    params(p, 32, 0);
    measure(f, "a\nb", p, m2);
    CHECK(m2[2] == 2, "a newline forces a break");
    /* a long word breaks where it must, and every piece fits */
    measure(f, "abcdefghij", p, m);
    params(p, 32, m[0] * 0.35f);
    measure(f, "abcdefghij", p, m2);
    CHECK(m2[2] == 3 || m2[2] == 4, "a word longer than the width is broken");
    CHECK(m2[0] <= m[0] * 0.35f + 0.5f, "and no line is wider than the width");
    /* what measure says is what text draws */
    params(p, 40, 0);
    int64_t n = text(f, "The quick brown fox", p, q, 256);
    measure(f, "The quick brown fox", p, m);
    CHECK(n == 16, "sixteen inked letters");
    CHECK(fabsf((right_edge(q, n) - left_edge(q, n)) - m[0]) < 0.12f * 40 + 2.0f * ST_SPREAD * 40 / ST_EM, "the measured width is the drawn extent, give or take the letters' side bearings and the field's margin");
    /* right alignment: the right ends of both lines agree, and sit at the anchor */
    params(p, 32, 0); p[4] = 2; p[20] = 2; p[0] = 500;
    n = text(f, "a\nThe quick", p, q, 256);
    measure(f, "a\nThe quick", p, m);
    float pad = ST_SPREAD * 32.0f / ST_EM + 3.0f;
    CHECK(n == 9 && fabsf(q[2] - 500) < pad && fabsf(right_edge(q, n) - 500) < pad, "right alignment puts each line's right end on the anchor");
    /* outline and shadow add passes under the fill */
    params(p, 32, 0); p[9] = 1.5f; p[13] = 1.0f; p[14] = 2; p[15] = 2; p[19] = 0.5f;
    CHECK(text(f, "Hi", p, q, 256) == 6, "outline and shadow are two more passes");
    CHECK(q[13 + 0] == q[13 + 16 * 4] && q[0] > q[16 * 4] && q[14 + 32] > 0 && q[14 + 64] == 0, "shadow first (offset), then outline (widened), then the fill");
    /* a layout is worked out once */
    params(p, 32, 0);
    double lay = st_stat(6);
    text(f, "cache me", p, q, 256); text(f, "cache me", p, q, 256);
    CHECK(st_stat(6) == lay + 1, "the same text is laid out once");
    CHECK(st_text_bound(10) >= 30, "the bound covers three passes");
    /* a character no font has: the missing glyph's box, one per character, not a gap */
    params(p, 32, 0);
    warm(f, "a\xe4\xbd\xa0\xe5\xa5\xbd" "b");
    st_frame_begin();
    CHECK(text(f, "a\xe4\xbd\xa0\xe5\xa5\xbd" "b", p, q, 256) == 4, "characters no font has draw a box each");
    CHECK(q[16] > q[0] && q[32] > q[18] - 12 && q[48] > q[32], "and the line has no gap or overlap");
}

static void test_fallback(void) {
    int64_t latin = load("test/fonts/NotoSans-Latin.ttf"), sc = load("test/fonts/NotoSansSC-Sub.otf");
    CHECK(latin > 0 && sc > 0, "the fonts load");
    CHECK(st_font_set_fallbacks(latin, &sc, 1) == 1, "a fallback list is taken");
    float p[ST_PARAMS], q[ST_QUAD_FLOATS * 64], g1[8], g2[8];
    params(p, 32, 0);
    warm(latin, "a\xe4\xbd\xa0" "b");
    st_frame_begin();
    CHECK(text(latin, "a\xe4\xbd\xa0" "b", p, q, 64) == 3, "Latin with one Chinese character is three glyphs in one line");
    /* the middle glyph is the Chinese font's: its cell is that font's glyph for the character */
    st_glyph(sc, st_font_glyph(sc, 0x4f60), 0, g1); st_glyph(latin, 0, 0, g2);
    CHECK(q[16 + 4] == g1[1] && q[16 + 5] == g1[2], "the Chinese character comes from the fallback font");
    CHECK(!(q[16 + 4] == g2[1] && q[16 + 5] == g2[2]) , "not the missing-glyph box");
    CHECK(q[16] > q[0] && q[32] > q[16], "in reading order");
    /* CJK breaks between any two characters */
    float m[4], m2[4];
    params(p, 32, 0);
    st_measure(sc, "\xe4\xbd\xa0\xe5\xa5\xbd\xe4\xb8\x96\xe7\x95\x8c", 12, p, m);
    params(p, 32, m[0] * 0.55f);
    st_measure(sc, "\xe4\xbd\xa0\xe5\xa5\xbd\xe4\xb8\x96\xe7\x95\x8c", 12, p, m2);
    CHECK(m[2] == 1 && m2[2] == 2, "a CJK run breaks between characters");
}

static int64_t shape(int64_t font, const char *s, float *out, int64_t cap) { return st_shape(font, s, (int64_t)strlen(s), 100.0, out, cap); }

static void test_shaping(void) {
    int64_t latin = st_font_default(), arabic = load("test/fonts/NotoSansArabic-Sub.ttf");
    CHECK(arabic > 0, "the Arabic font loads");
    float g[6 * 32], a1[6], v1[6];
    /* kerning: AV is narrower set together than its letters set apart */
    CHECK(shape(latin, "AV", g, 32) == 2 && shape(latin, "A", a1, 1) == 1 && shape(latin, "V", v1, 1) == 1, "AV is two glyphs");
    CHECK(g[5] + g[11] < a1[5] + v1[5] - 1.0f, "AV is kerned");
    CHECK(shape(latin, "fi", g, 32) == 1, "fi is one glyph: a ligature");
    CHECK(shape(latin, "f i", g, 32) == 3, "f, space, i are three");
    /* Arabic: joined forms, right to left */
    const char *salam = "\xd8\xb3\xd9\x84\xd8\xa7\xd9\x85";
    float iso[6];
    CHECK(shape(arabic, salam, g, 32) == 4, "the word is four glyphs");
    CHECK(g[4] > g[10] && g[10] > g[16] && g[16] > g[22], "in visual order the clusters run backwards: right to left");
    CHECK(g[2] < g[8] && g[8] < g[14] && g[14] < g[20], "and the glyphs are placed left to right");
    CHECK(st_shape(arabic, salam, 2, 100.0, iso, 1) == 1 && iso[1] != g[19], "the first letter in the word is not its form standing alone: it is joined");
    /* a number inside a right-to-left line reads left to right */
    const char *count = "\xd8\xb9\xd8\xaf\xd8\xaf 120";
    int64_t n = shape(arabic, count, g, 32);
    float x1 = -1, x2 = -1, x0 = -1, xa = -1;
    for (int64_t i = 0; i < n; i++) { int c = (int)g[i * 6 + 4]; if (c == 7) x1 = g[i * 6 + 2]; if (c == 8) x2 = g[i * 6 + 2]; if (c == 9) x0 = g[i * 6 + 2]; if (c == 0) xa = g[i * 6 + 2]; }
    CHECK(n == 7, "three letters, a space, three digits");
    CHECK(x1 >= 0 && x1 < x2 && x2 < x0, "1, 2, 0 stay in that order, left to right");
    CHECK(xa > x0, "and the word's first letter is to the right of the number");
    /* nothing to shape */
    CHECK(shape(latin, "", g, 32) == 0, "an empty string shapes to nothing");
    CHECK(st_shape(latin, "a\xc3\x28", 3, 100.0, g, 32) == 0, "invalid UTF-8 shapes to nothing");
    /* the same line is shaped once */
    double before = st_stat(7);
    shape(latin, "shape me once", g, 32); shape(latin, "shape me once", g, 32);
    CHECK(st_stat(7) == before + 1, "the same line reaches HarfBuzz once");
    /* Hebrew and Devanagari at least come out whole */
    int64_t he = load("test/fonts/NotoSansHebrew-Sub.ttf"), hi = load("test/fonts/NotoSansDevanagari-Sub.ttf");
    CHECK(he > 0 && shape(he, "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d", g, 32) == 4 && g[4] > g[22], "Hebrew is right to left");
    n = hi > 0 ? shape(hi, "\xe0\xa4\xa8\xe0\xa4\xae\xe0\xa4\xb8\xe0\xa5\x8d\xe0\xa4\xa4\xe0\xa5\x87", g, 32) : 0;
    int notdef = 0; for (int64_t i = 0; i < n; i++) if (g[i * 6 + 1] == 0) notdef = 1;
    CHECK(n >= 3 && n <= 5 && !notdef, "Devanagari joins its conjunct: six characters, fewer glyphs, none missing");
    /* a long word still breaks without splitting a cluster: wrap the Devanagari word narrowly */
    float p[ST_PARAMS], m[4]; params(p, 32, 20);
    st_measure(hi, "\xe0\xa4\xa8\xe0\xa4\xae\xe0\xa4\xb8\xe0\xa5\x8d\xe0\xa4\xa4\xe0\xa5\x87", 18, p, m);
    CHECK(m[2] >= 2 && m[2] <= 4, "a narrow wrap breaks the word at clusters, not at every character");
}

static void test_review(void) {
    int64_t f = st_font_default();
    float g[8];
    /* no more new pixels in a frame than the renderer carries to the GPU in one */
    st_atlas_config(2048, 4); st_set_raster_budget(1000000000);
    st_frame_begin();
    int ready = 0, waited = 0;
    for (int c = 0x21; c < 0x7f; c++) { if (st_glyph(f, st_font_glyph(f, c), 1, g) == 1) ready++; else waited++; }
    long bytes = 0; int rects = 0, page, x, y, w, h, stride; const uint8_t *px;
    while (st_atlas_dirty(&page, &x, &y, &w, &h, &px, &stride)) { bytes += (long)w * h; rects++; }
    CHECK(bytes <= ST_UPLOAD_BYTES && rects <= ST_UPLOAD_RECTS, "a frame's new glyphs fit what one frame uploads");
    CHECK(ready > 10 && waited > 0, "the rest wait, however large the time budget");
    st_frame_begin();
    CHECK(st_glyph(f, st_font_glyph(f, 0x7e), 1, g) == 1, "and come on a later frame");
    /* the glyph table makes room for new glyphs however many it has seen */
    st_atlas_config(256, 1);
    for (int id = 1000; id < 51000; id++) { if (id % 16 == 0) st_frame_begin(); st_glyph(f, id, 0, g); }
    st_frame_begin();
    CHECK(st_glyph(f, st_font_glyph(f, 'A'), 0, g) == 1, "after fifty thousand glyphs have come and gone, a new one is still drawn");
    st_atlas_config(2048, 4); st_set_raster_budget(1000000);
    /* setting the same fallback list again forgets no layouts */
    int64_t sc = load("test/fonts/NotoSansSC-Sub.otf"), lat = load("test/fonts/NotoSans-Latin.ttf");
    float p[ST_PARAMS], m[4]; params(p, 20, 0);
    st_font_set_fallbacks(lat, &sc, 1);
    measure(f, "kept across a font call", p, m);
    double lay = st_stat(6);
    st_font_set_fallbacks(lat, &sc, 1);
    measure(f, "kept across a font call", p, m);
    CHECK(st_stat(6) == lay, "an unchanged fallback list keeps the layouts");
    /* what waited in the frame that just ended is still known after the next begins */
    st_set_raster_budget(0); st_atlas_config(2048, 4);
    st_frame_begin(); st_glyph(f, st_font_glyph(f, 'Z'), 0, g); st_frame_begin();
    CHECK(st_stat(1) == 0 && st_stat(8) == 1, "the last frame's waiting count outlives the frame");
    st_set_raster_budget(1000000);
}

int main(void) {
    test_utf8();
    test_fonts();
    test_atlas();
    test_budget_and_pressure();
    test_layout();
    test_fallback();
    test_shaping();
    test_review();
    printf("text_test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}

/* Strata text: fonts, a glyph atlas, and laying text out as quads for the 2D
 * layer. Knows nothing of Vulkan: the shim asks it what changed in the atlas
 * and uploads that. Everything here runs on the main thread. */
#ifndef STRATA_TEXT_H
#define STRATA_TEXT_H
#include <stdint.h>

#define ST_QUAD_FLOATS 16      /* a quad, as Strata.Ui lays it out */
#define ST_PARAMS 24           /* floats in a text call's parameters (see st_text) */
#define ST_EM 48               /* pixels to the em at which glyphs are stored */
#define ST_SPREAD 8            /* how far the stored distance field reaches, in those pixels */
#define ST_BIG 3               /* text larger than ST_BIG_FROM pixels is stored ST_BIG times finer: big letters keep their corners */
#define ST_BIG_FROM 80.0f

/* Code points in [n] bytes of UTF-8, or -1 if they are not valid UTF-8. */
int64_t st_utf8_count(const char *s, int64_t n);

/* Fonts. A handle is 1 or more; 0 is failure. Handle 1 is the built-in font. */
int64_t st_font_default(void);
int64_t st_font_load(const char *path, int64_t nbytes, int64_t face_index);
int64_t st_font_load_mem(const unsigned char *bytes, int64_t n, int64_t face_index);   /* the bytes must outlive the font */
/* Where [font] lacks a character, take it from the first of these that has it. */
int64_t st_font_set_fallbacks(int64_t font, const int64_t *handles, int64_t n);
/* The glyph a font has for a code point, 0 if it has none. */
int64_t st_font_glyph(int64_t font, int64_t codepoint);

/* The atlas. Pages are square, [page_px] on a side; at most [max_pages].
 * Calling this empties it. Default 2048 and 4: 16 MB. */
void st_atlas_config(int64_t page_px, int64_t max_pages);
/* A glyph's place in the atlas: out = page, x, y, w, h (atlas pixels), then
 * left and top bearing in those pixels. [big] 0 is the ordinary picture
 * (ST_EM pixels to the em), 1 the fine one for large text (ST_EM * ST_BIG). Rasterises it on first use. Returns 1
 * when it can be drawn, 0 while it waits (the frame's time for rasterising is
 * spent, or the atlas has no room). A glyph with no ink is 1 with w = h = 0. */
int64_t st_glyph(int64_t font, int64_t glyph, int64_t big, float out[8]);
/* One rectangle of the atlas that changed since last asked, oldest first:
 * 1 and the rectangle, or 0 when there is none. [pixels] is the page's first
 * byte; rows are [stride] apart. */
int64_t st_atlas_dirty(int *page, int *x, int *y, int *w, int *h, const uint8_t **pixels, int *stride);
/* The size of the rectangle st_atlas_dirty would hand out next: 1, or 0 when there is none. */
int64_t st_atlas_dirty_peek(int *w, int *h);
int64_t st_atlas_page_px(void);
int64_t st_atlas_max_pages(void);

/* A frame begins: the time for rasterising is whole again, and glyphs used
 * from now on count as used this frame (they are not evicted). */
void st_frame_begin(void);
void st_set_raster_budget(int64_t microseconds);       /* a frame; default 1000 */
/* 0 glyphs in the atlas, 1 glyphs that waited since the last frame began,
 * 2 evictions so far, 3 atlas bytes held, 4 glyphs rasterised so far,
 * 5 microseconds spent rasterising this frame, 6 layouts worked out so far
 * (a cached one is not counted), 7 runs HarfBuzz has shaped so far. */
double st_stat(int64_t which);

/* Lay out [n] bytes of UTF-8 and write its glyphs as quads, in device pixels.
 * params (ST_PARAMS floats):
 *   0 x, 1 y        the point the block is placed by
 *   2 size          pixels to the em
 *   3 wrap          wrap lines to this width; 0: only at newlines
 *   4 align         lines within the block: 0 left, 1 centre, 2 right
 *   5..8            colour, rgba
 *   9 outline       outline width in pixels, 0: none;  10..13 its colour
 *   14, 15          shadow offset, both 0: none;       16..19 its colour
 *   20, 21          which point of the block sits at (x, y): 0, 1, 2 = left,
 *                   middle, right; and top, middle, bottom
 *   22              1: y is the first line's baseline, not the block's top
 * Returns the quads written (never more than [cap]). */
int64_t st_text(int64_t font, const char *s, int64_t n, const float *params, float *out, int64_t cap);
/* The same layout, measured: out = width, height, lines, first line's ascent. */
int64_t st_measure(int64_t font, const char *s, int64_t n, const float *params, float out[4]);
/* A line as shaped: per glyph six floats, in visual order, left to right:
 * font handle, glyph index, x and y (pixels from the line's left end and
 * baseline), cluster (the byte its characters start at), advance. Text is
 * put in visual order (the Unicode bidirectional algorithm), split into runs
 * of one direction, script and font, and each run shaped by HarfBuzz.
 * Returns the glyph count, at most [cap]. */
int64_t st_shape(int64_t font, const char *s, int64_t n, double size, float *out, int64_t cap);
/* Quads st_text can write for [n] bytes at most. */
int64_t st_text_bound(int64_t n);

#endif

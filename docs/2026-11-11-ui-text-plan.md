# Strata Interface and Text Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A 2D layer drawn over the 3D frame, and text from real fonts that is sharp at any size and correct in most of the world's scripts, enough to build a game's display on.

**Architecture:** The game builds a list of quads each frame through a pure March module, `Strata.Ui`; the shim copies the list to a per-frame buffer and draws it in one call after the spray. Text is ordered with SheenBidi, shaped with HarfBuzz and rasterised with FreeType into a distance-field atlas that fills on demand; all three live in a C file of their own, `native/strata_text.c`, which knows nothing of Vulkan and is tested without a GPU. March sees font handles (small integers), numbers and arrays.

**Tech Stack:** March (origin/main via `scripts/env.sh`), C, Vulkan 1.2 on MoltenVK, FreeType 2.15, HarfBuzz 15, SheenBidi (vendored), GLSL via `native/build_shaders.sh`.

**Spec:** `docs/2026-11-11-ui-text-design.md`

## Global Constraints

- Work on a new branch `interface` cut from `fluids`. Commit after each task.
- `scripts/verify.sh` prints PASS at the end of every task. It must not show a window: every scripted run uses `STRATA_HIDDEN=1 STRATA_OFFSCREEN=1 STRATA_PIXELS=1`.
- No font, glyph or text type crosses into March: handles are `Int`, everything else is numbers, strings and native arrays.
- Each frame in flight keeps its own copy of what the CPU writes (the quad list).
- After any shader edit run `native/build_shaders.sh` (it stamps `strata_vk.c`; GAPS S14).
- Nothing shared between worker tasks is passed through a hot loop (GAPS S15). All text work is on the main thread.
- Golden images are looked at before they are stored. Existing goldens must still pass unchanged: with no `Strata.Ui` calls a frame is byte-for-byte what it is today and issues the same draw count.
- Test fonts are open-licence subsets kept under `test/fonts/` with their licence file. The gate never reads a system font.
- FreeType and HarfBuzz come from Homebrew (`-I/opt/homebrew/include/freetype2`, `-I/opt/homebrew/include/harfbuzz`, `-lfreetype -lharfbuzz`); SheenBidi's sources are vendored under `native/sheenbidi/` with its licence. Strata does not link ICU.
- Interface pixels: at scale 1 one interface pixel is one window point. Scale may be fractional; rectangles, frame seams and glyph origins snap to device pixels.
- Targets (spec): display GPU 0.2 ms; CPU 0.2 ms for 200 elements and 500 glyphs of unchanged text; 1 draw call added; at most 1 ms of rasterising in a frame; atlas 16 MB.

## Review Focus

1. A string that is not valid UTF-8, is empty, or holds only spaces or newlines: no crash, nothing drawn, a measured size of zero width.
2. A character no font in the fallback list has: one "missing" box per character, never a gap in the line or a crash.
3. The glyph atlas full while every glyph in it is on screen this frame: text still draws (the atlas grows or the newest glyphs wait), nothing is drawn with another glyph's pixels.
4. More quads in a frame than the quad buffer holds: the extra are dropped from the end and counted, never written past the buffer.
5. A font file that is missing, truncated, or not a font: `font` answers a failure the game can see; drawing with a bad handle draws the default font.

---

### Task 1: How a string reaches C, and the dependencies

**Files:** `native/strata_text.c` (new), `native/strata_text.h` (new), `native/tests/text_test.c` (new), `native/sheenbidi/` (vendored), `lib/strata/ffi/text.march` (new), `test/text_ffi_test.march` (new), `viewer/forge.toml`, `demo/forge.toml`, `scripts/verify.sh`, `GAPS.md`

**Interfaces — Produces:**
```
C     int64_t st_utf8_count(const char *s, int64_t nbytes)      -- code points, or -1 for invalid UTF-8
March Strata.Ffi.Text.utf8_count(s : String) : Int
```

- [ ] **Step 1:** Find how a March `String` crosses to a C `extern` on March main (read `stdlib/` for an extern that takes one; `specs/` FFI notes). If it arrives as pointer and length, use that. If not, the crossing is a `NativeU8Arr` of the string's UTF-8 bytes built once per distinct string; record which in `GAPS.md` as S16.
- [ ] **Step 2: failing C test** in `text_test.c`: `st_utf8_count` gives 5 for "hello", 2 for "é!" , 1 for a four-byte emoji, 0 for "", -1 for the bytes C3 28 and for a truncated sequence.
- [ ] **Step 3: failing March test**: `utf8_count("héllo")` is 5.
- [ ] **Step 4:** Implement. Add `native/strata_text.c` to the `[ffi]` sources and the FreeType and HarfBuzz flags to `viewer/forge.toml`; vendor SheenBidi and compile it into `strata_text.c` by inclusion. `scripts/verify.sh` compiles `text_test.c` with the same flags.
- [ ] **Step 5:** Tests pass; `scripts/verify.sh` PASS. Commit `feat(ui): strings cross to C; FreeType, HarfBuzz and SheenBidi are linked`.

### Task 2: The 2D layer — rectangles and block icons

**Files:** `lib/strata/ui.march` (new), `test/ui_test.march` (new), `native/strata_vk.c`, `native/shaders/ui.vert`, `native/shaders/ui.frag` (new), `native/build_shaders.sh`, `lib/strata/ffi/vk.march`, `lib/strata/render.march`, `viewer/lib/strata_viewer.march`, `scripts/golden.sh`

**Interfaces — Produces:**
```
type Strata.Ui.Ui                                   -- this frame's display: a quad list and the screen it is laid out on
Strata.Ui.top_left() .. bottom_right() : Int        -- nine anchors, 0..8, row-major
Strata.Ui.begin(width_pt : Float, height_pt : Float, scale : Float, device_scale : Float) : Ui
Strata.Ui.rect(u : Ui, anchor : Int, x : Float, y : Float, w : Float, h : Float, rgba : (Float, Float, Float, Float)) : Ui
Strata.Ui.icon(u : Ui, anchor : Int, x : Float, y : Float, size : Float, block_texture : Int) : Ui
Strata.Ui.quad_count(u : Ui) : Int
Strata.Ui.quads(u : Ui) : NativeF32Arr              -- 12 floats a quad: x0 y0 x1 y1 (device pixels), u0 v0 u1 v1, r g b a; kind and layer packed in a 13th
Strata.Render.draw_ui(u : Ui) : Bool                -- hands the list to the renderer for the next frame
C     int64_t sv_ui_set(void *floats, int64_t nquads)
C     sv_stat(9): quads dropped this frame for want of room
```
An element's (x, y) is its offset from the anchor in interface pixels, positive right and down; the element's own matching corner or centre sits at that point (a bottom-centre element is placed by its bottom-centre). Quad capacity 16 384.

- [ ] **Step 1: failing March tests:** each of the nine anchors places a 10 x 10 rect where expected on an 800 x 600 screen; scale 2 doubles sizes and offsets; scale 1.5 with device scale 1 yields edges on whole device pixels; `quad_count` counts; quads come out in call order.
- [ ] **Step 2:** Implement `Strata.Ui` (pure).
- [ ] **Step 3:** Shim: a per-frame-in-flight host-visible buffer of 16 384 quads; `sv_ui_set` copies and clamps (Review Focus 4, counted in `sv_stat(9)`); pipeline kind 7 (blend, no depth) drawn last in the final pass, one instanced draw, only when the list is not empty. `ui.frag` kinds: 0 plain colour, 1 block texture layer.
- [ ] **Step 4:** Viewer: a crosshair at centre and a hotbar of nine block icons at bottom-centre, the chosen one framed, number keys 1 to 9 choosing; `STRATA_UI=0` turns the display off; scripted runs default to off so existing goldens are unchanged.
- [ ] **Step 5: goldens** (`STRATA_UI=1`): the display over the overview at scale 1, 1.5 and 2 (three images; look at each: same layout, larger, no blurred edges). A scene that asks for 20 000 quads draws 16 384 and reports 3 616 dropped.
- [ ] **Step 6:** `scripts/verify.sh` PASS. Commit `feat(ui): a 2D layer — rectangles and block icons by anchor, one draw`.

### Task 3: Images and frames

**Files:** `lib/strata/ui.march`, `test/ui_test.march`, `native/strata_vk.c`, `native/shaders/ui.frag`, `lib/strata/ffi/vk.march`, `lib/strata/render.march`, `viewer/lib/strata_viewer.march`, `viewer/lib/textures.march`

**Interfaces — Produces:**
```
Strata.Render.ui_atlas(width : Int, height : Int, rgba : NativeU8Arr) : Bool      -- one image holding all of a game's interface art
type Strata.Ui.Region = (Int, Int, Int, Int)                                        -- x, y, w, h in atlas pixels
Strata.Ui.image(u, anchor, x, y, w, h, region : Region, rgba) : Ui
Strata.Ui.frame(u, anchor, x, y, w, h, region : Region, border : Int, rgba) : Ui    -- nine-slice: nine quads
```

- [ ] **Step 1: failing March tests:** `frame` emits nine quads whose outer corners keep `border` atlas pixels at their size and whose middle stretches; a frame smaller than twice its border clamps without negative sizes; seams fall on whole device pixels at scale 1.5.
- [ ] **Step 2:** Implement; shim binding for the atlas (one more sampler in the descriptor set; a 1 x 1 white default); `ui.frag` kind 2.
- [ ] **Step 3:** Viewer: the hotbar's slots and the chosen-slot frame come from a small painted atlas.
- [ ] **Step 4: goldens** re-stored for the three display scenes after looking; a scene of one frame at five sizes.
- [ ] **Step 5:** `scripts/verify.sh` PASS. Commit `feat(ui): an interface atlas, images and nine-slice frames`.

### Task 4: FreeType and the glyph atlas

**Files:** `native/strata_text.c`, `native/strata_text.h`, `native/tests/text_test.c`, `native/strata_vk.c`, `native/shaders/ui.frag`, `test/fonts/` (new), `lib/strata/ffi/text.march`, `lib/strata/ui.march`, `viewer/lib/strata_viewer.march`

**Interfaces — Produces:**
```
C     int64_t st_font_load(path, nbytes, face_index)        -- a handle >= 1, or 0 on failure
C     int64_t st_font_default(void)                         -- handle of the built-in font
C     atlas: 2048 x 2048 single-channel distance field, glyphs stored at 48 px with an 8 px spread, shelf-packed
C     int64_t st_glyph(font, glyph_index, float out[8])     -- atlas rect and bearing; rasterises on first use; 0 while it waits
C     int64_t st_atlas_dirty(int *x, int *y, int *w, int *h, const uint8_t **pixels)   -- what changed since last asked
March Strata.Ui.font(path : String, face : Int, fallbacks : List(Int)) : Int       -- 0 on failure
March Strata.Ui.default_font() : Int
March Strata.Ui.text(u, anchor, x, y, font : Int, size : Float, rgba, s : String) : Ui
```
In this task `text` places glyphs by each glyph's own advance, left to right, with no kerning: Latin only. The default font is Noto Sans Regular, Latin subset, embedded in the shim as bytes (SIL Open Font Licence; the licence file goes in `native/`).

- [ ] **Step 1: failing C tests:** loading the test font gives a handle and loading a missing path, a truncated file and a text file each give 0 (Review Focus 5); the glyph for "A" has a non-empty rect inside the atlas and its field is above the edge value inside the stem and below it outside; two glyphs' rects do not overlap; asking for the same glyph twice rasterises once.
- [ ] **Step 2:** Implement the atlas and FreeType rasterising (FreeType's own distance-field renderer).
- [ ] **Step 3:** Shim: the atlas as a texture updated from `st_atlas_dirty` before the frame's draws; `ui.frag` kind 3 turns the field into coverage with a screen-space derivative so the edge is one pixel wide at any size.
- [ ] **Step 4:** Viewer: a readout at top-left (frame rate, position, light mode) in the default font.
- [ ] **Step 5: goldens:** a text sheet, one line each at 8, 10, 12, 16, 24, 48, 96 and 200 pixels; look for blur, shimmer between two frames (compare frames 58 and 59: identical), and rounded corners at 200. If corners round visibly, record it and switch to a multi-channel field before going on (the spec's first risk). If 8 and 10 pixel text is thin, small sizes get plain rasterised glyphs (the second risk). Record which in the ledger.
- [ ] **Step 6:** `scripts/verify.sh` PASS. Commit `feat(ui): text — FreeType, a distance-field atlas, sharp at any size`.

### Task 5: HarfBuzz, direction, and a cache

**Files:** `native/strata_text.c`, `native/tests/text_test.c`, `lib/strata/ffi/text.march`, `lib/strata/ui.march`, `test/ui_test.march`

**Interfaces — Produces:**
```
C     int64_t st_shape(font, utf8, nbytes, float size, float *out, int64_t cap)
      -- per glyph, 6 floats: font handle, glyph index, x, y (pixels from the line's origin), cluster, advance.
      -- Returns the glyph count; text is ordered by SheenBidi, split into runs by direction and script, shaped by HarfBuzz.
C     a cache of shaped lines keyed by (font, size, bytes), 1024 entries, least recently used dropped
```

- [ ] **Step 1: failing C tests** with the test fonts: "AV" is narrower shaped than the sum of its advances (kerning); "fi" in a font with that ligature is one glyph; the Arabic word سلام yields its joined forms, four glyphs right to left with decreasing clusters; "عدد 120" keeps 1, 2, 0 in that left-to-right order; invalid UTF-8 and "" give 0 glyphs (Review Focus 1); shaping the same line twice calls HarfBuzz once.
- [ ] **Step 2:** Implement. `Strata.Ui.text` now draws from `st_shape`.
- [ ] **Step 3: goldens:** kerning and ligature pairs beside their unkerned forms; the text sheet re-stored.
- [ ] **Step 4:** `scripts/verify.sh` PASS. Commit `feat(ui): shaped text — HarfBuzz, mixed direction, a line cache`.

### Task 6: Fallback fonts and the atlas under pressure

**Files:** `native/strata_text.c`, `native/tests/text_test.c`, `test/fonts/`, `viewer/lib/strata_viewer.march`, `scripts/golden.sh`

**Interfaces — Produces:**
```
C     st_font_set_fallbacks(font, const int64_t *handles, int64_t n)
C     st_set_raster_budget(microseconds)        -- default 1000 a frame
C     st_frame_begin(void)                      -- starts a frame: resets the budget, marks nothing as used yet
C     st_stat(0..3): glyphs in the atlas, glyphs waiting, evictions, atlas bytes
```
A character the font lacks is taken from the first fallback that has it and the run is split there; one no font has draws the first font's "missing" glyph (Review Focus 2). A glyph not yet rasterised when the frame's budget is spent draws nothing this frame and is first in line next frame. A full atlas drops glyphs unused for the most frames; one used this frame is never dropped; if every glyph is in use the atlas grows by a second page (Review Focus 3), up to 16 MB, beyond which new glyphs wait.

- [ ] **Step 1: failing C tests:** Latin text with one Chinese character shapes to two fonts' glyphs in one line; a private-use character gives the missing glyph; with a budget of zero a new glyph is reported waiting and appears after `st_frame_begin` with a budget; filling a 256 x 256 test atlas evicts the oldest and never one used this frame; with all in use a second page is made; no glyph's rect is ever handed to two glyphs.
- [ ] **Step 2:** Implement.
- [ ] **Step 3: goldens:** a line each of Chinese, Japanese, Korean, Arabic, Hebrew and Devanagari, and the Arabic line with a number, from the fonts in `test/fonts/` (Noto subsets holding just those characters). A native reader's check is not available: compare each line against the same text set by macOS (a screenshot from TextEdit kept in `docs/` as the reference) and record any difference.
- [ ] **Step 4:** Measure the first frame of the six-script scene: rasterising time against the 1 ms budget, and how many frames until nothing waits.
- [ ] **Step 5:** `scripts/verify.sh` PASS. Commit `feat(ui): fallback fonts; an atlas that fills, evicts and grows`.

### Task 7: Measuring, wrapping, styles

**Files:** `native/strata_text.c`, `native/tests/text_test.c`, `lib/strata/ffi/text.march`, `lib/strata/ui.march`, `test/ui_test.march`, `native/shaders/ui.frag`

**Interfaces — Produces:**
```
Strata.Ui.measure(font : Int, size : Float, s : String, wrap : Float) : (Float, Float, Int)     -- width, height, lines; wrap 0 is no wrapping
type Strata.Ui.Style                                   -- built with:
Strata.Ui.style() : Style
Strata.Ui.with_outline(st, px : Float, rgba) ; with_shadow(st, dx, dy, rgba) ; with_align(st, left|centre|right) ; with_wrap(st, width)
Strata.Ui.text_styled(u, anchor, x, y, font, size, rgba, st : Style, s : String) : Ui
Strata.Ui.text_runs(u, anchor, x, y, st : Style, runs : List((Int, Float, (Float, Float, Float, Float), String))) : Ui   -- font, size, colour, text: one line of several runs on a shared baseline
```
Lines break at spaces and newlines and after Chinese, Japanese and Korean characters; a word longer than the width breaks at a cluster boundary. Line height is the font's ascent plus descent plus line gap at that size.

- [ ] **Step 1: failing tests:** `measure` of "" is zero wide and one line high; of "a b" at a width that fits one word is two lines; a newline forces a break; a CJK run breaks between any two characters; a long word breaks without splitting a cluster; right alignment puts each line's right edge on the anchor; `text_runs` puts runs of different sizes on one baseline; the width `measure` gives equals the extent of the quads `text` emits, within a pixel.
- [ ] **Step 2:** Implement. Outline and shadow are extra passes over the same glyph quads in `ui.frag` (the field already holds the distance).
- [ ] **Step 3: goldens:** one paragraph wrapped at three widths; left, centre and right alignment; outline and shadow over bright and dark ground; a line of three runs. The viewer's readout gains an outline.
- [ ] **Step 4:** `scripts/verify.sh` PASS. Commit `feat(ui): measuring, wrapping, alignment, outline and shadow, mixed runs`.

### Task 8: Soak, measurements, documentation

**Files:** `scripts/soak.sh`, `viewer/lib/strata_viewer.march`, `RESULTS.md`, `README.md`, `GAPS.md`, `docs/`

- [ ] **Step 1:** The soak runs with the display on and a readout that changes every frame; it asserts no quads dropped, no glyphs left waiting at the end, and atlas bytes at or under 16 MB.
- [ ] **Step 2:** `RESULTS.md`: every row of the design's target table, met or missed, with the cause where missed. CPU time for 200 elements and 500 glyphs is measured in a scripted scene built for it, with unchanged text and with text that changes every frame.
- [ ] **Step 3:** README: what the layer does, the calls a game makes each frame, the limits (no widgets, no ICU line breaking, whole-list rebuilt each frame). Screenshots of the viewer's display and the script sheet into `docs/`.
- [ ] **Step 4:** `scripts/verify.sh` PASS. Commit `docs(ui): results, README, soak with the display on`.

---

## Out of scope for this plan

Menus, buttons, sliders, lists, text entry, dragging, a layout engine, language-aware line breaking and hyphenation, rich text markup, colour emoji, vertical text, text placed in the 3D world, an interface editor.

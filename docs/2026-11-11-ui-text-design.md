# Strata interface and text: design

Status: design, awaiting approval. Nothing here is built.
Date: 2026-11-11
Follows: `2026-11-10-strata-design.md`. A new project, beside the four the
first design named.

## Intent

Strata draws a world and nothing over it. A game needs a display while it
is played: a crosshair, a hotbar, numbers, labels, bars. This project gives
Strata a 2D layer drawn over the finished frame, and text good enough to
build a whole game's interface on.

## Decisions the owner made

1. The first use is an interface during gameplay, not menus.
2. Text support must be good.
3. Text is built on FreeType and HarfBuzz, as the large engines do it.
4. From the first design: the library names no game concept; frame rate
   stays the priority.

Everything else below is the designer's call.

## Goals

- Rectangles, images and text drawn over the 3D frame, in one batch, with
  a stated cost.
- The game describes its whole display fresh each frame. Nothing is kept
  between frames, so no interface state can fall out of step with the game.
- Text from real font files (TrueType, OpenType), sharp at any size, with
  outlines and shadows so it reads over a busy world.
- Text laid out properly: kerning and ligatures, line height, alignment,
  wrapping to a width, and measuring before drawing.
- Text in most of the world's scripts, including Chinese, Japanese and
  Korean, and scripts that need shaping (Arabic, Hebrew, Devanagari), with
  fallback fonts when the main font lacks a character.
- Right-to-left lines that hold numbers or Latin words read correctly: a
  health count in an Arabic display is not drawn backwards.
- More than one colour or font in a line.
- The same look on a retina display and an ordinary one, and at any
  window size.
- Icons from the block textures a game already supplied, and from images
  of its own.

## Non-goals

- Menus, buttons, sliders, lists, text entry, dragging: anything a pointer
  acts on. The layer is built so those can sit on top of it later.
- A layout engine. A game places things by anchor and offset; Strata does
  not solve constraints.
- Language-aware line breaking (Thai, hyphenation). That needs ICU.
- Rich text markup, emoji in colour, vertical text.
- Text or interface elements placed in the 3D world (name tags over
  entities). The pieces here would serve it; it is not designed here.
- An editor for interfaces.

## Anti-goals

- Blurry or shimmering text at any size the game asks for.
- A hitch the first time a new character is drawn.
- A draw call per element, or per-frame work that grows with anything but
  what is on screen.
- An interface that is a different size on a different display.
- Font or text types crossing into March.

## Constraints

- March and the existing C shim. FreeType and HarfBuzz are linked into
  the shim; March sees handles (small integers) and plain values.
- A third, small library orders mixed-direction text before it is
  shaped (designer's call: SheenBidi, a few source files with no
  dependencies, kept in `native/` beside the shim; it is not ICU).
  HarfBuzz does not do this, and without it "120" in an Arabic line is
  drawn "021".
- FreeType and HarfBuzz come from Homebrew, like GLFW and the Vulkan
  loader. FreeType is installed on the development machine; HarfBuzz is
  not and must be installed (`brew install harfbuzz`) before this is
  built. A consumer's `forge.toml` gains their link flags.
- Strata ships one default font, under a licence that allows it (the plan
  names it), so a game has text before it supplies any font.
- `scripts/verify.sh` stays green. Golden images for the 2D layer use
  pixel-exact sizes (`STRATA_PIXELS=1`).
- Each frame in flight keeps its own copy of what the CPU writes.

## The design

### 1. The 2D layer

After the world and the spray, one more draw: a list of quads in pixel
coordinates, each with a rectangle, a texture region, a colour, and a
kind (plain colour, image, text glyph). The game fills the list each
frame through `Strata.Ui`; the shim copies it to the GPU and draws it in
order, back to front, blended.

Coordinates are in **interface pixels**. At the default scale of 1, one
interface pixel is one point of the window, which is two device pixels
on a retina display and one on an ordinary one: an interface is the same
physical size everywhere and sharp everywhere. A game's scale makes
everything bigger: at scale 2 each interface pixel is two points, and
the screen is half as many interface pixels across. The scale may be
fractional (1.5); rectangles, frame seams and glyph origins are snapped
to device pixels so nothing blurs.

Placement is by **anchor**: a point of the screen (nine of them: corners,
edge middles, centre) plus an offset. A hotbar sits at bottom-centre, a
crosshair at centre, a readout at top-left, and they stay there when the
window changes shape.

### 2. Images

- **Block icons.** Any layer of the block texture array, drawn flat.
- **An interface atlas.** A game uploads images of its own (frames,
  hearts, slot borders) into one atlas and draws regions of it. Nine-slice
  drawing stretches a frame's middle and keeps its corners.

### 3. Text

Three steps, the standard ones:

1. **Order and shape.** The line is first split into runs of one
   direction and script and put in visual order (the Unicode
   bidirectional algorithm). HarfBuzz then turns each run into glyphs
   with positions: kerning, ligatures, and the joining and reordering
   that Arabic and Indic scripts need.
2. **Fallback.** A game gives a font a list of fallback fonts. A
   character the first font lacks is taken from the next that has it, and
   the run is split there.
3. **Rasterise and cache.** FreeType draws each glyph once, as a
   distance field, into an atlas that fills as glyphs are first used. One
   stored glyph serves every size, so the atlas stays small even for
   Chinese. A string's shaped result is cached too, so text that does not
   change costs nothing to lay out again.

A glyph met for the first time is rasterised that frame. To keep that
from being a hitch, at most a set number are rasterised per frame; the
rest draw a frame or two late. A game can warm the atlas with the text
it knows it will show.

When the atlas is full, the glyphs unused for longest are dropped.

**Drawing.** The fragment shader turns the distance field into a sharp
edge at any size, with optional outline and shadow in other colours.

**Layout** is in March's hands through measurement: a game asks how wide
and tall a string is in a font at a size, and where each line breaks for
a given width. Breaks fall at spaces and after Chinese, Japanese and
Korean characters; there is no hyphenation.

### 4. What a game sees

    Strata.Ui.font(path, face, fallbacks)           -- a font handle; face picks one of a collection; fallbacks are other handles
    Strata.Ui.default_font()
    Strata.Ui.begin(scale)                          -- start this frame's display
    Strata.Ui.rect(ui, anchor, x, y, w, h, colour)
    Strata.Ui.icon(ui, anchor, x, y, size, block_texture)
    Strata.Ui.image(ui, anchor, x, y, w, h, region) -- from the interface atlas
    Strata.Ui.frame(ui, anchor, x, y, w, h, region, border)   -- nine-slice
    Strata.Ui.text(ui, anchor, x, y, font, size, colour, string)
    Strata.Ui.text_styled(ui, ..., style)           -- outline, shadow, alignment, wrap width
    Strata.Ui.measure(font, size, string, wrap)     -- (width, height, lines)
    Strata.Ui.finish(ui)                            -- hand the frame's display to the renderer

`ui` is a plain value threaded through the calls, like everything else in
Strata. Colours are four numbers. The exact names are the builder's.

### 5. The viewer

- A crosshair, a hotbar of nine block icons with the chosen one framed
  and the number keys choosing, and a readout (frame rate, position,
  light mode) in the default font with an outline.
- Scenes for golden images: the display over the overview; a text sheet
  with sizes from 8 to 96 pixels; kerning and ligature pairs; a paragraph
  wrapped at three widths; a line each of Chinese, Japanese, Korean,
  Arabic, Hebrew and Devanagari, and an Arabic line with a number in it;
  outline and shadow over bright and dark ground; the same display at
  scale 1, 1.5 and 2.
- The script scenes use small open-licence fonts kept in the repository
  (subsets of Noto, with their licence), so the gate does not depend on
  which fonts an operating system release ships. The interactive viewer
  may also fall back to system fonts.

## Budget and targets

Designer's targets, stated so a miss is reported. 2560 x 1600, M3 Max.

| | target |
|---|---|
| GPU time for the viewer's display | 0.2 ms or less |
| CPU time per frame for a display of 200 elements and 500 glyphs, unchanged text | 0.2 ms or less |
| draw calls added | 1 |
| a glyph's first use | never more than 1 ms of rasterising in a frame |
| text edge | sharp from 8 to 200 pixels, no shimmer when still |
| glyph atlas memory | 16 MB or less by default |

## Testing

- **C tests** for the atlas (packing, eviction, the per-frame rasterising
  cap) and for shaping results on known strings (glyph counts, advances,
  a ligature, an Arabic word's joined forms, a fallback split).
- **March tests** for anchors, the quad list, measurement and wrapping.
- **Golden images** for every scene above, looked at before they are
  stored.
- **Soak:** the existing run with the display on, with the same
  assertions.

## Order of work

One plan. Each step leaves the viewer working.

1. The 2D layer: quad list, the draw, anchors, scale; rectangles and
   block icons. The viewer's crosshair and hotbar.
2. The interface atlas: images and nine-slice frames.
3. FreeType and the glyph atlas: one font, Latin text, distance-field
   drawing, sizes, outline and shadow. The viewer's readout.
4. HarfBuzz: shaping, kerning and ligatures, the shaped-text cache.
5. Fallback fonts, the on-demand atlas with its cap and eviction; the
   script scenes.
6. Measurement, wrapping, alignment, styled runs.
7. Scenes, soak, measurements against the targets, README.

## Left to the builder

- The quad format and how the list reaches the GPU.
- The kind of distance field (single or multi-channel) and its size.
- Atlas size, packing, and the eviction rule's details.
- The default font.
- How strings and font paths cross from March to C.
- The per-frame rasterising cap.

## Risks

- **Distance fields round sharp corners** at large sizes. A
  multi-channel field fixes it at more cost; the text sheet scene shows
  which is needed.
- **Small text** (under about 10 pixels) can look thin from a distance
  field. If so, small sizes get their own plain rasterised glyphs.
- **Dependencies.** Two more libraries to find at build time. They are
  the two nearly every engine carries.
- **Strings across the boundary.** March strings must reach C as UTF-8
  without a copy per character; if the runtime makes that slow, text is
  shaped once and referred to by handle.
- **Line breaking** without ICU is by spaces and by Chinese, Japanese
  and Korean characters only. Thai, which has no spaces, will not wrap
  well. This is stated in the README, not hidden.

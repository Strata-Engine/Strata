# Interface and text: ledger

Plan: `docs/2026-11-11-ui-text-plan.md`. Branch `interface`, cut from `swim`
(the newest work), not from `fluids` as the plan says.

- Ruling: order of work. HarfBuzz was not installed when the build began
  (the owner's `brew install` was waiting at a prompt), so the 2D layer and
  images came first and Task 1's linking moved to where it is first needed.
  Cost if wrong: none; the tasks do not depend on that order.
- Ruling: Tasks 2 and 3 are one commit. They share the shim's pipeline and
  the viewer's hotbar. Cost if wrong: a coarser history.
- Ruling: a quad is 16 floats, not 13. Sixteen keeps a quad at 64 bytes and
  leaves two numbers for text. Cost if wrong: 12 bytes a quad.
- Ruling: the layer keeps the last display handed over until the next call,
  so a game that skips a frame does not blink. Cost if wrong: a stale
  display if a game stops calling; it clears it with an empty one.
- Ruling: tests that call into C live in `viewer/test`, because the library
  alone links no C. Cost if wrong: none.
- Tasks 2 and 3: complete. `forge test` 12 new tests; `scripts/verify.sh` PASS
  with five new pictures (three scales, frames, a flood of 20 000 quads that
  draws 16 384 and reports 3 616 dropped). Every older picture unchanged.
- Ruling: text lives in `Strata.Text`, not `Strata.Ui`. `Strata.Ui` stays
  pure so its tests link without C; text calls into C. Cost if wrong: two
  module names where the design sketched one.
- Ruling: a March String crosses as the runtime's own value and is decoded
  in C (GAPS S24), not as a byte array. Cost if wrong: breaks if the runtime's
  string layout changes; a test guards it.
- Ruling: the distance field is worked out here from FreeType's coverage,
  not by FreeType's own distance-field renderer as the plan says. Measured:
  FreeType's takes 2 to 4 ms a glyph, this 45 microseconds. Cost if wrong:
  slightly less exact fields than from outlines; not visible on the sheet.
- Ruling (the spec's first risk): at 200 pixels letters from the 48-pixel
  field had rounded corners. Text over 80 pixels uses a second, finer
  picture of each glyph (144 pixels to the em) instead of a multi-channel
  field. Cost if wrong: large text takes about nine times the atlas room.
- Ruling (the spec's second risk): 8 and 10 pixel text is readable from the
  field. Dark text on a light ground was thin, because the frame blends in
  linear light; the shader lifts dark text's coverage. No plain rasterised
  path was added. Cost if wrong: small text is unhinted, softer than the
  system's.
- Ruling: layout (wrapping, alignment, outline, shadow) was written with
  Task 4 because it shares the one layout function. Task 7 adds its scenes.
- Ruling: one C call lays out and writes a text's quads straight into the
  display's array; a layout is remembered whole (1024 of them), keyed by
  font, text, wrap and alignment. Cost if wrong: none seen.
- Tasks 1 and 4: complete. C tests 72 checks; viewer tests 28; verify PASS.
  Frames 58 and 59 of the text sheet are identical.
- Finding: two externs with one March name in different modules are one
  function (GAPS S17). The text module's counts came back as the renderer's
  draw-call count until the names were made unique.
- Task 7's scenes and Task 8's soak and timings done ahead of Tasks 5 and 6,
  which wait for HarfBuzz. Soak with the display on: nothing dropped, no
  glyph waiting, atlas 4 MB; worst frame 9.5, 30.1 and 12.4 ms in three runs
  on a machine with a load average of 13 to 15 (the limit is 20).
- Measured: the timing display (200 rectangles, ten lines, about 520 glyph
  quads) is built and handed over in 0.09 ms unchanged, 0.11 ms when every
  line changes each frame. The viewer's display adds 0.03 ms of GPU time at
  2560 x 1600 and one draw call.
- HarfBuzz 15.1 installed from Homebrew (it also upgraded FreeType to 2.15).
- Ruling: the plan's Arabic test expected four glyphs for the word; the test
  font gives four, in visual order with clusters running backwards, and the
  test also checks the first letter is not its stand-alone form.
- Ruling: a font file loaded twice gives the same handle, so a game (and the
  viewer's script sheet) can ask for a font each frame. Cost if wrong: a
  file changed on disk is not reloaded.
- Ruling: each script line on the sheet uses its own font with Latin as the
  fallback. One list holding Chinese before Japanese would draw the shared
  characters of the Japanese line in the Chinese font: Strata has no
  language tag to choose by. Stated in the README.
- Ruling: the plan's comparison of each script line against macOS's own
  rendering was not done: it needs an app opened on the owner's screen. The
  lines were checked by eye for joining, direction and the number's order.
  Cost if wrong: a shaping fault a native reader would see is unnoticed.
- Measured: the six-script sheet's 121 glyphs arrive over 6 frames, each
  frame spending 1.0 to 1.08 ms drawing glyphs against the 1 ms budget.
- Tasks 5 and 6: complete. C tests 91 checks.
- Final review: one fresh reviewer on the whole branch (312cee7..a3dccf1).
- Final: fixed glyph drawn before its pixels are uploaded — "a frame's new glyphs fit what one frame uploads" RED→GREEN.
- Final: fixed glyph table never recovering — "after fifty thousand glyphs have come and gone" RED→GREEN.
- Final: fixed C writing into a shared display array — "two texts built on the same display do not write over each other" RED→GREEN.
- Final: fixed font() forgetting all layouts — "an unchanged fallback list keeps the layouts" RED→GREEN. `font` with an empty list now leaves a font's fallbacks alone.
- Final: fixed (re-graded from Minor: an assertion that cannot fail is no assertion) the soak's waiting check — "the last frame's waiting count outlives the frame" RED→GREEN.
- Final: fixed without a test (Minor, done because it sat in the same lines) a glyph with no room being redrawn every frame; dead free-cell code removed.
- Final: minor (deferred): text state survives a window close and reopen in one process; glyphs used before would draw blank.
- Final: minor (deferred): an embedded NUL in a font path cuts the path short.
- Final: minor (deferred): a text call reserves three quads per byte of its string; a megabyte string reserves 192 MB.
- Final: minor (deferred): loose tolerances in two C layout tests (measured width, right alignment); two March tests weaker than their names (bad font handle, invalid UTF-8).
- Final: minor (deferred): no suite test wraps right-to-left text; the reviewer's probe found it sound.

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
  in C (GAPS S16), not as a byte array. Cost if wrong: breaks if the runtime's
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

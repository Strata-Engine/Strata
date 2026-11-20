# Interface toolkit: ledger

Design: `docs/2026-11-19-gui-design.md`. Plan: `docs/2026-11-19-gui-plan.md`.
Branch `claude/recursing-volhard-8b2976`, not merged into main.

- Ruling: two modules. `Strata.Gui` is pure (layout, interaction, a draw list);
  `Strata.GuiDraw` holds what calls C (`paint`, `measurer`, `theme`, `sample`).
  The library's tests link no C (the interface ledger's ruling). Cost if
  wrong: one more module name.
- Ruling: the screen is laid out from a tree built each frame, then the
  frame's input is applied, then it is drawn with this frame's new values.
  A layout that a scroll moved is laid out a second time the same frame, so
  what is drawn and reported is where things now are. Cost if wrong: two
  layouts on a frame that scrolls.
- Ruling: the theme holds the text measure as a function (the design's
  first risk). It compiled and the tests pass with it. Cost if wrong: none seen.
- Ruling: an axis holds an action pushed either way (`Strata.Input`'s rule),
  so the stick bound to `ui_left` with weight -1 also "holds" it when pushed
  right. The toolkit takes a direction only when its value is positive.
  Found by the test "the left stick moves focus as the d-pad does". Cost if
  wrong: a game binding a direction with a negative weight on a key gets no
  movement from it.
- Ruling: Space is bound to `ui_activate` and also types a space. A focused
  field takes a key's activation only when the frame typed nothing or typed
  Enter, so a space goes in and Enter or a pad's south button submits.
- Ruling: Left and Right arrive both as typed events and as `ui_left/right`;
  a focused field and a focused slider keep focus on them (caret, step).
  Up and Down leave a field.
- Ruling: no scissor in the layer. A scroll region cuts boxes exactly and
  leaves out text that would cross its edge. Cost if wrong: a row half in
  view shows its box and not its text.
- Ruling: the focus ring shows after a key or pad moved focus and hides when
  the pointer moves, so a mouse user sees no stray ring; a focused field
  always shows it.
- Ruling: a click on nothing clears focus; a press while a choice list is open
  only closes it (or picks), and its release clicks nothing.
- Ruling: the voxel scale on the viewer's screen takes effect on the next start.
  The walker, the physics rig and the fluid are built for the start scale
  (`Ctx`, `PhysRig`); Apply with a new scale prints the `STRATA_SCALE` to use.
  Cost if wrong: the brief's "voxel scale" is a setting, not a live change.
- Ruling: the screen's generation sliders are settings that need no new atlas
  (detail height, density reach, tunnel width, cavern openness). Sea level
  and the seed rebuild the atlas, which takes minutes. Apply regenerates the
  island with them laid over the settings file named in the screen's field
  (`StrataViewer.Params.with_values`).
- Ruling: while the screen is open the world's `Input` is stepped with
  nothing held, so no world action (Escape's quit included) fires; mouse
  look is off and the cursor is freed (`Strata.Ffi.Vk.set_cursor`, new in the
  shim). Escape or pad east closes the screen.
- Found: qualified constructors (`Strata.Gui.Changed(...)`) do not parse as
  patterns outside their module, `on` and `by` are keywords, and a match on
  one line separates its arms with `|`. Worked round with accessors
  (`changed`, `toggled`, `chose`, `edited`, `submitted`, `fold_draws`,
  `draw_text`).

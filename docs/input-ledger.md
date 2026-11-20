# Input: ledger

Design: `docs/2026-11-19-input-design.md`. No separate plan: the design was
built in one pass on branch `claude/wonderful-margulis-48cefb`.

- Ruling: the dead zone is applied in March (`Strata.Input`), not in the
  shim. The shim reports what GLFW gives; a test can then set every number
  the decision rests on. Cost if wrong: a game reading `pad_axis` directly
  gets no dead zone and must call `dead_zone` or `stick` itself.
- Ruling: triggers are mapped to 0..1 in the shim, because GLFW rests them
  at -1 and every other rule here assumes an axis rests at 0. Cost if
  wrong: none seen; a pad whose triggers GLFW reports differently would
  read half pressed.
- Ruling: a raw state is plain data (lists of what is held, a list of
  axes), not a set of functions to ask. It can be written in a test,
  logged and replayed. `Strata.Render.input` fills it, asking the window
  only for the keys the map binds. Cost if wrong: a few short lists built
  each frame.
- Ruling: actions are named by strings and looked up by walking a list.
  The viewer's map is 41 actions and 60 bindings. Cost if wrong: per-frame
  string compares; the fix is an index inside the module, the API stays.
- Ruling: an action bound to an axis is held from half throw either way,
  after the dead zone, so one rule serves a trigger as a button and a stick
  as a direction. Cost if wrong: a game wanting one direction of a stick as
  a button binds it to its own action and tests the value's sign.
- Ruling: the sampler lives in `Strata.Render` (`input`), beside
  `ui_begin`, the other call that reads the window for a pure module.
  `Strata.Ffi.Vk` stays thin bindings and `Strata.Input` stays free of C.
  Cost if wrong: one more thing under a module named for rendering.
- Ruling: typing is one ordered queue of 64 events a frame, characters
  positive and non-typing keys negative, so "a, Backspace, b" within one
  slow frame is applied in that order. Only keys from 256 up are queued:
  letters arrive as characters. Cost if wrong: a 65th event in one frame
  is dropped.
- Ruling: the caret is a byte offset kept between characters. March
  strings are bytes; an offset can be handed straight to `string_slice`.
  `text_edit` moves a caret given inside a character back to its start.
  Cost if wrong: a caller counting characters must not use it as a count.
- Ruling: `text_field_at` is new and `text_field` is unchanged, so the
  browser work that uses `text_field` draws as before.
- Ruling: the test of `text_field_at` was dropped from `test/`: it calls
  `Strata.Text`, which links C, and the library's tests link none (the
  interface ledger's ruling). The function is typechecked by the build and
  not exercised by a test. Cost if wrong: a drawing bug in it goes unseen.
- Ruling: in the viewer the pad's look is its own pair of actions
  (`look_x`, `look_y`) at 2.6 and 1.8 radians a second, apart from the
  arrow keys' `turn` and `tilt` at 1.6 and 1.2, so the keyboard's rates are
  the numbers they were. Cost if wrong: two actions where one with weights
  would do.
- Ruling: a scripted run of the viewer steps its input with Escape alone.
  Before, each read was guarded by `!scripted` one at a time; those guards
  are kept as well. Cost if wrong: none.
- Ruling: the viewer keeps its own once-per-press bits (in `Cam`) for the
  keys that had them and uses `pressed` only for the new bumper controls.
  Rewriting them risked the pictures for no change in behaviour. Cost if
  wrong: two ways of finding an edge in one file.
- Found: `alias Strata.Input as X` resolves `X.f(...)` but not a type
  `X.Map` or a constructor `X.Key(...)` ("Unknown module"); `doc` before a
  `type` is a parse error; `\u{..}` is not a string escape. Worked round
  with full names, `--` comments and literal characters. Not yet in
  `GAPS.md`.
- Not checked: no physical gamepad was connected. That GLFW sees a pad,
  that the buttons and axes land where the table says, the trigger range,
  and how the sticks feel at the chosen rates are all unverified. Typing
  into a real window (the character callback, key repeat) was not driven
  either. What is verified: the mapping, edges, dead zones and text
  editing on raw states (18 tests), and that the viewer builds, lints and
  draws the pictures it drew before: `scripts/golden.sh` fails on
  `tilted-slab-lit`, `body-by-lake` and `combat-burning`, and the commit
  before this work (13de642) fails on the same three with the same
  figures; this work's pictures of them match that commit's pixel for
  pixel. Every other scene passes. The three are not this work's to
  restore.

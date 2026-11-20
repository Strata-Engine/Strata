# Strata interface toolkit: design

Status: design, built in the same branch; rulings made on the way are in
`gui-ledger.md`. Plan: `2026-11-19-gui-plan.md`.
Date: 2026-11-19
Follows: `2026-11-11-ui-text-design.md` (whose non-goals left menus, buttons,
lists and layout for later) and `2026-11-19-input-design.md`.

## Intent

`Strata.Ui` draws rectangles and `Strata.Text` draws text, both where the
caller says. `Strata.Controls` draws a slider and a field at positions the
caller works out, and leaves focus and pointer capture to the caller. A game
building a menu, a settings screen or an inventory writes the same layout
arithmetic, hit testing and focus bookkeeping each time. This project adds
those as one immediate-mode toolkit: the game describes a screen's controls
each frame as data; Strata works out where they go, what the pointer, keys
and pad did to them, and what to draw.

## Decisions taken from the brief

1. Immediate mode, pure March, beside `Strata.Controls`: the screen is
   described afresh each frame and the values it shows are the game's.
2. Layout is rows and columns with padding and spacing; a size is fixed,
   fitted to content, or a share of what is left; a screen is anchored to a
   window edge or the centre and scales with the window and the UI scale.
3. Widgets: label, button, toggle, slider, text field, panel, a scrolling
   list with a scroll bar, and a choice list that drops down.
4. Interaction by stable id: hover, press, click; pointer capture while
   dragging; focus moved by keyboard and pad, next, previous and by
   direction; activation; typed text into the focused field. Every key and
   button comes through `Strata.Input` actions so a game can rebind them.
5. Theme as data with a plain default.
6. Headless: a layout and a raw input give rectangles and events with no
   window. No game concept.

Everything else below is the designer's call.

## Goals

- A screen of a few dozen controls described in one expression, laid out,
  hit tested and drawn for well under 0.2 ms a frame.
- The same screen usable with the mouse alone, the keyboard alone, or a pad
  alone.
- Every number that decides a rectangle or an event can be set in a test.
- Nothing a game sees changes shape when a widget is added: a control is a
  value in a tree, an event is a value in a list.

## Non-goals

- A retained widget tree, animation, transitions, tooltips, drag and drop
  between controls, docking, windows the player moves.
- Multi-line text editing, selection, clipboard (none exist below it).
- Clipping by scissor: Strata's layer has none. A scrolling list cuts its
  rectangles exactly and leaves out text that would cross its edge.
- Images inside widgets (a game draws icons over a control's rectangle,
  which the result gives it).
- An editor. The planned editor overlay is a user of this, not part of it.

## Anti-goals

- State hidden from the game that decides what it sees: the values shown
  are the game's; the toolkit keeps only what immediate mode must (focus,
  the control holding the pointer, a field's caret, scroll offsets, which
  choice list is open).
- A control that works with the mouse and not the pad, or the other way.
- A game concept or a fixed key in the library.
- C in the pure module: tests link none.

## The design

### 1. Two modules

- **`Strata.Gui`** (pure): the node types, the theme, the state kept
  between frames, the input a frame gives, and `run`, which lays out,
  interacts and answers a result: rectangles by id, events, and a draw
  list of boxes and words.
- **`Strata.GuiDraw`** (calls C): `paint` turns a result's draw list into
  `Strata.Ui` rectangles and `Strata.Text` text; `measurer` is the text
  measure from `Strata.Text`; `sample` fills a frame's input from the
  window (pointer, wheel, button, typed events).

Text width is the one thing layout needs from C. The theme carries a
measure function `(font, size, text) -> (width, height)`; a test gives one
that counts characters, a game gives `Strata.GuiDraw.measurer()`.

### 2. Describing a screen

A node is a value:

```
Gui.column(Gui.fit(), Gui.fit(), [
  Gui.label("Settings"),
  Gui.slider("sea", "Sea level", sea, 20.0, 60.0, 1.0),
  Gui.toggle("shadows", "Shadows", on),
  Gui.choice("light", "Light", ["off", "sun", "shadows"], mode),
  Gui.row(Gui.share(1.0), Gui.fit(), [Gui.button("apply", "Apply"), Gui.button("close", "Close")])])
```

- **Sizes.** Each node has a width and a height, each `Fixed(px)`, `Fit`
  (its content's size) or `Share(w)` (a share, by weight, of what the
  parent has left along its axis, or the parent's whole inner size across
  it). Widgets default to fit across their content with a theme minimum.
- **Containers.** `row` and `column` lay children along one axis with the
  theme's gap between them and the theme's padding inside; `with_pad` and
  `with_gap` override them. `panel` is a column with a background.
  `scroll(id, w, h, children)` is a column seen through a viewport.
- **Placing.** A screen is a list of layers, each `place(anchor, x, y,
  node)`: one of `Strata.Ui`'s nine anchors and an offset. A node sized
  `Share` at the top is a share of the window, so a panel `Share(0.5)` wide
  is half the window whatever its size. Interface pixels and the UI scale
  are `Strata.Ui`'s: the toolkit lays out in interface pixels on a screen
  of the size `Strata.Ui.width/height` report.

Layout is two passes: sizes bottom up (what each node needs), then
rectangles top down (what each gets). Shares are split after fixed and
fitted children take theirs, and never below 0.

### 3. Interaction

A frame's input (`Gui.Frame`) is the pointer in interface pixels (or none
when the cursor is captured for looking), whether its button is down, the
wheel's delta, the typed events (`Strata.Ffi.Vk.typed`), and a
`Strata.Input.Input` carrying the toolkit's actions:

| action | does | default binding |
|---|---|---|
| `ui_next` | focus the next control | Tab, pad right bumper |
| `ui_prev` | focus the previous one | pad left bumper; Tab with `ui_shift` held |
| `ui_shift` | turns Tab into back-Tab | either Shift |
| `ui_up`, `ui_down`, `ui_left`, `ui_right` | move focus by direction; a focused slider takes left and right; an open list takes up and down | arrows, d-pad, left stick |
| `ui_activate` | press the focused control | Enter, Space, pad south |
| `ui_back` | close an open list; else reported for the game | Escape, pad east |

`Gui.bindings()` is that table as a binding list; a game appends it to its
own map or lists its own bindings for the same names.

- **Ids.** Every control that can be acted on has a string id of the
  game's. It must be stable from frame to frame and unique on a screen.
- **Hover** is the topmost control under the pointer: an open list over
  everything, later layers over earlier.
- **Press and capture.** The button going down over a control makes it the
  *active* control and focuses it. It stays active until the button comes
  up, wherever the pointer goes: a slider dragged off its track keeps
  following the pointer. A *click* is the button coming up over the control
  that went active.
- **Focus** is one id, kept across frames. Next and previous walk the
  focusable controls in tree order, wrapping. A direction picks the nearest
  control whose centre lies that way, measured along the direction plus
  twice the distance across it. Focus on a control that leaves the screen
  falls to the first focusable control when keys or pad next move it. With
  the pointer moving and no key pressed, focus is drawn only on fields, so a
  mouse user sees no stray ring; once a key or pad moves focus the ring
  shows until the pointer moves again.
- **Activation** (`ui_activate`) clicks a button, flips a toggle, opens or
  picks in a choice list, and submits a field.
- **Sliders** step by their step on left and right while focused, and follow
  the pointer while active, snapped to the step. The wheel over a slider
  steps it.
- **Fields** take the frame's typed events through
  `Strata.Controls.text_edit` while focused; the caret is kept in the
  state. Up and down move focus out; left and right move the caret (they
  arrive as typed events too, so the toolkit ignores `ui_left/right` there).
  Enter submits; with a pad, `ui_activate` submits.
- **Scroll regions** move by the wheel when hovered, by dragging their bar's
  thumb, and to keep the focused control inside them when focus moves by
  key or pad.
- **Choice lists** open on click or activation, as a column over every
  layer below the control (above it when there is no room). Up and down
  move a highlight; activation or a click picks; `ui_back` or a click
  outside closes.

`run(state, theme, frame, screen_w, screen_h, layers)` answers `(state,
result)`. The result holds every control's rectangle by id, the events in
the order they happened (`Clicked`, `Toggled`, `Changed`, `Edited`,
`Submitted`, `Chose`, `Back`), the hovered, active and focused ids, whether
the pointer is over any layer (so the game does not also dig through a
menu), and the draw list. Values drawn are the new ones: a slider dragged
this frame is drawn where it was dragged to.

### 4. Theme

A record of plain values: font and text size, padding, gap, control height,
scroll bar width, the minimum width of a widget, and colours for text, dim
text, panel, control at rest, hovered and pressed, accent (a toggle's mark,
a slider's fill, the caret), focus ring, field and track. `Gui.theme(m)` is
a plain dark default with measure function `m`.

### 5. The viewer

A settings screen opened with F1 or the pad's right stick click: sliders
for a few `island.gen` values (sea level, detail amplitude, density
amplitude, cavern openness), a choice list for the light mode, a slider
for the voxel scale, a toggle for the readout, and Apply and Close. Apply
regenerates the island with the chosen values over the file's. While it is
open the cursor is freed for the pointer and the world's controls are not
read. A golden scene shows it with a list open and a control focused.

## Testing

`test/gui_test.march`, with a measure that counts characters:

- layout: fixed, fit and share along and across, padding, gap, nested,
  every anchor, a share of the window, shares never negative;
- hit testing: hover, topmost layer, the open list over everything, the
  pointer over a layer;
- press, capture and click: click inside, release outside is no click,
  slider drag past the track end, capture held off the control;
- focus: next and previous order and wrapping, back-Tab, direction picks
  in a grid, focus following a click, a lost focus recovered;
- activation of each widget by key, and slider stepping;
- scrolling: wheel, clamped at both ends, thumb drag, focus kept visible,
  items outside not drawn and not hit;
- text entry: typed into the focused field only, caret kept, Enter submits;
- choice lists: open, highlight, pick, back closes, click outside closes.

## Risks

- **Closures in a record.** The theme holds a function. If the compiler
  makes that slow or wrong, the measure becomes a parameter of `run`.
- **No scissor.** Text crossing a scroll region's edge is left out rather
  than cut; a list's rows should be a whole number of lines tall.
- **The one-frame tree.** A game that builds different trees on two frames
  for the same id (a control moved between layers) sees a frame of odd
  hover; this is the price of no retained tree and is the game's to avoid.

# Strata input: design

Status: design and build in one pass; rulings made on the way are in
`input-ledger.md`.
Date: 2026-11-19
Follows: `2026-11-10-strata-design.md`, `2026-11-11-ui-text-design.md`
(whose non-goals left text entry for later).

## Intent

Strata reads a window's input as raw facts: is this GLFW key held, is this
mouse button held, where is the pointer. A game needs more than that and
should not write it again each time: a gamepad, one name for "jump" whether
it is Space or the pad's south button, the frame a button went down, and
the characters a player typed. This project adds those as mechanisms. What
the actions are called and what they are bound to is the game's data.

## Decisions taken from the brief

1. Gamepads come through GLFW's gamepad API in the existing shim. No new
   library.
2. Typed characters come from GLFW's character callback, with the editing
   keys a text field needs.
3. The mapping from actions to keys, buttons and axes is a pure March
   module, tested headless by feeding it raw states.
4. The viewer uses it for its existing controls; keyboard and mouse
   behaviour does not change.
5. From the first design: the library names no game concept.

Everything else below is the designer's call.

## Goals

- Connected pads, their buttons, sticks and triggers, read once a frame.
- A dead zone, so a stick at rest reads exactly 0 and one pushed just past
  the zone starts from 0, not from the zone's edge.
- Actions declared and bound as data: a list a game could as well read
  from a file.
- For each action each frame: held, pressed this frame, released this
  frame, and a value from -1 to 1.
- Several bindings to one action, digital and analogue mixed: W, S and a
  stick all drive one axis.
- Typing that survives a slow frame: characters and editing keys arrive in
  the order they were typed, with the system's key repeat.
- One pure function that applies a frame's typing to a string and a caret.

## Non-goals

- Rebinding screens, saving bindings to a file, glyphs for buttons.
- Pads GLFW's mapping database does not know (a joystick that is not a
  gamepad), rumble, gyros, touch pads.
- Chords, double taps, hold timers, input buffering. A game builds these
  on `pressed` and its own clock.
- Pointer motion and scroll as bindings. They are deltas, not states, and
  stay where they are in `Strata.Ffi.Vk`.
- Input method editors (composing Chinese or Japanese), clipboard,
  selection, multi-line editing.
- More than one local player's bookkeeping. A game steps one `Input` per
  pad; nothing here stops it.

## Anti-goals

- A game concept in the library: no "jump", no "move", no default map.
- State hidden in C that a test cannot set. The shim reports; March
  decides.
- A lost key press or character when a frame runs long.
- A pad left plugged in changing a scripted run's picture.

## The design

### 1. The shim (`native/strata_vk.c`)

`sv_poll` already pumps events once a frame. It gains:

- **Pads.** After the events, each of GLFW's 16 joystick slots that is a
  gamepad is read with `glfwGetGamepadState` into a table. `sv_pad_connected(p)`,
  `sv_pad_button(p, b)` (GLFW's 15 buttons, Xbox layout: 0 south .. 14
  d-pad left) and `sv_pad_axis(p, a)` read the table. Axes 0..3 are the
  two sticks as GLFW gives them, -1..1, y positive downward. Axes 4 and 5
  are the triggers, which GLFW rests at -1; the shim maps them to 0..1 so
  that every axis rests at 0. No dead zone in C.
- **Typing.** One queue of 64 events a frame, in order: a character
  callback pushes each Unicode code point (positive), and the key callback
  pushes the negated key code of each press or repeat of a key from 256
  up (GLFW's keys that type nothing: Enter, Backspace, arrows, Home, End).
  `sv_typed_count()` and `sv_typed(i)` read it; `sv_poll` empties it
  first. A 65th event in one frame is dropped.

`Strata.Ffi.Vk` binds these as `pad_connected`, `pad_button`, `pad_axis`,
`first_pad` and `typed` (the frame's events as a list).

### 2. `Strata.Input` (pure)

```
let map = Strata.Input.new([
    ("forward", Strata.Input.Key(87), 1.0),        -- W
    ("forward", Strata.Input.Key(83), -1.0),       -- S
    ("forward", Strata.Input.PadAxis(1), -1.0),    -- left stick, up is negative
    ("jump", Strata.Input.Key(32), 1.0),
    ("jump", Strata.Input.PadButton(0), 1.0)])
let input = Strata.Input.start(map)
-- each frame
let input = Strata.Input.step(input, Strata.Render.input(input, Strata.Ffi.Vk.first_pad()))
Strata.Input.value(input, "forward")    -- -1 .. 1
Strata.Input.pressed(input, "jump")     -- true on the frame it went down
```

- **A binding** is `(action, source, weight)`. A source is `Key(code)`,
  `MouseButton(b)`, `PadButton(b)` or `PadAxis(a)`. The action's name is a
  string of the game's choosing.
- **A raw state** is plain data: the keys held, the mouse buttons held,
  the pad buttons held, and the pad's axes. A test writes one by hand;
  `Strata.Render.input` fills one from the window, asking only for the keys
  the map names.
- **Value.** A digital source adds its weight while held. An analogue
  source adds its value, after the dead zone, times its weight. The sum is
  clamped to -1..1. So W and S cancel, and W with the stick forward is
  still 1.
- **Held.** An action is held when any digital source of it is held, or
  an analogue source of it, after the dead zone, is at or past the press
  point (0.5 unless the game says otherwise) either way. Pressed and
  released are the frames on which held changed; `step` keeps the last
  frame's answer for that.
- **Dead zone.** `dead_zone(v, zone)` is 0 inside the zone and runs from 0
  at its edge to 1 at full throw. Axes that form a stick (0 with 1, 2 with
  3) are treated together, by the stick's length (`stick`), so a diagonal
  push is not squared off; a trigger alone. The zone is 0.15 unless the
  game says otherwise (`with_dead_zone`).
- **No pad.** A raw state with no axes and no pad buttons is a pad at
  rest. `Strata.Input.idle()` is nothing held at all: what a scripted run
  steps with.

Looking a binding up is a walk of the map's list with string compares; a
map of a few dozen bindings costs microseconds a frame. If a game's map
grows past that the fix is an index inside this module, not a new API.

### 3. Text editing (`Strata.Controls`)

`text_edit(text, caret, events, limit)` applies a frame's `typed` events
to a string and a caret (a byte offset that always sits between
characters) and answers the new pair. A code point is inserted at the
caret as UTF-8 unless the text already holds [limit] characters;
Backspace and Delete remove a whole character; Left, Right, Home and End
move the caret. Control characters and other keys are ignored: Enter and
Escape are the game's to act on, by looking in the same list.

`text_field_at` draws a field with the caret where it is; `text_field`
keeps its end caret.

### 4. The viewer

The viewer's controls become one binding list and one `Input` carried
through the frame loop. Every key and mouse button it reads today is an
action bound to the same key. A pad is bound beside them: left stick
walks, right stick looks, south jumps or rises, east sinks or sprints,
triggers dig and build, bumpers step through the hotbar, north switches
flying. A scripted run (the goldens, the benches) steps with `idle()` and
never reads a pad.

## Testing

- `test/input_test.march`: values, cancelling, clamping, held, pressed and
  released across steps, analogue press point, dead zone scalar and
  radial, no pad, unknown action.
- `test/controls_test.march`: insertion, limit, UTF-8 characters, each
  editing key, caret bounds, ignored keys.
- `scripts/verify.sh` and `scripts/golden.sh` unchanged in what they ask.
- Not testable here: the shim's pad table against a physical pad, and
  real key repeat. Stated in the ledger and the README.

## Risks

- GLFW reports a pad only if its mapping database knows it. An unknown
  pad is not connected as far as Strata is concerned.
- macOS may ask for permission before a pad is seen, depending on the
  pad and system version.

# Plugins

A plugin is a game element packaged so other games can use it: a tactical
grid with turns and a camera, city-builder tools, and the like. It is a
forge package; Strata gives it a shape to follow and the shared types in
`Strata.Plugin`. Strata runs no loop of its own: the game keeps each
plugin's state and calls it. (A small optional runner that drives a list of
plugins may come later; nothing here rules it out.)

## Writing a plugin

A plugin's main module offers four parts:

```
setup(config, registry, ...) : (state, Strata.Blocks.Registry, List((String, Strata.Input.Source, Float)))
update(state, frame : Strata.Plugin.PluginFrame, ...) : (state, List(Strata.Plugin.Request), List(event))
overlay(state, ...) : Strata.Overlay.Overlay
gui(state, ...) : Strata.Gui.GNode
```

- **setup** runs once. Register blocks you need in the registry and return
  it; propose input bindings, each action named `<plugin>.<action>`
  (`tactics.rotate_left`). Return your starting state.
- **update** runs once a frame. Read the `PluginFrame` (the world for queries,
  the input, the pointer and its world ray, the wheel, the time step, the
  window's size, the camera on screen). Return your new state, the
  requests you make of the game, and events of your own type.
- **overlay** returns what you draw over the world this frame: quads,
  lines, and a cut that hides geometry (`Strata.Overlay`).
- **gui** returns your part of the interface for the game to place.

The `...` are arguments you ask of the game, such as a function that names
a unit.

Rules:

- **Never change the world yourself.** Ask with requests
  (`Strata.Plugin.edit`, `blast`, `sound`); the game applies the ones it
  accepts. A plugin that tracks the world (a grid over it) gets the edits'
  outcomes back from the game.
- **Never set the renderer's camera.** If you have a camera, offer
  `view_proj(state, aspect)`. The game chooses which camera is shown and
  passes its matrix back in the next frame, so your pointer ray matches the
  screen.
- **Use only Strata's public modules**, as any package can. If you need
  something Strata lacks, it is added to Strata's public API for everyone.
  `strata_tactics`, which ships in this repository, follows the same rule.
- **Native code.** forge does not build a dependency's C sources (GAPS
  S11), so a plugin with C code needs every game to list its files. Prefer
  March; renderer features belong in Strata core.

## Using a plugin

1. Add it to `forge.toml`:
   `strata_tactics = { path = "../strata/plugins/strata_tactics" }` (or a
   git dependency).
2. At start-up, call each plugin's `setup`, then merge bindings, the game
   first: `Strata.Input.merge([("game", mine), ("tactics", theirs)])`. Look
   at the clashes it reports and rebind.
3. Each frame:
   1. build the frame with last frame's view-projection:
      `Strata.Plugin.frame(world, input, pointer, wheel, dt, w, h, vp)`;
   2. call each plugin's `update`;
   3. apply the requests: `Strata.Plugin.apply(world, requests)` gives the
      new world, the outcomes, and what is left for you (sounds);
   4. hand the outcomes to plugins that track the world;
   5. merge the overlays (`Strata.Overlay.merge`) and call
      `Strata.Render.overlay`;
   6. place each plugin's gui in your screen;
   7. take the camera matrix from the plugin whose camera is shown, give it
      to `Strata.Ffi.Vk.set_camera`, and keep it for the next frame.

## Wiring example

`plugins/strata_tactics/example/lib/tactics_example.march` is a small game
on the tactics plugin (`plugins/strata_tactics/example/run.sh`; the picture
is `docs/tactics-example.png`). The lines it has only because of the plugin
are marked `-- plugin:` and number twelve:

1. the dependency in `forge.toml`: `strata_tactics = { path = ".." }`;
2. setup: `StrataTactics.setup(StrataTactics.config(grid_settings), reg, world)`;
3. merging bindings: `Strata.Input.merge([("game", mine), ("tactics", theirs)])`;
4. the frame: `Strata.Plugin.frame(world, input, pointer, wheel, dt, w, h, last_vp)`;
5. update: `StrataTactics.update(state, frame)`;
6. applying requests: `Strata.Plugin.apply(world, requests)`;
7. the stream told of the outcomes (`Strata.Stream.apply` for each);
8. the world changed: `StrataTactics.world_changed(state, world, outcomes)`;
9. the marks the game shows: `StrataTactics.show(state, items)`;
10. the overlay: `Strata.Render.overlay(StrataTactics.overlay(state, subject))`;
11. the camera shown: `StrataTactics.view_proj(state, aspect)` to
    `Strata.Ffi.Vk.set_camera`, kept for the next frame;
12. the gui: `StrataTactics.gui(state, label, k)` placed in a `Strata.Gui`
    layer.

Everything else in the example (units, moving them, blasting, ending turns,
the clock's advancing) is the game's own.

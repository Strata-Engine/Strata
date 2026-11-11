# Strata

A voxel engine library for March: a block registry, 32-cubed palette chunks,
a greedy mesher that emits 8-byte quads, and a Vulkan renderer that draws
them from one large GPU buffer. Games supply blocks and terrain; Strata names
no game concept.

Design: `docs/2026-11-10-strata-design.md`. Build plan:
`docs/2026-11-10-strata-voxel-core.md`.

## Toolchain

Strata compiles against March `origin/main`. Last verified against commit
`1a4251bc8` (march 0.7.0). `scripts/env.sh` expects that build at
`~/code/march-main` (a git worktree of the March repository, built with
`dune build`); set `STRATA_MARCH_HOME` to point elsewhere.

The renderer needs Vulkan and GLFW. On macOS:

    brew install molten-vk vulkan-loader vulkan-headers glfw

`glslang` is needed only to rebuild `native/strata_shaders.h` after editing
the shaders (`native/build_shaders.sh`).

## Running it

    viewer/run.sh            # walk a generated world
    cd demo && forge build --release && ./.march/build/release/strata_demo

In the viewer: W A S D move, the mouse looks, space jumps, F switches
between walking and flying (space and left shift then rise and sink), left
click carves a sphere, right click builds one, escape quits.

Environment switches for scripted runs are listed at the top of
`viewer/lib/strata_viewer.march`: a fixed scene (`STRATA_SCENE`), a frame
count and screenshot (`STRATA_FRAMES`, `STRATA_SHOT`), offscreen timing
(`STRATA_OFFSCREEN=1`), view radius, window size, quad capacity.

    scripts/golden.sh            # render the fixed scenes and compare with test/golden/
    scripts/golden.sh --update   # re-store them; look at each before committing

`viewer/` is the renderer's test bed. `demo/` runs the core headless and
prints what it cost; see `RESULTS.md`.

## Using it from a game

    [deps]
    strata = { path = "../strata" }

A game that draws also lists Strata's C shim itself, because forge does not
build a dependency's C sources:

    [ffi]
    sources = ["../strata/native/strata_vk.c"]
    link = ["-I../strata/native", "-I/opt/homebrew/include", "-L/opt/homebrew/lib",
            "-lglfw", "-lvulkan", "-Wl,-rpath,/opt/homebrew/lib"]

and must run with `MARCH_PIN_MAIN=1`. A game that only uses the core needs
neither.

```march
-- register blocks
let stone = Strata.Blocks.with_faces(Strata.Blocks.def(), 1, 1, 1)
let (reg, stone_id) = match Strata.Blocks.register(Strata.Blocks.empty(), stone) do
  Ok(p) -> p
  Err(_) -> (Strata.Blocks.empty(), 0)
end

-- a pure terrain function: (chunk key, seed) -> chunk
let gen = Strata.Terrain.gen(fn (k, seed) ->
  if Strata.Coord.ky(k) < 0 do Strata.Chunk.uniform(stone_id) else Strata.Chunk.uniform(0) end)

-- a world at one metre per voxel, streamed around the player
let world = Strata.World.new(reg, gen, 7, 1.0)
let s0 = Strata.Stream.start(world, Strata.Stream.config(6, 3, 8, 4))
let s1 = Strata.Stream.tick(Strata.Stream.set_focus(s0, Cons((0.0, 2.0, 0.0), Nil)))

-- each frame: tick, upload finished meshes, free unloaded chunks
let (s2, meshes) = Strata.Stream.take_meshes(s1)
let (s3, gone) = Strata.Stream.take_unloads(s2)

-- edit: the outcome carries the dirty chunks and a change record
let s4 = Strata.Stream.apply(s3, Strata.Edit.fill_sphere(Strata.Stream.world(s3), 0, 0, 0, 6, 0))
```

## Modules

| Module | Holds |
|---|---|
| `Strata.Coord` | chunk, local and world coordinates; chunk keys; voxel scale |
| `Strata.Blocks` | the block registry: definitions, state families, models |
| `Strata.Chunk` | 32-cubed chunks in uniform, paletted and direct forms |
| `Strata.Store` | the persistent chunk map |
| `Strata.Quad` | the packed 8-byte quad and instance formats |
| `Strata.Occupancy` | one-bit-per-voxel solid masks |
| `Strata.Mesh` | the greedy mesher |
| `Strata.Terrain` | the generator contract |
| `Strata.World` | loaded chunks, the edited-chunk cache, queued edits |
| `Strata.Edit` | single, list, box and sphere edits |
| `Strata.Stream` | loading around focus points; task-built chunks and meshes |
| `Strata.Query` | raycast and swept box collision |
| `Strata.Slots`, `Strata.Render` | renderer slot bookkeeping; keeping the renderer in step with a stream |
| `Strata.Model` | a model block's drawing geometry |
| `Strata.Ffi.Vk` | window, input and the Vulkan renderer |
| `Strata.Math.*`, `Strata.Noise`, `Strata.Tree` | vectors, matrices, noise, a persistent tree |

## Known limits

- No save format: edited chunks stay in memory.
- Lighting is a sun direction, corner shading and fog. No shadows, no
  emitted or ray-traced light yet.
- The view reaches the stream's radius, about 300 m by default. The far
  field is the next plan.
- Translucent quads are ordered by chunk, not within one, and draw from
  both sides, so a one-block pane blends twice.
- Model faces light as the nearest of the six axis directions.
- On MoltenVK presentation paces to the display even with vsync off;
  uncapped timing uses `STRATA_OFFSCREEN=1`.
- There is no separate upload path for devices without unified memory:
  the mapped buffer is correct everywhere and slower on a discrete GPU.
- The frame in which the camera enters a new chunk spends 5 to 7 ms in
  `Strata.Stream.set_focus`. See `RESULTS.md`.
- March and forge gaps met on the way are in `GAPS.md`.

## The gate

    scripts/verify.sh

runs `forge check`, `forge build --release`, `forge lint --strict` and
`forge test` in the library and in each app, and prints PASS.

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
click carves a sphere, right click builds one, escape quits. Carve the
rim of a lake in the hills and it drains. `L` steps
through the light modes and `[` and `]` lower and raise the sun.

The world is an island 2 km across, read from `Strata.Atlas`: rivers run
to the sea in their channels and lakes stand at their spill level. Under
its surface the land is a 3D density (`Strata.Lattice`) with overhangs and
caves that open here and there, its steepest slopes relaxed to scree
(`Strata.Thermal`), grass over dirt over stone laid by `Strata.Surface`. Its atlas
takes about 4 s to build and is cached in `.march/atlas` (read in about
30 ms after that); `STRATA_ISLAND=0` brings back the older noise terrain.

Environment switches for scripted runs are listed at the top of
`viewer/lib/strata_viewer.march`: a fixed scene (`STRATA_SCENE`), a frame
count and screenshot (`STRATA_FRAMES`, `STRATA_SHOT`), offscreen timing
(`STRATA_OFFSCREEN=1`), view radius, window size, quad capacity.

    scripts/golden.sh            # render the fixed scenes and compare with test/golden/
    scripts/golden.sh --update   # re-store them; look at each before committing

`viewer/` is the renderer's test bed. `demo/` runs the core headless and
prints what it cost; see `RESULTS.md`.

## Lighting

Light is ray traced against the voxels themselves, on the GPU, so it follows
an edit on the frame the edit is drawn. Design:
`docs/2026-11-10-lighting-design.md`.

- The sun casts shadows. Leaves cast dappled shade from the holes in their
  texture.
- Smooth blocks mirror: water and ice reflect the land and the sky, and carry
  the sun's glint. Anything not fully rough carries a softer highlight.
- Open sky lights what it can see, so it is darker under a canopy or down a
  tunnel.
- Blocks that emit light the ground around them, in their colour.

A block's part in this is two registry fields a game already sets:
`Strata.Blocks.with_material(def, roughness, 0)` (0 a mirror, 255 fully
rough, the default) and `Strata.Blocks.with_emission(def, r, g, b, strength)`.

| Call | Does |
|---|---|
| `Strata.Render.set_light_box(radius_h, radius_v)` | sizes the lighting to the stream's radii; once, before the first `sync` |
| `Strata.Render.set_sun(dx, dy, dz, r, g, b)` | the direction to the sun and its colour; the sky and fog follow |
| `Strata.Render.set_time(seconds)` | drives the water's ripples |
| `Strata.Render.set_quality(sky_rays, reflections)` | sky rays per pixel per sample (0 to 8, default 2); mirror rays on or off |
| `Strata.Ffi.Vk.set_lighting(mode)` | 4 everything (the viewer's default), 3 without sky light, 2 shadows only, 1 no rays, 0 the old flat shading |

Costs and what misses its target are in `RESULTS.md`.

![A lake at a low sun](docs/light-lake.png)
![Snow and ice on high ground](docs/light-snow.png)
![Lanterns at dusk](docs/light-lantern.png)

## Fluids

Water is a finite amount that moves. Dig out the bank of a lake and it runs
downhill, pools in any hollow, and otherwise carries on to the sea, while
the lake's surface drops as one level sheet. Design:
`docs/2026-11-10-fluid-design.md`.

A fluid is a family of eight block states, one per eighth of a voxel it
fills, so it is ordinary blocks changed by ordinary edits: meshing, lighting
and collision follow with no path of their own.

```march
-- eight states of water; a sea block that swallows what reaches it
let (reg1, lake) = match Strata.Blocks.register_fluid(reg0, water_def) do Ok(p) -> p  Err(_) -> (reg0, 0) end
let fluid0 = Strata.Fluid.new(reg1, Cons((lake, Strata.Fluid.flowing()), Cons((sea, Strata.Fluid.sink()), Nil)))

-- every edit the game makes: tell the fluid
let o = Strata.Edit.fill_sphere(Strata.Stream.world(s), x, y, z, 4, 0)
let s1 = Strata.Stream.apply(s, o)
let f1 = Strata.Fluid.wake(f, Strata.Edit.world(o), o)

-- ten times a second: end the step begun last time, begin the next
let (f2, s2) = Strata.Fluid.finish(job, f1, s1)
let (f3, job2) = Strata.Fluid.begin(f2, Strata.Stream.world(s2), focus)
-- Strata.Fluid.events(f2) says where water fell, how much, and how far: spray and sound
```

A step is worked out on a worker and costs the frame about half a
millisecond. `Strata.Fluid.step` does one at once instead, for tools and
tests. `Strata.Render.spray` draws particles that fly on the clock; the
viewer feeds it from the fluid's events.

![A breached lake draining](docs/fluid-lake.png)
![The same breach from below](docs/fluid-waterfall.png)

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
| `Strata.Fluid` | finite fluids: fall, run to a drop, stay level, sink, rest |
| `Strata.Slots`, `Strata.Render` | renderer slot bookkeeping; keeping the renderer in step with a stream |
| `Strata.Model` | a model block's drawing geometry |
| `Strata.Template`, `Strata.Ffi.Vox` | voxel templates (holes, probabilities, turns) blitted into chunks; MagicaVoxel `.vox` import (`native/strata_vox.cpp`, link with `-fno-exceptions`) |
| `Strata.Ffi.Vk` | window, input and the Vulkan renderer |
| `Strata.Math.*`, `Strata.Noise`, `Strata.Tree` | vectors, matrices, noise, a persistent tree |

## Known limits

- No save format: edited chunks stay in memory. A save would have to keep
  the fluid's active cells too.
- Water has no pressure: it does not rise in a pipe or push uphill behind a
  dam. It leaves a lake only over the lip of its outlet, so a big lake
  takes minutes to drain, and a wet bed keeps a film of an eighth or two.
- Water moves only in loaded chunks; at the edge of the loaded world it
  waits.
- Spray is a picture: it meets nothing and carries no water.
- Lighting reaches as far as the stream does and no further: nothing outside
  the loaded chunks casts a shadow or is reflected.
- One bounce of light. No light shafts, no refraction, no coloured shadows
  through glass.
- The lighting keeps a byte per voxel on the GPU for every loaded chunk:
  156 MB at the viewer's default radius.
- A lake that fills the view costs about 10 ms of GPU time at 2560 x 1600,
  because each water pixel traces its own rays every frame.
- Light from a small lamp takes several seconds to stop shimmering.
- A reflection shows surfaces lit by the sun and a flat sky term, without
  their own highlights or sky light, and misses thin things far away.
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

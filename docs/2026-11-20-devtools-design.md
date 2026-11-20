# Strata devtools: an agent drives a running Strata app

## Intent

An agent working on Strata can look at and act on a live viewer instead of
relaunching it for every look: check its own visual work, use the screens it
builds, see warnings as they happen. Today the viewer is driven only by about
70 `STRATA_*` switches read at start, and a scripted run is "pose N, draw N
frames, write a PPM, exit".

It is a development tool. It started in the viewer; since 2026-11-20 its
protocol lives in the library (`lib/strata/devtools.march`, `Strata.Devtools`)
so a game built on Strata (Ur first) can be driven and measured the same way.
What a game carries is inert unless `STRATA_DEVTOOLS` is set: each call is a
string comparison, `record` keeps nothing, and no folder is touched. The
library holds only the protocol and the commands every app shares; an app's
own commands (the viewer's camera and edits) stay in the app.

## Shape

Two parts, joined by a folder:

- **The app** (`Strata.Devtools` in `lib/strata/devtools.march`, called from
  the app's frame loop: the viewer's `dev_one`, Ur's `run_frames`), on only
  when `STRATA_DEVTOOLS=<dir>` is set. Once a frame it lists
  `<dir>/in/`, takes each request file in name order, applies it at one point
  in the frame loop, writes the reply to `<dir>/out/<id>.json` (written to a
  temporary name and renamed, so a reader never sees half a reply) and
  removes the request. Off, it costs one string comparison a frame.
- **The relay** (`tools/strata-mcp/server.mjs`), an MCP server over stdio in
  Node with no dependencies. It launches the app as its child (the viewer, an
  app given to `launch`, or the project's default: `STRATA_MCP_APP_BIN` and
  `STRATA_MCP_APP_CWD`, which Ur's `.mcp.json` sets), so it owns the app's
  output for `log`; turns each MCP tool call into a request file, waits for
  the reply, and encodes screenshots as PNG (Node's own zlib).

### Approaches considered

- **A TCP port in the viewer.** March's `Socket.listen` binds every address
  (`INADDR_ANY`), so a viewer that can edit the world and write files would be
  open to the network, and macOS would ask to allow incoming connections on
  every hidden run. Polling a socket from the frame loop also needs a
  millisecond timeout or a worker task. Rejected.
- **The MCP server inside the viewer.** Ties the protocol to the frame loop
  and to March's JSON and stdio, and the viewer could not report its own
  crash. Rejected.
- **A folder (chosen).** Local only by construction, no capability beyond the
  file access the viewer already has, no threads, a natural log, and the
  relay outlives a viewer crash and can say so.

### The request format

One line of `key=value` words, the first word the command:

    camera_set x=12.5 y=84 z=-3 yaw=1.57 pitch=-0.2

Replies are JSON objects written by the viewer: `{"id":…,"ok":true,…}` or
`{"id":…,"ok":false,"error":"…"}`. The viewer never parses JSON.

## Commands (phase 1)

| Command | Does |
|---|---|
| `status` | frame, paused, voxel size, the camera |
| `camera_get` | position (metres), yaw and pitch (radians), flying |
| `camera_set x y z yaw pitch` | any subset; switches to flying so the camera stays put |
| `look_at x y z` | turns the camera to face a point |
| `scene n` | the fixed pose n the goldens use |
| `light mode sun` | light mode 0..5, sun height in degrees |
| `block_at x y z` | the block at a point in metres, and its voxel |
| `raycast x y z dx dy dz max` | the first block hit; without a ray, from the camera along its view |
| `edit op=set\|box\|sphere\|blast …` | through the viewer's own edit path (`Strata.Sim`), so fluid, physics and the save store hear of it |
| `regenerate` | as the R key |
| `pause`, `resume`, `step n` | hold the world still, or advance exactly n frames (the reply waits for them) |
| `screenshot` | the next frame drawn, as a PPM the relay turns into a PNG |
| `stats` | the last frame's times (frame, March, shim, waiting, GPU), draw calls, chunks waiting, fluid cells, bodies |

Relay-only tools: `launch` (scene, size, extra switches; hidden or shown),
`quit`, `log` (the viewer's output since a cursor).

Shared commands, answered by `Strata.Devtools.handle` for any app (the viewer
answers its own versions of the first five): `status` (for an app with none),
`pause`, `resume`, `step n`, `screenshot`, `stats`, and the measuring ones
below.

## Measuring (added 2026-11-20)

From the hand benchmarking and profiling that took Ur's metro demo (100,000
citizens) from about 28 to about 200 frames a second, turned into tools.

In the app (`Strata.Devtools`):

| Command | Does |
|---|---|
| `perf_mark` | starts a window: `perf_report since=mark` covers frames after it |
| `perf_report frames=n since=mark from to` | for every recorded field: median, p90, mean of the slowest tenth, max; then findings |
| `perf_values field=k …` | one field's raw values over a window, oldest first |

Each frame the app calls `record(dev, nums)` (or `record_timed(dev, nums, dt)`,
which also keeps `game_s`, the game clock summed from the frame's `dt`) with
its own numbers (`frame_ms`, Ur's `sim_ms`, …); the library adds the
renderer's: `renderer_ms`, `waiting_ms`, `draw_calls`, `gpu_ms`,
`batch_instances`, and the churn between the last two frames from
`Vk.stat` 36..39: `instances` (`chunk_instance` calls), `motions`
(`chunk_motion` calls), `commits` (mesh uploads), `synced` (chunk-table
entries). It keeps the last 1,200 advancing frames; a paused frame is not
recorded. `from`/`to` select frames by `game_s`, so a faster build and a
slower one are compared over the same stretch of game time, not the same
frame numbers (a start-up that follows the clock lands on different frames).

Findings, in words: frames over twice the median and the fields that rose
most in them; slots placed again every frame (`instances`), meshes rebuilt
every frame (`commits`), heavy chunk-table churn (`synced`); GPU-bound.

In the relay:

| Tool | Does |
|---|---|
| `bench_compare a b rounds frames window/from_s,to_s fields` | alternating runs (A B B A …) of two builds, each launched hidden, paused before it is read; pooled median, p90, slowest-10% mean and max per field, per-run medians, and the difference |
| `profile seconds thread` | macOS `sample` on the app: where the work goes, waits left out, every thread together by default (a simulation on a worker counts); runtime functions (allocation, `memmove`, array sets, refcounting, drops) charged to the March functions that called them |
| `screenshot_diff a b take threshold save_diff` | pixels that differ, their box and the largest difference, a diff image; for proving a render change moved nothing else, and that a deliberate break shows |

Phase 2: player input through `Strata.Input`, and debug overlays.

## Set up now for a fixed timestep later

The later phase makes `step n` give the same frames every time and replays a
session's commands as a scripted run that becomes a stored test. Built in
from the start:

- Commands are applied at one point in the frame loop, never mid-frame.
- Every command applied is appended to `<dir>/commands.log` with the frame it
  took effect on.
- The frame's time step and the simulation clock are read through
  `Devtools.dt` and `Devtools.clock`; pausing works there, and a fixed step
  replaces the clock there later.
- `step` and `pause` are in frames, not seconds.
- Arguments are metres and voxels, never "where the mouse is".

## Limits

- Pause holds the simulation (walker, physics, fluid, the match's clock);
  GPU particles already in flight keep flying.
- The camera commands move the viewer's own camera; in a match the match
  places the player and wins.
- One viewer per relay.

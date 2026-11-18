# Ur browser preview

From the Strata root, run:

```sh
browser/ur/build.sh
python3 -m http.server 8765 --bind 127.0.0.1 --directory browser
```

Open [Ur](http://127.0.0.1:8765/ur/) in a browser with WebGPU. Click a
highlighted shoreline parcel to found a harbour, then use the road and
zoning tools. Use **Overpass** to select two existing streets at the ends of a
clear, straight crossing. **Advance month** settles the economy. The clock in
the 3D view drives the lighting and street activity.

The build uses the Ur checkout at `~/code/ur` by default. Set
`STRATA_UR_HOME` to override it. The March checkout selected by
`scripts/env.sh` must include the WASM arithmetic and string helpers,
the `math` host imports, and opt-in native array reference counting.

Ur runs its March parcel simulation behind a WebGPU terrain renderer. Toggle **3D terrain** to switch between
the relief view and the top-down planning map. Both views edit the same city.
The 3D view uses the desktop orthographic camera. Focus the map and use
the arrow keys to pan, Q/E to orbit, and Home to reset. Scroll or use +/− to zoom.
Each page load starts a new city. The Ur allocator reclaims unreachable
blocks between commands and reuses them. WASM retains its high-water page
count, and the preview caps linear memory at 256 MiB.

Run the long-session check with:

```sh
node browser/ur/test-memory.mjs
```

Use `browser/ur/build.sh --relink` to rebuild the C runtime and bridge
against the last emitted Ur IR after working on the allocator.


Open the [starter town](http://127.0.0.1:8765/ur/?showcase) to see houses
and a workplace immediately. The 3D renderer draws quarter-metre pieces
from Ur’s March architecture generator using the simulation’s building
levels. Clicking a roof or facade targets its parcel.

Open the [traffic preview](http://127.0.0.1:8765/ur/?traffic) for a
compact city with an overpass and active commuters. This preset starts at
09:00 and cycles through 24 street hours in 24 real seconds.

The build uses Ur’s desktop terrain, architecture, packed quads, textures,
deck meshes, vehicle poses, and pedestrian gait. WebGPU uses the desktop
texture filtering, junction geometry and signal phases, and distant-sea
lighting. Sunlight traces the terrain and posed deck voxel volumes on the GPU,
then reuses and refines the visibility samples across frames. Powered streetlights,
windows, and vehicle headlights use the desktop lighting formulas. Indirect
voxel lighting and reflections remain desktop-only. Lighting and signal changes remesh
only the batches that contain affected parcels.

Run the scene regression check with:

```sh
node browser/ur/test-scene.mjs
```

## Compare desktop and WebGPU

[Day](http://127.0.0.1:8765/ur/?compare=day) and
[night](http://127.0.0.1:8765/ur/?compare=night) freeze the traffic fixture
at 09:00 and 20:00, respectively. Both use the desktop orthographic camera
and a 900 × 660 viewport. The native runner draws the scene without the HUD.

Run both builds against the same Ur checkout. With the preview server running
and a WebGPU-enabled Chrome debugging port at 9229, capture the pairs from
the Strata root. The image assembly script uses Pillow.

```sh
browser/ur/build.sh
browser/ur/compare/native.sh
node browser/ur/compare/capture.mjs day
node browser/ur/compare/capture.mjs night
python3 browser/ur/compare/pair.py day
python3 browser/ur/compare/pair.py night
```

The captures and paired images are in `browser/.build/ur-comparison/`.

Use `?sun=raster` to select the previous sunlight renderer. On a Chrome
adapter with timestamp queries, run the live scene benchmark in both modes.
It reports GPU submission duration and browser animation-frame cadence.

```sh
node browser/ur/compare/capture.mjs day http://127.0.0.1:9229 'http://127.0.0.1:8765/ur/?gpuTiming=live&sun=raster'
node browser/ur/compare/capture.mjs day http://127.0.0.1:9229 'http://127.0.0.1:8765/ur/?gpuTiming=live'
```

# Map generation: the map grammar, buildings, and strata_maps

Status: design, for approval. Nothing is built.
Date: 2026-11-20
Follows: `2026-11-12-population-design.md` (population, props, urban
tools), `2026-11-20-plugins-tactics-design.md` (plugins, the tactical grid),
`2026-11-20-mech-construction-design.md` (mechs, rubble).

## Intent

Steel Rain ("FFT meets XCOM meets MechWarrior") fights its battles on
procedurally generated maps. The owner wants:

- **bounded, roughly square maps**;
- **a strong building grammar** for buildings that look realistic;
- **a map grammar that makes maps interesting**: the first maps open and
  gentle, teaching terrain and movement as the first FFT and XCOM maps do;
  deeper into a run, more variety: extreme height differences, big rivers,
  dense buildings, open plains, lava, thick forests.

The setting is the near future. Settlements range from **industrial
areas** to **ruined city quarters** to **medium-density exurbs** of up to
four storeys with trees.

This design covers how a map is made, what it is made of, and how it is
checked for play. The generic parts are Strata tools and a shareable plugin,
`strata_maps`; Steel Rain supplies data.

## Decisions the owner made

- Maps are bounded and roughly square.
- Early maps are open and gentle, introducing terrain and movement; deeper
  maps open up in variety (extreme height, big rivers, dense buildings,
  open plains, lava, thick forests).
- **Layered blends for most maps, plus some archetypes**, which are named,
  tuned blends.
- Near-future setting; three settlement styles: industrial areas, ruined
  city quarters, medium-density exurbs (up to four storeys, trees).
- **Approach C**: a layered pipeline, with authored set pieces placed by
  the grammar.
- Plugin rule carried over: generic parts are shareable, nothing in Strata
  core names a game concept, `strata_maps` uses only public Strata.

## Designer's calls in this document (overturn any)

- **Missions are objective sites with requirements.** The generator knows
  no mission types. A mission asks for sites ("two spawn zones on opposite
  edges", "a defensible high point", "an exit far from the player's
  spawn", "a target building of size L"); the generator places them and
  the tactical check proves them.
- **Ruin is a setting, not a style.** A ruined quarter is a city quarter
  generated whole, then broken by a damage pass that uses the game's own
  blast and fracture, so pre-ruined and freshly destroyed buildings look
  alike. Any map can be dialled from intact to wrecked.
- **Buildings are hollow with real floors** (the urban tools' level 2:
  floors, stairwells, openings), so a blasted wall shows rooms. Rooms and
  furnishing (levels 3 and 4) come later.
- **Scale:** world voxels 0.4 m (as the mech design set), tactical tiles
  of 8 voxels (3.2 m), mechs about 5 m tall, storeys 3.2 m (8 voxels), so a
  four-storey building is about 13 m. Maps up to 64 x 64 tiles (205 m a
  side, 512 voxels); heights up to 40 m of relief (100 voxels) on extreme
  maps.
- **A backdrop ring** of 16 tiles round the playable square: the terrain
  and layers continue at lower intensity with no objective sites, so the
  edge does not look like the end of the world. The tactical grid covers
  the playable square only. Its edge is shown by the overlay.
- **Lava is static in this project:** a glowing, non-colliding hazard
  block (it lights its surroundings) the tactical layer reads as a terrain
  kind. Flowing lava is a later project.
- **Rivers are water** in Strata's fluid model, so a breached bank drains
  as it does on the island.
- **Trees move into Strata core** as a population generator kind
  (`branching`, as the population design planned), out of the viewer, and
  are **stamped as world voxels** (not props) in this project, so forests
  block movement, sight and give cover through the tactical grid as any
  voxels do; blasted trunks bring their crowns down through
  `Strata.Support` and settle as rubble. (Props would be invisible to the
  grid, line of sight and cover today.)
- **How a map enters the world:** `strata_maps` turns its map data
  (heights, water, roads, buildings, set pieces, vegetation, hazards) into
  a **terrain generator** (`Strata.Terrain.Gen`: a pure function from chunk
  key to chunk, reading the data), so the stream builds the world like any
  other. Only the damage pass is **edits**, applied by the game. A save is
  the seed, the settings and the edits. To check a map, `generate` builds
  its own scratch world from that generator over the playable square; it
  never changes the game's world (the plugin rule).
- **Past the ring** the base terrain continues flat at the ring's edge
  height with nothing on it, out to the fog. The tactics camera's focus is
  kept within the playable square (a small `strata_tactics` addition).
- **The damage pass imitates destruction** rather than simulating it:
  hashed blasts with `Strata.Edit.blast`, the voxels `Strata.Support` says
  come loose removed, and rubble heaps stamped below where they fell,
  deterministically and without physics. Close to in-play destruction, which
  ends as rubble blocks too. (Running physics before battle to settle
  everything would not fit the budget reliably.)
- **Art in this project is placeholder, built in code:** set pieces
  (tanks, cranes, pipe runs, containers, a wrecked vehicle, a tower) from
  simple primitives, and Steel Rain's materials as blocks with plain
  textures. Real MagicaVoxel art replaces them later, through the same
  templates.
- **The 3 s budget is per attempt.** Cheap checks run early (spawns
  connected over the height field before buildings go in) so whole-map
  retries are rare; the generator gives up after 8 attempts and reports.
- **Generation runs before a battle and may take a few seconds**; a target
  of under 3 s for a 64 x 64 map in release, the tactical check included.
- **Deterministic:** the same seed and settings give the same map.

## Goals, non-goals, anti-goals

Goals:
- In Strata core: the **urban tools** the population design planned:
  roads (a graph rasterised to voxels with ground adaptation and bridges),
  blocks and lots, ground adaptation under footprints, and the **building
  grammar** at levels 1 and 2, with **styles** a game declares; plus
  stamping a template or building as an edit, and the tree generator as a
  population generator kind.
- **`strata_maps`**, a plugin: the map pipeline, its layers and their
  knobs, archetypes as named settings, set-piece placement, the damage
  pass, objective sites, and the tactical check, generic for any tactical
  game.
- Steel Rain's three building styles and a starter set of archetypes,
  written as data in an example, so the look can be judged.
- An example that generates maps from seeds and settings, renders them, and
  shows the check's verdict and the objective sites.

Non-goals (this project):
- Building interiors past level 2 (rooms, furniture).
- Flowing lava, weather, day and night.
- Mission logic: the generator places sites; the game runs missions.
- The overworld and how it picks maps (Steel Rain).
- A city generator or simulation (the urban toolkit stance: tools, not a
  city engine).

Anti-goals:
- A map that fails its check reaching play.
- Buildings that do not look like buildings: walls without openings,
  floating roofs, doors into cliffs.
- Layers that fight: a city on a cliff face, forest in lava, a road into a
  river with no bridge.
- Game concepts (missions, Steel Rain's styles) in Strata core.

## Approaches considered

- **A. A layered pipeline.** Terrain base, then layers, then a tactical
  check. Endless variety and destructible by construction; the most
  generator work.
- **B. Authored tiles stitched by rules** (wave-function collapse).
  Good-looking quickly, but variety is bounded by the art made, and large
  features (extreme height, a long river) fight the tile grid.
- **C. A's pipeline with authored set pieces** (cranes, tanks, a crashed
  dropship, a landmark tower) placed by the grammar. **Chosen.** Set
  pieces are population templates, which Strata already places.

## The pipeline

A map is made from **settings** (a seed and every layer's knobs) in stages.
Each stage reads the stages before it and writes voxels and **data** the
later stages and the game use (heights, water, roads, lots, buildings,
sites).

1. **Base terrain.** A height field over the playable square and its ring:
   plains, rolling, highlands, canyon, plateau-and-cliff, each a recipe of
   noise, terracing and erosion (`Strata.Erosion`, `Strata.Thermal`),
   scaled by the **relief** knob (from gentle steps a mech can jump to
   cliffs that need a route round). Steps are quantised to the height
   step the tactical grid uses, so rises read as clean ledges, not ramps
   of half-steps.
2. **Water.** Rivers as paths from edge to edge (or edge to lake) carved
   into the height field with banks, width from the **water** knob (a
   stream to a big river); lakes in basins (`Strata.Flood`). Water is
   written as fluid blocks at rest.
3. **Roads and lots.** A road graph suited to the settlement style (a grid
   for exurbs, arterials and yards for industrial areas, a broken grid for
   city quarters), clipped to buildable ground; bridges where roads cross
   water; the ground adapted under roads (cut and fill to a grade). Blocks
   between roads, split into lots that each touch a road. The
   **settlement** knob sets how much of the map is built (none, a hamlet,
   a district, the whole map).
4. **Buildings.** Each lot gets a building from the building grammar in the
   lot's style, footprint fitted to the lot, ground adapted under it.
   Height and density follow the **density** knob within the style's
   limits (exurbs at most four storeys).
5. **Set pieces.** Authored templates placed by rules in zones: industrial
   equipment in yards, landmarks on high points, wrecks on roads. Each set
   piece declares its footprint, what it needs under it, and how often.
6. **Vegetation.** Trees (the `branching` generator) and ground cover
   through the population system, density from the **vegetation** knob
   (scattered trees to thick forest), kept off roads, water and lots'
   footprints, thicker by water.
7. **Hazards.** Lava pools and channels in basins or along a canyon floor,
   area from the **hazard** knob; ground round them scorched.
8. **Damage.** The **ruin** knob (0 intact .. 1 wrecked): blasts and
   collapses on buildings, chosen by a hash, using Strata's blast and
   fracture; debris left as rubble blocks (not physics bodies) so the map
   starts at rest; craters on open ground at high ruin.
9. **Objective sites.** The game's requirements placed: spawn zones,
   points, exits, targets, each with its rule (distance, height, size,
   which edge).
10. **Tactical check.** The tactical grid built over the playable square
    (`strata_tactics`), then measured: the spawns connected, by at least
    two routes not sharing a choke tile; every site reachable from both
    spawns; cover tiles neither absent nor everywhere; no spawn in a
    hazard or water; sight lines not all blocked between spawns. A map
    that fails is regenerated from the next seed (the settings are kept);
    after a bounded number of tries the generator reports what failed most.

Each layer may be off. A layer that is on but finds no room (no buildable
ground for a settlement) reports it in the map's data.

## Layers and knobs

Every knob is a number from 0 to 1 the game sets; the stages map it to
lengths and densities through tables the game can change.

| Knob | 0 | 1 |
|---|---|---|
| relief | flat | 40 m relief, cliffs |
| water | none | a big river (about 25 m wide) |
| settlement | none | the whole map built |
| density | low, spread out | dense, tall within the style |
| vegetation | bare | thick forest |
| hazard | none | large lava areas |
| ruin | intact | wrecked |

Plus choices: the base recipe, the settlement style, the road pattern.

**Compatibility rules** keep layers from fighting: buildable ground is
under a slope limit and off water and hazards; trees avoid roads and
footprints and lava's scorched ring; rivers bend round set pieces' hard
footprints; a style may forbid a base (no exurbs on a canyon floor). Rules
are data with defaults; a map whose layers leave a layer no room reports it.

**Archetypes** are named settings that fix some knobs and choices and
narrow others ("Lava Foundry: canyon base, relief 0.6-0.8, hazard 0.7,
industrial settlement 0.3, ruin 0.2"). **Blends** draw every knob within
limits the game sets (by run depth in Steel Rain). Both are the same thing
to the pipeline: settings.

## The building grammar (Strata core)

The population design's grammar generator, now specified. A building is
made from a **lot** (a polygon touching a road, with its street side) and
a **style** (data the game declares):

- **Mass:** the footprint fitted to the lot (setbacks, coverage), split
  into one or more volumes (a main block, wings, a tower), each a number of
  storeys within the style's range; for industrial styles, halls with
  large spans and low roofs.
- **Facade:** each wall split into bays; each bay on each storey gets an
  opening type from the style's rules (windows in rows, shopfronts on the
  ground floor facing the street, loading doors on industrial walls, blank
  walls where buildings meet); corners and cornices; materials by storey.
- **Roof:** flat with parapets, low pitched, saw-tooth for halls, with
  roof clutter (plant, tanks, vents) from the style.
- **Hollow (level 2):** floors at each storey, a stair core aligned through
  them, the openings real holes, interior faces in the style's interior
  material.
- **Data out:** the volumes, storey heights, openings, the stair graph, and
  the footprint, so the damage pass, the tactical layer and a game can use
  them.

Each face of the grammar works at generation (declared, evaluated by
chunk, as population layers are) and as an edit at run time (stamp a
building onto a lot), returning the usual edit outcome.

**Styles** are data: storey height range, coverage, volume rules, bay
width, opening tables, materials (block ids), roof types, clutter, and
how a building of the style breaks (what the damage pass may remove: corners,
floors, facades).

Steel Rain's three styles (data, in the example):
- **Industrial:** halls and sheds, one to three storeys, wide spans,
  saw-tooth and flat roofs, loading doors, metal cladding, tanks and pipe
  runs as clutter; yards fenced.
- **City quarter:** four to eight storeys, perimeter blocks, shopfronts at
  street level, regular windows, flat roofs with parapets. Usually built
  with ruin up.
- **Exurb:** two to four storeys, detached and terrace houses and small
  apartment blocks, pitched and flat roofs, yards with trees.

## Objective sites

A site requirement names: a kind (zone, point, building), a size in
tiles, rules (on an edge, far from another site by a distance, on high
ground, in or near a settlement, open sky), and how many. The generator
places them after the damage pass; the tactical check proves them
reachable. The game's missions are lists of requirements (Steel Rain's:
wipe out, defend, escape, destroy, recover).

## strata_maps

A plugin under `plugins/strata_maps/` (forge package, public Strata only,
uses `strata_tactics` for the check). It offers:
- `generate(settings, requirements, world) -> (world edits, map data,
  check report)`, run once before a battle (the plugin shape's `setup`
  stage; it has no per-frame part);
- settings, knobs, archetypes, compatibility rules and styles as data
  types with defaults, and a data-file loader so archetypes and styles
  can be tuned without rebuilding;
- the map data: heights, water, roads, lots, buildings, set pieces, sites,
  the grid, and the check's measurements.

## Testing and the example

- Unit tests per stage on small maps: terrain recipes within their relief;
  rivers edge to edge with banks; every lot touching a road; every
  building inside its lot with an opening on its street side and floors
  aligned with its stairs; no tree on a road; lava only in its zone; the
  damage pass leaves no floating voxels; sites obey their rules; the check
  rejects a map with a single route and accepts one with two.
- Determinism: the same seed gives the same voxels (a hash).
- Budget: a 64 x 64 map with every layer on generates in under 3 s in
  release (timed, asserted at 4x).
- **Example** `plugins/strata_maps/example/`: generates a map from a seed
  and an archetype or a blend, flies the camera over it (the tactics
  camera), shows the sites and the check's verdict, and steps through
  seeds and archetypes by key; scripted mode writes pictures of each
  archetype for `docs/`. The owner judges the look from these.

## Left to the builder

- The exact noise recipes for each base, within the relief targets.
- The road layout algorithms for each pattern.
- The lot-splitting algorithm.
- Opening tables and bay widths of the three styles (to be judged by eye).
- How the check counts routes (edge-disjoint paths over the tile graph is
  enough).
- The data-file format for archetypes and styles.
- Order of work, as long as each stage lands with its tests and a picture.

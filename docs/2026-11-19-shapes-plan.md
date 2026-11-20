# Strata shapes: build plan (population stage 4, props)

**Goal:** population props as static fine-voxel shapes that are drawn,
shadowed, hit by rays, carved, fall when cut, and are saved.

**Spec:** `docs/2026-11-19-shapes-design.md`. Rulings made on the way:
`docs/shapes-ledger.md`.

## Global constraints

- Branch `claude/hungry-pasteur-784291`; not merged into main. Commit
  after each task. Merge main in before the final gate.
- `lib/strata/physics.march` gains public calls only (another session is
  repairing its goldens and a slow test); its stepping is not touched.
- The viewer keeps stamped trees unless `STRATA_PROPS=1`, so no stored
  golden changes.
- March first; C only in the renderer (`native/strata_vk.c`), where the
  GPU is.
- Every viewer run needs `MARCH_PIN_MAIN=1`.
- Timings are judged only under a load average of 8; otherwise recorded
  with the load beside them.
- Known failures on main that are not this work's: goldens
  `tilted-slab-lit`, `body-by-lake`, `combat-burning`, and the slow test
  in `test/physics_test.march`.

## Tasks

### Task 1: `Strata.Shape`

Files: `lib/strata/shape.march`, `test/shape_test.march`.

- [x] The type: grid, voxel size, pose (corner, quaternion), mode; the
      three mode constants; `of_template(t, turn)` (the turn baked into
      the grid, so a placed prop's rotation is the identity and its
      voxels sit on the fine lattice).
- [x] `palette`, `bounds` (world box), `solid_count`.
- [x] `ray(shape, registry, origin, dir, max, stop)`: nearest voxel, face
      and distance, stepping the grid in the shape's frame.
- [x] `carve_sphere`, `carve_box` and `carve_where`: the grid left and the count taken.
- [x] `held(registry, world, shape, world_scale)` and `parts(registry,
      world, shape, world_scale)`: the standing grid and the loose parts
      as shapes.
- [x] `encode` / `decode` (palette and run-length bytes; `Result`).
- [x] Tests as the design's section 11 lists for a shape.

### Task 2: prop layers in the population

Files: `lib/strata/population.march`, `lib/strata/populate.march`,
`lib/strata/population_file.march`, their tests.

- [x] `Population.props(p, layer, fine_set)`; `freeze` checks; `fit_all`
      leaves prop sets alone and checks the ratio; readers
      `layer_props(f, l)` and `layer_detail(f, l)`.
- [x] `props = NAME` in a population file.
- [x] `Populate.place_with` skips a prop layer's stamps at level 0 when
      the world is coarser than the detail; `Populate.props_with` lists
      the chunk's props.
- [x] Tests: the listing against `instances_with`; a column's chunks
      partition its instances; nothing stamped at level 0, stamps above;
      the same declaration stamps in a world at the detail scale.

### Task 3: `Strata.Shapes`

Files: `lib/strata/shapes.march`, `lib/strata/terrain.march`
(`with_props`), `test/shapes_test.march`.

- [x] Prototypes; `add_chunk(shapes, key, props)` and `drop_chunk`;
      ids; the pose of an instance.
- [x] `raycast`; `carve_sphere` (standing shapes carved, loose parts
      returned as shapes for physics); `unrooted`.
- [x] The memory: gone and carved by hash, applied when a chunk's props
      arrive.
- [x] `sync(shapes, stream, gen, seed)`: worker tasks for the chunks
      `take_changed` lists.
- [x] Draw lists for the renderer: what appeared, changed and left since
      the last take.

### Task 4: drawn and in the rays

Files: `native/strata_vk.c`, `lib/strata/ffi/vk.march`,
`lib/strata/render.march`.

- [x] `sv_body_instance(slot, prototype)`: a body slot sharing another's
      mesh ranges, with its own pose and voxel size.
- [x] `sv_body_share(handle, from)`: a ray row sharing another's class
      bytes.
- [x] `Strata.Render.shapes(r, shapes)`: prototypes meshed once,
      instances placed, carved shapes meshed alone, gone shapes freed.

### Task 5: physics takes a shape

Files: `lib/strata/physics.march` (two public calls), `test/shapes_test.march`.

- [x] `Physics.add_shape`, `Physics.shape_of`.
- [x] A cut trunk in a test world: the standing part is held, the rest
      is one dynamic body whose grid holds the prop's voxels above the
      cut.

### Task 6: the viewer

Files: `viewer/lib/strata_viewer.march`, `viewer/assets/island.pop`,
`scripts/golden.sh`, `test/golden/`.

- [x] `STRATA_PROPS=1`: tree templates built at 0.25 m as a second set,
      `props = trees_fine` on the forest layer, the generator's props
      function, shapes synced and drawn each frame.
- [x] The blast carves shapes and hands loose parts to physics; the
      aiming ray sees shapes.
- [x] Scenes: the forest as props; a tree cut by a scripted blast and
      fallen. Goldens stored and looked at.

### Task 7: saved

Files: `lib/strata/shapes.march`, `viewer`, `scripts/save_check.sh` or a
new check.

- [x] `Shapes.save` and the region section read in `sync`.
- [x] A test: carve, encode, decode into fresh shapes: the same stand.
- [x] The viewer writes and reads the section; a carved tree is carved
      after quit and load.

### Task 8: measured and written up

- [x] Cost per chunk (listing) and per frame (sync, renderer rows), a
      blast on a prop; `RESULTS.md`.
- [x] `README.md`: Population limits, the modules table, a Shapes
      section.
- [ ] Merge main; `scripts/verify.sh`.

### Task 9 (if time allows): settled bodies, proxy boxes, promotion

- [ ] `Shapes.settle`: sleeping fine bodies become static shapes and are
      saved (design section 7).
- [ ] Proxy boxes for walkers; promotion of static shapes near awake
      bodies to pinned bodies (design section 8).
- [ ] Site layers as prop layers (the ruins' loose parts).

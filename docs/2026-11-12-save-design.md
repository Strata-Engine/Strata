# Strata save and load: design

Status: design, awaiting approval. Nothing here is built.
Date: 2026-11-12
Follows: `2026-11-10-strata-design.md`, whose project 4 promised a save
format and whose first project made the chunk type serialisable for it.
Builds on `Strata.World` (the edited-chunk cache, the far field's coarse
levels, `regenerate`), `Strata.Stream` (chunk generation on workers), and
the entity snapshot of `2026-11-11-entities-design.md` section 8. Planned
against `main` as it stands, not the entities branch it was written on,
which was 61 commits behind (the far field, `regenerate` and a newer March
toolchain landed on `main` meanwhile).

## Intent

Every edit a player makes is lost when the game quits. This project keeps a
Strata world on disk while it is played, so a game can be quit at any moment
and picked up again: the blocks as the player left them, the entities, and
the game's own state. Strata provides the store; the game decides when to
open one, what its own data means, and what a save slot is.

## Decisions the owner made

1. **Settled, not exact.** A loaded world is the world at rest. Rubble that
   was falling when the game stopped is not resumed in the air; water starts
   flowing again where the world was edited; entities come back unseen and
   are set on the ground as the camera reaches them.
2. **The world saves as it plays.** There is no "save" moment the player
   waits for. Quitting at any time loses at most a few seconds.
3. **Region files** (approach A below).
4. **Broadly usable.** The store serves every kind of game Strata is for:
   the arena, bullet storm and city sim the entities design names, and
   games with no generated terrain at all. Nothing in it names a game
   concept.
5. **A changed generator loads with seams.** When a game ships a new
   terrain generator (or Strata changes one of its own), an old store still
   loads: chunks the player edited keep their saved content, and everything
   never touched regenerates with the new generator. A seam can show where
   the two meet. Only a changed block registry refuses.
6. From the first design: frame rate is the priority, the library names no
   game concept, and nothing reachable from library code panics.

Everything else below is the designer's call.

## Approaches

**A. Region files (chosen).** The world on disk is a folder: a small header
file and one file per region, each holding the edited chunks of a block of
the world. When a chunk changes, its region is rewritten by a worker from a
copy, through a temporary file renamed over the old one. Save cost follows
what changed, not the world's size; it extends the in-memory cache
`Strata.World` already keeps; nothing new comes in. Minecraft's model.

**B. An edit journal.** Every edit appended to a log, folded into a
snapshot now and then. Tiny, crash-safe writes, but loads replay the log,
folding is its own machinery, and edit operations need a stable encoding
as well as chunks. Rejected: loads slow down the longer a session runs.

**C. An embedded database (SQLite).** Robust and transactional, but a large
C dependency. The owner's standing rule allows C only as a marked stopgap,
and forge does not build a dependency's C, so every game would carry it.
Rejected.

## Goals

- A game opens a **store** (a folder it names) for its world, and from then
  on edited chunks reach disk on their own as they change and unload.
- The store keeps exactly what cannot be regenerated: every chunk that
  differs from its generation, and every edit queued for a chunk that is not
  loaded (`Strata.World`'s pending edits).
- The store holds the **entity snapshot**, rewritten every few seconds and
  on `flush`.
- The store holds the **game's own data at three levels**, as byte strings
  under names the game chooses, which Strata stores and never reads:
  - **world sections**: one per name for the whole world (a quest log, the
    city's budget, the match score);
  - **region sections**: one per name per region, saved with that region's
    chunks (a city's zoning or ownership map for that area, crop growth,
    building health), so large per-area game state also saves as it plays
    and costs nothing for regions never touched;
  - **player records**: one per player id (inventory, position, stats), so
    a split-screen or networked game keeps players apart.
- **Loading** opens the store and hands the world back: chunks come from
  disk when the stream wants them, read on the stream's workers, never on
  the frame.
- Edited chunks that are not loaded **leave memory** once on disk, so a long
  session's memory no longer grows with everything ever edited (today the
  cache only grows).
- **Worlds with no generated terrain** work the same way: a game whose map
  is built by in-game edits uses a generator that makes empty chunks, and
  the store holds every chunk that has anything in it.
- A store made by a different block registry, voxel scale or seed is
  **refused with a reason**, never loaded wrong. A store made by a different
  generator version **loads with seams** (owner's decision 5), and every
  region records the generator version that wrote it, so a game can find
  the seams if it cares.
- The **far field keeps showing edits** after they leave memory: a crater
  or a city district does not vanish from the horizon because its chunks
  were written to disk.
- A crash of the game process at any moment leaves the store **readable**:
  every file is either its old content or its new content, never half of
  each. (Power loss and operating-system crashes are not covered: March's
  standard library has no way to force data to disk, and the owner's rule
  forbids C for it. This is recorded as a gap; see Risks.)
- A damaged region file **costs that region, not the world**: it is set
  aside, reported, and its chunks regenerate.
- Saving never makes a frame late: the frame only takes copies (chunks are
  immutable values, so a copy is a reference), and encoding and writing run
  on workers.

## Non-goals

- Exact resumption: physics bodies in flight, the fluid's active set, entity
  seen state, sound. (Owner's decision 1.)
- Migrating a store to a changed block registry. It is refused with the
  reason; a migration tool is later work. Room is left (the header records
  the registry's fingerprint).
- Smoothing seams between saved chunks and a changed generator.
- Importing a whole authored map into a store in bulk (a `put_chunk` path).
  A hand-built world here is one built by edits; bulk import is later work.
- Save slots, menus, thumbnails, cloud sync. A slot is a folder; the game
  manages folders.
- A network protocol. The region store is the natural base for a server
  later; it is not designed here.
- Compression beyond the chunk's own compact forms (uniform, paletted).
  Room is left (a per-region encoding byte).
- Concurrent access: one store is open by one game process at a time. A
  lock file makes a second `open` refuse instead of corrupting the store
  (designer's call, added while planning).

## Anti-goals

- A hitch when the world saves.
- A store that loads silently wrong: wrong blocks, half a region, a game
  section from one moment and chunks from much later presented as one
  state without saying so.
- A store format that names a game concept (a "player inventory" field, a
  "building" record).
- Library code that panics on a damaged, truncated or hostile file.
- C. The owner's standing rule: Strata code is March, and C only as a
  marked stopgap. File access is March's standard library.

## Constraints

- Builds with the March toolchain `main` uses (it moved to a newer March
  while this was designed: file and settings calls go through
  `Authority.must`). `scripts/verify.sh` stays green after every task.
  Gaps go in `GAPS.md`; March is not changed.
- File access only through the standard library: `File.write`, `File.read`,
  `File.rename`, `File.delete`, `Dir.mkdir_p`, `Dir.list`. Writes are whole
  files (a temporary file, then a rename).
- Workers are `Task`s, as `Strata.Stream` uses; nothing blocks the caller
  except an explicit `flush` or `close`.
- Every lookup total, every refusal counted and readable, as the other
  modules do.

## The design

### 1. The store on disk

A store is a folder:

    <store>/
      store.bin                  header: format version, identity, world sections, entity snapshot
      players/<id>.bin           one file per player record
      regions/r.<x>.<y>.<z>.bin  one file per region that holds anything

A **region** is a cube of 8 x 8 x 8 chunks (256 voxels a side; 256 m at
1 m voxels, 25.6 m at 10 cm). A region file exists only for a region with
an edited chunk, a queued edit, or a region section.

Every file starts with a magic number, the format version and a checksum
of the rest, and is written to `<name>.tmp` then renamed over `<name>`.
Leftover `.tmp` files are deleted when a store opens.

### 2. What the header holds

- **Identity.** The world's seed, its voxel scale, and the registry's
  fingerprint: a hash of every registered block's flags, style, layer and
  family, in id order, so a game that registers its blocks differently is
  caught. A different seed, scale or fingerprint refuses.
- **The generator version** current when the header was written: the
  game's Int combined with Strata's own generation version (raised by
  Strata whenever its built-in generators change their output). A
  difference does not refuse (decision 5).
- **World sections:** name to bytes.
- **The entity snapshot** (`Strata.Entities.snapshot`'s three arrays), or
  none for a game without entities.
- **A save counter**, raised on every header write. Each region file
  records the counter of the header write before it, so a reader can tell
  how far apart a region and the header are (for the game's information;
  a settled world accepts the difference).

### 3. What a region file holds

The generator version that wrote it. For each chunk of the region that
differs from its generation: its key and its content in `Strata.Chunk`'s
compact form. For each chunk with edits
queued: its key and the queued `Strata.World.Op`s in order. Then the
region's sections: name to bytes.

### 4. Saving as it plays

`Strata.Save.tick(store, stream, dt)` is called once a frame, after
`Strata.Stream.tick`:

- It takes the keys of chunks edited, unloaded edited, or queued since the
  last call (`Strata.World` gains a "changed since last asked" read, kept
  the way `Strata.Stream.take_changed` keeps its log, switched on only once
  a store is open, so a game without a store pays nothing), and marks their
  regions **dirty**.
- At most one region write is started per tick, oldest dirty first, after
  the region has been quiet for a short time (default 2 s, so a player
  digging a tunnel does not rewrite the same region every frame). The write
  task gets the region's chunks and queued edits as values, encodes them and
  writes the file.
- The header is rewritten when a world section or the entity snapshot has
  changed and at least the header interval has passed (default 5 s).
- Writes started earlier are collected two ticks later, as the stream
  collects its work; a failed write is retried on the next dirty pass and
  counted.

Once a region file holding a chunk is written, `Strata.World` drops that
chunk from its in-memory cache if it is not loaded: the store now holds it.
It keeps in its place a level-1 coarsening of the chunk (one voxel for each
2 x 2 x 2, an eighth of the size, made the way the far field already
coarsens), so the far field's coarse levels go on showing the edit without
reading disk. A game editing heavily across a whole world therefore still
grows memory, but by an eighth of what it grows today.

`Strata.World.regenerate` while a store is open is allowed: the game passes
the new generator version (`Strata.Save.set_generator_version`), the header
records it, and regions written afterwards carry it.

### 5. Loading

`Strata.Save.identity(dir)` reads only the header's identity (seed, scale,
generator version), so a game whose generator depends on the seed (the
island's atlas does) can build it before opening.

`Strata.Save.open(dir, world, generator_version)` answers either a store
and a world ready to stream, or a refusal with its reason (registry
mismatch, seed mismatch, scale mismatch, an unreadable header, a newer
format). An empty or missing folder makes a new store.

When the stream wants a chunk, its generation task first looks for the
chunk's region on disk: a region read is cached on the store (a small
number of decoded regions, least recently used out), so the 512 chunks of
one region cost one file read. A saved chunk takes the generated one's
place, and saved queued edits are applied, exactly as `Strata.World.insert`
treats its cache today. This runs on the stream's workers.

After loading, the game restores its entities from the header's snapshot
(`Strata.Entities.restore`), reads its world sections and player records,
and wakes the fluid around the chunks the store says were edited, so water
the player disturbed starts moving again.

### 6. Settling

The store saves continuously, so it does not settle anything on its own.
`Strata.Save.flush(store, stream, physics, snapshot)` is what a game calls
when it quits or leaves a world: it writes every loose physics body into the
world as blocks (`Strata.Physics.to_blocks`), takes a final entity snapshot,
writes every dirty region and the header, and waits for the writes. A game
that crashes loses only what changed since the last writes; bodies that
were in flight are lost, as decision 1 accepts.

### 7. The game's data

    Strata.Save.set_section(store, name, bytes) / section(store, name)
    Strata.Save.set_region_section(store, region, name, bytes) / region_section(store, region, name)
    Strata.Save.set_player(store, id, bytes) / player(store, id) / players(store)
    Strata.Save.region_of(x, y, z)            -- the region holding a point, in metres

Setting a world section or a player record marks the header or that
player's file dirty; setting a region section marks the region dirty. They
are written by the same `tick` pass. Bytes are opaque; versioning them is
the game's.

### 8. What a game sees

    Strata.Save.identity(dir)                        -- Option((seed, scale, generator_version)), read before building the generator
    Strata.Save.open(dir, world, generator_version)  -- Result((Store, World), String)
    Strata.Save.set_generator_version(store, v)      -- after Strata.World.regenerate
    Strata.Save.tick(store, stream, dt)              -- once a frame; dt drives the quiet time and header interval
    Strata.Save.set_entities(store, snapshot)        -- the game hands a snapshot when it likes; tick writes it on the header interval
    Strata.Save.entities(store)                      -- Option(Snapshot), for restore
    Strata.Save.edited_keys(store)                   -- for waking the fluid after a load
    Strata.Save.flush(store, stream, physics, snapshot)
    Strata.Save.close(store, stream, physics, snapshot)   -- flush, then release
    Strata.Save.stats(store)                         -- dirty regions, writes in flight, writes failed, damaged regions set aside
    sections, region sections, players: section 7

`Strata.Stream` gains one input: the store to read chunks from, given when
the stream starts (or none, as today).

### 9. Damage

A file that fails its checksum or does not decode is renamed to
`<name>.bad`, counted in the stats, and treated as absent: a damaged region
regenerates, a damaged player record reads as none, a damaged header makes
`open` refuse with the reason. Nothing panics.

## Budget and targets

| What | Target |
|---|---|
| `Strata.Save.tick` on the frame, nothing to write | under 0.05 ms |
| `Strata.Save.tick` on the frame, starting a region write | under 0.5 ms |
| A region of 512 edited paletted chunks, encoded and written on a worker | under 50 ms |
| First frame of a loaded world, compared with a new one | no slower than one extra region read per wanted region |
| `flush` with 100 dirty regions | under 2 s |

Measured and recorded in `RESULTS.md` under the load rule the other
projects use (judged only with a load average under 8).

## Testing

- **Round trips** (properties that raise on failure, per GAPS S31): random
  chunks and queued edits through a region file and back are equal; random
  sections, player records and entity snapshots through the header and back
  are equal.
- **The world through a store:** stream a world, edit it widely, unload
  what was edited, close the store; open it in a fresh process; stream the
  same places; the world hash equals the original.
- **Memory:** after edited chunks are written and unloaded, the in-memory
  cache holds none of them.
- **Crash safety:** a `.tmp` beside each file at open is ignored and
  deleted; a store whose write was cut between the temporary file and the
  rename opens with the old content.
- **Refusals:** a different registry, seed, scale, a newer format and a
  damaged header are each refused with their reason.
- **Seams:** a store saved with generator version 1 and opened with version
  2 loads; edited chunks keep their saved content, untouched ones come from
  the new generator, and the regions report version 1.
- **The far field:** after edited chunks are written and dropped from
  memory, the coarse chunks over them still show the edits (they equal the
  coarse chunks built before the drop).
- **Damage:** a truncated or bit-flipped region is set aside as `.bad`,
  counted, and its chunks regenerate; the rest of the world loads.
- **Generator-less world:** a world whose generator makes empty chunks,
  built by hand, saves and loads whole.
- **No store, no cost:** a stream with no store behaves and performs as
  today, and keeps no change log.
- **In the viewer:** `STRATA_STORE=<dir>` saves the walked world as it plays;
  quitting and starting again with the same folder shows the edits.

## Order of work

1. Encoding: chunks, queued edits, sections, the header and region formats,
   checksums, round-trip properties. Pure March, no files.
2. Files: atomic write, read, damage handling, `.tmp` cleanup, `open`'s
   refusals.
3. `Strata.World`'s change log and dropping written chunks from its cache.
4. `Strata.Save.tick`: dirty regions, quiet time, region writes on workers,
   the header interval, player files.
5. Loading through `Strata.Stream`: region reads on generation workers, the
   region cache.
6. `flush` and `close` with settling; entities and fluid wake after a load;
   the world-through-a-store and generator-less tests.
7. The viewer's `STRATA_STORE`; budgets into `RESULTS.md`; README section.

## Left to the builder

- The exact byte layouts, checksum choice, magic numbers and the region
  cache's size, provided the targets are met.
- The quiet time and header interval defaults (2 s and 5 s are starting
  points).
- How `Strata.World` represents "written to the store" so a chunk is
  dropped from memory only once its region write has finished.
- Names, where they do not collide with a March keyword (GAPS S1, S2, S4,
  S7, S21, S22, S29).

## Risks

- **The stream's worker reads a region while a write of the same region is
  in flight.** The read must see either the old or the new file; atomic
  rename gives that on one machine. The region cache must also be updated
  when a write finishes, or a reload could serve an older region than the
  chunk that was just unloaded. The builder must order these and test it.
- **Big regions under heavy editing.** A city sim that edits every chunk of
  a region rewrites up to 512 chunks per write. The quiet time bounds how
  often; the 50 ms target bounds the cost. If it is missed, the region size
  is the lever.
- **The header and the regions drift apart after a crash.** Accepted under
  "settled" (the entity snapshot may be a few seconds older than the
  blocks). The save counter makes the gap visible to a game that cares.
- **Power loss.** Without a way to force data to disk, a rename can reach
  the disk before the data it names. After a power cut a region can then
  fail its checksum and be set aside, and its edits regenerate away. The
  builder records this as a GAPS entry (no fsync in March's standard
  library); a later March change, not C, is the fix.
- **Registry fingerprints are only as good as what they hash.** Two
  registries that differ only in textures load as the same; that is
  intended (textures do not change what a block is). Two that differ in a
  field the fingerprint omits would load wrong; the fingerprint must cover
  every field that changes behaviour.

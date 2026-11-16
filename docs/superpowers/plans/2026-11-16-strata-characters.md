# Strata Characters Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Twelve distinct characters, stored as MagicaVoxel files, loaded at start, played by the player and the match's fighters, each hit and collided by its own size.

**Architecture:** A library .vox reader (`Strata.Vox`) and a library builder that turns a .vox file following the character convention into a rig plus its size (`Strata.Character`). A Python script generates the twelve files. The viewer loads them into a roster, dresses fighters and the player from it, and replaces combat's fixed 1.8 m body with each character's size.

**Tech Stack:** March (forge), Strata's Rig/Anim, Python 3 for the generator.

**Spec:** `docs/superpowers/specs/2026-11-16-strata-characters-design.md`

## Global Constraints

- Voxels 1/16 m (`Rg.new(0.0625)`).
- Parts 0-14 exactly as today's humanoid (0 pelvis, 1 spine, 2 head, 3-5 left arm upper/fore/hand on +x, 6-8 right, 9-11 left leg thigh/shin/foot, 12-14 right); same two hand sockets, both legs declared (sole 0.0625), mask `[6]`. All nine `Fg.clips` play unchanged.
- Extras are parts 15 and up, parented to any earlier part, no clip keys.
- Rig limits: 64 parts, 128 voxels a side, palette 1..255.
- Weapons and sword reach (1.6 m, 2.4 m) do not change with the character.
- Hit box, collision, eye height and feet-to-middle follow each character's size.
- Trim shade belongs to the fighter (today's `shirt_in`), not the character.
- Missing or broken file: one printed line, that slot uses today's humanoid; the game starts.
- Hand-edited files are never overwritten silently by the generator.
- `scripts/verify.sh` passes; turning fight at 180°/s keeps p99 under 11.1 ms (`scripts/bench.sh`).
- Before forge: `. scripts/env.sh`. Hidden runs: `STRATA_HIDDEN=1 STRATA_OFFSCREEN=1 STRATA_PIXELS=1`.

## File convention (fixed here; Task 2 enforces it)

- One file per character: `viewer/characters/<id>.vox`, ids `brakk vesper rook unit7 mother skrell gorrum ixa sabine kade hex torvald`, roster order as listed.
- Each part is one model, named through its shape node's `_name` attribute: `pelvis spine head arm_l_upper arm_l_fore arm_l_hand arm_r_upper arm_r_fore arm_r_hand leg_l_thigh leg_l_shin leg_l_foot leg_r_thigh leg_r_shin leg_r_foot`, extras `extra:<parent name>:<label>` (parent may be another extra listed earlier in the file).
- Axes: rig x = vox x, rig y = vox z (up), rig z = vox y. The character faces vox +y. Left arm on +x.
- A model's box in figure space is its transform node's translation (MagicaVoxel centres a model on its translation: low corner = t - floor(size/2)), mapped through the axes.
- Anchors: pelvis, spine, head turn about the middle of their bottom face; arm and leg parts about the middle of their top face; extras about their low corner. Pivot = anchor point in the parent's grid (computed from the two boxes). Feet: the root pivot is placed so the lowest voxel of either foot sits at y 0.
- A joined part's anchor must lie within its parent's box grown by 1 voxel, or the file is refused.
- Sockets: middle of each hand's bottom face, plus 0.5 voxel down, identity rotation (as today, socket 0 left, 1 right).
- Palette: index 0 empty. Indices 241-244 are trim: their file colour is the neutral trim; in a team match 241-244 take the fighter's shade, each scaled by its file colour's brightness over 241's (so a darker trim stays darker). Material chunk (MATL) `_emit` gives emission (0..1, times `_flux` if present), `_rough` roughness (default 0.8).

## Review Focus

1. A file MagicaVoxel itself saves (version 200, with LAYR, rOBJ, rCAM, NOTE chunks the reader doesn't need) must load: unknown chunks are skipped. Test: fixture with extra unknown chunks loads (Task 1).
2. A model with zero voxels (a pure joint in the file) must load as an empty part, not fail. Test in Task 2.
3. A file with a part missing, a duplicate name, or an extra naming an unknown parent is refused with a message naming the part. Tests in Task 2.
4. An odd-sized model's centre rounding (MagicaVoxel's floor(size/2)) must put parts where the editor shows them; off by one breaks joins. Test in Task 2 with odd and even sizes.
5. A fighter that respawns, swims, or is struck by fire must use its own size everywhere `0.9`/`1.8` appear today, not some of them. Test in Task 5 (the grep check plus Ixa/Brakk hit tests).

---

### Task 1: The .vox reader

**Files:**
- Create: `lib/strata/vox.march`
- Test: `test/vox_test.march` (with a small encoder in the test module to build fixtures as bytes)

**Interfaces:**
- Produces:
  - `type Model = { name : String, sx : Int, sy : Int, sz : Int, tx : Int, ty : Int, tz : Int, voxels : NativeU8Arr }` (voxels as x, y, z, index quads)
  - `type Vox = { models : List(Model), palette : NativeU8Arr, emit : NativeF32Arr, rough : NativeF32Arr }` (palette 256 x RGBA, with MagicaVoxel's shift: file entry k is index k+1; emit/rough 256 each)
  - `fn parse(bytes : NativeU8Arr) : Result(Vox, String)`
  - `fn read(path : String) : Result(Vox, String)` (File.read then Bytes, as `Strata.Atlas.load_or_build` does)

- [ ] **Step 1: Write failing tests** in `test/vox_test.march`:
  - `parses one model`: fixture of SIZE 2x3x4 and XYZI with 3 voxels; `models` has 1, sizes (2,3,4), 3 voxels with the given indices.
  - `reads names and translations`: nTRN/nGRP/nSHP graph with two shapes, `_name` "head" at `_t` "1 -2 30" and "pelvis" at "0 0 0"; models carry those names and translations.
  - `default palette without RGBA`: index 1 colour equals MagicaVoxel's default palette entry 1.
  - `palette shift`: RGBA entry 0 (file) is index 1.
  - `materials`: MATL id 5 `_emit` "0.5" `_flux` "2" → emit[5] = 1.0 (clamped to 1); `_rough` "0.3" → rough[5] 0.3; unset → emit 0, rough 0.8.
  - `skips unknown chunks` (Review Focus 1): LAYR, rOBJ, NOTE chunks before and between; still parses.
  - `refuses bad magic` and `refuses truncated`: Err containing "VOX " / "truncated".
- [ ] **Step 2: Run** `. scripts/env.sh && forge test --filter Vox`. Expected: FAIL, module not found.
- [ ] **Step 3: Implement** `Strata.Vox` per the format (RIFF-style: "VOX " + version, MAIN with children; chunk = id, content size, children size). Dictionaries are (count, key/value strings). Shapes map to models in SIZE/XYZI order by model id; translation from the transform node directly above each shape (only one level of group nesting is needed; sum translations down the path for more).
- [ ] **Step 4: Run** the filter. Expected: all Vox tests PASS. Then `forge test`: 0 failures.
- [ ] **Step 5: Commit** `feat(vox): read MagicaVoxel files: models, names, placement, palette, materials`.

### Task 2: Character from a .vox file

**Files:**
- Create: `lib/strata/character.march`
- Test: `test/character_test.march` (fixtures built with Task 1's test encoder; move that encoder to `test/vox_fixture.march` so both use it)

**Interfaces:**
- Consumes: `Strata.Vox.Vox`, `Strata.Rig` (`new part anchor voxel colour socket leg mask`).
- Produces:
  - `type Size = { height : Float, half_w : Float, eye : Float }` (metres; height from feet to the top voxel, half_w the larger half-extent in x or z of all parts at rest, eye = height - 0.12)
  - `type Character = { rig : Strata.Rig.Rig, size : Size, trim : List((Int, Float, Float, Float)) }` (trim = index and file colour for 241-244 present)
  - `fn from_vox(v : Strata.Vox.Vox) : Result(Character, String)`
  - `fn load(path : String) : Result(Character, String)`
  - `fn trimmed(c : Character, r : Float, g : Float, b : Float) : Strata.Rig.Rig` (palette 241-244 recoloured per the convention)
  - `fn part_names() : List(String)` (the 15 in order)

- [ ] **Step 1: Write failing tests:**
  - `humanoid file matches the humanoid`: a fixture reproducing today's humanoid boxes in the convention builds a rig whose part count is 15, whose parents equal the humanoid's, and whose anchors and pivots equal `StrataViewer.Figures.humanoid()`'s within 1e-4. (If the test can't see the viewer, copy the expected numbers from figures.march into the test.)
  - `odd and even sizes join` (Review Focus 4): a head of 5 voxels and of 6 on a spine of 8; each pivot lands on the spine's top middle.
  - `empty model is a joint` (Review Focus 2): `arm_l_hand` with 0 voxels builds; part has size 0.
  - `extra on a part`: `extra:head:antenna` becomes part 15, parent 2, anchor its low corner.
  - `refusals` (Review Focus 3): missing `leg_r_foot` → Err mentions "leg_r_foot"; duplicate `head` → "head"; `extra:tail_base:x` with no such part → "tail_base"; a hand 3 voxels away from its forearm → "arm_l_hand"; a model 129 voxels long → "128".
  - `size`: a fixture 32 voxels tall gives height 2.0, eye 1.88.
  - `trim`: indices 241 (0.8 grey) and 242 (0.4 grey) recoloured to (0, 0, 1) give 241 = (0,0,1) and 242 = (0,0,0.5); other entries unchanged.
  - `sockets, legs, mask`: two sockets on parts 5 and 8, legs (9,10,11) and (12,13,14) with sole 0.0625, mask [6].
- [ ] **Step 2: Run** `forge test --filter Character`. Expected: FAIL.
- [ ] **Step 3: Implement** per the file convention above.
- [ ] **Step 4: Run** the filter, then `forge test`. Expected: PASS, 0 failures.
- [ ] **Step 5: Commit** `feat(character): a rig and a size from a MagicaVoxel character file`.

### Task 3: The generator and the twelve files

**Files:**
- Create: `scripts/characters.py`, `viewer/characters/*.vox` (12), `viewer/characters/generated.txt` (one line per file: id and SHA-256 of what the script last wrote)
- Test: `viewer/test/roster_test.march` (loads all twelve via `Strata.Character.load`)

**Interfaces:**
- Produces: the twelve files per the convention, roster order as above.

- [ ] **Step 1: Write the failing test** `viewer/test/roster_test.march`:
  - `all twelve load`: each `viewer/characters/<id>.vox` loads Ok.
  - `silhouettes differ`: heights span at least 1.55 to 2.15 m (Ixa smallest, Brakk and Torvald tallest); no two characters have the same (height, half_w, part count) triple.
  - `every character has trim`: each has at least one of 241-244.
- [ ] **Step 2: Run** `cd viewer && forge test --filter Roster`. Expected: FAIL (no files).
- [ ] **Step 3: Write `scripts/characters.py`** (standard library only). It writes version-150 .vox files a MagicaVoxel 0.99 install opens: one SIZE/XYZI per part, a scene graph (root nTRN → nGRP → per part nTRN with `_t` and nSHP with `_name`), RGBA, MATL for glowing entries. Recipes are short per character: shared helpers for limbs (box, taper, mirror left/right), each character's proportions, colours and extras per the spec's roster table (heights: Ixa 1.6, Brakk 2.1, Torvald 2.1, Mother 2.0, Skrell 2.0, Gorrum 1.7 hunched, others 1.75-1.85). Faces get at least eyes or a visor. Overwrite rule: if a target exists and its SHA-256 isn't the one in generated.txt, skip it and print `characters: <id> was edited by hand; not overwritten (use --force)`.
- [ ] **Step 4: Run** `python3 scripts/characters.py` then the roster test. Expected: twelve files written, roster tests PASS. Run the script a second time after editing one byte of `ixa.vox`: it prints the hand-edit line and leaves ixa.vox alone.
- [ ] **Step 5: Commit** `feat(characters): the twelve characters, generated as MagicaVoxel files`.

### Task 4: The viewer's roster: dress fighters and the player

**Files:**
- Create: `viewer/lib/roster.march`
- Modify: `viewer/lib/figures.march` (`body_in`, `forearm`, `piece_in` take a rig instead of a shirt colour), `viewer/lib/match.march` (`dress`, `finish_pieces`, `shirt_in` callers), `viewer/lib/strata_viewer.march` (load the roster at start; `STRATA_CHARACTER`)
- Test: `viewer/test/roster_test.march` (extend)

**Interfaces:**
- Consumes: `Strata.Character.{load, trimmed, Character, Size}`.
- Produces:
  - `fn load_all(dir : String) : List(Strata.Character.Character)` (12, roster order; a failed file prints `strata_viewer: character <id>: <reason>; using the humanoid` and is replaced by a Character built from `Fg.humanoid()` with Size height 1.8, half_w 0.3, eye 1.68)
  - `fn body_of(r : Strata.Rig.Rig) : Strata.Rig.Rig` (the rig with head and arms emptied, as `body_in` does today)
  - `fn forearm_of(r : Strata.Rig.Rig) : Strata.Rig.Rig` (parts 7 and 8, the right forearm and hand, as one rig in their colours, at the size today's forearm uses)
  - `fn piece_of(r : Strata.Rig.Rig, part : Int) : Strata.Rig.Rig` for every part including extras.
- Assignment: fighter i (0..11) plays character i; the player (fighter 0) plays `STRATA_CHARACTER` (0..11, default 0), and whoever would have had it plays character 0. Trim shade = `shirt_in(teams, i)`.

- [ ] **Step 1: Write failing tests:** `missing file falls back` (a dir with one file missing gives 12 characters, the missing one with 15 parts and height 1.8); `pieces cover extras` (Skrell's tail part gives a non-empty piece); `forearm of Hex` uses Hex's metal colour, not skin.
- [ ] **Step 2: Run** the filter. Expected: FAIL.
- [ ] **Step 3: Implement** and wire `dress` to the roster. Death pieces iterate over all parts of the character's rig, not 15.
- [ ] **Step 4: Run** viewer tests, then a hidden match run with `STRATA_SHOT` to a scratch PNG and look at it: twelve different characters on the field.
- [ ] **Step 5: Commit** `feat(viewer): fighters and the player dressed from the character roster`.

### Task 5: A body of each character's size in combat

**Files:**
- Modify: `viewer/lib/match.march` (every body-sized constant), `viewer/lib/walker.march` if the player's eye height lives there
- Test: `viewer/test/combat_test.march` (extend)

**Interfaces:**
- Consumes: `Strata.Character.Size` per fighter (store on `MtBody` as `mb_size`).
- Produces: `pfn mid(b : MtBody) : Float` (height / 2, replaces 0.9), entity kinds per character.

- [ ] **Step 1: Write failing tests:**
  - `Ixa is missed above the head`: an arrow passing 1.7 m over Ixa's feet misses; the same over the humanoid hits.
  - `Brakk is hit at 2.0 m`.
  - `feet stay on the ground`: Ixa and Brakk spawned on flat ground have their lowest voxel within 0.07 m of the floor.
  - `player eye`: playing Ixa, the eye is 1.48 m over the feet; playing Brakk, 1.98 m.
- [ ] **Step 2: Run** the filter. Expected: FAIL.
- [ ] **Step 3: Implement.** Kinds: one walking fighter kind and one placed player kind per character, half extents `(half_w, height / 2, half_w)`; fighters get their character's kind. Replace each body-sized use found by `grep -n "0\.9\b\|1\.8\b" viewer/lib/match.march` (spawn and respawn placement, nav cell lookup, fluid depth box, figure placement, the player's body under the eye, line-of-sight from chest height, the wall in front) with the fighter's size. Nav keeps one walker sized to the tallest character (2.1 m) so every path fits everyone. Sword reach and weapon sizes stay as they are.
- [ ] **Step 4: Run** viewer tests and `grep` again: every remaining `0.9` / `1.8` is commented as not a body size. Expected: PASS.
- [ ] **Step 5: Commit** `feat(match): each fighter is hit, collides and sees by its own size`.

### Task 6: Contact sheet, benchmark, gate

**Files:**
- Modify: `viewer/lib/match.march` (a scripted scene that lines the twelve up, `STRATA_ROSTER=1`), `test/golden/*` as needed
- Create: `docs/strata-characters.png`

- [ ] **Step 1:** Hidden run of the roster scene: front and side, neutral, blue team and red team. Assemble one contact sheet PNG (Python, scratchpad) and commit it as `docs/strata-characters.png`. Check by eye against the anti-goals: no two read as recolours; trim visible at combat distance (one shot from 20 m).
- [ ] **Step 2:** `scripts/bench.sh` turning fight at 180°/s with twelve characters, trace scale 2. Expected: p99 frame under 11.1 ms, at most 2 frames over in 1,800.
- [ ] **Step 3:** `scripts/verify.sh` hidden. Goldens that show fighters change; re-record only those, after looking at each new picture, and list them in the commit message.
- [ ] **Step 4: Commit** `feat(characters): the roster contact sheet; goldens with the new fighters`.

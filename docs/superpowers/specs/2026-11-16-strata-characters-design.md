# Strata characters: a roster of twelve, from MagicaVoxel files

## Intent

The game has one humanoid (viewer/lib/figures.march), the same body
everywhere with only its shirt colour changing. We want a dozen distinct
characters for the player and the NPCs, as varied as an Unreal Tournament
roster: mercs, cyborgs, aliens and gladiators side by side, no story for
how they got there. Each should be recognisable at a distance by
silhouette and colour, in every match, team or not.

The characters live as MagicaVoxel (.vox) files, so they can be opened,
looked at and painted by hand. The first versions are generated (by a
script), not hand painted.

## Decided

- **Voxel size 1/16 m**, the same as today's humanoid. Not 0.25 m: at that
  size a figure is 7 voxels tall and can show no face or kit.
- **One shared skeleton.** Every character has today's 15 parts in the
  same order and hierarchy (0 pelvis root, 1 spine, 2 head, 3-5 left arm,
  6-8 right arm, 9-11 left leg, 12-14 right leg), the same two hand sockets,
  the same declared legs and the same mask, so all nine existing clips
  (idle, walk, run, jump, crouch, swim, swing, flinch, fall) play on every
  character unchanged. Proportions differ: part sizes and pivots are each
  character's own.
- **Extras**: further parts parented to one of the 15 (tail, antennae,
  backpack, cannon housing, cape, coat tails, horns), with no clip keys of
  their own. They move rigidly with their parent. Parts 15 and up.
- **Team colour as trim.** Each character keeps its own colours in every
  match; a set of trim voxels on each takes the team colour (blue or red,
  today's shades per fighter) in a team match, and a character-chosen
  neutral colour otherwise. Trim must read at combat distance; it may glow.
- **Weapons stay separate**, held in the hand sockets as now (sword, bow,
  fire and ice balls).
- **The roster** (heights approximate):

| # | Name | World | Silhouette | Extras | Trim |
|---|---|---|---|---|---|
| 1 | Brakk | Gladiator | Huge, 2.1 m, bare-armed | Crested helmet, one big shoulder plate | Helmet crest, belt sash |
| 2 | Vesper | Merc | Lean, average height | Long coat tails, goggles | Armband, coat lining |
| 3 | Rook | Merc | Squat, armoured | Ammo backpack, short antenna | Chest plate stripes |
| 4 | Unit 7 | Cyborg | Thin, chrome | Glowing visor, spine cables | Visor and joint lights |
| 5 | Mother | Cyborg | Tall, heavy | Shoulder cannon housing | Cannon glow, chest core |
| 6 | Skrell | Alien | Tall, thin, big skull | Long tail | Glowing gill stripes |
| 7 | Gorrum | Alien | Wide hunchback, short legs | Spined back ridge | Spine tips |
| 8 | Ixa | Alien | Small and quick, 1.6 m | Two antennae | Antenna tips, wrist bands |
| 9 | Sabine | Gladiator | Athletic | Net-fighter's cape, one bare arm | Cape |
| 10 | Kade | Merc | Average height | Hood, scarf over the face | Scarf |
| 11 | Hex | Cyborg | Half human, half machine | One mechanical arm | Arm lights |
| 12 | Torvald | Gladiator | Bearded giant | Horned helmet, fur cape | Fur trim, horn bands |

## Approaches considered

- **Shapes in code**: each character a March recipe of boxes and mirrored
  limbs, like humanoid_in today. Quickest to write and adjust, but nobody
  can paint them by hand.
- **MagicaVoxel files (chosen by the user)**: characters are .vox files
  the game loads. Hand-editable in a free, common tool; costs a loader and
  a convention for parts and pivots.
- **Text grids**: each part as layers of letters. Diffable but long and
  slow to write and change. Dropped.

## How the files work (my calls, the builder may refine)

- **One .vox file per character**, under viewer/characters/ (name.vox).
  Each part is one model in the file's scene, named by its part
  (`pelvis`, `spine`, `head`, `arm_l_upper` ... `leg_r_foot`, then extras
  as `extra:<parent part name>:<label>`). One file keeps a character
  together for whoever paints it.
- **Placement from the scene.** MagicaVoxel's transform nodes give each
  model's position; the loader turns the models' relative positions into
  the rig's pivots (where a part joins its parent) and anchors (the point
  it turns about). The joint convention (for example: a limb part turns
  about the middle of its top face, the root about the middle of its
  bottom face) is fixed once, written down, and checked by the loader,
  which refuses a file whose parts don't join up.
- **Palette.** The .vox palette gives colours. MagicaVoxel's material
  chunk gives emission (glow) and roughness where set. A reserved range of
  palette indices marks trim; the loader recolours those per match.
- **The loader** reads .vox (RIFF chunks: SIZE, XYZI, RGBA, nTRN, nGRP,
  nSHP, MATL, LAYR as needed) and builds a Strata.Rig.Rig through the
  existing Rig calls (part, anchor, voxel, colour, socket, leg, mask).
  Whether it lives in the library (Strata) or the viewer is the builder's
  call; a library home is better if other games will load characters.
- **Generation.** A script writes the twelve .vox files from short
  recipes, so the first versions are consistent and can be regenerated.
  Once someone hand-edits a file, the file is the source; the script must
  not overwrite edited files silently.

## Constraints

- Rig limits: up to 64 parts, each 0 to 128 voxels a side; palette 1..255.
- Feet must still land: legs are declared for foot placement, so each
  character's leg parts and sole height must be right for its proportions.
- Combat's hit test, the held-weapon socket and first-person view (the
  player's own body and forearm, figures.body_in / forearm) must work on
  every character; heights from about 1.6 to 2.2 m.
- Frame rate: twelve characters on screen at once must keep the turning
  fight benchmark above 90 fps (scripts/bench.sh), as the humanoid does.
- The game's existing tests and golden pictures (scripts/verify.sh) keep
  passing; today's humanoid can stay as the default where pictures depend
  on it.

## Goals, non-goals, anti-goals

- Goal: twelve characters selectable for the player and assignable to
  NPCs in the viewer and the match, each recognisable by silhouette.
- Goal: anyone can open a character in MagicaVoxel, repaint it, save, and
  see it in game with no code change.
- Non-goal: new skeletons, new animations, or extras that animate on
  their own (a swinging tail). Possible later.
- Non-goal: per-character abilities or stats, other than the size that
  comes with each body's own hit box.
- Anti-goal: twelve recolours of one body. If two characters only differ
  by colour, that has failed.
- Anti-goal: trim that can't be seen in a fight, so teams can't be told
  apart.

## Hit box (the user's decision)

- **A box per character.** Each character is hit, collides and sees from
  its own height and width, taken from its body. Today combat assumes one
  1.8 m body (match.march places the middle 0.9 m over the feet); that
  becomes per character: the hit test, the physics box, the eye height
  and anything else sized for 1.8 m. Size is therefore a stat (Ixa is
  harder to hit, Brakk easier), accepted by the user; balancing beyond
  that is not part of this work.

## Settled after the builder check (my calls)

- **Weapons don't scale.** Sword, bow and balls keep one size and one
  reach on every character; size is never a stat.
- **First-person arm** is built from the character's own right forearm
  and hand parts (its colours, Hex's metal arm), not a hand-made model per
  character. No extra model in the .vox file.
- **Loaded at run time** from viewer/characters/ when the game starts. A
  missing or broken file is reported and that character falls back to
  today's humanoid; the game still starts.
- **Trim shade belongs to the fighter**, not the character: each slot
  keeps today's per-fighter blue or red shade and its trim takes it, so
  the same character may appear twice in a match and still differ.

## Left to the builder

- Exact proportions, colours and voxel detail of each character, within
  the roster above.
- How the player picks a character and how NPCs are assigned (a setting
  such as STRATA_CHARACTER, a match roster), unless it conflicts with the
  match code.
- The .vox parser's structure, the trim index range, and the script's
  language.
- A contact sheet of all twelve in game (front and side, neutral and both
  teams) as the check that the roster reads.

## Changes decided while building (2026-11-16, the user's decisions)

- **Voxels of 1/32 m**, not 1/16 m, for detail. Each file says its voxel
  size (the root transform's `_strata_voxel`); a file without it is 1/16 m.
- **Gritty, not cartoony**: realistic proportions (a head about 1/7.5 of
  the height), muted colours, shading and wear baked into the voxels'
  colours, faces with brows over set-in eyes.
- **Roster changes**: Brakk is replaced by **Corvin** (gladiator, a scarred
  veteran legionnaire in segmented plate, open helmet, short cloak); Ixa by
  **Nyx** (alien hunter, chitin plates, swept crest, mandibles, folded
  forearm blades). Mother has a **power pack** on her back with cables over
  her shoulders, not a shoulder cannon. Torvald's helmet has no horns.
- **Sizes within 10%**: every character stands 1.78 to 1.88 m. The hit box
  is per character in height only; its **width is shared, 0.6 m** (0.3 m
  half width, as combat uses today), whatever the body's width.

# Combat in the demo: a free-for-all to 30

Status: design, for approval. Nothing is built.

## What it is, and why

The viewer shows the engine's pieces one at a time: light, water, figures,
entities. Nothing yet makes them work together as a game. This adds a small
complete one: a free-for-all match on the island between the player and
eleven computer fighters, with bows and fireballs, on terrain everyone can
be blocked by and the player can reshape. It is a demonstration that the
engine's parts (entities, figures, physics, light, fluids, edits, the 2D
layer, sound) compose, and a place to find where they do not.

Started with `STRATA_COMBAT=1`. Without it the viewer is unchanged, and
every stored picture stays as it is.

## What the user decided

- The player fights at range only: no melee for the player.
- Two weapons. **Arrows**: accurate, long range, less damage. **Fireballs**:
  more damage, shorter range, less accurate.
- The player can dig and place single blocks during a fight, from the
  hotbar, as in the viewer today.
- Fire destroys ice, trees (trunks and leaves), wood and grass.
- Enemies come in two kinds: some chase and strike up close, some keep
  their distance and shoot.
- Enemies fight each other too. The player is one fighter among many.
- Enemies spawn around the map.
- A hit: the figure is knocked back and flashes, with a new flinch
  animation. A death: a new fall animation, then the figure falls apart
  into loose pieces that tumble.
- The player has a health bar and respawns.
- The match ends when any fighter reaches 30 kills.
- Twelve fighters in all.

## The match

Twelve fighters: the player, seven rushers and four shooters. Each computer
fighter keeps its name, shirt colour and kind for the whole match, and has
its own kill count. A fighter that dies comes back 3 seconds later at a
spawn point. The fighter that lands the last hit takes the kill; a fighter
killed by its own fireball gives no one a kill. At 30 the match stops, the
screen shows the winner and the table, and a key starts another.

**The ground.** The match is held on a circle of 80 m radius, so twelve
fighters meet often. Its middle is found from the `STRATA_SHORE=1` start by
moving inland until at least four fifths of the circle is dry ground; the
player's first spawn is the point nearest the shore. Sixteen spawn points
are chosen once, at match start, on dry ground inside the circle, spread
evenly. A respawn takes the point farthest from every living fighter. A
computer fighter outside the circle walks back toward its middle before
anything else. The player is not held in, but flight (the viewer's F key)
is off in a match, and while the player is more than 150 m from the middle
the match is paused, with a line on screen saying so: the world is only
loaded, and entities only move, near the player.

**Water** (open: see the question to the user at the end). As designed:
computer fighters treat water deeper than one block as a wall and do not
enter it. The player can swim, as now, and can be shot there. An arrow or
fireball stops at the water's surface; a fireball there burns nothing and
hurts no one.

## Fighters

Every fighter has health and takes the same damage from the same hit.

| | health | attack |
|---|---|---|
| Player | 100 | arrows and fireballs |
| Rusher | 100 | a swing, 25, reach 1.6 m, once a second |
| Shooter | 60 | arrows only |

**What a computer fighter does**, decided about four times a second:

1. Outside the circle: walk back in.
2. Pick the nearest living fighter it has a clear line to, within 40 m. A
   clear line is a ray through the terrain as it now stands, so a wall the
   player places hides them.
3. With no target: wander toward the middle of the circle.
4. A rusher runs straight at its target and swings when within reach.
5. A shooter stops between 12 and 25 m from its target and shoots an arrow
   every 1.5 s, with its aim off by up to 4 degrees. Nearer than 12 m it
   backs away.

There is no path finding. A fighter steps up one block as the walking mover
allows; blocked for a second, it sidesteps and tries again. A pit holds it
until it finds a one-block step out or is killed. A computer fighter that
has neither moved 2 m nor given or taken a hit for 20 s is put back at a
spawn point with full health; no one takes a kill for it. Computer fighters
do not dig, place, or throw fire.

## Weapons

| | Arrow | Fireball |
|---|---|---|
| Damage | 35 | 60 direct; from 60 at the centre to 20 at 2 m around the landing |
| Speed | 60 m/s | 18 m/s |
| Path | nearly flat | a clear arc |
| Spread | 0.5 degrees | 3 degrees |
| Reach | 80 m | about 30 m |
| Between shots | 0.6 s | 1.5 s |
| Terrain | breaks nothing; sticks for 5 s | burns (below) |

So three arrows fell a rusher and two a shooter; one fireball fells a
shooter and two a rusher. A fireball hurts everything within 2 m of where
it lands, the one who threw it included.

The player's weapons sit in hotbar slots 1 and 2. Stone and dirt, there
today, move to 3 and 4, since they are what a wall is built of and do not
burn; which two of the other blocks leave the bar is the builder's call. The left button shoots with a weapon chosen, and digs with a
block chosen, as now; the right button places.

**Fire.** Where a fireball lands, within 2 m: ice, wood, leaves and tall
grass are removed; grass-topped ground becomes dirt. Stone, dirt, sand,
snow, water and everything else are untouched. Fire does not spread. The
removals are one edit, so loose terrain above them falls by the physics
that exists. In flight a fireball lights the ground as the torch's flame
does.

## Being hit, and dying

- A hit knocks the fighter back along the shot (about 1.5 m for an arrow,
  3 m from a fireball's centre), plays the flinch on the upper body over
  whatever the legs are doing, and flashes the figure pale for a tenth of a
  second.
- At zero health the fall plays (about 0.6 s). Then the figure is removed
  and each of its parts becomes a loose piece thrown outward, which tumbles,
  comes to rest and is cleared after 8 s.
- The player hit: the screen's edge flashes red. The player dead: the view
  drops to the ground for 3 s, then the player stands at a spawn point with
  full health.

## On screen

- A health bar, bottom left, above the hotbar's line.
- The player's kills and the leader's name and kills, top right.
- A line for each kill within 40 m ("Ash felled Rook"), fading after 4 s;
  at most four at once.
- Tab held: the whole table, fighters in order of kills.
- At the end: the winner's name, the table, and "press Enter for another".

## How it is built on what exists

Intent, not instruction; the builder may find better.

- **Fighters and projectiles are entities** (`Strata.Entities`): walking
  movers for fighters, flying movers for arrows and fireballs, a kind for
  the player's own box that is set to the player's position each frame so
  arrows find the player like anyone else. Touch pairs report arrow against
  fighter; block hits report where a projectile lands; `raycast` and the
  world's ray answer the clear line.
- **Fighters are drawn as figures** (`Strata.Anim`, the viewer's humanoid):
  the game places each fighter's figure at its entity each frame. Twelve is
  few; the entities work's own figure binding (its Task 7) is not needed and
  not waited for. When it lands, moving to it is a later tidy-up.
- **The viewer does not use entities today.** Combat brings them in: the
  entity kernel's C file in `viewer/forge.toml`, and in the viewer the one
  entity world's open, its copy of the blocks after each stream tick, and
  its step, all behind `STRATA_COMBAT`. The entities work plans its own
  viewer scenes later (its Task 10); the builder tells that session what
  was added so its task builds on it.
- **A projectile's first step is ray-tested by the game.** An entity added
  between ticks moves its first tick as unseen, and unseen flyers pass
  through blocks; at 60 m/s that is a metre, enough to pass a one-block
  wall the shooter stands against.
- **Animations**: idle, walk, run and swing exist. New in
  `viewer/lib/figures.march`: `flinch` (upper body, 0.25 s) and `fall`
  (whole body, 0.6 s), written like the existing clips. Shooters hold a bow
  in the hand socket, as the torch-bearer holds a torch.
- **The rules are pure code** in a new `viewer/lib/combat.march`: damage,
  health, credit, the table, target choice, spawn choice, what fire burns.
  No renderer, no entities in it, so the rules are tested by unit tests.
  The glue (entities, figures, input, drawing) is a second new file. The
  viewer's own file gains only the calls into them, behind `STRATA_COMBAT`:
  it is shared by six sessions and conflicts on every merge.
- **The 2D layer** (`Strata.Ui`, `Strata.Text`) draws the bar, counts and
  table.
- **Sound**: existing sources where one fits (a strike for a hit); new
  sounds are not part of this.

## Left to the builder

- **Falling apart.** `Strata.Physics` bodies are grids of blocks at the
  world's one scale (1 m here); a figure's parts are 0.1 m voxels in a
  palette. If physics cannot hold such pieces, each part becomes a one-part
  figure moved by a small ballistic step of the game's own (gravity, one
  bounce off the voxel ground, rest). Either is acceptable; they must
  tumble and settle on the ground as it is.
- **The flash and shirt colours.** A rig has one palette and there is no
  per-figure tint; a figure's rig is fixed when it is spawned and clips
  belong to one rig, so swapping rigs for a flash is not practical. The
  flash is a per-figure tint added to the figure draw path: a small engine
  change in the animation work's files, made by this work, with that
  session told before it is merged. Whether shirt colours use the same
  tint or twelve rigs differing in palette is the builder's call.
- **How a fighter is steered** each decision: a short route to the target,
  or a velocity. Whatever the walking mover takes.
- **The fireball's light**: the traced-figure glow the torch uses is the
  intent; if a moving glow costs too much, the fireball is bright but casts
  no light, and the results say so.
- Names, colours, the exact look of the bar and table.

## Not in this

- Melee for the player. Armour, pickups, ammunition, health that returns.
- Path finding, cover seeking, team play, difficulty settings.
- Enemies that dig, place or burn.
- Fire that spreads; ice that melts to water.
- New sounds or music.
- More than one map, saving a match, network play.

## It would be a failure if

- The viewer without `STRATA_COMBAT` changed at all: a stored picture, a
  frame time, a key.
- A match could not end: fighters that never meet, or a fighter stuck where
  nothing can reach it for the rest of the match.
- A frame with all twelve fighting took noticeably longer than the same
  view with twelve walkers does today.
- Two runs of the same scripted match gave different tables.

## How we know it works

- Unit tests of the rules: the damage table above, credit and
  self-kills, the table's order, target and spawn choice, the burn list.
- A scripted, hidden match (fixed seed, the player standing still or
  absent, a fixed number of frames at a fixed step): it must reach 30
  kills, print the table, and print the same table on a second run.
  Scripted runs ignore input and leave the display and support off unless
  `STRATA_UI=1` and `STRATA_SUPPORT=1` are set; the match's runs set what
  they need.
- A stored picture of a fight in progress and one of the end screen.
- `scripts/verify.sh` prints PASS with every earlier picture unchanged.
- The user plays a match.

## Calls made by the designer, to check

1. The match is held in an 80 m circle at the shore start, not across the
   whole island, so fighters meet. "Spawn around the map" is read as
   "around this ground".
2. Seven rushers and four shooters.
3. Every number in the tables.
4. Computer fighters neither dig, place nor throw fire; only the player's
   fireballs burn.
5. Fire turns grass-topped ground to dirt and removes ice outright.
6. Combat lives in its own two files, a mode of the viewer, not a new app.
7. It does not wait for the entities work's figure binding or viewer task,
   and wires entities into the viewer itself, behind `STRATA_COMBAT`.
8. The circle's middle is moved inland so most of it is dry.
9. Flight is off in a match; the match pauses while the player is far away.
10. A computer fighter idle for 20 s is respawned, so a match always ends.
11. The hit flash is a small engine addition (a per-figure tint).

## Open question for the user

Water. The start is on a shore, and the entity movers know nothing of
fluids: left alone, computer fighters would walk the sea bed and shots
would pass through the surface. The design above makes water a wall for
computer fighters and a stop for shots, and lets the player swim. The
other choices are fighters that swim (a mover of the game's own: more
work) or accepting the sea-bed walking.

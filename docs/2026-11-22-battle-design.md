# Steel Rain's battle

Status: approved 2026-11-22. Nothing is built.
Date: 2026-11-22
Follows: `2026-11-20-plugins-tactics-design.md` (the plugin shape and
`strata_tactics`), `2026-11-20-mech-construction-design.md` (`strata_kit` and
Steel Rain's mechs), `2026-11-20-map-generation-design.md` (`strata_maps`).

## Intent

Steel Rain ("FFT meets XCOM meets MechWarrior") is a mech roguelike. This
design covers one fight: a squad of the player's mechs against enemy mechs
and gun emplacements on a generated map. The campaign around it (the
overworld, what a lost mech costs the run, rewards, unlocks) and the hangar
are later projects; this one ends when a fight ends with a result.

What the owner wants a fight to feel like, in their words: "There should be
some danger and tension but finding advantageous ground and positioning your
units well should help you strike first and keep up pressure, or you set a
trap and lay down cross fire."

So the ground decides fights. High ground, cover, flanks and sightlines are
what a good player reads; striking first and keeping the enemy pinned is
what they get for it; a watch laid over a lane is a trap. Danger comes from
what you cannot see, from enemies that play by the same rules, and from
cover being shot away under you.

## Goals

- Turn-based fights on `strata_maps` maps with up to 4 player mechs.
- Shots traced through the voxel world inside a weapon's accuracy cone:
  cover is physical, wears away and falls; height sees over it.
- Hits land on the mech part they strike; parts carve, break off and take
  their weapons and mounts with them.
- Pressure: hits and near misses push the target's next turn back on the
  FFT clock. Heat limits the attacker.
- Watches: a mech can hold an arc and fire at anything that moves through
  it; two watches over a lane are a crossfire.
- Three missions: Destroy, Hold, Break through; mechs can withdraw off the
  player's own edge.
- Enemies: mechs from the same parts, and fixed gun emplacements. An AI that
  plays by the player's rules, with squad tactics on top.
- Fog of war: each side sees only what its units see.
- The whole fight runs headless: plain data and functions, no window. The
  screen shows it. Tests run fights to the end, AI against AI.

## Non-goals

- The campaign, overworld, rewards, unlocks and what a lost mech costs a run.
- The hangar screen (the mech-construction design covers it; this fight
  takes builds as given).
- Infantry, vehicles, drones, air units.
- Multiplayer.
- Final balance numbers. The design fixes the rules and where the numbers
  live; tuning comes from the headless runs.

## Anti-goals (a failure even if it works)

- **Dice that ignore the map.** If a shot's outcome does not come from what
  stands between the shooter and the target, the ground stops mattering.
- **An AI that cheats.** It sees only what its units see and obeys every
  rule the player does. Difficulty comes from numbers, parts and
  emplacements.
- **Rules that only exist in the renderer.** If the screen decides anything
  (a hit, a fall, a death), the headless fight and the shown fight drift
  apart, and replays and AI training lie.
- **Unreadable outcomes.** The player must be able to see why a shot hit
  what it hit: the cone, what is in it, and where rounds went.

## Approaches

1. **All in the game.** Rules, AI, missions and screens inside
   `games/steel_rain`. Fastest start; the general mechanics stay locked in
   one game, against the owner's aim of shareable game elements.
2. **General mechanics in `strata_tactics`, Steel Rain's rules in the game.**
   Traced cone shots, watches, per-side sight, clock delay and a tile-scoring
   frame are useful to any tactics game; numbers, heat, parts, missions and
   squad behaviour are Steel Rain's.
3. **Approach 2, and the fight runs headless.** The fight is a state, an
   order and a new state, with no window; the screen only presents it.

**Chosen: 3.** Squad AI cannot be tuned by eye: it needs thousands of fights
played out in tests. A headless fight also gives exact replays and makes
saving mid-fight a matter of writing the state.

## Decisions the owner made

- Shots are **traced through the voxels with an accuracy cone** that widens
  with range (not dice, not exact lines).
- Pressure is **turn delay** (suppression pushes the target back on the FFT
  clock) **and heat** (firing heats the attacker).
- Enemies are **mechs and placed guns** only.
- Missions are **Destroy, Hold and Break through**, with withdrawal off the
  player's edge (withdrawal was proposed; the owner chose that option).
- The enemy AI has **squad tactics**: it scores ground by the player's rules
  and squads coordinate roles.
- The battle is built as **approach 3**: shared mechanics in
  `strata_tactics`, rules in Steel Rain, the fight headless.
- **A mech fires any or all of its weapons in one action**, each at its own
  target; heat is what stops a full volley every turn.
- **Floods are in this project**: breaching a bank floods ground during the
  fight. Lava burns.
- **Rounds hit anything**, allies included.

## The fight

### State and orders

A fight is one value, `Fight`: the map data and its world, the tactical
grid, the units, the clock, each side's sight, the watches, the mission and
its progress, a random seed, and a log of what happened. Nothing else holds
fight state.

`step(fight, order) -> (fight, events)` applies one order for the unit
whose turn it is. Orders: move along a path, fire (a volley: any of the
unit's intact weapons, each at its own point), brace, hold a watch,
withdraw, end the turn (with a facing). Each order is checked first (the
path is reachable, each weapon is intact and in range, the volley stays
under the hard heat cap); an illegal order is rejected with its reason and changes nothing.
Events describe what happened in order (moved a tile, a shot left, rounds
struck voxels or parts, a part broke off, a block fell, a watch fired, a
turn was delayed, a unit overheated, a site changed hands, the fight
ended), for the screen to play back and for tests to check.

The fight is **deterministic**: the same fight, the same orders and the same
seed give the same result. All randomness comes from the fight's seed mixed
with the turn and shot counter (`Strata.Seed`), never a global source. A
replay is the starting fight plus its orders.

### Turns: the FFT clock

`strata_tactics`' clock decides who acts: CT rises by each unit's speed and
a unit acts at 100. On its turn a unit may move and act once each, in
either order (moving after acting is allowed, as in FFT). The turn's cost
follows the clock's settings: a base cost, more for moving, more for
acting; a unit that waits keeps more CT.

**Speed** comes from the chassis and the movement piece, less a penalty for
weight over a share of the chassis limit (the mech-construction design
leaves the formula to the game; it lives in Steel Rain's rules data).

### Moving

Reach and paths come from `StrataTactics.Reach` with the mech's mover:
move points from its legs, climb and drop from the movement piece (legs
climb, treads do not, hover crosses water), its height for clearance.
Tiles holding another unit are pass-through for allies and blocked for
enemies. Moving into an enemy watch's arc can trigger it (below), tile by
tile: a watch fires when the mover enters a tile it can see, and the move
continues afterwards unless the mover is knocked out or immobilised.

### Shots

A weapon has a range band, a cone (its spread at its best range and how it
widens past it), rounds per shot, damage per round, the radius each round
carves, heat per shot and the delay each round adds on a hit.

A volley fires its weapons one after another in the order given; each
weapon's shot is resolved fully (including what falls) before the next.
Firing one weapon at a point (a tile, a voxel, or a part of a seen enemy):

1. The **muzzle** is where the weapon is on the mech (from the build's
   effect markers, at the standing pose). The aim is the chosen point.
2. Each round gets a direction inside the cone, drawn from the fight's seed.
   The cone's width grows with range past the weapon's best range, and is
   wider when the shooter moved this turn or is under suppression this turn.
3. Each round is traced through the voxel world from the muzzle until it
   strikes a solid voxel or a unit's hit volume, or leaves the map.
4. A round that strikes **terrain** carves it (a small blast at that point,
   `Strata.Edit.blast` with the weapon's radius) and loose pieces fall
   (see Falling, below); any mech whose footing went falls (Mechs falling). Cover wears away this way, and buildings come
   down.
5. A round that strikes a **unit**, of either side, hits the part whose box
   it entered first (below), dealing its damage to that part. There is no
   friendly-fire exception: a crossfire must be laid so its cones miss
   one's own mechs, and the shot preview shows allies inside a cone.
6. A round that passes within a near-miss distance of a unit's hit volume
   without striking it counts as a **near miss** on that unit.

Height matters with no special rule: a shooter on high ground traces over
walls that block a shooter below.

**Hit volumes.** In the headless fight a mech is a set of boxes, one per
part, from the parts' grids placed by the build at the standing pose,
turned to the unit's facing. The screen shows the animated rig; when an
event says a round struck part P at a point, the screen carves the rig's
part P there (`StrataKit.Damage.carve`). The headless boxes and the shown
rig need not agree to the voxel, only to the part.

### Falling (builder's call, marked)

There is no physics in the fight. After every change to the world, loose
pieces are found (`Strata.Support.loose`, over a box round the change, as
the ruin pass does with `loose_box`) and handled the same way every time:
the piece is removed and its voxels drop straight down, column by column,
onto the first solid below, as rubble blocks (the ruin pass's rubble heap,
generalised). Debris landing on a unit's tile damages its core and top
parts by the debris's volume. The screen may animate the fall, but the
voxels end where the fight put them.

### Mechs falling (the owner's change)

Ground a mech stands on can be shot out from under it: an embankment, a
ledge, a roof, a bridge deck. After every change to the world, each unit's
footing is checked:

- **its tile's floor dropped**: the mech falls straight down to the new
  floor of its tile;
- **its tile no longer holds it** (no floor left within its height of
  where it stood, or what is left is too narrow or too steep to stand on:
  less than half the tile's columns within one step of its old floor):
  it slides off to the lowest tile next to it it can stand on, towards
  the side the ground was lost, and falls to that tile's floor.

A fall of more than the mech's drop:
- damages its movement piece by the height fallen, and its core as well
  past a second, larger height (rules numbers);
- knocks it down: it loses CT by the height fallen, so it acts later;
- may put it in water or lava, with their rules.

A mech that lands on another unit's tile takes the lowest free tile next
to it instead, and both take a small amount of damage.

This is a tactic in its own right: shooting the bank out from under an
enemy on high ground drops it, delayed and damaged, into the open or into
the river. The AI knows it both ways: standing on ground that can be shot
away under it (thin banks, ledges, decks over a drop) counts as exposure,
and dropping an enemy is a shot it can value.

### Damage

Every part has armour points (from its tags). A round's damage comes off the
part it struck, more when it strikes from the side or back (a facing
multiplier: front, side, rear; the unit's facing is the direction of its
last move or shot, or the facing it chose at the end of its turn: ending a
turn may set a facing for free, and an immobilised mech can still turn —
builder's call, marked). A part at 0 is **destroyed**: it breaks off
(`StrataKit.Damage.break_off` on the screen; in the fight, the build loses
that subtree and its voxels drop as rubble, as Falling does), and
everything mounted on it goes with it.

A mech is:
- **out** when its core is destroyed (removed from the clock; its wreck
  drops into world voxels on and round its tile, as rubble, and becomes
  cover like any other blocks);
- **immobilised** when its movement piece is destroyed (it can still turn,
  fire and hold watches);
- **disarmed** when it has no weapon left (it can still move and withdraw).

An emplacement is one part on a fixed footprint, out at 0.

### Pressure: delay

Every round that hits a unit pushes it back on the clock by that weapon's
delay (CT taken away, never below 0); a near miss pushes it back by a
smaller amount. A unit hit this turn also fires with a wider cone on its
next turn (suppressed). Bracing (an order) halves delay until the unit's
next turn. The clock's turn-order bar shows the delay as it lands, so
pinning a unit is visible.

### Heat

Each weapon adds heat per shot; moving adds a little; each of the unit's
turns sheds heat at a rate from its chassis and power budget (a build near
its power limit sheds less). Above a warning level the unit's cone widens;
above the limit it **overheats**: it loses a turn's worth of CT, and parts
with a heat-damage tag take damage. An order that would push heat past a
hard cap is illegal (the hangar's power budget therefore has a job in the
fight).

### Water, floods and lava

The map's water is the fluid simulation's (`Strata.Fluid`: rivers are its
sea, as in the maps example), and it is part of the fight's state. It only
moves when something breaks what holds it: a bank, weir or wall carved
away by rounds. Then it flows, **a set number of fluid steps after each
unit's turn** (a rules number), so a flood spreads over a few turns and
the player can see it coming. The fluid is stepped inside `step`,
deterministically; the screen only shows it.

Water in play:
- a tile's water depth is read from the world after each step;
- legs and treads may stand in water up to their wade depth (the mover's);
  deeper water blocks them; hover crosses any water;
- moving through water costs more; a mech standing in water sheds heat
  faster;
- a flood that rises past a unit's wade depth on its tile pushes it back
  on the clock (it is stuck) until it moves out, and an immobilised mech in
  rising water is out when the water covers its core.

Lava (the map's hazard, not flowing): a mech on a tile next to lava gains
heat each of its turns; a mech that enters a lava tile has its movement
piece destroyed.

The AI's ground scoring knows the water: it reads depth after each step,
and treats tiles a known breach will flood within a few turns as exposed.

### Watches

Holding a watch is an action: the unit chooses an arc (a direction and a
width, up to a limit from its sensor part) and a weapon. Until its next
turn, the first time an enemy it can see enters a tile inside the arc, it
fires that weapon at the mover (a full shot: cone, tracing, damage, delay,
heat). Each watch fires once. Two watches covering a lane are a crossfire;
nothing more is needed. A unit delayed past its next turn keeps its watch
until that turn comes.

### Sight and fog of war

Each side sees what its units see: a tile is seen if any of the side's
units has a line to it (`StrataTactics.Sight.line`, clear or partial)
within its sensor range. Enemy units on unseen tiles are hidden from that
side's view and from its AI. Last-seen positions are remembered and shown
faded. Terrain is always known (the map is not hidden; units are). Sight is
recomputed after every move and every change to the world.

### Emplacements

Emplacements stand on sites the map places for the mission (high ground by
preference). Each has a weapon, a sight range and an arc it covers, and
holds a watch over that arc on every one of its turns. They do not move.

## Missions

Each mission is a set of site rules for `strata_maps` (where spawns,
targets and holds go) and a win check over the fight.

- **Destroy.** Targets (a battery of emplacements, or a supply site's
  structure) are placed on target sites. A structure target is out when
  half of its voxels are gone (builder's call, marked). Won when every target is out, and
  the surviving mechs have either withdrawn or the enemy has no units left.
- **Hold.** A hold site in the middle third. The player must keep a mech on
  it, with no enemy mech on it, for N of the player's turns in total.
  Enemies arrive in waves from the far edge. Won when the count reaches N.
- **Break through.** An exit site on the far edge. Won when at least K of
  the mechs that started reach it (each leaves the map on arriving).

**Withdrawal.** A mech on a tile of the player's own spawn edge can
withdraw: it leaves the fight intact. **Lost** when every player mech is out
or withdrawn without the mission won. A fight's result names the mission's
outcome and each mech's state (in, withdrawn, out) with its build as it
ended, for the campaign to use later.

## The enemy

### Scoring ground (the shared frame)

`strata_tactics` gains a tile-scoring frame: given a unit, its reachable
tiles and what its side knows, score each tile as a weighted sum of terms
the game supplies, and return the best few. Steel Rain's terms:

- cover against each known enemy (`StrataTactics.Sight.against`);
- height over known enemies;
- shots available from the tile (seen targets in range, with the cone's
  expected hits and the facing bonus, less allies inside the cone);
- water and lava: depth against wade, rising floods, heat from lava;
- footing: how much of the ground holding the tile could be shot away, and
  the drop below it;
- exposure: known enemies and watches that could see it;
- distance to the unit's goal (its squad role's target);
- heat and damage: a hot or badly damaged unit weighs cover and distance
  from enemies higher.

A unit's turn: score tiles for each action it could take (fire from here,
fire after moving, hold a watch over a likely lane, brace, withdraw if its
squad is falling back), pick the best, issue the orders.

### Squad tactics

Enemy units belong to squads (2 to 4 mechs). Each squad has a role that its
members' scores lean towards:

- **pin**: keep fire on a chosen player mech (delay it), from cover;
- **flank**: move round the side the pinned mech's cover does not face;
- **overwatch**: hold watches over the lanes the player is likely to use;
- **fall back**: withdraw to better ground together when the squad's
  strength drops below a share.

A squad commander function looks at what the squad knows at the start of
each member's turn and assigns roles (one pinning, one flanking, and so
on), changing them when the situation changes (a pinned target is out, the
squad is badly hurt, a new player mech is seen). The AI uses only what its
side can see.

### Difficulty

Difficulty sets the number of enemy mechs and emplacements, their parts and
the AI's scoring weights. It never gives the AI information or rules the
player lacks.

## The screen

The screen follows the fight; it never decides anything. It:

- shows the map, units, the clock's turn-order bar with the delay as it
  lands, each unit's heat and parts;
- for the active player unit: reach and path (`strata_tactics` marks),
  then the chosen weapon's **cone** drawn from the muzzle with what is inside
  it highlighted, and the expected spread of hits on the target (from a
  quick headless preview of the shot's cone without randomness);
- watches as arcs on the ground;
- plays the fight's events: the walk, the shot's rounds as tracers, terrain
  carved and falling (as the ruin requests do), parts carved and breaking
  off (`strata_kit`), delay ticks on the bar;
- fog: units seen only by the other side are not drawn; last-seen markers
  are faded.

The existing prototype (`games/steel_rain`: the Warden walking the map)
becomes the screen's starting point.

## Where the code lives

**`strata_tactics` (shared, game-agnostic):**
- `Shot`: trace rounds through a world inside a cone from a muzzle, given a
  seed and per-unit hit volumes; returns what each round struck (voxel, or
  unit and part, or nothing) and the near misses. Also the no-randomness
  preview (the spread of expected hits).
- `Watch`: arcs (origin tile, direction, width, range), which tiles an arc
  covers, which watches a step into a tile triggers.
- `Fog`: per-side seen tiles from units' positions, eye heights and sensor
  ranges, using `Sight.line`.
- `Clock`: delay (take CT from a unit, floored at 0).
- `Score`: the tile-scoring frame (reachable tiles, game-supplied weighted
  terms, best N).

**Steel Rain (`games/steel_rain`):**
- `Rules`: weapon, part and chassis numbers (data), speed, heat, damage,
  facing multipliers, delay values, mission parameters.
- `Fight`: the state, `step`, the orders and events, mission checks and
  results.
- `Ai`: the scoring terms, unit turns, squad roles and the commander.
- `Missions`: site rules for the three missions and how emplacements and
  waves are placed.
- The screen: input, the views above, playback of events.

## Stages

The project is big; it is built in stages, each ending with something that
plays:

1. **Core rules and Destroy, both sides played by hand.** Fight state,
   `step`, moving, cone shots against terrain and parts, falling terrain
   and mechs falling, damage,
   delay, heat, withdrawal, emplacements, the Destroy mission, the screen
   for all of it. Headless tests play scripted fights to the end.
2. **Fog of war and the enemy AI**: per-side sight first (so the AI never
   sees more than it should), then the scoring frame and unit turns; AI
   plays one side. Headless runs: AI against AI to the end without illegal
   orders or stalls.
3. **Squad tactics**: squads, roles, the commander; headless runs compare
   squad AI against stage 2's AI (it should win more often).
4. **Hold and Break through.**
5. **Floods and lava**: the fluid stepped in the fight, breaches, water and
   lava effects, the AI's water terms.

(Builder's call, marked: emplacements moved to stage 1, because Destroy
needs them; fog moved to stage 2, because an all-seeing AI would be tuned
wrong.)

(Watches fit in stage 1 or 2; the builder decides.)

## Constraints

- March and Strata as they are; public Strata only in `strata_tactics`
  (the plugin rules).
- The world is 0.4 m a voxel, tiles 8 voxels, mech parts 0.1 m (the
  mech-construction and map designs).
- The headless fight must not need a window, the renderer or `Strata.Anim`
  (hit volumes come from part grids and the build, not the animated rig).
- The fluid steps after a turn must take under 100 ms in a release build
  (so a flood never stalls play).
- An AI turn must finish within 1 second in a release build (64 x 64 tile
  map, 4 player mechs, up to 8 enemy mechs and 6 emplacements).
- A headless AI-against-AI fight to the end should take under a minute in a
  release build, so test suites can run several.

## Left to the builder

- Exact names and types; how events are shaped.
- The starting numbers (weapons, armour, heat, delay, speed formula): any
  sensible values, kept in `Rules` as data, to be tuned from headless runs.
- Whether a round's traced line uses the world's ray cast or its own walk.
- How the near-miss distance, cone growth and facing sectors are measured.
- How the screen draws cones, arcs and tracers.
- Whether watches arrive in stage 1 or 2.

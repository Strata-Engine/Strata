# Finding the way: Strata.Nav and Strata.Steer

Status: built. The user asked for "tools for entity AI and path finding",
tested with the combat match, on March main, and chose to have it built
without a design round; the calls below are the builder's.

## What it is, and why

The match's fighters had no path finding: a fighter ran straight at its
goal, sidestepped when blocked for a second, and was put back at a spawn
point after 20 s of getting nowhere. A wall, a pit or a cliff between two
fighters ended the chase. Any game with things that walk needs the same
three answers, and none of them is a game's own: where can this walker
stand, how does it get from here to there, and which way should it head
this frame. Two library modules give them. They name no game concept: a
fighter, a villager and a cart are walkers of different sizes.

## Strata.Nav

Everything is in voxels; a place is the cell a walker's feet are in.

**A walker** is a footprint (2 radius + 1 cells square), a height, the step
it climbs and the drop it takes, and whether it swims. `walker_of` makes
one from a box in metres at the world's scale (the box rounded up, the step
and drop down), so the same fighter is one cell wide at 1 m a voxel and
three at 0.25 m.

**Standing.** A cell holds a walker when the cells of its body are free of
solid blocks and either the block under its middle is solid and its head is
out of any fluid (standing), or it swims and the cell is the top cell of a
fluid (afloat). So a walker wades water up to its chest and no deeper; a
swimmer floats across. A chunk that is not loaded is air: nothing stands in
it.

**A move** is to one of the eight neighbouring columns: level, up by at
most the step (the cells over the walker's head must be free for the rise),
or down by at most the drop (the cells over where it lands must be free for
the fall). A diagonal move is level only and needs both cells it passes
between free, so no corner is cut. Costs: 10 straight, 14 diagonal, 5 more a
cell climbed, 1 more a cell dropped, three times as much into water.

**`find`** is A* over those moves, from one standable cell to another,
through the world as it is loaded now. It works in a window of at most
96 x 40 x 96 cells about the two places and expands at most a budget of
cells the caller gives. It reads each cell of the world at most once and
keeps what it read, so one search sees one world. When it cannot arrive
(no way, the budget spent, the goal outside the window or the loaded
world) it answers the way to the cell it reached that is nearest the goal,
marked not arrived: a game walks that and asks again.

**`nearest`** is the same search with no goal: the cheapest way to any
cell a game's own test accepts. Cover from a threat, dry land, a cell out
of sight are all one call.

**`straight`** says whether the walker can go directly from one cell to
another (the line between is walked a straight step at a time), and
**`smooth`** uses it to keep only a path's corners. **`points`** turns
cells into metres for a mover and **`advance`** drops the points reached.

**Why A* on the voxel grid, not a navigation mesh or a flow field.** The
world is edited every few seconds (craters, fire, a wall the player
builds). A mesh would have to be rebuilt chunk by chunk after every edit; a
search that reads the voxels sees an edit the next time it is asked, with
no state to keep right. A flow field serves many walkers with one goal; a
free-for-all has as many goals as fighters. A hierarchy over chunks would
make long paths cheap and is the natural next step if a game needs paths
longer than the window; nothing here stands in its way.

## Strata.Steer

Pure functions of positions over the ground, each answering a direction
or a push: `seek`, `flee`, `arrive` (slows and stops short), `range` (hold
a distance), `apart` (keep clear of neighbours), `blend`, and `turn` (a
facing turned at a limited rate). A game adds what it wants and multiplies
by its mover's speed.

## The match

- Four times a second a computer fighter that returns, wanders, chases or
  swims asks `straight` for its goal. If the straight way is shut it plans
  (`find`, 1200 cells at most), keeps the path's corners, and heads for the
  next one; the plan is kept a second (two when it did not arrive) and made
  again. Blocked for a second on a path, it drops the path and plans anew.
  The old sidestep stays underneath for fighters in each other's way.
- A swimmer plans its way to its target if it has one, else to the spawn
  point it was making for, and follows it in the water.
- A rusher goes on after a target that has stepped out of sight for 6 s
  (while it is within 40 m). Before, a wall between them made it forget.
- `STRATA_COMBAT_NAV=0` turns all three off, for comparison.
- The **fence** scene (`STRATA_COMBAT_SCENE=fence`): a rusher has seen its
  target 20 m off; at frame 30 a stone wall 13 long and 4 high rises between
  them. With path finding it goes round and strikes; without, it never
  arrives. `scripts/combat_check.sh` runs both.

## Not here

- Paths longer than the window (about 90 m at 1 m a voxel) in one call.
- Walkers that dig, build, jump gaps or climb ladders.
- Other walkers as obstacles: a path goes through where another fighter
  stands; `Strata.Steer.apart` is the tool for that, and the match does not
  use it yet.
- Planning on a worker. A plan in the match costs a few milliseconds on the
  frame that makes it (see RESULTS).

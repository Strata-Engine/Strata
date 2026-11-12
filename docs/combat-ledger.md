# Combat ledger — plan: docs/2026-11-12-combat-plan.md

Spec: docs/2026-11-12-combat-design.md. Executed inline (native), one reviewer at the end.

Pre-flight: Task 3 consumes Task 1's roster, ring, spawn_at, target, intent and Task 2's clips, bow: names agree. Task 4 consumes Task 1's hit, tick, idle, winner: agree. Task 7 consumes Task 1's tool and Task 5's throw_fire: agree. Task 8's tint is produced and consumed inside Task 8. No conflicts found.

Task 1: Ruling: `hit`'s third parameter is `who`, not `by` — `by` is a reserved word in March (parse error) — nothing if wrong.
Task 1: Ruling: another module cannot match on Combat's constructors (no qualified constructor patterns; the codebase reads foreign types through accessors), so Combat also exports `credit`, `killed`, `intent_code`, `intent_target`, `kind_code`, `player`/`rusher`/`shooter`, `at`, `kills`, `deaths`, `health`, `alive`, `name`, `kind` — the plan's Produces block named the types only — later tasks read outcomes and intents as numbers.
Task 1: Ruling: test modules also need `needs IO.Clock` — charged to every test module once any uses the clock (GAPS S3).
Task 1: complete (commit a224ef0, tests: forge test test/combat_test.march -> 72/72 pass, 14 of them combat's)
Task 2: Ruling: a fallen fighter comes apart into 15 pieces, one a humanoid part; the plan's "10" (Task 8's check) miscounted the humanoid's parts — nothing if wrong.
Task 2: Ruling: `piece(part)` is a box of the part's size in its main colour (the head keeps its hair and eyes), not a copy of the part's voxels — the rig API does not expose a part's voxels; at 1/16 m the difference is the belt and a forearm's sleeve.
Task 2: complete (commit 1787009, tests: forge test test/combat_test.march -> 75/75 pass; scripts/golden.sh PASS, figure scenes unchanged)
Task 3: Ruling: `Match.frame` answers (match, stream, block changes) and the viewer applies the changes with its own `edit` (stream, fluid wake, physics wake); the plan had it answer the fluid — an edit must go through the viewer's Sim so support and fluids hear of it — nothing if wrong.
Task 3: Ruling: `Match.start` takes the start's height and whether there is a player; the middle is searched in eight directions (40 to 160 m) rather than "inland", since the shore's direction is not known to the match — same result: the first circle four fifths dry.
Task 3: Ruling: the match keeps the figure world itself (`Match.anim`, `Match.rigs`), not a Crowd; `Figures.body_id` and `clip_ids` expose what it needs.
Task 3: Finding: the viewer's sea (block 5) is a plain non-colliding block, not a fluid state, so `Figures.dry_ground_at` called the sea bed dry. It now treats block 5 as water. The STRATA_FIGURES crowd used the same function: it could have stood walkers on the sea bed.
Task 3: Finding (for Task 7): the viewer's dig is a sphere of radius 6 and its build a sphere of radius 2 (`act`). The user asked for single-block edits; a match must dig and place one block.
Task 4: Ruling: an arrow's thrower is kept in the match's own shot record, not in an entity slot — the match already holds a record a shot for its figure — nothing if wrong.
Task 4: Ruling: an arrow that meets a block has its entity removed and its figure left stuck for 5 s; a flying entity left in place would keep reporting the block under gravity.
Task 4: Ruling: `Combat.lead` also answers the seconds in flight (the test flies the shot for that long).
Task 4: Ruling: a fighter with no target jogs (2.8 m/s) to within 14 m of the middle and then between the inner spawn points; at a walk (1.4 m/s) round fixed points the bots-only match stood at 9 kills for its leader after 5 minutes.
Task 4: Ruling: loosing an arrow or beginning a swing counts as activity for the idle rule (design: "given or taken a hit"): a shooter holding its ground and missing for 20 s is not stuck.
Task 4: Ruling: the bots-only match's budget in combat_check.sh is 90,000 frames, not the plan's 54,000 — measured: a winner at frame 52,356 (14.5 minutes of play), 249 kills in all, the top four within 3 kills; the plan's number left 3% of margin — cost if wrong: a match that slows down further still passes until it takes 25 minutes.
Task 4: Finding: 68 idle respawns in that match (4.7 a minute): fighters stuck where they cannot get out. The field is 53/64 dry; walkers that enter the sea walk its bed. Expect Task 6 (swimming) to lower it; measure again there.
Task 4: Ruling: scenes are numbers to the match (1 duel, 2 trap); the viewer maps STRATA_COMBAT_SCENE's name. The trap's check line is `combat: trap inside I, respawns R` (I == 0, R >= 1) over 1,800 frames, not the plan's `freed F` at 1,500: the idle rule needs 20 s after the block is set at frame 300.
Task 3: complete (commit 026d244, shared with Task 4; tests: scripts/combat_check.sh -> field ok; scripts/golden.sh PASS)
Task 4: complete (commit 026d244, tests: forge test -> 78/78 pass; scripts/combat_check.sh -> field, duel, trap, match ok, each equal across two runs)
Task 5: Ruling: the fire scene's check asks for wood >= 1 and grass >= 1; leaves are not required — the mark is a metre up a trunk and the canopy of a generated tree starts well above 3 m.
Task 5: Ruling: scripted scenes set the camera (`Match.staged`, `Match.eye`); the viewer takes its eye and view from them. Needed for any picture of a scene: the scripted camera otherwise stays at the shore start.
Task 5: Ruling: `ground` in the match searches 160 m down from above the start (Figures.ground_at searches 6 m and answered 58 m in the air; Task 4's duel and trap fighters were being dropped from there). The trap scene's fighters now stand still, so the pit and the block act on them; its idle respawns are 2.
Task 5: Finding (not resolved): with support on, a tree whose trunk burns through is gone from the terrain 6 frames later and nothing is seen falling or lying there in the scripted pictures. Strata.Physics makes loose pieces into bodies (physics.march:866); whether they are made and not drawn, or dropped, was not established. Raise with the physics session.
Task 5: Not measured yet: the fireball's light cost with four in the air (plan: under 1 ms). Left to Task 10's measurements.
Task 5: complete (commit 4b2a7ad, tests: forge test test/combat_test.march -> 79/79; combat_check.sh -> field, duel, trap, fire, match ok; golden combat-fire stored after looking)
Task 6: Note: `water_point` and its test were written together; the test was not watched failing first.
Task 6: Ruling: swimmers swim at 2.5 m/s, not the design's 1.5 (a designer's number): at 1.5 the bots-only match slowed from 14.5 to 23.7 minutes.
Task 6: Ruling: a swimming fighter makes for the nearest spawn point (dry land by construction), whatever its target. Design said "toward where it was going"; two swimmers chasing each other met in the water, could not attack, and stood there until the idle rule took them.
Task 6: Ruling: fighters with nothing to do gather at a rally point, the open ground nearest the middle, not the middle itself: this field's middle is in a river.
Task 6: Ruling: spawn points are on open ground: dry, not leaves or wood, with two cells of headroom.
Task 6: Ruling: the sidestep of a blocked fighter grows (0.5, 1, 2, 4 s) and alternates left and right until it has moved 2 m; the design's fixed half second to the left left fighters stuck behind two-block terraces at two spawn points for the whole match.
Task 6: Measured: bots-only match, winner at frame 41,945 (11.6 minutes), 133 kills, 5 idle respawns (Task 4: 52,356 frames, 249 kills, 68 idle respawns).

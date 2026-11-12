# Combat ledger — plan: docs/2026-11-12-combat-plan.md

Spec: docs/2026-11-12-combat-design.md. Executed inline (native), one reviewer at the end.

Pre-flight: Task 3 consumes Task 1's roster, ring, spawn_at, target, intent and Task 2's clips, bow: names agree. Task 4 consumes Task 1's hit, tick, idle, winner: agree. Task 7 consumes Task 1's tool and Task 5's throw_fire: agree. Task 8's tint is produced and consumed inside Task 8. No conflicts found.

Task 1: Ruling: `hit`'s third parameter is `who`, not `by` — `by` is a reserved word in March (parse error) — nothing if wrong.
Task 1: Ruling: another module cannot match on Combat's constructors (no qualified constructor patterns; the codebase reads foreign types through accessors), so Combat also exports `credit`, `killed`, `intent_code`, `intent_target`, `kind_code`, `player`/`rusher`/`shooter`, `at`, `kills`, `deaths`, `health`, `alive`, `name`, `kind` — the plan's Produces block named the types only — later tasks read outcomes and intents as numbers.
Task 1: Ruling: test modules also need `needs IO.Clock` — charged to every test module once any uses the clock (GAPS S3).

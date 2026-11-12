# SDD ledger — plan: docs/2026-11-12-population-core-plan.md
Spec: docs/2026-11-12-population-design.md.

Task 1: Ruling: `rule`'s second parameter is named `curves`, not `by` (GAPS S2: `by` cannot be a name); its meaning is the brief's — controller ruling — none.
Task 1: Ruling: built-in ground fields are declared by the game with `field_ground(p, name, slot)`; a slot outside 0..9 is a freeze error — controller ruling — none.
Task 1: Ruling: fields, zones, shape sets and layers are four namespaces; a name is unique within its kind only — controller ruling — none.
Task 1: Ruling: `Frozen` keeps every kind as a list in declaration order (layers in run order) beside a list of its names, and every reference as an index into those lists (a cover field holds its layer's run-order index); later tasks turn what they evaluate per column into arrays — lists are walked by index, which is linear; a task that reads a list per column must first copy it into an array.
Task 1: Ruling: `curve` sorts its points by x (stable, so points at one x keep the order given, making a step) — the brief says only "piecewise linear" — a game relying on given order for a non-monotone x would be surprised.
Task 1: Ruling: a scatter cell must be above 0 (0 is refused as well as negative: Strata.Feature divides by it), and jitter must be within 0..0.6 (below 0 is refused as well as above 0.6) — the brief names only "negative cell" and "above 0.6" — none.
Task 1: Ruling: a ring zone's inner and outer radii and every site's radius (in rings and site layers) are "radii" and are refused when negative; inner above outer is accepted (an empty ring) — none.
Task 1: Ruling: freeze also refuses curve fields that read each other in a cycle and zones made of each other in a cycle (both would loop forever when evaluated); the brief names only the cover cycle — none.
Task 1: Ruling: run order is "repeatedly the earliest declared layer whose read layers have all run", so a layer with no reads keeps its declared place relative to others and a reading layer runs at the first point after all it reads — other stable orders exist; this one is what the brief's examples give — none.
Task 1: Ruling: a layer's reads are the cover fields reached from its rule's curves and from the bands of its inside and outside zones (through zone_all, zone_any, zone_not and curve fields) — none.
Task 1: Ruling: `missing_ground` lists the slots of ground fields reached through curve chains from any layer's rule curves or from any declared zone's band, even a zone no rule uses; a curve field nothing reads does not count — "some rule or zone reaches" read literally — a game may be asked to supply a slot only an unused zone needs.
Task 1: Ruling: weights are kept in declaration order with every declaration, a second weights for the same shape set and zone included; which one applies where zones overlap is the placer's call (Task 3) — none.
Task 1: Ruling: freeze evaluates all its checks and returns the first in this order: duplicates (fields, zones, shape sets, layers), fields, zones, shape sets, weights, layers, field cycles, zone cycles, then the cover cycle — none.
Task 1: Ruling: noise kind and octaves are not checked here (Task 2 knows what `Strata.Noise` accepts) — none.

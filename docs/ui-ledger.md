# Interface and text: ledger

Plan: `docs/2026-11-11-ui-text-plan.md`. Branch `interface`, cut from `swim`
(the newest work), not from `fluids` as the plan says.

- Ruling: order of work. HarfBuzz was not installed when the build began
  (the owner's `brew install` was waiting at a prompt), so the 2D layer and
  images came first and Task 1's linking moved to where it is first needed.
  Cost if wrong: none; the tasks do not depend on that order.
- Ruling: Tasks 2 and 3 are one commit. They share the shim's pipeline and
  the viewer's hotbar. Cost if wrong: a coarser history.
- Ruling: a quad is 16 floats, not 13. Sixteen keeps a quad at 64 bytes and
  leaves two numbers for text. Cost if wrong: 12 bytes a quad.
- Ruling: the layer keeps the last display handed over until the next call,
  so a game that skips a frame does not blink. Cost if wrong: a stale
  display if a game stops calling; it clears it with an empty one.
- Ruling: tests that call into C live in `viewer/test`, because the library
  alone links no C. Cost if wrong: none.
- Tasks 2 and 3: complete. `forge test` 12 new tests; `scripts/verify.sh` PASS
  with five new pictures (three scales, frames, a flood of 20 000 quads that
  draws 16 384 and reports 3 616 dropped). Every older picture unchanged.

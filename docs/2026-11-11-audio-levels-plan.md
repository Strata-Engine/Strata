# Strata Audio Levels Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A game can adjust how loud Strata's sound is: one main level over everything, and a level for each of four groups the game names (music, effects, and so on), each set from a 0..1 slider value.

**Architecture:** The mixer gains a main gain applied to the whole mix before the limiter, gliding per sample like the group gains it already has. `Strata.Sound` maps a 0..1 level to a gain through a cubic curve, sends the main and group gains, and puts every voice into a group when it is played, including the fluid's and the wind's.

**Tech Stack:** March (origin/main via `scripts/env.sh`), C (`native/strata_mix.h`).

**Spec:** the owner's request in conversation (2026-11-11: "expose sound in an adjustable way": main volume, music, maybe effects), within `docs/2026-11-11-audio-design.md` §8, whose four groups this makes reachable.

## Global Constraints

- Branch `audio` (merged into main after each task whose gate passes, as before). `scripts/verify.sh` prints PASS after each task; no device opened, nothing played aloud.
- GAPS S1 to S21 apply; in particular S16 (no `&&` guarding a call that can fail), S17 (extern names unique), S18 (constructor names unique), S14 (run `native/stamp_audio.sh` after editing `strata_mix.h`).
- Four groups, numbered 0..3; group 0 is the default. Strata names no group: the game decides what each is for.
- Level → gain: `gain = level³` for level in 0..1, clamped; 0 is silent, 1 is unity (0.5 is about -18 dB). Non-finite levels count as 0.
- Every level change glides; no step between device blocks.
- Out of scope: drawing a menu, saving settings, mute as a separate switch, reading levels back, ducking.

## Review Focus

1. A level changed every frame while a slider is dragged: no click, no zipper, no growing command list.
2. A level outside 0..1, negative, or NaN from a game: clamped, never louder than unity, never non-finite.
3. A group number outside 0..3 on `play` or on a level: the voice plays in group 0; the level change is ignored.
4. The main level at 0 and back to 1: the reverb's tail and every voice resume, nothing is lost or stuck.

---

### Task 1: The main level in the mixer

**Files:** `native/strata_mix.h`, `native/tests/mix_test.c`, `native/strata_audio.c` (stamp only)

**Interfaces — Produces:**
```c
SM_OP_MASTER = 12        /* f0 = gain, 0..1; the whole mix, before the limiter */
```
The mixer keeps the main gain asked and the gain now, and glides between them per sample within each block, as `group_from`/`group_step` do.

- [ ] **Step 1: failing tests** in `mix_test.c` (`test_master`):
  - a flat sine at level 0.4; MASTER 0.5: after 100 ms the RMS is 6.0 dB (±0.3) below before;
  - MASTER changed 1 → 0.2 while rendering block by block (SM_BLOCK frames a call): no sample-to-sample step over 0.02 across 40 blocks;
  - MASTER also scales placed voices (speakers and headphones) and the reverb's tail: a click into a 2 s space, MASTER 0 at 0.3 s, then 1 at 0.6 s: silent (< -90 dB) between 0.4 and 0.6 s, and the voices and the space work afterwards (a new click is heard and rings);
  - MASTER NaN, -1 and 5: output finite and never above what MASTER 1 gives.
- [ ] **Step 2:** `cc -O1 -Inative -o native/tests/bin/mix_test native/tests/mix_test.c && native/tests/bin/mix_test` — expect FAIL (`SM_OP_MASTER` undefined).
- [ ] **Step 3:** implement; `native/stamp_audio.sh`.
- [ ] **Step 4:** run; expect `mix_test: ok`. `scripts/verify.sh` PASS. Commit `feat(audio): a main level over the whole mix`.

### Task 2: Levels and groups in Strata.Sound, and the viewer

**Files:** `lib/strata/sound.march`, `test/sound_test.march`, `soundcheck/lib/soundcheck.march`, `viewer/lib/strata_viewer.march`, `README.md`, `docs/2026-11-11-audio-design.md` (§8)

**Interfaces — Produces:**
```
Strata.Sound.op_master() : Int                                   -- 12
Strata.Sound.groups() : Int                                      -- 4
Strata.Sound.level_gain(level : Float) : Float                   -- level³, clamped 0..1; NaN → 0
Strata.Sound.set_master(s, level : Float) : Sound                -- MASTER record, gain = level_gain(level)
Strata.Sound.set_group(s, g : Int, level : Float) : Sound        -- now a 0..1 level, not a raw gain; g outside 0..3 ignored
Strata.Sound.play(s, src, group : Int, x, y, z, gain, pitch, priority) : (Sound, Int)   -- group is the PLAY record's b; outside 0..3 → 0
Strata.Sound.play_flat(s, src, group : Int, gain, pan, pitch) : (Sound, Int)
Strata.Sound.with_fluid_group(s, g : Int) : Sound                -- the group the fluid's pour voices play in (default 0)
Strata.Sound.with_gust(s, follow_openness : Bool, group : Int) : (Sound, Int)
viewer: groups 0 world (strikes, footsteps, recordings, water), 1 wind;
        keys 9 / 0 lower / raise the main level by 0.1, printing it; STRATA_AUDIO_LEVEL=x sets it at start
```
All existing callers of `play`, `play_flat` and `with_gust` (sound_test, soundcheck, the viewer) pass group 0 (wind: 1 in the viewer). Fingerprints must not move: group 0 at level 1 is unity.

- [ ] **Step 1: failing tests** in `test/sound_test.march`:
  - `level_gain(0.0) == 0.0`, `level_gain(1.0) == 1.0`, `level_gain(0.5) == 0.125`, `level_gain(2.0) == 1.0`, `level_gain(-1.0) == 0.0`, `level_gain(float_nan) == 0.0`;
  - `set_master(s, 0.5)` sends one record, op `op_master()`, float 0 = 0.125;
  - `set_group(s, 2, 0.5)` sends op_group, a = 2, float 0 = 0.125; `set_group(s, 7, 0.5)` sends nothing;
  - `play(s, 1, 2, …)` sends a PLAY whose b is 2; `play(s, 1, 9, …)` sends b 0; `play_flat(s, 1, 3, …)` b 3;
  - with `with_fluid_group(s, 2)`, the falling-fluid test's pour PLAY carries b 2; `with_gust(s, true, 1)`'s PLAY carries b 1;
  - dragging: 60 frames each calling `set_master` with a new value send one MASTER record a frame (no build-up).
- [ ] **Step 2:** `. scripts/env.sh && forge test --filter "Strata.Sound"` — expect FAIL (`level_gain` missing).
- [ ] **Step 3:** implement; update every caller; README "Sound" gains a short "Levels" paragraph (main level, four groups the game names, the cubic slider curve, changes glide; the menu and saving are the game's); the design's §8 lists the new calls.
- [ ] **Step 4:** run; expect PASS. `scripts/audio_golden.sh` — expect every fingerprint to match unchanged. A scripted viewer run with `STRATA_AUDIO=wav:/tmp/level.wav STRATA_AUDIO_LEVEL=0.5 STRATA_DIG=1 STRATA_FRAMES=70` renders the dig strike about 18 dB below the same run at 1.0 (`native/tests/bin/wav_fp` on both).
- [ ] **Step 5:** `scripts/verify.sh` PASS. Commit `feat(audio): main and group levels a menu can set`. Merge into main as before.

**Hear it:** `viewer/run.sh`, press 9 and 0 while walking: everything quietens and returns smoothly.

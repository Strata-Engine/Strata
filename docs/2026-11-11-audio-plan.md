# Strata Audio Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Sound for Strata games: recordings and generated sound, placed in the voxel world, muffled through blocks, echoing by the listener's surroundings, falling water heard as one roar, 3D on headphones and stereo on speakers, never hitching a frame.

**Architecture:** March decides what is heard; C makes the samples. `Strata.Sound` is a pure value that turns a game's calls into command records once a frame; `Strata.Ffi.Audio` hands them to `native/strata_audio.c` in one extern call, which pushes them into a wait-free ring drained by the mixer (`native/strata_mix.h`, no miniaudio in it) on miniaudio's device thread, or mixed on the caller's thread in game time for renders. `Strata.Acoustics` walks rays through the blocks on a worker ten times a second.

**Tech Stack:** March (origin/main via `scripts/env.sh`), C, miniaudio v0.11.25 vendored from `~/code/cube_forge/native/miniaudio.h`, MIT KEMAR head-related measurements turned into a generated header.

**Spec:** `docs/2026-11-11-audio-design.md`

## Global Constraints

- Branch `audio`, cut from the newest branch when work starts (today `fluids`). Commit after each task. Execution: native, one independent review at the end.
- `scripts/verify.sh` prints PASS at the end of every task, with no sound device opened and nothing played aloud. Every scripted viewer run uses `STRATA_HIDDEN=1 STRATA_OFFSCREEN=1 STRATA_PIXELS=1` and `STRATA_AUDIO=0` or `STRATA_AUDIO=wav:path`.
- GAPS S1 to S15 apply: unique aliases across the suite (S6), `needs IO.Clock` and `needs IO.Process` in every test module (S3), externs wrapped in ordinary functions, full module path for Unit externs (S10), no extern result bound to `_` (S9), a stamp line in `strata_audio.c` for every generated header it includes (S14), no shared value passed through a hot loop (S15), worker jobs awaited two ticks later (S12).
- No C or device type crosses into March. `strata_audio.c` and `strata_mix.h` include nothing from Vulkan or GLFW.
- On the device thread: no March, no allocation, no lock, no file access.
- Fixed values: 48 000 Hz, 32-bit float, blocks of 256 frames. 64 voices mixed, 1024 tracked, 64 new voices a frame, 8 first walks a frame. Third-order ambisonics (16 channels); binaural filters 128 taps. 4 groups. Output limited to -1 dBFS.
- Acoustics: 10 updates a second by the game's `dt`; 5 walks a voice (straight, and four offset 1 m left, right, up, down as seen from the listener); 64 listener rays to 64 m; up to 8 early reflections; an 8-line feedback delay network.
- Fluid emitters: 8 m grid, at most 16, controls fall over 1 s, a cell silent for 2 s is freed.
- Decoded recordings budget: 64 MB by default.
- Timings are trusted only when `uptime` shows a load average under 8; otherwise recorded as not judged.
- Every task ends with **Hear it** (a command or WAV for the owner) and a check that needs no ears.

## Review Focus

1. **A burst or a stall on the main thread:** thousands of commands in one frame, or no frames for 100 ms. The ring refuses and counts, the frame never waits, the device thread keeps mixing what it has, no voice is stuck playing. Tests: Task 1 (ring full), Task 8 (stall soak).
2. **Handles and recordings at the wrong time:** a command for a voice that ended, a voice never played, a recording not ready, a recording unloaded while it plays. Ignored or counted, never a crash or a click. Tests: Task 2, Task 3.
3. **Degenerate geometry:** a source exactly at the listener, a listener or source inside a solid block, a zero facing vector. No NaN, no infinite gain, a finite output. Tests: Task 4, Task 5.
4. **The edge of the world and the voxel scale:** walks through unloaded chunks and at 0.1 m voxels. Unloaded is air; a 1 m stone wall cuts the same decibels at either scale. Test: Task 5.
5. **No device:** no output device, or opening fails. `open` answers false, the game runs silent, every call is a no-op that counts. Test: Task 2.

---

### Task 1: The mixer, headless

**Files:**
- Create: `native/strata_mix.h`, `native/tests/mix_test.c`, `native/tests/fp_diff.c`, `native/tests/mix_demo.c`

**Interfaces — Produces** (C, all in `strata_mix.h`, static inline or static; no globals):
```c
#define SM_RATE 48000
#define SM_BLOCK 256
#define SM_MIXED 64
#define SM_TRACKED 1024
typedef struct SmRing SmRing;             /* SPSC ring of SmCmd; wait-free push and pop */
typedef struct { int32_t op, voice, a, b; float f[12]; } SmCmd;   /* the command record */
int  sm_ring_push(SmRing *r, const SmCmd *c);   /* 1 pushed, 0 full (caller counts) */
int  sm_ring_pop(SmRing *r, SmCmd *out);
typedef struct SmMixer SmMixer;           /* every buffer allocated in sm_open */
SmMixer *sm_open(void);  void sm_close(SmMixer *);
void sm_command(SmMixer *, const SmCmd *);      /* apply one command (the device side drains the ring through this) */
void sm_mix(SmMixer *, float *stereo_out, int frames);   /* frames a multiple of SM_BLOCK */
typedef struct { int playing, virtual_, refused, ring_refused, underruns; float cpu, peak_db, rms_db; } SmStats;
SmStats sm_stats(const SmMixer *);
int sm_wav_write(const char *path, const float *stereo, int frames);    /* 16-bit PCM */
void sm_fingerprint(const float *stereo, int frames, float *out_db);    /* 10 octave bands (31.5 Hz..16 kHz) per 100 ms window, dB */
```
Ops (the `op` field): 1 PATCH (voice = patch id; floats carry the patch, several records per patch, `a` = record index), 2 PLAY (a = patch or recording id, b = group; f = gain, pitch, priority, x, y, z, flat?, pan), 3 MOVE, 4 CONTROL (a = control 0..3, f[0] = value), 5 STOP (f[0] = fade seconds), 6 LISTENER, 7 OCCLUDE (f = gain, cutoff Hz, send), 8 ENVIRON (reverb), 9 GROUP, 10 OUTPUT (a = 0 speakers, 1 headphones). Tasks 4 and 6 give 6 to 8 meaning; until then they are accepted and ignored.

A patch: 4 sources (kind 0 off, 1 sine, 2 triangle, 3 saw, 4 square, 5 white, 6 pink, 7 brown, 8 recording; pitch ratio, level, detune cents), 2 filters (kind 0 off, 1 low-pass, 2 high-pass, 3 band-pass, 4 peak; cutoff, resonance), gain envelope and filter-1 envelope (attack, decay, sustain, release, depth), 2 modulators (kind 1 sine LFO, 2 smoothed random walk; rate, target 0 pitch / 1 cutoff 1 / 2 gain, depth), 4 controls (target as the modulators plus 3 source-1 level and 4 filter-2 cutoff, low, high, curve 0 linear / 1 exponential). Noise sources are seeded from the voice number, so a render repeats.

- [ ] **Step 1: failing tests** in `mix_test.c`, in `spray_test.c`'s style (`CHECK`, prints `mix_test: ok`):
  - ring: 10 pushes pop in order; a ring of capacity 16 refuses the 17th push and the count is the caller's; push never loops or waits;
  - a sine patch at 1 kHz played flat for 0.5 s: RMS within 0.5 dB of the level asked, fingerprint's 1 kHz band the loudest by 20 dB;
  - envelope: attack 0.1 s reaches 90% between 0.09 and 0.11 s; release ends in silence (peak below -90 dB) within release + one block;
  - filters: white noise through a 1 kHz low-pass at resonance 0.707: the 4 kHz band at least 20 dB below the 250 Hz band;
  - a control mapped to gain changes the level within 20 ms and with no step larger than the slew allows (no sample-to-sample jump over 0.05 at full scale);
  - limiter: 20 voices of full-level noise never exceed -1 dBFS (0.891);
  - STOP on a voice never played, and a CONTROL on a stopped voice, change nothing and do not crash (Review Focus 2);
  - two renders of the same commands produce identical samples;
  - `sm_wav_write` then reading the header back gives 48 000 Hz, 2 channels, the frame count.
- [ ] **Step 2:** add the test to the C loop (it is picked up by `native/tests/*_test.c`); run `scripts/verify.sh` with `STRATA_NO_GPU=1`; expect FAIL (header missing).
- [ ] **Step 3:** implement `strata_mix.h`. Filters are state-variable (Chamberlin or TPT) so cutoff can move per sample; every parameter slews over one block.
- [ ] **Step 4:** `fp_diff.c`: `fp_diff stored.fp new.fp [band_db=1.5] [total_db=0.5]` exits 0 within tolerance, 1 outside, 2 on a shape mismatch. The `.fp` format is text: a header line `fp 1 bands=10 windows=N`, then one line of 10 numbers a window. Add its self-checks to the test (identical passes, a 2 dB band change fails, a different window count is 2).
- [ ] **Step 5:** `mix_demo.c` writes one WAV per building block (each source kind, each filter sweeping, both modulators, an envelope) to a directory given on its command line.
- [ ] **Step 6:** run; expect PASS. **Hear it:** `cc -O2 -Inative -o /tmp/mix_demo native/tests/mix_demo.c && /tmp/mix_demo /tmp/strata-mix` and open the WAVs.
- [ ] **Step 7:** `scripts/verify.sh` PASS. Commit `feat(audio): the mixer, headless — ring, patches, filters, envelopes, limiter, WAV, fingerprints`.

### Task 2: The device, the bindings, the first sounds in the viewer

**Files:**
- Create: `native/miniaudio.h` (copied from `~/code/cube_forge/native/miniaudio.h`, v0.11.25, licence header kept), `native/strata_audio.c`, `lib/strata/ffi/audio.march`, `lib/strata/sound.march`, `test/sound_test.march`, `soundcheck/forge.toml`, `soundcheck/lib/soundcheck.march`, `soundcheck/scenes/*.txt`, `test/golden-audio/`, `scripts/audio_golden.sh`
- Modify: `scripts/verify.sh`, `viewer/forge.toml`, `viewer/lib/strata_viewer.march`, `README.md`

**Interfaces — Produces:**
```
C (strata_audio.c; miniaudio with MA_NO_ENCODING MA_NO_GENERATION MA_NO_RESOURCE_MANAGER MA_NO_NODE_GRAPH, playback only):
int64_t sa_open(int64_t headphones);          /* 1 opened; 0 no device (stderr says why) */
int64_t sa_open_null(void);                   /* miniaudio's null device: real-time pace, silent */
int64_t sa_render_open(const char *path);     /* game time: no device; mixes on the caller's thread into a WAV */
void    sa_close(void);                       /* finishes the WAV in game time */
int64_t sa_send(void *ints_i32, void *floats_f32, int64_t n);   /* n records (4 ints, 12 floats each); answers how many the ring took */
void    sa_advance(double seconds);           /* game time only: mix that much, carrying the remainder; otherwise nothing */
double  sa_stat(int64_t which);               /* 0 playing, 1 virtual, 2 refused, 3 ring refused, 4 underruns, 5 callback CPU share, 6 peak dB, 7 RMS dB,
                                                 10 latency ms of the last PLAY (sa_send to the block that mixed it, plus the device's buffer), 11 bytes sm_open allocated */

March:
Strata.Ffi.Audio (needs Audio; extern "strata_audio" : Cap(Audio)):
  open(headphones : Bool) : Bool;  open_null() : Bool;  render(path : String) : Bool;  close() : Unit
  send(c : Strata.Sound.Commands) : Int;  advance(seconds : Float) : Unit;  stat(which : Int) : Float
Strata.Sound (pure):
  type Sound;  type Commands   -- a NativeI32Arr and a NativeF32Arr and a count
  type Patch;  Strata.Sound.patch() : Patch;  with_source(p, slot, kind, pitch, level, detune) / with_filter(p, slot, kind, cutoff, res)
     / with_env(p, which, a, d, s, r, depth) / with_mod(p, slot, kind, rate, target, depth) / with_control(p, slot, target, low, high, curve) : Patch
  new() : Sound
  upload(s, p : Patch) : (Sound, Int)                         -- patch id
  play(s, src : Int, x, y, z, gain, pitch, priority : Float) : (Sound, Int)   -- voice
  play_flat(s, src : Int, gain, pan, pitch : Float) : (Sound, Int)
  move(s, v, x, y, z) / control(s, v, n : Int, value : Float) / stop(s, v, fade : Float) : Sound
  set_group(s, g : Int, gain : Float) / set_output(s, headphones : Bool) : Sound
  update(s, w : Strata.World.World, f : Strata.Fluid.Fluid, dt : Float) : (Sound, Commands)   -- world and fluid unused until Tasks 5 and 7
  strike() : Patch                                            -- hardness, size, energy on controls 0..2
viewer: STRATA_AUDIO=0 | 1 | wav:path  (default 1 interactive, 0 when STRATA_FRAMES or STRATA_SOAK is set); STRATA_AUDIO_METER=1
```
The game's loop: `let (s2, cmds) = Strata.Sound.update(s1, world, fluid, dt)` then `Strata.Ffi.Audio.send(cmds)` then `Strata.Ffi.Audio.advance(dt)`. Voice numbers come from a counter in `Sound` and wrap at 2^24. `soundcheck` is an app (like `demo/`) that lists `../native/strata_audio.c` in `[ffi]`, reads a scene file of timed calls, renders it in game time at a fixed 1/60 s a frame, and writes `name.wav` and `name.fp`.

- [ ] **Step 1: probe** whether `forge test` in an app links its `[ffi]` sources (a one-line extern call in a scratch app's test). Record the answer in GAPS.md as S16 if it does not; either way, everything that needs C is checked through `soundcheck` and `scripts/audio_golden.sh`, and `test/sound_test.march` tests only the pure `Strata.Sound`.
- [ ] **Step 2: failing tests** in `test/sound_test.march` (alias `Strata.Sound as Snd`):
  - `play` answers voice numbers 1, 2, 3 in order, and `update` then emits a PATCH-free command list with three PLAY records carrying those numbers and the gains given;
  - a frame with 70 `play` calls emits 64 PLAY records; the other 6 are counted by `Snd.refused(s)` (cap of 64 new voices a frame);
  - `stop` and `control` on a voice number never played emit nothing and count nothing (Review Focus 2);
  - `upload` of `Snd.strike()` emits PATCH records whose float payload round-trips through `Snd.patch_floats`;
  - `update` with no calls emits no records.
- [ ] **Step 3:** run `forge test --filter sound`; expect FAIL. Implement `Strata.Sound` and `Strata.Ffi.Audio`; expect PASS.
- [ ] **Step 4: soundcheck failing check:** `scripts/audio_golden.sh` builds `soundcheck`, renders each `soundcheck/scenes/*.txt`, and compares with `test/golden-audio/<name>.fp` through `fp_diff`. First scenes: `blocks` (each source kind in turn, flat), `strike` (twelve strikes across hardness and size). With no stored fingerprints it fails; `--update` stores them and prints the WAV paths.
- [ ] **Step 5:** implement `strata_audio.c`: device mode drains the ring in miniaudio's callback; `sa_render_open` mode drains in `sa_advance`. When no device opens, `sa_open` answers 0 and every later call is a no-op that counts in stat 3 (Review Focus 5) — test this in soundcheck by forcing failure with `STRATA_AUDIO_FAIL=1`.
- [ ] **Step 6:** `scripts/verify.sh` runs `scripts/audio_golden.sh` after the C tests, even with `STRATA_NO_GPU=1`. Add `soundcheck` to its app loop.
- [ ] **Step 7: viewer.** List `../native/strata_audio.c` in `viewer/forge.toml` `[ffi] sources`. A `strike` plays flat when carving and building. `STRATA_AUDIO_METER=1` prints once a second `audio: playing p virtual v refused r ring r2 underruns u cpu c% peak pdB rms rdB`. README: the consumer `[ffi]` block gains the file; a "Sound" section.
- [ ] **Step 8:** `scripts/audio_golden.sh --update`; owner listens to `blocks.wav` and `strike.wav` before the fingerprints are committed. **Hear it:** `viewer/run.sh`, carve and build.
- [ ] **Step 9:** `scripts/verify.sh` PASS. Commit `feat(audio): device, bindings, Strata.Sound, soundcheck goldens, strikes in the viewer`.

### Task 3: Recordings

**Files:** `native/strata_mix.h`, `native/strata_audio.c`, `native/tests/mix_test.c`, `lib/strata/ffi/audio.march`, `lib/strata/sound.march`, `soundcheck/`, `viewer/lib/strata_viewer.march`

**Interfaces — Produces:**
```
C: int64_t sa_load(const char *path); int64_t sa_ready(int64_t id); void sa_unload(int64_t id); void sa_set_budget(int64_t bytes);
   stat 8 decoded bytes, 9 loads refused by the budget
March: Strata.Ffi.Audio.load(path : String) : Int; ready(id : Int) : Bool; unload(id : Int) : Unit; set_budget(bytes : Int) : Unit
Recording ids and patch ids share one number space: a PLAY's source is either. A patch source of kind 8 names its recording in a field of its own.
viewer: N places the next WAV from viewer/sounds/ 5 m ahead (flat until Task 4)
```
Decoding runs on one loader thread created by `strata_audio.c`, which converts to 48 kHz float with `ma_decoder` (WAV, FLAC, MP3) and publishes the buffer by an atomic pointer store; the device side reads it with an atomic load. Unload marks the recording; its buffer is freed by the loader thread once no voice holds it (a reference count the device thread decrements by message, not by freeing).

- [ ] **Step 1: failing checks** in soundcheck scene `recording`: render a 1 kHz tone to a WAV with `sm_wav_write`, load it, wait for `ready`, play it: the fingerprint matches the tone scene's within tolerance. In `mix_test.c`: a recording unloaded while playing keeps playing to its end, then is freed; a PLAY of an id not ready is refused and counted (Review Focus 2); a budget of 1 MB refuses a 2 MB load and stat 9 counts it.
- [ ] **Step 2:** implement; expect PASS. A stereo recording played at a position is folded to mono (tested: equal channels after Task 4; flat until then).
- [ ] **Step 3:** **Hear it:** put any WAV in `viewer/sounds/`, run the viewer, press N. `scripts/verify.sh` PASS. Commit `feat(audio): recordings decoded off the frame, with a budget`.

### Task 4: Sound placed in the world

**Files:** `native/strata_mix.h`, `native/strata_hrir.h` (generated), `native/tools/make_hrir.c`, `native/hrir/` (the KEMAR source files and their README), `native/strata_audio.c`, `native/tests/mix_test.c`, `lib/strata/sound.march`, `test/sound_test.march`, `soundcheck/scenes/`, `viewer/lib/strata_viewer.march`, `README.md`

**Interfaces — Produces:**
```
Strata.Sound.listener(s, x, y, z, fx, fy, fz, ux, uy, uz) : Sound          -- sent once a frame by update
Strata.Sound.with_range(s, v, reference, maximum : Float) : Sound          -- defaults 1 m and 100 m
C: LISTENER and OUTPUT ops take effect; per block each voice gets distance, direction, gain = reference / max(distance, reference), air absorption low-pass from 20 kHz at 0 m to 4 kHz at 200 m, encoded into 16 SN3D/ACN channels
viewer: H switches headphones and speakers
```
Ranking each block: loudness at the listener (gain × distance gain × occlusion gain) × priority; the top 64 mix; a voice entering or leaving the mix fades over 5 ms; a virtual voice's play position advances; a virtual one-shot that passes its end ends; past 1024 tracked, the lowest-ranked is dropped and counted (stat 2).

`make_hrir.c` (built and run by hand, not by the gate) reads the MIT KEMAR compact set, resamples to 48 kHz, projects onto third-order spherical harmonics with max-rE weighting over the measured directions, truncates to 128 taps, and writes `strata_hrir.h` with a hash; `strata_audio.c` carries `#define STRATA_HRIR_STAMP "<hash>"` (S14).

- [ ] **Step 1: ask the owner** before downloading the KEMAR set (source: MIT Media Lab's KEMAR page, compact set, a few MB). Keep the files and their attribution in `native/hrir/`.
- [ ] **Step 2: failing tests** in `mix_test.c`:
  - a source at (10, 0, 0) with the listener facing -z: in speaker mode the right channel at least 6 dB above the left; mirrored for (-10, 0, 0);
  - binaural: the same holds, and a source behind has a different 4 to 8 kHz band balance from one in front by at least 3 dB;
  - encode then each decoder keeps a diffuse field's energy within 1 dB;
  - doubling distance beyond the reference lowers the level 6 dB (±0.5);
  - a source exactly at the listener and a zero facing vector give finite output at the voice's own gain (Review Focus 3);
  - 100 voices: the 64 loudest mix, 36 are virtual; a looped virtual voice brought back resumes at its advanced position; no block-to-block jump larger than the 5 ms fade allows;
  - 1100 voices: 76 dropped and counted.
- [ ] **Step 3:** run; expect FAIL. Implement ranking, distance, the bus, both decoders (speakers: a two-channel max-rE decode at ±30°).
- [ ] **Step 4:** soundcheck scenes `orbit` (a source circling the listener at 5 m over 8 s, both outputs) and `distance` (a source walking away to 120 m). `--update` after the owner listens.
- [ ] **Step 5:** viewer sends the camera as listener; strikes and placed recordings are positional. **Hear it:** `viewer/run.sh` with headphones; press H; walk around a placed recording.
- [ ] **Step 6:** measure stat 5 with 64 patch voices in binaural mode in a soundcheck scene (`load64`); record against the 10% target. `scripts/verify.sh` PASS. Commit `feat(audio): listener, distance, third-order bus, binaural and stereo decoders, ranking`.

### Task 5: Muffling through blocks

**Files:** `lib/strata/blocks.march`, `lib/strata/acoustics.march` (new), `lib/strata/sound.march`, `test/blocks_test.march`, `test/acoustics_test.march` (new), `native/strata_mix.h`, `native/tests/mix_test.c`, `soundcheck/`, `viewer/lib/strata_viewer.march`

**Interfaces — Produces:**
```
Strata.Blocks.with_sound(d : Def, absorption : Int, loss : Int, damping : Int) : Def   -- each 0..255; loss in dB a metre
Strata.Blocks.sound_of(r : Registry, id : Int) : (Int, Int, Int)   -- set values, or the default by opacity class:
     empty (0, 0, 0); solid (20, 25, 200); see-through and cutout (60, 4, 80); a fluid family (40, 10, 220)
Strata.Acoustics.walk(w : World, r : Registry, x0, y0, z0, x1, y1, z1 : Float) : (Float, Float)   -- (loss dB, damping-weighted dB) summed over voxels crossed, scaled by metres a voxel
Strata.Acoustics.occlusion(w, r, listener : (Float, Float, Float), source : (Float, Float, Float)) : (Float, Float)   -- (gain 0..1, cutoff Hz) from the least obstructed of 5 walks
type Strata.Acoustics.Job;  begin(w, r, listener, voices : List((Int, Float, Float, Float))) : Job;  finish(j : Job) : List((Int, Float, Float))   -- voice, gain, cutoff
Strata.Sound.update: casts `occlusion` at once for up to 8 new positional voices (the rest counted by Snd.unwalked); begins a job every 0.1 s of dt, finishes it two frames later (S12), emits OCCLUDE records
```
The def gains an eleventh field and the registry's per-id ints a twelfth (`stride()` becomes 12): bits 0-7 absorption, 8-15 loss, 16-23 damping, bit 24 set. The GPU block table and light classes do not change. Cutoff from damping: 20 kHz × 10^(-damping_db / 40), floored at 200 Hz. The walk reads chunks through a per-chunk window and a per-chunk array of the three values built once per chunk on the worker (S15); unloaded chunks are air.

- [ ] **Step 1: failing tests** in `test/blocks_test.march`: an unset block's `sound_of` is its class default; `with_sound` values round-trip and clamp to 0..255; every existing test passes (the GPU table is byte-identical for an unchanged registry).
- [ ] **Step 2: failing tests** in `test/acoustics_test.march` (alias `Strata.Acoustics as Ac`):
  - a walk through 3 m of stone loses 75 dB (±1); through 3 m of leaves 12; through air 0;
  - the same wall at 0.1 m voxels (30 voxels thick) loses the same 75 dB (Review Focus 4);
  - a walk into an unloaded chunk counts it as air (Review Focus 4);
  - property (`Check.all`, random 16-cube worlds of stone and air, random endpoints): `walk` equals a brute-force sum over 1 cm steps within 2 dB;
  - occlusion behind a 2 m wall is below -40 dB gain; around a corner (the straight line blocked, the "left" walk clear) above -12 dB;
  - a listener inside stone: finite numbers (Review Focus 3);
  - the job: `begin` then `finish` gives the same answers as `occlusion` for each voice.
- [ ] **Step 3:** run `forge test --filter acoustics`; expect FAIL. Implement; expect PASS.
- [ ] **Step 4:** in `mix_test.c`: OCCLUDE gain and cutoff glide over 100 ms with no jump; an occluded voice ranks below a clear one of the same level.
- [ ] **Step 5:** soundcheck gains a small hand-built world (stone, air, leaves) and scene `wall`: a source passing behind a 2 m stone wall and back. **Hear it:** the viewer, a placed sound behind a hill; carve through the hill and hear it open within 250 ms.
- [ ] **Step 6:** record the job's time per update on a worker (target 2 ms) with 64 voices. `scripts/verify.sh` PASS. Commit `feat(audio): block sound values, the walk, occlusion on a worker`.

### Task 6: The listener's space and its echo

**Files:** `lib/strata/acoustics.march`, `lib/strata/sound.march`, `test/acoustics_test.march`, `native/strata_mix.h`, `native/tests/mix_test.c`, `soundcheck/`

**Interfaces — Produces:**
```
type Strata.Acoustics.Space   -- openness 0..1, mean free path m, mean absorption 0..1, mean damping 0..1, up to 8 reflections (delay s, direction, gain)
Strata.Acoustics.space(w, r, listener : (Float, Float, Float)) : Space   -- 64 rays (a fixed Fibonacci sphere), 64 m
Strata.Acoustics.openness(sp : Space) : Float;  decay(sp : Space) : Float   -- seconds
ENVIRON record: decay s, damping, wet level, then reflections (several records)
```
Decay: Eyring's formula on a room whose mean free path is the rays' mean hit distance, `T = 0.161 × V/(−S ln(1−a))` written as `T = 0.04 × mfp / (−ln(1 − a_eff))` with `a_eff = a + openness × (1 − a)`, clamped to 0.1..6 s. Wet level = 0.6 × (1 − openness). Reflections: the 8 nearest hits, delay = 2 × distance / 343, gain from absorption and distance, encoded from the hit's direction. Each voice's send = its gain × min(1, distance / 20 m).

- [ ] **Step 1: failing tests** (March): a closed 10 m stone box: openness 0, decay above 1.0 s; open flat ground: openness above 0.45, decay below 0.3 s; the same box with absorbent walls (absorption 230): decay below half the stone box's; a wall 2 m to the listener's left gives the first reflection from the left at 11.7 ms (±0.5).
- [ ] **Step 2: failing tests** (`mix_test.c`): an impulse through the network with decay 2.0 s measures an RT60 of 2.0 s ± 10%; damping makes the 8 kHz band decay faster than 500 Hz; ENVIRON changes glide without clicks.
- [ ] **Step 3:** run; FAIL. Implement; PASS. `space` runs in the same job as occlusion.
- [ ] **Step 4:** soundcheck scenes `cave` and `field`: the same strikes in each; their fingerprints differ in the tail windows by at least 10 dB. **Hear it:** the viewer, strike while walking from open ground into a carved cave.
- [ ] **Step 5:** `scripts/verify.sh` PASS. Commit `feat(audio): the listener's space, early reflections and reverb`.

### Task 7: Water heard, wind, footsteps

**Files:** `lib/strata/fluid.march`, `test/fluid_test.march`, `lib/strata/sound.march`, `lib/strata/emitters.march` (new), `test/emitters_test.march` (new), `viewer/lib/strata_viewer.march`, `viewer/lib/walker.march`, `soundcheck/`

**Interfaces — Produces:**
```
Strata.Fluid.steps(f : Fluid) : Int          -- steps applied; unchanged by a step finish_world dropped
sink event: Event(x, y, z, eighths, fell, true) at the cell the sink emptied, in the same events list
Strata.Sound.pour() : Patch                  -- controls 0 flow (eighths a second, 0..400), 1 height (cells fallen, 0..32)
Strata.Sound.gust() : Patch                  -- controls 0 strength 0..1, 1 roughness 0..1
type Strata.Emitters.Emitters;  feed(e, events : List(Event), step : Int, scale : Float) : Emitters;  tick(e, dt) : Emitters;  live(e) : List((Int, Float, Float, Float, Float, Float))   -- id, x, y, z, flow, height
Strata.Sound.update: feeds the fluid's events once per new step number; keeps one looping pour voice per live emitter
Strata.Sound.with_gust(s, follow_openness : Bool) : Sound   -- a flat gust voice whose strength follows the Space's openness
```
Emitter cells: 8 m grid in metres (`scale` × voxel coordinates). Flow rises to a new value in one step and falls with a 1 s time constant; a cell silent 2 s fades and is freed; past 16, the quietest at the listener merges into its nearest neighbour.

- [ ] **Step 1: failing fluid tests:** `steps` counts 1, 2, 3 over three applied steps and stays put after a step dropped by `finish_world`; water falling onto a sink yields one landed event with the eighths swallowed; the amount and every existing fluid test unchanged.
- [ ] **Step 2: failing emitter tests:** a steady 80 eighths a step for 30 steps gives a flow within 5% of 800 a second; doubling the events doubles the flow; the same step fed twice counts once; silence for 2 s frees the cell; 20 separated cells keep 16 and conserve the total flow; positions follow the voxel scale.
- [ ] **Step 3:** run; FAIL. Implement; PASS. The viewer's spray also splashes on sink events.
- [ ] **Step 4:** viewer: walker footsteps every 0.5 m of horizontal travel on ground (`strike` with hardness and size from the block underfoot: the viewer's own table), a landing strike scaled by fall speed, gust following openness, fluid emitters.
- [ ] **Step 5:** soundcheck scene `falls`: a hand-built column of falling fluid over 4, 12 and 24 cells; fingerprints louder and brighter with height. **Hear it:** `viewer/run.sh` with `STRATA_BREACH=30` in scene 18; walk into a tunnel and hear the wind drop.
- [ ] **Step 6:** `scripts/verify.sh` PASS. Commit `feat(audio): fluid emitters and pour, sink events, gust, footsteps`.

### Task 8: Scenes, soak, targets

**Files:** `scripts/soak.sh`, `viewer/lib/strata_viewer.march`, `RESULTS.md`, `README.md`, `GAPS.md`, `docs/audio-ledger.md`

- [ ] **Step 1:** soak with `STRATA_AUDIO=null` (the null device) and `STRATA_STALL=600` (a 100 ms sleep on the main thread every 600 frames); the soak asserts underruns 0, ring refused 0, and the worst frame unchanged within 0.5 ms of a run with audio off (Review Focus 1). A soak line gains `audio_cpu`, `audio_main_ms`, `acoustics_ms`.
- [ ] **Step 2:** measure every target in the design's table (latency from stat 10 with the real device in an interactive run; C memory from stat 11; occlusion response as the frames from an edit to the OCCLUDE record that reflects it, in a soundcheck scene) and write a RESULTS section in the same form as the fluid's: target, result, met or missed, and the cause of each miss.
- [ ] **Step 3:** the listening scenes in the viewer (open ground into a cave, a wall between listener and sound, the breached lake, footsteps over three materials) are listed in README with their commands.
- [ ] **Step 4:** `scripts/verify.sh` PASS, `scripts/soak.sh` PASS. Commit `docs(audio): results, README, soak with audio`.
- [ ] **Step 5:** one independent review of the whole branch by a fresh reviewer; fixes each with a red test first; the ledger records rulings.

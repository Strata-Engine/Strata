# Strata audio: design

Status: design, awaiting approval. Nothing here is built.
Date: 2026-11-11
Follows: `2026-11-10-strata-design.md`. A new project beside the four the
first design named. Uses the fluid events built from
`2026-11-10-fluid-design.md` and the ray walk of `Strata.Query`; depends on
nothing that is not built (interface, physics, far field, saves).

## Intent

Strata is silent. This project lets a game built on it make sound, and lets
the voxel world shape that sound: it fades with distance, dulls and drops
through material, and echoes according to the space around the listener, so
a cave rings and an open field is dry. Water is heard as water: the many
small events of a breached lake become one continuous roar that grows with
the flow.

## Decisions the owner made

1. Strata plays a variety of sounds (wind, water and footsteps are examples,
   not a list).
2. Sound fades with distance and through materials, and reflects off
   surfaces. The space must be heard: a cave echoes, open ground does not.
3. Sounds come from recordings a game loads and from sound made in code;
   both, mixed freely.
4. Strata itself generates sound: it offers building blocks a game wires
   together, and ready-made sounds built from them that a game turns on and
   tunes. Which ready-made sounds ship first is the designer's call.
5. A block may say how it treats sound. This is optional, and the game sets
   it, as it sets roughness and emission for light.
6. Music is left to the game for now.
7. Strata, not the game, turns fluid events into one continuous sound that
   grows with the flow.
8. Device output and decoding come from miniaudio, vendored under
   `native/`.
9. Headphones and speakers both: rendered in 3D for headphones, panned for
   speakers.
10. Offline renders are compared "close enough", not byte for byte.
11. From the first design: high frame rate under heavy simulation is the
    priority; audio never hitches a frame and a frame hitch never glitches
    audio; the library names no game concept.
12. The gate runs with no sound device and never plays sound aloud.

Everything else below is the designer's call, and is marked where it is not
obvious.

## What most engines do (for reference)

- Voice limits: FMOD defaults to 64 real voices, Unity to 32 real and 512
  virtual, Wwise is configured per project. Beyond the limit the quietest
  are made virtual: tracked, not mixed.
- Occlusion and reverb from geometry: Steam Audio and Microsoft Project
  Acoustics trace or bake rays against the level. In voxel games, the
  Minecraft mod Sound Physics Remastered casts rays from each sound through
  the blocks to choose muffling and one of four reverbs. Vanilla Minecraft
  does neither.
- 3D on headphones: Steam Audio and Google's Resonance Audio mix every sound
  into an ambisonic bus (a sound field described by a fixed number of
  channels) and turn that into two ears once, with head-related filters
  (HRTF). The cost is then fixed, not per sound.

Strata follows the voxel-mod approach for the world (rays through the
blocks, on the CPU) and Resonance's approach for the ears.

## Goals

- A game plays a sound at a point, attached to something that moves, or
  with no position (for interface sounds), and can move, retune and stop it.
- Recordings: WAV, FLAC and MP3 files, decoded off the frame and off the
  device thread.
- Generated sound: a small fixed set of building blocks a game combines into
  a **patch** (sources, filters, envelopes, modulators, live controls), and
  ready-made patches Strata builds the same way.
- The world shapes every positional sound: distance, muffling through
  material, and the listener's surroundings deciding the echo.
- Fluid motion is heard without the game doing anything but handing Strata
  the fluid each step.
- 3D sound on headphones and stereo on speakers from one mix.
- Many sounds at once with a stated limit, and a stated, counted behaviour
  beyond it.
- Every step can be heard by the owner and checked by the gate without
  listening.

## Non-goals

- Music, scores, beat-synchronised scheduling.
- Surround output (5.1 and up). The mix is built so it is one more decoder
  later.
- Doppler shift. Room is left: a voice already knows its velocity's source,
  its position over time.
- Echo that depends on where the source is. One environment, the listener's,
  shapes all reverb (see Limits).
- Ogg Vorbis and other formats. WAV, FLAC and MP3 only.
- Streaming long files from disk while they play. Every recording is decoded
  whole into memory (see the memory budget).
- Microphone input, voice chat, recording the player.
- A sound editor or a patch language read from files.
- Sound for chunks that are not loaded: they are silent and transparent.

## Anti-goals

- Any library call that can block the frame: no extern waits on the device
  thread, a lock, a file, or a decode.
- March code, allocation, a lock or file access on the device thread.
- A main thread stall audible as a click, gap or stutter.
- Sound named after a game concept in the library (footstep, grass,
  explosion). Ready-made patches are named for what they physically are.
- A device opened, or sound played aloud, by the gate or any scripted run
  that does not ask for it.
- A sound that is silently dropped: every refusal is counted.

## Constraints

- March `origin/main` through `scripts/env.sh`; never install over
  `~/.opam/march`. `scripts/verify.sh` stays green.
- No C or device type crosses into March: small integers, floats and native
  arrays only, as in `Strata.Ffi.Vk`.
- The audio C file knows nothing of Vulkan or GLFW and builds and tests
  without a sound device. A game that only wants audio does not link the
  renderer, and the reverse.
- forge does not build a dependency's C sources (GAPS S11): a consumer lists
  `native/strata_audio.c` in its own `[ffi]` sources. forge reuses a build
  when only a header changed (S14): any generated header gets a stamp line
  in the C file, as the shaders do.
- No way to poll a March task (S12): work begun on a worker is awaited two
  ticks later, as in `Strata.Stream`.
- A value shared between worker tasks is not passed through a hot loop
  (S15): the acoustic ray walk reads per-chunk arrays its task owns.
- macOS on an M3 Max is the only measured platform. miniaudio covers others
  by design; nothing is claimed for them.

## The design

### 1. The split, and what crosses to the device thread

March decides what is heard; C makes the samples. This is cube_forge's
split and the fluid step's pattern: pure work in March (on a worker where it
is heavy), results handed to C whole.

```
 main thread (March)                C, main-thread side           device thread (C)
 ------------------                 -------------------           -----------------
 Strata.Sound calls  --extern-->    write a command record  -->   ring  --> drained at the
                                    into a fixed ring; never           start of each block
                                    waits; full ring: count it
 Strata.Acoustics job (worker) --> results as commands, 10 Hz
                     <--extern--    read counters and meters  <-- atomics written per block
```

- **Commands** are fixed-size records (play, move, set a control, stop, set
  listener, set occlusion, set environment, load finished) pushed into a
  single-producer single-consumer ring. Pushing is wait-free. A full ring
  refuses and counts; the game sees the refusal count.
- **The device thread** runs miniaudio's callback. At the start of each
  block it drains the ring, then mixes. All its memory is allocated when the
  mixer is opened. It never calls March, never locks, never allocates, never
  touches a file.
- **Back to March** go only numbers in atomics, written once a block:
  voices playing, virtual, refused; ring refusals; underruns; the callback's
  CPU time; peak and RMS level of the output. Read by plain extern getters.
- **A frame hitch** leaves the device thread mixing with the last positions
  and parameters it was given. Every parameter slews to its new value over
  the time between updates, so late updates arrive as a glide, not a jump.
- **An audio hitch** (an overrun callback) cannot reach the frame: nothing
  on the main thread waits for the device thread.

Handles: `Strata.Sound` numbers voices itself (a counter in the value), so
`play` returns a handle at once without asking the device thread. A
handle whose voice has ended is ignored by later commands.

**Two clocks** (designer's call). The mixer is the same code under both:

- **Device time**, for play: miniaudio's device (or its silent null device)
  pulls blocks on its own thread at real-time pace. Interactive runs and the
  soak use it; the soak's stalls and underrun count are measured against it.
- **Game time**, for renders that must repeat: there is no device thread;
  each `Strata.Sound.update(..., dt)` mixes exactly `dt` seconds of blocks on
  the calling thread and appends them to a WAV, carrying the remainder to the
  next call. Commands apply at the block where the update that sent them
  begins. `STRATA_AUDIO=wav:path` and the audio goldens use it, so a scripted
  run at a fixed 1/60 s a frame renders the same sound however fast frames
  really ran.

Everything Strata times on the March side (the acoustics updates, fluid
emitter fades, caps per frame) counts the `dt` the game passes, not the wall
clock, so both modes agree with the game's own time.

### 2. The mixer (C, testable without a device)

The mixer is plain C with no miniaudio in it (`native/strata_mix.h`, as
`strata_spray.h` holds the spray ring's logic). `native/strata_audio.c`
binds it to a miniaudio device, to an offline renderer that writes a WAV,
and to miniaudio's decoders. The mixer runs at 48 kHz, in 32-bit float, in
blocks of 256 frames.

Per block:

1. Drain commands.
2. Rank voices (see 5) and choose up to 64 to mix.
3. For each mixed voice: produce its block (sample playback or patch), apply
   its envelope and gain, its distance gain and air absorption, its
   occlusion gain and low-pass, then encode it into the spatial bus at its
   direction from the listener, and add its reverb send.
4. Run the reverb into the spatial bus.
5. Decode the spatial bus to the output: binaural for headphones, stereo
   for speakers.
6. Group and master gain, then a limiter so the output never clips.
7. Publish meters and counters.

### 3. Sources: recordings and patches

**Recordings.** `Strata.Sound.load(path)` answers an id at once. A loader
thread in C (not a March task: the device side owns it, and its state can
be polled, which a March task cannot, S12) decodes the file with miniaudio,
converts it to 48 kHz float, mono or stereo, and publishes it to the mixer
by handing over a pointer. `Strata.Sound.ready(id)` says whether it has
arrived; playing it before then is counted as a refusal, not a wait. A
stereo recording plays unspatialised or is folded to mono when placed in the
world (designer's call: a placed sound has one position).

Decoded memory has a budget (default 64 MB); a load past it fails and is
reported. Recordings can be unloaded; a voice still playing one keeps it
until it ends.

**Patches.** A patch is a fixed shape a game fills in, built in March as
data and uploaded once:

- up to four **sources**: sine, triangle, saw, square, white, pink or brown
  noise, or a recording; each with its own pitch, level and detune;
- two **filters** in series: low-pass, high-pass, band-pass or peak, each
  with cutoff and resonance;
- an **envelope** on the gain (attack, decay, sustain, release) and one on
  the first filter's cutoff;
- two **modulators**: a sine LFO and a smoothed random walk (for gusts and
  turbulence), each routable to pitch, cutoff or gain with a depth;
- four **controls**: numbers a game sets live on a playing voice, each
  mapped to any of the above with a range and a curve.

Patches are values: a game cannot add a new kind of building block without
changing Strata, and that is intended. A fixed shape keeps the per-sample
cost known and the device thread free of interpretation.

**Ready-made patches** (designer's call: the sounds the owner named, made
generic). Strata builds them with the same calls a game would:

| patch | what it is | its controls |
|---|---|---|
| `gust` | turbulent air: brown and pink noise through a moving band-pass, swelling on a random walk | strength, roughness |
| `pour` | falling liquid: broadband noise whose level and brightness follow flow and height; fed by the fluid (see 7) | flow, height |
| `strike` | a short impact: a noise burst through a resonant filter | hardness, size, energy |

Liquid moving sideways without falling has no sound in this version: the
fluid reports only where water arrives from above, and adding sideways
events to its hottest path is not worth it here (designer's call; a stated
limit).

A game makes footsteps from `strike` with a hardness and size per block it
chooses; physics will later drive `strike` from impulses. The names describe
the sound, not a game's use of it. `gust` may follow the listener's openness
to the sky (see 6) if the game asks.

### 4. Placing sound in the world

**Listener.** The game sets the listener's position, facing and up each
frame (the camera's, normally). The device thread recomputes every voice's
distance and direction from the latest listener each block, so turning the
head moves sounds within one block, not one acoustic update.

**Distance.** Gain falls as reference distance over distance, clamped at a
per-sound maximum where the voice goes virtual. Air absorption is a gentle
low-pass that closes with distance. All distances are in metres, so the
world's voxel scale does not change how sound behaves.

**The spatial bus** is third-order ambisonics: sixteen channels describing
the sound field around the listener. Encoding a voice costs sixteen gains a
sample; the decoders then run once for the whole mix:

- **headphones**: a binaural decoder, sixteen pairs of short filters (128
  taps) derived from a measured head-related set, giving front and back,
  above and below;
- **speakers**: a stereo decoder from the same bus.

The output mode is a setting the game exposes. A surround decoder later is a
third decoder on the same bus. The head-related set is vendored as data:
designer's choice is the MIT KEMAR measurements (free to use with
attribution), turned into the decoder's filters once by a script and
committed as a generated header with a stamp line (S14).

### 5. How many at once, and beyond

- **64 voices are mixed.** Up to **1024** are tracked; a tracked voice that
  is not mixed is **virtual**: its time advances, nothing is computed.
- Every block, voices are ranked by how loud they would be at the listener
  after distance and occlusion, times a priority the game gives at `play`.
  The top 64 are mixed. A voice that crosses into the mix fades in over a
  few milliseconds, and out the same way, so ranking never clicks. A looping
  or long voice that becomes audible again plays from where it would have
  been.
- A one-shot that is virtual when it would have ended simply ends.
- Past 1024, the lowest-ranked is refused or dropped, and counted.
- Up to 64 new voices a frame; past that, refused and counted.

### 6. The world shaping sound: `Strata.Acoustics`

All of this is pure March, testable headless, and runs on a worker ten times
a second.

**Block properties.** `Strata.Blocks.with_sound(def, absorption, loss,
damping)` sets three optional bytes:

- `absorption`: how much of the sound a surface soaks up when sound reflects
  off it (0 rings like stone or ice, 255 deadens like wool);
- `loss`: how much a metre of the block cuts sound passing through it, in
  decibels;
- `damping`: how much more it cuts the high frequencies than the low, both
  in reflection and passing through.

A block that sets nothing takes a default from its opacity class (designer's
call, to be tuned by listening): empty, nothing; opaque solid, absorption
low, about 25 dB a metre, strong damping; see-through and cutout (glass,
leaves), little loss; a fluid family, moderate loss, strong damping.
Per-chunk arrays of these values are built once per chunk when the walk
needs them (S15), not read from the registry per voxel.

**The walk.** A ray from one point to another, stepped voxel by voxel like
`Strata.Query.raycast`, that does not stop at the first hit but sums the
loss and damping of every voxel it crosses, scaled by the world's metres per
voxel. Unloaded chunks count as air.

**Occlusion** of each audible positional voice: one walk straight from the
listener to the source, and four to points offset around the source (about
a metre to each side, up and down). The least obstructed path decides, so a
sound around a corner is muffled, not lost, and a sound behind a thick hill
nearly is. The result is a gain and a low-pass cutoff per voice.

A new voice cannot wait 100 ms for its first answer: a distant sound behind
a wall would play clear for a moment. So `play` does not send anything; it
records the voice in the `Sound` value and answers `(sound, voice)`. The
frame's `update`, which has the world, casts the single straight walk for up
to 8 new positional voices and sends each play command with its occlusion
already set. The rest start with no occlusion until the worker's next
answer, and are counted. A sound therefore starts at the end of the frame
that asked for it, never in the middle.

**The listener's environment.** 64 rays from the listener in fixed,
evenly spread directions, each to at most 64 metres. From them:

- the share that escape (reach open sky or the limit): **openness**;
- the mean distance to what they hit and the absorption and damping there;
- per ray, the distance and direction of the first hit.

These give the reverb: the decay time grows with the mean distance and
falls with absorption and with openness; brightness follows damping; the
amount of reverb grows as openness falls. The nearest hits give up to eight
**early reflections**, each a delay and a direction encoded into the
spatial bus, so a wall close on the left is heard on the left. A stone cave
gives a long, bright, strong tail; a field gives almost none; a wooden room
the game marks absorbent gives a short dull one.

The reverb itself (C) is an eight-line feedback delay network after the
early reflections, its decay and damping set from the environment and
slewed over the update interval.

Each voice's reverb send is its occlusion-adjusted gain times a send that
grows with distance, so far sounds are wetter.

**The job.** `Strata.Acoustics.begin(world, listener, voices)` starts the
walks on a worker and answers a job; `finish` two ticks later answers the
results, which `Strata.Sound` sends to C as commands. Ten updates a second;
the device thread glides between them.

### 7. Fluids heard

`Strata.Sound` keeps a set of **fluid emitters**, fed by
`Strata.Fluid.events` each fluid step:

- Events are gathered into cells of a coarse grid (designer's call: 8
  metres). Per cell, per step: eighths that arrived, the mean distance
  fallen, and the share that landed.
- Each live cell is one looping `pour` voice, its flow control following
  eighths a second and its height control following the distance fallen.
- Controls rise quickly and fall over about a second, so ten steps a second
  of events become one continuous sound. A cell with no events for two
  seconds fades out and is freed.
- Up to 16 emitters, the loudest at the listener kept; the rest merge into
  their nearest neighbour.

Two small, additive changes to `Strata.Fluid` (designer's call; with
`with_sound`, the only changes this project makes outside its own modules):

- **a step number** (`Strata.Fluid.steps`), counting steps that were
  applied, so each step's events are heard once; today a step dropped by the
  worker leaves the previous events in place (fluid ledger);
- **an event where a sink swallows water**, marked as landed, so a waterfall
  into the sea is heard. Today it makes no event. The spray the viewer
  throws from events gains the same splash, which the ledger lists as
  missing.

Every flowing family sounds as `pour`; the event does not say which family
moved (a stated limit: a game with two liquids hears both alike).

### 8. The API a game sees

```
Strata.Sound.open(config)               -- the device; or render(path) for an offline WAV
Strata.Sound.load(path) / ready(id) / unload(id)
Strata.Sound.patch(...)                 -- build a patch; upload answers its id
Strata.Sound.gust() / pour() / strike()               -- the ready-made patches
Strata.Sound.play(s, patch_or_recording, x, y, z, gain, pitch, priority) -> (s, voice)
Strata.Sound.play_flat(s, patch_or_recording, gain, pan, pitch) -> (s, voice)   -- no position
Strata.Sound.move / control / stop (s, voice, ...) -> s
Strata.Sound.listener(s, x, y, z, fx, fy, fz, ux, uy, uz) -> s
Strata.Sound.set_output(headphones?) / set_group(group, gain)
Strata.Sound.update(s, world, fluid, dt) -> s   -- once a frame, last: first walks, sends
                                                -- the frame's commands, acoustics job,
                                                -- fluid emitters; in game-time mode, mixes dt
Strata.Sound.stats(...)                     -- voices, virtual, refused, underruns, CPU, levels
Strata.Blocks.with_sound(def, absorption, loss, damping)
```

`Strata.Sound` is a value, as `Strata.Fluid` is: it holds the voices the
game started, the acoustic job, and the fluid emitters. `Strata.Acoustics`
is pure. `Strata.Ffi.Audio` holds the externs, each wrapped in an ordinary
function, in a capability of its own. Four groups (designer's call) let a
game set separate volumes for, say, world, ambience, interface and music
it plays itself.

### 9. The viewer

- Sound is on in interactive runs and off in every scripted run unless asked
  for: `STRATA_AUDIO=0` off, `=1` the device, `=wav:path` the whole session
  rendered to a file and no device opened.
- `strike` footsteps whose hardness follows the block underfoot, landings,
  and `strike` when carving and building.
- `gust` following openness, so the wind drops when the camera goes
  underground.
- Fluid emitters from the fluid it already steps.
- A key (`H`) switches headphones and speakers.
- If a folder `viewer/sounds/` exists, WAV files in it can be placed with a
  key, to hear recordings. The repository ships none (designer's call: no
  recorded assets of uncertain licence).
- Once a second, with `STRATA_AUDIO_METER=1`, a line on standard output:
  voices, virtual, refused, underruns, callback CPU, peak and RMS.
- Scripted scenes for listening and for the gate: a walk from open ground
  into a cave, a stone wall between the listener and a sound, the test lake
  breached, footsteps across three materials.

## Budget and targets

Designer's targets, stated so a miss is reported. M3 Max, release build.

| | target |
|---|---|
| the frame's `update` to a played sound's first sample in the device's buffer | 20 ms or less (256-frame blocks; the device's own buffer counted). A `play` early in a frame adds the rest of that frame |
| voices mixed / tracked | 64 / 1024 |
| device thread CPU, 64 patch voices, third-order bus, binaural, reverb | 10% of one core or less (100 ms per second of audio) |
| main thread per frame, all of `Strata.Sound` | 0.2 ms or less |
| acoustics job per update (64 voices x 5 walks, 64 listener rays) | 2 ms or less on a worker |
| occlusion follows a move or an edit | within 250 ms |
| C mixer memory, excluding recordings | 16 MB or less |
| decoded recordings | 64 MB by default, a setting; refused past it, counted |
| underruns over a soak with a 100 ms main thread stall every 10 s | 0 |
| output peak | never over -1 dBFS |
| added to the worst frame of the existing soak | under 0.5 ms |

## Testing

Every step has a way to hear it and a check that does not need ears.

- **C tests** (`native/tests/`, run by the gate like `spray_test.c`): the
  command ring (order, full ring counted, no blocking); envelopes; filters
  (a sweep measured against the asked cutoff); the voice ranking and the
  fade on crossing into the mix; virtual voices keep time; encoding and both
  decoders preserve a sound's energy and put a source on the left on the
  left; the reverb's measured decay time within 10% of the asked; the
  limiter holds -1 dBFS; a WAV written and read back.
- **March tests**: block sound defaults by opacity class; the walk against a
  brute-force sum on random worlds; occlusion of a stone wall, a leaf hedge
  and a corner; the environment of a closed stone box against open ground
  (openness, decay time ordering); fluid emitters (events become a steady
  flow, louder flow is louder, a dropped step is not counted twice, emitters
  fade and are freed, the cap merges, a sink's swallowing is heard).
- **Audio goldens**: scripts of timed commands rendered in game time to a
  WAV, with no device and no GPU, from a small March test program over a
  hand-built world (a stone box, open ground, a wall, a falling column of
  fluid), so they run under `STRATA_NO_GPU=1` too. The viewer's scenes are
  for listening and for the meters, not fingerprinted. What is stored is a fingerprint: loudness in octave bands
  over 100 ms windows. A render passes when every band is within 1.5 dB of
  the stored fingerprint and overall loudness within 0.5 dB. `--update`
  writes the WAVs for the owner to listen to before the fingerprints are
  stored, as images are looked at before they are stored.
- **Hearing a step**: each task ends with the viewer command or WAV path
  that lets the owner hear it.
- **Soak**: the existing soak with audio rendered to a file through
  miniaudio's null device (paced like a real one, silent), a main thread
  stall injected every 10 seconds, and the counters asserted: no underruns,
  nothing refused that the scene did not overload on purpose, and the frame
  figures within the target.
- **The gate** opens no device. Tests and goldens render offline; scripted
  viewer runs use `STRATA_AUDIO=0` or `wav:`.

## Order of work

One plan. Each step leaves the viewer working and can be heard.

1. The mixer, headless: ring, voices, patches, envelopes, filters, limiter,
   offline WAV; C tests. Hear it: a rendered WAV of each building block.
2. The device: `strata_audio.c` on miniaudio, `Strata.Ffi.Audio`,
   `Strata.Sound` play and stop, flat sounds; the viewer strikes when it
   carves. Audio goldens begin.
3. Recordings: loader thread, decoding, budget, `viewer/sounds/`.
4. Place in the world: listener, distance, the spatial bus, both decoders,
   ranking and virtual voices.
5. `Strata.Acoustics`: block sound properties, the walk, occlusion, the
   worker job.
6. The listener's environment, early reflections and the reverb.
7. Fluid emitters, the step number and sink event, `pour`, `gust` with openness;
   footsteps in the viewer.
8. Scenes, soak, measurements against the targets, README and RESULTS.

## Left to the builder

- The command record's layout and the ring's size.
- Filter forms (state-variable or biquad), the LFO and random-walk shapes,
  control curves.
- The exact distance, air absorption and send curves; the openness and
  decay formulas; how early reflections are chosen.
- The binaural filter derivation and the stereo decoder.
- Default sound values per opacity class, tuned by listening.
- How the acoustics job chooses which voices to update when there are more
  than it can afford.
- Names of functions and modules beyond those the owner will see in the
  README; file layout; test organisation.
- miniaudio's configuration (which backends compiled in, which decoders).

## Limits, stated now

- One environment shapes all reverb: a cave heard from outside its mouth
  does not echo with the cave's echo.
- Unloaded chunks are air to sound.
- Liquid moving sideways makes no sound; only falling liquid does.
- Every flowing family sounds alike.
- No Doppler, no surround, no streaming from disk.
- Measured on one machine only.

## Risks

- **The acoustics job's cost.** Five walks for each of 64 voices, ten
  times a second, may not fit 2 ms on a worker if a voxel lookup costs what
  a fluid cell did. Fallbacks in order: fewer offset walks for quiet
  voices, five updates a second, fewer listener rays.
- **Binaural quality** from third-order ambisonics is smoother than direct
  per-voice HRTF; front and back can blur. If the owner hears it so, the
  nearest few voices take direct head-related filters beside the bus.
- **Defaults that sound wrong.** Loss and absorption numbers are guesses
  until listened to; they are tuned in step 5 and 6 with the owner.
- **miniaudio on CoreAudio's real-time thread** must never touch March's
  runtime. The C side is the only code there; the gate's soak checks
  underruns.
- **Licensing of the head-related data.** MIT KEMAR asks for attribution;
  it goes in the README and beside the generated header.

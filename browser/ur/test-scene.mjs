import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { cameraProjection, parcelRay, trafficCamera } from './camera.mjs';

const path = process.argv[2] ?? 'browser/.build/ur/core.wasm';
const { instance } = await WebAssembly.instantiate(readFileSync(path), { math: Math });
const core = instance.exports;
assert.equal(core.ur_step(-3, 0), 1);

function readScene(count) {
  return new Uint32Array(core.memory.buffer, core.ur_scene_ptr(), count * 8);
}
function pose(index) {
  return new Float32Array(core.memory.buffer, core.ur_scene_poses() + index * 32, 8);
}
function meshes(phase) {
  const decks = new Map();
  let lamps = 0, layers = 0;
  for (let lot = 0; lot < 2640; lot += core.ur_scene_batch_size()) {
    const words = readScene(core.ur_scene_batch(lot, phase));
    lamps += core.ur_scene_static_count();
    const stride = core.ur_scene_volume_words();
    const volume = new Uint32Array(core.memory.buffer, core.ur_scene_volume_ptr(), core.ur_scene_batch_size() * 4 * stride);
    for (let offset = 0; offset < volume.length; offset += stride) {
      if (!volume[offset + 1]) continue;
      assert.ok(volume[offset] > 0 && volume[offset] <= 32);
      layers++;
    }
    const deckStride = core.ur_scene_deck_words();
    const shapes = new Uint32Array(core.memory.buffer, core.ur_scene_decks_ptr(), core.ur_scene_batch_size() * deckStride);
    for (let offset = 0; offset < shapes.length; offset += deckStride) {
      if (!shapes[offset]) continue;
      const length = shapes[offset + 1];
      const width = shapes[offset + 3];
      const solid = 0xffffffff >>> (32 - length);
      assert.equal(shapes[offset + 4], solid);
      assert.equal(shapes[offset + 6], solid, 'outside rail missing from the shadow shape');
      assert.equal(shapes[offset + 10], 0, 'the open deck interior must not cast shadows above the road');
      assert.equal(shapes[offset + 4 + (width - 1) * 4 + 2], solid, 'far rail missing');
    }
    for (let i = 0; i < words.length; i += 8) {
      const index = words[i + 3];
      if (index) decks.set(index, pose(index).slice());
    }
  }
  return { decks, lamps, layers };
}

const day = meshes(0.9), night = meshes(1.5);
assert.equal(day.decks.size, 3);
assert.ok(day.layers > 100, 'terrain shadow volumes missing');
assert.deepEqual(day.decks, night.decks, 'lighting changed the overpass geometry');
assert.equal(day.lamps, 0);
const changedPtr = core.ur_scene_changes(0.9, 1.5);
const affected = new Uint8Array(core.memory.buffer, changedPtr, 2640 / core.ur_scene_batch_size());
const changes = affected.reduce((total, changed) => total + changed, 0);
assert.ok(changes > 0 && changes < affected.length, 'night lighting did not target specific batches');
assert.ok(night.lamps > 0, 'the powered street has no lamps at night');
for (const [x, y, z, scale, qx, qy, qz, qw] of day.decks.values()) {
  assert.ok(x >= 9 * 8 && x < 10 * 8 && y > 0 && z >= 18 * 8 && z < 21 * 8);
  assert.ok(scale >= 0.25 && scale <= 0.26);
  assert.ok(Math.abs(Math.hypot(qx, qy, qz, qw) - 1) < 1e-6);
  assert.ok(Math.abs(qx) + Math.abs(qz) > 0.01, 'the deck has no slope');
}
const batchCount = 2640 / core.ur_scene_batch_size();
function changedBatches(tick) {
  const pointer = core.ur_scene_changes(0.9, 0.9, 0, tick);
  return new Uint8Array(core.memory.buffer, pointer, batchCount);
}
let signalTick = 1;
while (signalTick < 32 && !changedBatches(signalTick).includes(1)) signalTick++;
assert.ok(signalTick < 32, 'the fixture junction did not change its signal');
assert.equal(changedBatches(signalTick).reduce((sum, flag) => sum + flag, 0), 1);
const junction = changedBatches(signalTick).indexOf(1) * core.ur_scene_batch_size();
const beforeSignal = readScene(core.ur_scene_batch(junction, 0.9, 0)).slice();
const afterSignal = readScene(core.ur_scene_batch(junction, 0.9, signalTick));
assert.equal(beforeSignal.length, afterSignal.length);
assert.notDeepEqual(beforeSignal, afterSignal, 'the lit signal materials stayed the same');

const view = cameraProjection(trafficCamera);
const u = 0.6, v = 0.4, y = 13;
const [ox, oz, dx, dz] = parcelRay(trafficCamera, u, v);
const point = [ox * 8 + dx * y, y, oz * 8 + dz * y, 1];
const clip = offset => point.reduce((sum, value, index) => sum + value * view[offset + index], 0);
assert.ok(Math.abs(clip(4) - (2 * u - 1)) < 1e-10);
assert.ok(Math.abs(clip(8) - (1 - 2 * v)) < 1e-10);

assert.equal(core.ur_overpass(0, 1), 0, 'accepted an invalid crossing');

const drivers = new Map(), walkers = new Map();
const times = [];
let headlights = 0, warmBytes;
for (let f = 0; f < 1440; f++) {
  const time = f * 0.05;
  const hour = (9 + time) % 24;
  const start = performance.now();
  const count = core.ur_scene_frame(time, 0.05, hour * 0.075);
  times.push(performance.now() - start);
  const light = new Float32Array(core.memory.buffer, core.ur_scene_light(), 12);
  if (hour >= 18 || hour < 6) headlights = Math.max(headlights, light[10]);
  else assert.equal(light[10], 0);
  if (hour === 12) assert.ok(light[1] > 0 && light[4] > 0);
  if (hour === 0) assert.equal(light[4], 0);
  const words = readScene(count);
  for (let i = 0; i < words.length; i += 8) {
    const slot = words[i + 7] - 1;
    const driver = slot >= 16384 && slot < 16480;
    if (!driver && (slot < 16120 || slot >= 16216)) continue;
    const set = driver ? drivers : walkers;
    if (!set.has(slot)) set.set(slot, new Set());
    const [x, y, z] = driver ? pose(words[i + 3]) : words.subarray(i + 4, i + 7);
    set.get(slot).add(`${x},${y},${z}`);
  }
  if (f === 960) warmBytes = core.memory.buffer.byteLength;
}
assert.ok([...drivers.values()].some(positions => positions.size > 20), 'no cars travelled');
assert.ok([...walkers.values()].some(positions => positions.size > 20), 'no pedestrians walked');
assert.ok(headlights > 0, 'night traffic had no headlights');
assert.equal(core.memory.buffer.byteLength, warmBytes, 'WASM memory grew during the final day');
times.sort((a, b) => a - b);
console.log({ frames: times.length, deckSections: day.decks.size, nightLamps: night.lamps, headlights,
  p95Ms: times[Math.floor(times.length * 0.95)].toFixed(2), wasmMiB: (warmBytes / (1024 * 1024)).toFixed(2) });

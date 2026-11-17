import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { readFileSync } from 'node:fs';

const [wasm, native] = process.argv.slice(2);
const module = new WebAssembly.Module(readFileSync(wasm));
assert.deepEqual(WebAssembly.Module.imports(module), []);
const { exports: core } = new WebAssembly.Instance(module);
assert.equal(core.init(12345, 1), 0);

function mesh() {
  assert.ok(core.mesh_bytes() > 0);
  assert.equal(core.mesh_bytes() % 8, 0);
  return Buffer.from(new Uint8Array(core.memory.buffer, core.mesh_ptr(), core.mesh_bytes()));
}

const reference = execFileSync(native);
const count = reference.readUInt32LE(0);
assert.equal(count, core.mesh_bytes());
for (let i = 0; i < 3; i++) {
  assert.equal(core.noise_sample(i), reference.readDoubleLE(4 + i * 8));
}
assert.deepEqual(mesh(), reference.subarray(28));
const oldBuffer = core.memory.buffer;
core.memory.grow(1);
assert.equal(oldBuffer.byteLength, 0);
assert.deepEqual(mesh(), reference.subarray(28));
console.log(`Native/WASM parity: 3 noise samples, ${count / 8} ordered quads, memory growth`);

const initial = core.mesh_revision();
const cameraBefore = Buffer.from(new Uint8Array(core.memory.buffer, core.camera_ptr(), 64));
assert.equal(core.step(1000 / 60, 1 << 3, 0, 0, 0), 0);
assert.equal(core.mesh_revision(), initial);
const cameraAfter = Buffer.from(new Uint8Array(core.memory.buffer, core.camera_ptr(), 64));
assert.notDeepEqual(cameraAfter, cameraBefore);

const move = (held, frames = 1) => {
  for (let i = 0; i < frames; i++) assert.equal(core.step(62.5, held, 0, 0, 0), 0);
};
assert.equal(core.step(1000 / 60, 1 << 1, 0, 0, 0), 0);
const place = 1 << 7;
const remove = 1 << 6;
const edit = (pressed) => assert.equal(core.step(0, 0, pressed, 0, 0), 1);
edit(place);
assert.equal(core.mesh_revision(), initial + 1);
assert.notDeepEqual(mesh(), reference.subarray(28));
edit(remove);
assert.deepEqual(mesh(), reference.subarray(28));

const baseline = core.mesh_heap_high_water();
const memory = core.memory.buffer.byteLength;
for (let i = 0; i < 10_000; i++) {
  const held = i % 2 === 0 ? 1 << 3 : 1 << 1;
  const pressed = i % 100 === 0 ? (i % 200 === 0 ? place : remove) : 0;
  assert.equal(core.step(1000 / 60, held, pressed, 0, 0), pressed ? 1 : 0);
}
assert.equal(core.mesh_revision(), initial + 102);
assert.equal(core.mesh_heap_high_water(), baseline);
assert.equal(core.memory.buffer.byteLength, memory);
assert.deepEqual(mesh(), reference.subarray(28));
console.log(`10,000 steps with 100 edits: peak scratch ${baseline} bytes, linear memory ${memory} bytes`);

// Fly a serpentine path over all 32×32 columns and remove one voxel per
// column. A 62.5 ms strafe advances half a voxel. A 90° yaw turns the strafe
// axis to Z for a row change, then the yaw returns for the next row.
move(1 << 3, 32);
core.step(0, 0, 0, Math.PI / (2 * 0.003), 0);
move(1 << 1, 32);
core.step(0, 0, 0, -Math.PI / (2 * 0.003), 0);
for (let z = 0; z < 32; z++) {
  for (let x = 0; x < 32; x++) {
    edit(remove);
    if (x < 31) move(z % 2 === 0 ? 1 << 1 : 1 << 3, 2);
  }
  if (z < 31) {
    core.step(0, 0, 0, Math.PI / (2 * 0.003), 0);
    move(1 << 3, 2);
    core.step(0, 0, 0, -Math.PI / (2 * 0.003), 0);
  }
}
assert.equal(core.mesh_bytes(), 0);
assert.equal(core.mesh_revision(), initial + 1126);
assert.equal(core.step(0, 0, remove, 0, 0), 0);
assert.equal(core.mesh_revision(), initial + 1126);
console.log('Removing the final voxel publishes an empty mesh');

const streamed = new WebAssembly.Instance(module).exports;
assert.equal(streamed.init(12345, 1), 0);
assert.equal(streamed.chunk_capacity(), 9);

const chunk = (x, z) => {
  for (let i = 0; i < streamed.chunk_capacity(); i++) {
    if (streamed.chunk_revision(i) && streamed.chunk_x(i) === x && streamed.chunk_z(i) === z) {
      return Buffer.from(new Uint8Array(streamed.memory.buffer, streamed.chunk_ptr(i), streamed.chunk_bytes(i)));
    }
  }
};

for (let i = 0; i < 8; i++) assert.equal(streamed.stream_one(), 1);
assert.equal(streamed.stream_one(), 0);
assert.deepEqual(chunk(0, 0), reference.subarray(28));
assert.notDeepEqual(chunk(-1, 0), chunk(0, 0));
assert.equal(streamed.step(0, 0, remove, 0, 0), 1);
const editedOrigin = chunk(0, 0);
assert.notDeepEqual(editedOrigin, reference.subarray(28));

const timings = [];
let replacements = 0;
function fly(held, frames) {
  for (let frame = 0; frame < frames; frame++) {
    streamed.step(100, held, 0, 0, 0);
    const start = performance.now();
    const loaded = streamed.stream_one();
    if (loaded) {
      timings.push(performance.now() - start);
      replacements++;
    }
  }
}
function roundTrip() {
  fly(1 << 1, 400);
  assert.equal(chunk(0, 0), undefined);
  assert.ok(chunk(10, 0)?.length);
  fly(1 << 3, 800);
  assert.ok(chunk(-10, 0)?.length);
  fly(1 << 1, 400);
  assert.deepEqual(chunk(0, 0), editedOrigin);

  streamed.step(0, 0, 0, Math.PI / (2 * 0.003), 0);
  fly(1 << 3, 400);
  assert.ok(chunk(0, 10)?.length);
  fly(1 << 1, 800);
  assert.ok(chunk(0, -10)?.length);
  fly(1 << 3, 400);
  streamed.step(0, 0, 0, -Math.PI / (2 * 0.003), 0);
  assert.deepEqual(chunk(0, 0), editedOrigin);
}
roundTrip();
const steadyPages = streamed.memory.buffer.byteLength;
const steadyScratch = streamed.mesh_heap_high_water();
for (let trip = 0; trip < 3; trip++) {
  roundTrip();
  assert.equal(streamed.memory.buffer.byteLength, steadyPages);
  assert.equal(streamed.mesh_heap_high_water(), steadyScratch);
}
timings.sort((a, b) => a - b);
console.log(`Streaming: 4 round trips, ${replacements} loads/reloads, ${steadyPages} bytes WASM, ${steadyScratch} bytes scratch`);
console.log(`Node chunk generation and meshing: p95 ${timings[Math.ceil(timings.length * 0.95) - 1].toFixed(2)} ms, max ${timings.at(-1).toFixed(2)} ms`);

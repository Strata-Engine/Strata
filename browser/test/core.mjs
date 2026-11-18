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
assert.equal(streamed.chunk_capacity(), 27);

const chunk = (x, z, y = 0) => {
  for (let i = 0; i < streamed.chunk_capacity(); i++) {
    if (streamed.chunk_revision(i) && streamed.chunk_x(i) === x && streamed.chunk_y(i) === y && streamed.chunk_z(i) === z) {
      return Buffer.from(new Uint8Array(streamed.memory.buffer, streamed.chunk_ptr(i), streamed.chunk_bytes(i)));
    }
  }
};

for (let i = 0; i < 26; i++) assert.equal(streamed.stream_one(), 1);
function settle() {
  for (let jobs = 0; jobs < 54; jobs++) if (!streamed.stream_one()) return;
  assert.fail('Neighbor work did not settle');
}
settle();
assert.notDeepEqual(chunk(0, 0), reference.subarray(28));
// At z=12 the seed has touching voxels at y=3 on both sides.
function seamFace(mesh, x, face) {
  for (let offset = 0; offset < mesh.length; offset += 8) {
    const word = mesh.readUInt32LE(offset);
    const y = (word >>> 5) & 31, z = (word >>> 10) & 31;
    if ((word & 31) === x && ((word >>> 25) & 7) === face &&
        y <= 3 && y + ((word >>> 20) & 31) >= 3 &&
        z <= 12 && z + ((word >>> 15) & 31) >= 12) return true;
  }
  return false;
}
const turn = Math.PI / (2 * 0.003);
assert.equal(seamFace(chunk(1, 0), 0, 3), false, 'Touching blocks hide the seam face');
assert.equal(seamFace(chunk(0, 0), 31, 2), false);
for (let i = 0; i < 40; i++) streamed.step(62.5, 1 << 3, 0, 0, 0);
settle();
assert.equal(chunk(1, 0), undefined);
assert.equal(seamFace(chunk(0, 0), 31, 2), true, 'Eviction exposes the resident face');
for (let i = 0; i < 40; i++) streamed.step(62.5, 1 << 1, 0, 0, 0);
settle();
assert.equal(seamFace(chunk(0, 0), 31, 2), false, 'Reload hides the shared face again');
streamed.step(0, 0, 0, turn, 0);
for (let i = 0; i < 8; i++) streamed.step(62.5, 1 << 1, 0, 0, 0);
streamed.step(0, 0, 0, -turn, 0);
for (let i = 0; i < 30; i++) streamed.step(62.5, 1 << 1, 0, 0, 0);
assert.equal(streamed.step(0, 0, remove, 0, 0), 1);
// Crossing into the east chunk also creates missing terrain jobs.
for (let i = 0; i < 2; i++) streamed.step(62.5, 1 << 1, 0, 0, 0);
assert.equal(streamed.stream_one(), 1);
assert.equal(seamFace(chunk(1, 0), 0, 3), true, 'Edit refresh runs before missing terrain jobs');
assert.equal(streamed.step(0, 0, remove, 0, 0), 1, 'Edit the eastern side too');
for (let i = 0; i < 2; i++) streamed.step(62.5, 1 << 3, 0, 0, 0);
settle();
for (let i = 0; i < 30; i++) streamed.step(62.5, 1 << 3, 0, 0, 0);
streamed.step(0, 0, 0, turn, 0);
for (let i = 0; i < 8; i++) streamed.step(62.5, 1 << 3, 0, 0, 0);
streamed.step(0, 0, 0, -turn, 0);
assert.notDeepEqual(chunk(-1, 0), chunk(0, 0));
assert.equal(streamed.step(0, 0, remove, 0, 0), 1);
settle();
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
  settle();
  assert.deepEqual(chunk(0, 0), editedOrigin);

  streamed.step(0, 0, 0, Math.PI / (2 * 0.003), 0);
  fly(1 << 3, 400);
  assert.ok(chunk(0, 10)?.length);
  fly(1 << 1, 800);
  assert.ok(chunk(0, -10)?.length);
  fly(1 << 3, 400);
  streamed.step(0, 0, 0, -Math.PI / (2 * 0.003), 0);
  settle();
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
console.log(`Streaming: 4 round trips, ${replacements} mesh jobs, ${steadyPages} bytes WASM, ${steadyScratch} bytes scratch`);
console.log(`Node chunk work: p95 ${timings[Math.ceil(timings.length * 0.95) - 1].toFixed(2)} ms, max ${timings.at(-1).toFixed(2)} ms`);

const restored = new WebAssembly.Instance(module).exports;
assert.equal(restored.restore_edit(0, 0, -(18 * 32768), 0), 0);
const journal = new Int32Array(streamed.memory.buffer, streamed.edit_words(), streamed.edit_count() * 4);
for (let i = 0; i < journal.length; i += 4)
  assert.equal(restored.restore_edit(...journal.subarray(i, i + 4)), 1);
assert.equal(restored.init(12345, 1), 0);
for (let jobs = 0; jobs < 54 && restored.stream_one(); jobs++) {}
for (const x of [0, 1]) {
  const slots = Array.from({ length: restored.chunk_capacity() }, (_, i) => i);
  const slot = slots.find(i => restored.chunk_x(i) === x && restored.chunk_y(i) === 0 && restored.chunk_z(i) === 0);
  assert.deepEqual(Buffer.from(new Uint8Array(restored.memory.buffer, restored.chunk_ptr(slot),
    restored.chunk_bytes(slot))), chunk(x, 0));
}
console.log('Restored journal reproduces meshes on both sides of the seam');

const vertical = new WebAssembly.Instance(module).exports;
const column = 16 + 32 * 16;
assert.equal(vertical.restore_edit(0, 0, column + 31 * 1024, 1), 1);
assert.equal(vertical.restore_edit(0, 0, column + 32768, 1), 1);
assert.equal(vertical.init(12345, 1), 0);
function settleVertical() {
  for (let jobs = 0; jobs < 54; jobs++) if (!vertical.stream_one()) return;
  assert.fail('Vertical streaming did not settle');
}
function lowerTopFace() {
  const data = new DataView(vertical.memory.buffer, vertical.mesh_ptr(), vertical.mesh_bytes());
  for (let offset = 0; offset < data.byteLength; offset += 8) {
    const word = data.getUint32(offset, true);
    if ((word & 32767) === (16 | (31 << 5) | (16 << 10)) && ((word >>> 25) & 7) === 0) return true;
  }
  return false;
}
assert.equal(lowerTopFace(), true);
settleVertical();
assert.equal(lowerTopFace(), false, 'Upper voxel hides the lower top face');
for (let i = 0; i < 40; i++) vertical.step(100, 1 << 4, 0, 0, 0);
settleVertical();
assert.equal(vertical.step(0, 0, remove, 0, 0), 1);
assert.equal(vertical.stream_one(), 1);
assert.equal(lowerTopFace(), true, 'Removing the upper voxel exposes the lower face');
const verticalMemory = vertical.memory.buffer.byteLength;
for (let trip = 0; trip < 3; trip++) {
  for (const held of [1 << 4, 1 << 5]) {
    for (let i = 0; i < 120; i++) {
      vertical.step(100, held, 0, 0, 0);
      vertical.stream_one();
    }
    settleVertical();
  }
  assert.equal(vertical.memory.buffer.byteLength, verticalMemory);
}
console.log('Vertical seam edit and repeated ascent/descent passed');

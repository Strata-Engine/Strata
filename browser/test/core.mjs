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

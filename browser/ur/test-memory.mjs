import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const [modulePath = 'browser/.build/ur/core.wasm', referencePath] = process.argv.slice(2);
async function load(path) {
  const { instance } = await WebAssembly.instantiate(readFileSync(path),
    { math: { sin: Math.sin, cos: Math.cos, atan2: Math.atan2 } });
  return instance.exports;
}

const core = await load(modulePath);
const reference = referencePath && await load(referencePath);
function snapshot(c) {
  return Buffer.concat([
    Buffer.from(new Uint8Array(c.memory.buffer, c.ur_info(), 12)),
    Buffer.from(new Uint8Array(c.memory.buffer, c.ur_parcels(), 2640 * 12)),
  ]);
}
function step(action, parcel, compare) {
  assert.equal(core.ur_step(action, parcel), 1);
  if (compare && reference) {
    assert.equal(reference.ur_step(action, parcel), 1);
    assert.deepEqual(snapshot(core), snapshot(reference));
  }
}

for (const [action, parcel] of [[-1, 0], [1, 462], [1, 463], [2, 464], [3, 402]])
  step(action, parcel, true);
const startingBytes = core.memory.buffer.byteLength;
let warmBytes;
let settledBytes;
for (let month = 1; month <= 300; month++) {
  step(0, 0, month <= 90);
  assert.equal(new Int32Array(core.memory.buffer, core.ur_info(), 3)[1], month);
  if (month === 120) warmBytes = core.memory.buffer.byteLength;
  if (month === 240) settledBytes = core.memory.buffer.byteLength;
}
assert.ok(core.memory.buffer.byteLength <= warmBytes + 1024 * 1024, 'Memory grew more than 1 MiB after month 120');
assert.equal(core.memory.buffer.byteLength, settledBytes, 'Memory grew after month 240');
console.log({ startingBytes, warmBytes, finalBytes: core.memory.buffer.byteLength, liveBytes: core.ur_live_bytes() });

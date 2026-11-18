// Run against a local Chrome started with remote debugging, with an open test tab.
import assert from 'node:assert/strict';
import { setTimeout as delay } from 'node:timers/promises';

const [debugUrl = 'http://127.0.0.1:9229', previewUrl = 'http://127.0.0.1:8765/preview.html'] =
  process.argv.slice(2);
const tabs = await (await fetch(new URL('/json/list', debugUrl))).json();
const target = tabs.find(tab => tab.type === 'page');
assert.ok(target, 'Chrome needs an open test tab');
const socket = new WebSocket(target.webSocketDebuggerUrl);
await new Promise((resolve, reject) => {
  socket.onopen = resolve;
  socket.onerror = reject;
});
let id = 0;
const pending = new Map();
socket.onmessage = ({ data }) => {
  const reply = JSON.parse(data);
  const complete = pending.get(reply.id);
  if (complete) {
    complete(reply);
    pending.delete(reply.id);
  }
};
async function call(method, params = {}) {
  const reply = await new Promise(resolve => {
    pending.set(++id, resolve);
    socket.send(JSON.stringify({ id, method, params }));
  });
  assert.equal(reply.error, undefined, JSON.stringify(reply.error));
  return reply.result;
}
async function evaluate(expression) {
  const reply = await call('Runtime.evaluate', { expression, returnByValue: true });
  assert.equal(reply.exceptionDetails, undefined, JSON.stringify(reply.exceptionDetails));
  return reply.result.value;
}
async function until(expression) {
  for (let attempts = 0; attempts < 100; attempts++) {
    if (await evaluate(expression)) return;
    await delay(50);
  }
  throw new Error('Timed out: ' + expression);
}

try {
  await call('Page.enable');
  await call('Emulation.setDeviceMetricsOverride', {
    width: 1040, height: 755, deviceScaleFactor: 1, mobile: false,
  });
  await call('Page.addScriptToEvaluateOnNewDocument', { source: `
    globalThis.flight = { times: [], intervals: [], loadTimes: [], loads: 0, active: false };
    const raf = requestAnimationFrame;
    let previous;
    globalThis.requestAnimationFrame = (callback) => raf((time) => {
      const start = performance.now();
      callback(time);
      if (flight.active) {
        flight.times.push(performance.now() - start);
        if (previous !== undefined) flight.intervals.push(time - previous);
        previous = time;
      }
    });
    const instantiate = WebAssembly.instantiateStreaming;
    WebAssembly.instantiateStreaming = async (...args) => {
      const { instance } = await instantiate(...args);
      const core = instance.exports;
      flight.core = core;
      return { instance: { exports: { ...core, stream_one() {
        const start = performance.now();
        const loaded = core.stream_one();
        if (flight.active && loaded) {
          flight.loadTimes.push(performance.now() - start);
          flight.loads++;
        }
        return loaded;
      } } } };
    };
  ` });
  await call('Page.navigate', { url: previewUrl });
  await until('document.documentElement?.dataset.rendered === "true"');
  await call('Page.bringToFront');
  const [x, y] = await evaluate(`(() => {
    const r = document.querySelector('#play').getBoundingClientRect();
    return [r.x + r.width / 2, r.y + r.height / 2];
  })()`);
  for (const type of ['mousePressed', 'mouseReleased'])
    await call('Input.dispatchMouseEvent', { type, x, y, button: 'left', clickCount: 1 });
  await until('document.pointerLockElement === document.querySelector("canvas")');
  await until('flight.core.mesh_revision() >= 9');
  await delay(250); // Let queued seam refreshes finish before measuring the flight.
  const memory = await evaluate('flight.core.memory.buffer.byteLength');

  async function fly(key, milliseconds) {
    const code = 'Key' + key.toUpperCase();
    await call('Input.dispatchKeyEvent', { type: 'keyDown', code, key });
    await delay(milliseconds);
    await call('Input.dispatchKeyEvent', { type: 'keyUp', code, key });
  }

  await evaluate('flight.active = true');
  await fly('a', 4000);
  await fly('d', 8000);
  await fly('a', 4000);
  const result = await evaluate(`(() => {
    flight.active = false;
    const summarize = values => {
      const sorted = values.slice().sort((a, b) => a - b);
      return { p95: sorted[Math.ceil(sorted.length * 0.95) - 1], max: sorted.at(-1) };
    };
    return {
      frames: flight.times.length, meshJobs: flight.loads,
      cpuFrameMs: summarize(flight.times), frameIntervalMs: summarize(flight.intervals),
      chunkWorkMs: summarize(flight.loadTimes), wasmBytes: flight.core.memory.buffer.byteLength,
      scratchBytes: flight.core.mesh_heap_high_water()
    };
  })()`);
  assert.ok(result.frames > 300, 'Expected an active animation loop');
  assert.ok(result.meshJobs >= 6, 'Expected multiple chunk boundaries to be crossed');
  assert.equal(result.wasmBytes, memory, 'Linear memory grew after cache warm-up');
  console.log(JSON.stringify(result, null, 2));
} finally {
  await evaluate('document.exitPointerLock()').catch(() => {});
  socket.close();
}

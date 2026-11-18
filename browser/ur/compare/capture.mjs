import assert from 'node:assert/strict';
import { writeFileSync } from 'node:fs';
import { setTimeout as delay } from 'node:timers/promises';

const [variant = 'day', endpoint = 'http://127.0.0.1:9229', page = 'http://127.0.0.1:8765/ur/'] = process.argv.slice(2);
assert.ok(['day', 'night'].includes(variant));
const preview = new URL(page);
const liveTiming = preview.searchParams.get('gpuTiming') === 'live';
if (liveTiming) preview.searchParams.set('traffic', '');
else preview.searchParams.set('compare', variant);
const url = new URL('/json/new?about:blank', endpoint);
const tab = await (await fetch(url, { method: 'PUT' })).json();
const socket = new WebSocket(tab.webSocketDebuggerUrl);
await new Promise((resolve, reject) => {
  socket.onopen = resolve;
  socket.onerror = reject;
});
const pending = new Map();
const errors = [];
let id = 0;
socket.onmessage = ({ data }) => {
  const event = JSON.parse(data);
  if (event.method === 'Runtime.exceptionThrown') errors.push(event.params.exceptionDetails);
  if (event.method === 'Runtime.consoleAPICalled' && event.params.type === 'error') {
    errors.push(event.params.args.map(arg => arg.description ?? arg.value));
  }
  pending.get(event.id)?.(event);
  pending.delete(event.id);
};
async function send(method, params = {}) {
  const response = await new Promise(resolve => {
    pending.set(++id, resolve);
    socket.send(JSON.stringify({ id, method, params }));
  });
  assert.equal(response.error, undefined, JSON.stringify(response.error));
  return response.result;
}

async function status() {
  const { result } = await send('Runtime.evaluate', {
    expression: '({ ready: document.documentElement?.dataset.sceneReady, status: document.querySelector("#status")?.textContent })',
    returnByValue: true,
  });
  return result.value;
}

try {
  await send('Page.enable');
  await send('Runtime.enable');
  await send('Network.enable');
  await send('Network.setCacheDisabled', { cacheDisabled: true });
  await send('Emulation.setDeviceMetricsOverride', { width: 900, height: 660, deviceScaleFactor: 1, mobile: false });
  const timing = preview.searchParams.has('gpuTiming');
  if (timing) {
    const { readFileSync } = await import('node:fs');
    const source = readFileSync(new URL('./gpu-timing.js', import.meta.url), 'utf8');
    await send('Page.addScriptToEvaluateOnNewDocument', { source });
  }
  await send('Page.navigate', { url: preview.href });
  let scene;
  for (let attempt = 0; attempt < 200; attempt++) {
    scene = await status();
    if (scene?.ready || errors.length) break;
    await delay(75);
  }
  assert.deepEqual(errors, []);
  assert.equal(scene?.ready, 'true', scene?.status);
  if (timing) {
    const measure = await send('Runtime.evaluate', {
      expression: `window.urGpuTiming(${liveTiming})`, awaitPromise: true, returnByValue: true,
    });
    assert.equal(measure.exceptionDetails, undefined, JSON.stringify(measure.exceptionDetails));
    assert.deepEqual(errors, []);
    console.log(preview.searchParams.get('sun') ?? 'voxel', measure.result.value);
  }
  if (!timing) {
    const shot = await send('Page.captureScreenshot', { format: 'png' });
    writeFileSync(new URL(`../../.build/ur-comparison/${preview.searchParams.get('sun') === 'raster' ? 'raster' : 'webgpu'}-${variant}.png`, import.meta.url), Buffer.from(shot.data, 'base64'));
    console.log('Captured the fixed WebGPU scene at 900 × 660');
  }
} finally {
  socket.close();
  await fetch(new URL(`/json/close/${tab.id}`, endpoint));
}

// Injected by the comparison capture. The normal preview requests no timing
// features or buffers. Measure the full set of render passes per submission.
(() => {
  const requestDevice = GPUAdapter.prototype.requestDevice;
  const beginPass = GPUCommandEncoder.prototype.beginRenderPass;
  const finish = GPUCommandEncoder.prototype.finish;
  const createEncoder = GPUDevice.prototype.createCommandEncoder;
  const encoders = new WeakMap();
  let device, querySet, resolveBuffer, readBuffer, sample, submitted;
  GPUAdapter.prototype.requestDevice = async function (descriptor = {}) {
    if (!this.features.has('timestamp-query')) throw new Error('Timestamp queries are unavailable.');
    device = await requestDevice.call(this, { ...descriptor,
      requiredFeatures: [...(descriptor.requiredFeatures ?? []), 'timestamp-query'] });
    querySet = device.createQuerySet({ type: 'timestamp', count: 2 });
    resolveBuffer = device.createBuffer({ size: 16, usage: GPUBufferUsage.QUERY_RESOLVE | GPUBufferUsage.COPY_SRC });
    readBuffer = device.createBuffer({ size: 16, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
    return device;
  };
  GPUDevice.prototype.createCommandEncoder = function (...args) {
    const encoder = createEncoder.apply(this, args);
    if (sample && this === device) {
      encoders.set(encoder, { first: true });
      sample = false;
    }
    return encoder;
  };
  GPUCommandEncoder.prototype.beginRenderPass = function (descriptor) {
    const timing = encoders.get(this);
    if (!timing) return beginPass.call(this, descriptor);
    const timestampWrites = { querySet };
    if (timing.first) timestampWrites.beginningOfPassWriteIndex = 0;
    if (descriptor.colorAttachments.length) timestampWrites.endOfPassWriteIndex = 1;
    timing.first = false;
    return beginPass.call(this, { ...descriptor, timestampWrites });
  };
  GPUCommandEncoder.prototype.finish = function (...args) {
    if (encoders.has(this)) {
      this.resolveQuerySet(querySet, 0, 2, resolveBuffer, 0);
      this.copyBufferToBuffer(resolveBuffer, 0, readBuffer, 0, 16);
      submitted();
    }
    return finish.apply(this, args);
  };
  window.urGpuTiming = async live => {
    const canvas = document.querySelector('canvas');
    const timings = [], gaps = [];
    let startTime;
    let measuring = true;
    function cadence(time) {
      if (!measuring) return;
      if (startTime !== undefined) gaps.push(time - startTime);
      startTime = time;
      requestAnimationFrame(cadence);
    }
    requestAnimationFrame(cadence);
    canvas.focus();
    for (let i = 0; i < 100; i++) {
      const next = new Promise(resolve => { submitted = resolve; });
      sample = true;
      if (!live) {
        canvas.dispatchEvent(new KeyboardEvent('keydown', {
          bubbles: true, key: i % 2 ? 'ArrowLeft' : 'ArrowRight',
        }));
        if (sample) throw new Error('The preview did not submit a frame.');
      }
      await next;
      await readBuffer.mapAsync(GPUMapMode.READ);
      const [start, end] = new BigUint64Array(readBuffer.getMappedRange());
      readBuffer.unmap();
      if (i >= 10) timings.push(Number(end - start) / 1e6);
    }
    measuring = false;
    const quantile = (values, q) => values.sort((a, b) => a - b)[Math.ceil(values.length * q) - 1];
    return { frames: timings.length, medianMs: quantile(timings, 0.5), p95Ms: quantile(timings, 0.95),
      rafP95Ms: quantile(gaps, 0.95) };
  };
})();

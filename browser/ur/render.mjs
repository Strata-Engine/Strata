import { desktopRenderer } from './desktop-render.mjs?v=4';
import { cameraProjection } from './camera.mjs';

// Draw one instance per parcel, with the same row order used for input picking.
export async function makeRenderer(canvas, width, depth, rasterSun = false) {
  const adapter = await navigator.gpu?.requestAdapter();
  if (!adapter) throw new Error('WebGPU is unavailable.');
  const device = await adapter.requestDevice();
  device.addEventListener('uncapturederror', event => console.error(event.error.message));
  const context = canvas.getContext('webgpu');
  const format = navigator.gpu.getPreferredCanvasFormat();
  context.configure({ device, format });
  const parcelCount = width * depth;
  const shader = device.createShaderModule({ code: `
    @group(0) @binding(0) var<storage, read> colors: array<vec4f>;
    struct Vertex {
      @builtin(position) position: vec4f,
      @location(0) color: vec4f,
    }
    @vertex fn vertex(@builtin(vertex_index) v: u32, @builtin(instance_index) i: u32) -> Vertex {
      let cell = vec2f(f32(i % ${width}u), f32(i / ${width}u));
      let corner = array(0u, 1u, 2u, 2u, 1u, 3u)[v];
      let c = vec2f(f32(corner & 1u), f32(corner >> 1u));
      let uv = (cell + c * 0.98) / vec2f(${width}.0, ${depth}.0);
      return Vertex(vec4f(uv * vec2f(2, -2) + vec2f(-1, 1), 0, 1), colors[i]);
    }
    @fragment fn fragment(v: Vertex) -> @location(0) vec4f { return v.color; }
  ` });
  const pipeline = await device.createRenderPipelineAsync({
    layout: 'auto',
    vertex: { module: shader, entryPoint: 'vertex' },
    fragment: { module: shader, entryPoint: 'fragment', targets: [{ format }] },
    primitive: { topology: 'triangle-list' },
    depthStencil: { format: 'depth24plus', depthWriteEnabled: true, depthCompare: 'less' },
  });
  const colors = new Float32Array(parcelCount * 4);
  const buffer = device.createBuffer({
    size: colors.byteLength, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST,
  });
  const view = new Float32Array(36);
  const viewBuffer = device.createBuffer({ size: view.byteLength, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
  const depthTexture = device.createTexture({
    size: [canvas.width, canvas.height], format: 'depth24plus', usage: GPUTextureUsage.RENDER_ATTACHMENT,
  });
  const bind = device.createBindGroup({
    layout: pipeline.getBindGroupLayout(0), entries: [{ binding: 0, resource: { buffer } }],
  });
  const desktop = await desktopRenderer(device, format, viewBuffer, width, depth, rasterSun, canvas);

  let frame = 0;
  let priorCamera;

  function render(parcels, harbour, hover, landing, isRelief, camera) {
    if (!isRelief) {
      for (let i = 0; i < width * depth; i++) {
        const height = parcels[i * 3], road = parcels[i * 3 + 1], zone = parcels[i * 3 + 2];
        const shade = Math.min(height / 70, 0.32);
        let red = 0.29 + shade, green = 0.41 + shade, blue = 0.29 + shade * 0.5;
        if (i === harbour) { red = 0.94; green = 0.65; blue = 0.30; }
        else if (road) { red = 0.77; green = 0.70; blue = 0.53; }
        else if (zone === 1) { red = 0.43; green = 0.71; blue = 0.43; }
        else if (zone === 2) { red = 0.82; green = 0.57; blue = 0.35; }
        else if (!height) { red = 0.10; green = 0.24; blue = 0.30; }
        else if (harbour < 0 && landing(parcels, i)) { red = 0.35; green = 0.75; blue = 0.71; }
        const highlight = i === hover ? 0.22 : 0;
        const offset = i * 4;
        colors[offset] = red + highlight;
        colors[offset + 1] = green + highlight;
        colors[offset + 2] = blue + highlight;
        colors[offset + 3] = 1;
      }
      device.queue.writeBuffer(buffer, 0, colors);
    }
    view.set([width, depth, 0, frame++]);
    view.copyWithin(20, 8, 20);
    view.set(cameraProjection(camera, canvas.width / canvas.height), 4);
    const sameCamera = priorCamera && camera.cx === priorCamera.cx && camera.cz === priorCamera.cz &&
      camera.yaw === priorCamera.yaw && camera.zoom === priorCamera.zoom;
    view.set(desktop.historyState(!!sameCamera), 32);
    priorCamera = { ...camera };
    device.queue.writeBuffer(viewBuffer, 0, view);
    const encoder = device.createCommandEncoder();
    if (isRelief && rasterSun) desktop.drawShadows(encoder);
    const attachments = [{
      view: context.getCurrentTexture().createView(),
      clearValue: { r: 0.075, g: 0.29, b: 0.37, a: 1 }, loadOp: 'clear', storeOp: 'store',
    }];
    if (isRelief && !rasterSun) attachments.push(desktop.historyAttachment());
    const pass = encoder.beginRenderPass({ colorAttachments: attachments, depthStencilAttachment: {
      view: depthTexture.createView(), depthClearValue: 1, depthLoadOp: 'clear', depthStoreOp: 'discard',
    } });
    if (isRelief) desktop.draw(pass);
    else {
      pass.setPipeline(pipeline);
      pass.setBindGroup(0, bind);
      pass.draw(6, parcelCount);
    }
    pass.end();
    device.queue.submit([encoder.finish()]);
  }
  return Object.assign(render, { desktop });
}

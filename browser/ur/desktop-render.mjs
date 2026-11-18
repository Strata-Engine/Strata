import { parcelRay } from './camera.mjs';
import { buildDeckGrid } from './sun-volumes.mjs';

// Desktop Ur emits two Strata quad words and a chunk origin in quarter metres.
const wordsPerQuad = 8;
const atlasSide = 16;
const atlasLayers = 32;
const mipLevels = 1 + Math.log2(atlasSide);
const maxQuads = 1 << 20;

// Match Vulkan's linear-filtered sRGB blit when building each atlas level.
const linearByte = Float64Array.from({ length: 256 }, (_, byte) => {
  const c = byte / 255;
  return c <= 0.04045 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4;
});

function downsampleAtlas(bytes, side) {
  const smaller = side / 2;
  const result = new Uint8Array(smaller * smaller * atlasLayers * 4);
  for (let layer = 0; layer < atlasLayers; layer++) {
    for (let y = 0; y < smaller; y++) {
      for (let x = 0; x < smaller; x++) {
        const input = (layer * side * side + y * 2 * side + x * 2) * 4;
        const output = (layer * smaller * smaller + y * smaller + x) * 4;
        for (let channel = 0; channel < 4; channel++) {
          const a = bytes[input + channel], b = bytes[input + 4 + channel];
          const c = bytes[input + side * 4 + channel], d = bytes[input + (side + 1) * 4 + channel];
          if (channel === 3) {
            result[output + channel] = Math.round((a + b + c + d) * 0.25);
            continue;
          }
          const average = (linearByte[a] + linearByte[b] + linearByte[c] + linearByte[d]) * 0.25;
          const encoded = average <= 0.0031308 ? 12.92 * average : 1.055 * average ** (1 / 2.4) - 0.055;
          result[output + channel] = Math.round(encoded * 255);
        }
      }
    }
  }
  return result;
}

export async function desktopRenderer(device, format, viewBuffer, width, depth, rasterSun, canvas) {
  const atlas = device.createTexture({
    size: [atlasSide, atlasSide, atlasLayers], format: 'rgba8unorm-srgb', mipLevelCount: mipLevels,
    usage: GPUTextureUsage.COPY_DST | GPUTextureUsage.TEXTURE_BINDING,
  });
  const repeat = { addressModeU: 'repeat', addressModeV: 'repeat' };
  const nearest = device.createSampler(repeat);
  const filtered = device.createSampler({
    ...repeat, magFilter: 'linear', minFilter: 'linear', mipmapFilter: 'linear', maxAnisotropy: 8,
  });
  const atlasView = atlas.createView({ dimension: '2d-array' });
  const shadowSampler = rasterSun ? device.createSampler({ compare: 'less-equal', magFilter: 'linear', minFilter: 'linear' }) : null;
  const shadow = rasterSun ? device.createTexture({
    size: [2048, 2048], format: 'depth32float',
    usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.TEXTURE_BINDING,
  }).createView() : null;
  const response = await fetch(new URL('./desktop.wgsl', import.meta.url), { cache: 'no-cache' });
  if (!response.ok) throw new Error('Could not load the Ur WebGPU shader.');
  const shader = device.createShaderModule({ code: await response.text() });
  const shadowPipeline = rasterSun ? await device.createRenderPipelineAsync({
    layout: 'auto', vertex: { module: shader, entryPoint: 'shadowVertex' },
    depthStencil: { format: 'depth32float', depthWriteEnabled: true, depthCompare: 'less', depthBias: 2, depthBiasSlopeScale: 2 },
  }) : null;
  const historyFormat = 'r32uint';
  const history = rasterSun ? [] : Array.from({ length: 2 }, () => device.createTexture({
    size: [canvas.width, canvas.height], format: historyFormat,
    usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.TEXTURE_BINDING,
  }).createView());
  const targets = rasterSun ? [{ format }] : [{ format }, { format: historyFormat }];
  const backgroundTargets = rasterSun ? targets : [{ format }, { format: historyFormat, writeMask: 0 }];
  const pipeline = await device.createRenderPipelineAsync({
    layout: 'auto', primitive: { topology: 'triangle-list', cullMode: 'back' },
    vertex: { module: shader, entryPoint: 'vertex' },
    fragment: { module: shader, entryPoint: rasterSun ? 'rasterFragment' : 'fragment', targets },
    depthStencil: { format: 'depth24plus', depthWriteEnabled: true, depthCompare: 'less' },
  });
  const seaPipeline = await device.createRenderPipelineAsync({
    layout: 'auto', vertex: { module: shader, entryPoint: 'seaVertex' },
    fragment: { module: shader, entryPoint: 'seaFragment', targets: backgroundTargets },
    depthStencil: { format: 'depth24plus', depthWriteEnabled: true, depthCompare: 'less' },
  });
  const skyPipeline = await device.createRenderPipelineAsync({
    layout: 'auto', vertex: { module: shader, entryPoint: 'skyVertex' },
    fragment: { module: shader, entryPoint: 'skyFragment', targets: backgroundTargets },
    depthStencil: { format: 'depth24plus', depthWriteEnabled: false, depthCompare: 'less-equal' },
  });
  const makeBuffer = (size, usage) => device.createBuffer({ size, usage: usage | GPUBufferUsage.COPY_DST });
  const buffer = makeBuffer(maxQuads * wordsPerQuad * 4, GPUBufferUsage.STORAGE);
  const movingBuffer = makeBuffer((1 << 16) * wordsPerQuad * 4, GPUBufferUsage.STORAGE);
  const poses = makeBuffer(17001 * 8 * 4, GPUBufferUsage.STORAGE);
  const light = makeBuffer(12 * 4, GPUBufferUsage.UNIFORM);
  const lamps = makeBuffer(128 * 8 * 4, GPUBufferUsage.STORAGE);
  const lampCells = makeBuffer(width * depth * 8, GPUBufferUsage.STORAGE);
  const lampList = makeBuffer((1 << 18) * 4 * 4, GPUBufferUsage.STORAGE);
  const volumeWords = 1092;
  const deckWords = 132;
  const volumeBuffer = rasterSun ? null : makeBuffer(width * depth * 4 * volumeWords * 4, GPUBufferUsage.STORAGE);
  const deckBuffer = rasterSun ? null : makeBuffer(width * depth * (8 + 64 + deckWords) * 4, GPUBufferUsage.STORAGE);
  const emission = makeBuffer(32 * 4 * 4, GPUBufferUsage.STORAGE);
  const bufferAt = (binding, buffer) => ({ binding, resource: { buffer } });
  const bind = (pipeline, entries) => device.createBindGroup({ layout: pipeline.getBindGroupLayout(0), entries });
  const tiles = [{ binding: 2, resource: atlasView }, { binding: 3, resource: nearest },
    { binding: 12, resource: filtered }];
  const sea = bind(seaPipeline, [bufferAt(1, viewBuffer), bufferAt(5, light), ...tiles]);
  const sky = bind(skyPipeline, [bufferAt(1, viewBuffer), bufferAt(5, light)]);
  function bindings(quads) {
    const geometry = [bufferAt(0, quads), bufferAt(1, viewBuffer), bufferAt(4, poses), bufferAt(5, light)];
    return {
      shadow: rasterSun ? bind(shadowPipeline, geometry) : null,
      color: (rasterSun ? [null] : history).map(previous => {
        const color = [bufferAt(0, quads), bufferAt(1, viewBuffer), bufferAt(4, poses), bufferAt(5, light),
          ...tiles, bufferAt(8, lamps), bufferAt(9, emission), bufferAt(10, lampCells), bufferAt(11, lampList)];
        if (rasterSun) {
          color.push({ binding: 6, resource: shadow }, { binding: 7, resource: shadowSampler });
        } else {
          color.push(bufferAt(13, volumeBuffer), bufferAt(14, deckBuffer), { binding: 15, resource: previous });
        }
        return bind(pipeline, color);
      }),
    };
  }
  const stationary = bindings(buffer), moving = bindings(movingBuffer);
  let movingCount = 0, sunlit = false, shadowDirty = true;
  let count = 0;
  let historyIndex = 0, historyValid = false, movingSun = false;
  let lastSun;
  const parcels = new Map();
  let staticPoses;
  return {
    configureSun(volumeStride, deckStride) {
      if (volumeStride !== volumeWords || deckStride !== deckWords) throw new Error('Incompatible Ur sunlight volume layout.');
    },
    sunVolumes(batches, decks) {
      for (const [first, words] of batches) {
        device.queue.writeBuffer(volumeBuffer, first * 4 * volumeWords * 4, words);
      }
      if (decks) {
        const grid = buildDeckGrid(decks, staticPoses, width, depth);
        if (grid.byteLength > deckBuffer.size) throw new Error('Ur deck shadows exceed the browser volume capacity.');
        device.queue.writeBuffer(deckBuffer, 0, grid);
      }
    },
    textures(bytes, palette) {
      for (let level = 0, side = atlasSide; level < mipLevels; level++, side /= 2) {
        device.queue.writeTexture({ texture: atlas, mipLevel: level }, bytes,
          { bytesPerRow: side * 4, rowsPerImage: side }, [side, side, atlasLayers]);
        if (side > 1) bytes = downsampleAtlas(bytes, side);
      }
      device.queue.writeBuffer(emission, 0, palette);
    },
    poses(values, first = 0) {
      device.queue.writeBuffer(poses, first * 8 * 4, values);
      if (first === 0) staticPoses = values.slice();
      shadowDirty = true;
    },
    staticLights(batches) {
      const cells = Array.from({ length: width * depth }, () => []);
      for (const batch of batches) {
        for (let i = 0; i < batch.length; i += 4) {
          const x = batch[i], z = batch[i + 2];
          // A desktop lamp reaches four metres and each parcel is eight metres wide.
          const minX = Math.max(0, Math.floor((x - 4) / 8));
          const minZ = Math.max(0, Math.floor((z - 4) / 8));
          for (let cz = minZ; cz < depth && cz <= Math.floor((z + 4) / 8); cz++) {
            for (let cx = minX; cx < width && cx <= Math.floor((x + 4) / 8); cx++) {
              cells[cx + cz * width].push(batch.subarray(i, i + 4));
            }
          }
        }
      }
      const ranges = new Uint32Array(cells.length * 2);
      let count = 0;
      for (const [index, cell] of cells.entries()) {
        ranges[index * 2] = count;
        ranges[index * 2 + 1] = cell.length;
        count += cell.length;
      }
      if (count * 16 > lampList.size) throw new Error('Ur street lighting exceeds the browser lamp capacity.');
      const positions = new Float32Array(count * 4);
      let offset = 0;
      for (const cell of cells) {
        for (const lamp of cell) {
          positions.set(lamp, offset);
          offset += 4;
        }
      }
      device.queue.writeBuffer(lampCells, 0, ranges);
      if (count) device.queue.writeBuffer(lampList, 0, positions);
    },
    frame(words, lighting, headlights) {
      movingCount = words.length / wordsPerQuad;
      if (movingCount && words.byteLength <= movingBuffer.size) device.queue.writeBuffer(movingBuffer, 0, words);
      else if (movingCount) throw new Error('Ur traffic exceeds the browser mesh capacity.');
      device.queue.writeBuffer(light, 0, lighting);
      if (headlights.length) device.queue.writeBuffer(lamps, 0, headlights);
      const sun = lighting.slice(0, 3);
      if (lastSun) {
        const delta = Math.hypot(...sun.map((value, i) => value - lastSun[i]));
        movingSun = delta > 0.001;
        if (delta > 0.05) historyValid = false;
      }
      lastSun = sun;
      sunlit = lighting[4] > 0 && lighting[1] > 0;
      shadowDirty = true;
    },
    drawShadows(encoder) {
      if (!shadowDirty) return;
      shadowDirty = false;
      const pass = encoder.beginRenderPass({ colorAttachments: [], depthStencilAttachment: {
        view: shadow, depthClearValue: 1, depthLoadOp: 'clear', depthStoreOp: 'store',
      } });
      if (sunlit) {
        pass.setPipeline(shadowPipeline);
        pass.setBindGroup(0, stationary.shadow);
        pass.draw(6, count);
        pass.setBindGroup(0, moving.shadow);
        pass.draw(6, movingCount);
      }
      pass.end();
    },
    replace(batches, landingLots) {
      const wordCount = batches.reduce((total, words) => total + words.length, 0);
      if (wordCount / wordsPerQuad > maxQuads) throw new Error('Ur scene exceeds the browser mesh capacity.');
      const instances = new Uint32Array(wordCount);
      let offset = 0;
      parcels.clear();
      for (const words of batches) {
        // Keep each parcel's range for geometry-based mouse picking.
        let start = 0;
        while (start < words.length) {
          const lot = words[start + 4] / 32 + width * words[start + 6] / 32;
          let end = start + wordsPerQuad;
          while (end < words.length && words[end + 4] / 32 + width * words[end + 6] / 32 === lot) end += wordsPerQuad;
          if (landingLots) {
            for (let i = start; i < end; i += wordsPerQuad) words[i + 2] = landingLots[lot];
          }
          parcels.set(lot, words.subarray(start, end));
          start = end;
        }
        instances.set(words, offset);
        offset += words.length;
      }
      if (wordCount) device.queue.writeBuffer(buffer, 0, instances);
      count = wordCount / wordsPerQuad;
      historyValid = false;
      shadowDirty = true;
    },
    historyState(sameCamera) { return [Number(historyValid), movingSun ? 4 : 64, Number(sameCamera), 0]; },
    historyAttachment() {
      return { view: history[1 - historyIndex], loadOp: 'clear', storeOp: 'store', clearValue: [0, 0, 0, 0] };
    },
    draw(pass) {
      pass.setPipeline(pipeline);
      pass.setBindGroup(0, stationary.color[historyIndex]);
      pass.draw(6, count);
      pass.setBindGroup(0, moving.color[historyIndex]);
      pass.draw(6, movingCount);
      pass.setPipeline(seaPipeline);
      pass.setBindGroup(0, sea);
      pass.draw(6);
      pass.setPipeline(skyPipeline);
      pass.setBindGroup(0, sky);
      pass.draw(3);
      if (!rasterSun) {
        historyIndex ^= 1;
        historyValid = true;
      }
    },
    pick(u, v, camera, aspect) { return pickScene(parcels, width, depth, u, v, camera, staticPoses, aspect); },
  };
}

export function pickScene(parcels, width, depth, u, v, camera, staticPoses, aspect) {
  const [ox, oz, dx, dz] = parcelRay(camera, u, v, aspect);
  let best = -1, height = -1;
  const within = (value, low, span) => value >= low - 1e-6 && value <= low + span + 1e-6;
  const minZ = Math.max(0, Math.floor(Math.min(oz, oz + 4 * dz)) - 1);
  const maxZ = Math.min(depth - 1, Math.floor(Math.max(oz, oz + 4 * dz)) + 1);
  const minX = Math.max(0, Math.floor(Math.min(ox, ox + 4 * dx)) - 1);
  const maxX = Math.min(width - 1, Math.floor(Math.max(ox, ox + 4 * dx)) + 1);
  for (let z = minZ; z <= maxZ; z++) {
    for (let x = minX; x <= maxX; x++) {
      const lot = z * width + x;
      const words = parcels.get(lot);
      if (!words) continue;
      let lastPose = -1;
      let origin, direction;
      for (let i = 0; i < words.length; i += wordsPerQuad) {
        const pose = words[i + 3];
        if (pose !== lastPose) {
          if (pose && !staticPoses) continue;
          [origin, direction] = pose ? localRay(staticPoses, pose * 8, [ox, 0, oz], [dx, 1, dz])
            : [[ox - x, 0, oz - z], [dx, 1, dz]];
          lastPose = pose;
        }
        const word = words[i], face = (word >>> 25) & 7;
        const [rx, ry, rz] = origin, [vx, vy, vz] = direction;
        const qx = (word & 31) / 32, qz = ((word >>> 10) & 31) / 32;
        const qy = ((pose ? 0 : words[i + 5]) + ((word >>> 5) & 31)) / 32;
        const w = (1 + ((word >>> 15) & 31)) / 32;
        const h = (1 + ((word >>> 20) & 31)) / 32;
        const drop = ((word >>> 28) & 7) / 8;
        let y, hit;
        if (face < 2 && Math.abs(vy) > 1e-9) {
          y = (qy + (face === 0 ? (1 - drop) / 32 : 0) - ry) / vy;
          hit = within(rx + vx * y, qx, w) && within(rz + vz * y, qz, h);
        } else if (face >= 2 && face < 4 && Math.abs(vx) > 1e-9) {
          y = (qx + (face === 2 ? 1 / 32 : 0) - rx) / vx;
          hit = within(ry + vy * y, qy, h) && within(rz + vz * y, qz, w);
        } else if (face >= 4 && Math.abs(vz) > 1e-9) {
          y = (qz + (face === 4 ? 1 / 32 : 0) - rz) / vz;
          hit = within(ry + vy * y, qy, h) && within(rx + vx * y, qx, w);
        }
        if (hit && y >= 0 && y > height) { height = y; best = lot; }
      }
    }
  }
  return best;
}

function localRay(poses, offset, origin, direction) {
  const [px, py, pz, scale, qx, qy, qz, qw] = poses.subarray(offset, offset + 8);
  function inverseRotate([x, y, z]) {
    const tx = 2 * (qz * y - qy * z);
    const ty = 2 * (qx * z - qz * x);
    const tz = 2 * (qy * x - qx * y);
    return [(x + qw * tx + qz * ty - qy * tz) * 0.25 / scale,
      (y + qw * ty + qx * tz - qz * tx) * 0.25 / scale,
      (z + qw * tz + qy * tx - qx * ty) * 0.25 / scale];
  }
  return [inverseRotate([origin[0] - px / 8, origin[1] - py / 8, origin[2] - pz / 8]), inverseRotate(direction)];
}

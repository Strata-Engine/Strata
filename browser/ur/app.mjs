import { makeRenderer } from './render.mjs?v=4';
import { regionCamera, trafficCamera } from './camera.mjs';

const width = 60, depth = 44;
const canvas = document.querySelector('canvas');
const status = document.querySelector('#status');
const city = document.querySelector('#city');
const parcel = document.querySelector('#parcel');
const monthButton = document.querySelector('#month');
const tools = [...document.querySelectorAll('[data-tool]')];
const viewButton = document.querySelector('#view');
let tool = 1;
let overpassStart = -1;
let relief = true;
let initialCamera = regionCamera;
const camera = { ...initialCamera };

try {
  const params = new URLSearchParams(location.search);
  const comparison = params.has('compare');
  if (comparison) document.body.classList.add('comparison');
  const { instance } = await WebAssembly.instantiateStreaming(fetch('../.build/ur/core.wasm', { cache: 'no-cache' }),
    { math: { sin: Math.sin, cos: Math.cos, atan2: Math.atan2 } });
  const core = instance.exports;
  const rasterSun = params.get('sun') === 'raster';
  const render = await makeRenderer(canvas, width, depth, rasterSun);
  const demo = comparison || params.has('traffic') ? 'traffic' :
    params.has('showcase') ? 'starter' : null;
  const startAction = { traffic: -3, starter: -2 }[demo] ?? -1;
  if (!core.ur_step(startAction, 0)) throw new Error('Ur could not start.');
  render.desktop.textures(new Uint8Array(core.memory.buffer, core.ur_scene_textures(), 32 * 1024),
    new Float32Array(core.memory.buffer, core.ur_scene_emission_ptr(), 32 * 4));
  // Desktop advances one street hour per real second at 1× speed.
  const day = 1.8;
  const clockRate = day / 24;
  let clock = comparison && params.get('compare') === 'night' ? 20 * clockRate :
    demo === 'starter' ? 9 * clockRate : core.ur_initial_clock() / 1000;
  let motionTime = 0;
  let meshPhase = -1, meshTick = 0;
  let meshBatches, lampBatches;
  const hour = phase => Math.floor(phase * 24 / day);
  const signalTick = () => Math.floor(motionTime * 2);
  const night = h => h >= 18 || h < 6;
  const poseCount = core.ur_scene_pose_count();
  const dynamicFirst = core.ur_scene_dynamic_first();
  function uploadPoses(first, end) {
    const offset = core.ur_scene_poses() + first * 8 * 4;
    render.desktop.poses(new Float32Array(core.memory.buffer, offset, (end - first) * 8), first);
  }
  const volumeWords = core.ur_scene_volume_words();
  const deckWords = core.ur_scene_deck_words();
  render.desktop.configureSun(volumeWords, deckWords);
  let sceneVersion = 0, sceneUpdating = false;
  function rebuildScene(relight = false) {
    const version = ++sceneVersion;
    const phase = clock, tick = signalTick();
    const batchSize = core.ur_scene_batch_size();
    const batchCount = Math.ceil(width * depth / batchSize);
    const pending = relight ? meshBatches.slice() : new Array(batchCount);
    const lights = relight ? lampBatches.slice() : new Array(batchCount);
    const volumes = [];
    const decks = [];
    let jobs = Array.from({ length: batchCount }, (_, batch) => batch);
    if (relight) {
      const changes = core.ur_scene_changes(meshPhase, phase, meshTick, tick);
      const affected = new Uint8Array(core.memory.buffer, changes, batchCount);
      jobs = jobs.filter(batch => affected[batch]);
      if (!jobs.length) {
        meshPhase = phase;
        meshTick = tick;
        return;
      }
    }
    let next = 0;
    const { stats, parcels } = snapshot();
    const landingLots = stats[0] < 0 ?
      Uint8Array.from({ length: width * depth }, (_, lot) => Number(landing(parcels, lot))) : null;
    function loadFrame() {
      if (version !== sceneVersion) return;
      try {
        const start = performance.now();
        do {
          const batch = jobs[next++];
          const count = core.ur_scene_batch(batch * batchSize, phase, tick);
          const memory = core.memory.buffer;
          pending[batch] = new Uint32Array(memory, core.ur_scene_ptr(), count * 8).slice();
          const lamps = core.ur_scene_static_count();
          if (!rasterSun) {
            const first = batch * batchSize;
            volumes.push([first, new Uint32Array(memory, core.ur_scene_volume_ptr(), batchSize * 4 * volumeWords).slice()]);
            if (!relight) {
              const shapes = new Uint32Array(memory, core.ur_scene_decks_ptr(), batchSize * deckWords);
              for (let offset = 0; offset < shapes.length; offset += deckWords) {
                if (shapes[offset]) decks.push(shapes.slice(offset, offset + deckWords));
              }
            }
          }
          lights[batch] = new Float32Array(memory, core.ur_scene_static_ptr(), lamps * 4).slice();
        } while (next < jobs.length && performance.now() - start < 6);
        if (next < jobs.length) requestAnimationFrame(loadFrame);
        else {
          uploadPoses(0, dynamicFirst);
          if (!rasterSun) render.desktop.sunVolumes(volumes, relight ? null : decks);
          render.desktop.staticLights(lights);
          render.desktop.replace(pending, landingLots);
          meshBatches = pending;
          lampBatches = lights;
          meshPhase = phase;
          meshTick = tick;
          sceneUpdating = false;
          if (relief) draw(hovered);
          if (comparison && !rasterSun) {
            let remaining = 63;
            function settleSun() {
              draw();
              if (--remaining > 0) requestAnimationFrame(settleSun);
              else document.documentElement.dataset.sceneReady = 'true';
            }
            requestAnimationFrame(settleSun);
          } else document.documentElement.dataset.sceneReady = 'true';
        }
      } catch (error) {
        sceneUpdating = false;
        status.textContent = error.message;
        console.error(error);
      }
    }
    sceneUpdating = true;
    if (!relight) delete document.documentElement.dataset.sceneReady;
    requestAnimationFrame(loadFrame);
  }
  if (demo) {
    initialCamera = demo === 'traffic' ? trafficCamera : { ...regionCamera, cx: 76, cz: 156, zoom: 42 };
    Object.assign(camera, initialCamera);
  }

  function snapshot() {
    const memory = core.memory.buffer;
    return {
      stats: new Int32Array(memory, core.ur_info(), 3),
      parcels: new Int32Array(memory, core.ur_parcels(), width * depth * 3),
    };
  }

  function landing(parcels, index) {
    const height = parcels[index * 3];
    if (height <= 0 || height > 18) return false;
    const x = index % width, z = Math.floor(index / width);
    return (x > 0 && parcels[(index - 1) * 3] === 0) ||
      (x < width - 1 && parcels[(index + 1) * 3] === 0) ||
      (z > 0 && parcels[(index - width) * 3] === 0) ||
      (z < depth - 1 && parcels[(index + width) * 3] === 0);
  }

  let lastCaption = '';
  function updateCaption(month, cash) {
    const streetHour = String(hour(clock)).padStart(2, '0');
    const caption = `Month ${month} · $${cash.toLocaleString()} · ${streetHour}:00`;
    if (caption !== lastCaption) city.textContent = lastCaption = caption;
  }

  function draw(hover = -1) {
    const { stats, parcels } = snapshot();
    const [harbour, month, cash] = stats;
    updateCaption(month, cash);
    monthButton.disabled = harbour < 0;
    for (const button of tools) button.disabled = harbour < 0;
    render(parcels, harbour, hover, landing, relief, camera);
  }

  let lastFrame, animationFailed = false;
  function animate(now) {
    if (animationFailed) return;
    if (!comparison) requestAnimationFrame(animate);
    if (!relief || document.hidden) { lastFrame = undefined; return; }
    if (lastFrame !== undefined && now - lastFrame < 50) return;
    const dt = lastFrame === undefined ? 0 : Math.min((now - lastFrame) / 1000, 0.1);
    lastFrame = now;
    try {
      const founded = snapshot().stats[0] >= 0;
      const step = founded ? dt : 0;
      motionTime += step;
      clock = (clock + step * clockRate) % day;
      const count = core.ur_scene_frame(motionTime, step, clock);
      const memory = core.memory.buffer;
      const light = new Float32Array(memory, core.ur_scene_light(), 12);
      uploadPoses(dynamicFirst, poseCount);
      render.desktop.frame(new Uint32Array(memory, core.ur_scene_ptr(), count * 8), light,
        new Float32Array(memory, core.ur_scene_lamps(), light[10] * 8));
      const currentHour = hour(clock), meshHour = hour(meshPhase);
      const lightingChanged = currentHour !== meshHour && (night(currentHour) || night(meshHour));
      if ((lightingChanged || signalTick() !== meshTick) && !sceneUpdating && document.documentElement.dataset.sceneReady) {
        rebuildScene(true);
      }
      const { stats, parcels } = snapshot();
      updateCaption(stats[1], stats[2]);
      render(parcels, stats[0], hovered, landing, relief, camera);
    } catch (error) {
      animationFailed = true;
      status.textContent = error.message;
      console.error(error);
    }
  }

  function advance(action, index = 0) {
    try {
      const before = Array.from(snapshot().stats);
      if (!core.ur_step(action, index)) return;
      if (before[0] < 0 || action === 0) clock = core.ur_initial_clock() / 1000;
      rebuildScene();
      const { stats, parcels } = snapshot();
      if (before[0] < 0 && stats[0] >= 0) {
        initialCamera = { ...regionCamera, cx: stats[0] % width * 8 + 4,
          cz: Math.floor(stats[0] / width) * 8 + 4, zoom: 60 };
        Object.assign(camera, initialCamera);
      }
      if (action === 0) status.textContent = `Month ${stats[1]} settled.`;
      else if (before[0] < 0) status.textContent = stats[0] < 0
        ? 'Choose a highlighted shoreline parcel.'
        : 'Harbour founded. Select Road and click an inland parcel.';
      else status.textContent = parcels[index * 3 + (action === 1 ? 1 : 2)] === (action === 1 ? 1 : action - 1)
        ? 'Parcel updated.' : 'Ur rejected this placement.';
      draw(action === 0 ? hovered : index);
    } catch (error) {
      status.textContent = 'Simulation stopped. Reload to start a new city.';
      for (const button of [...tools, monthButton]) button.disabled = true;
      canvas.style.pointerEvents = 'none';
      console.error(error);
    }
  }

  function pick(event) {
    const rect = canvas.getBoundingClientRect();
    const u = (event.clientX - rect.left) / rect.width;
    const v = (event.clientY - rect.top) / rect.height;
    if (comparison) return -1;
    if (relief) return document.documentElement.dataset.sceneReady ? render.desktop.pick(u, v, camera, canvas.width / canvas.height) : -1;
    const x = Math.floor(u * width);
    const z = Math.floor(v * depth);
    return x >= 0 && x < width && z >= 0 && z < depth ? z * width + x : -1;
  }

  let hovered = -1;
  canvas.addEventListener('pointermove', event => {
    const index = pick(event);
    if (index === hovered) return;
    hovered = index;
    parcel.textContent = index < 0 ? '' : `Parcel ${index % width}, ${Math.floor(index / width)}`;
    draw(index);
  });
  canvas.addEventListener('pointerleave', () => {
    hovered = -1;
    parcel.textContent = '';
    draw();
  });
  canvas.addEventListener('click', event => {
    const index = pick(event);
    if (index < 0) return;
    if (tool !== 4) { advance(tool, index); return; }
    if (overpassStart < 0) {
      overpassStart = index;
      status.textContent = 'Choose the other street on the crossing.';
      return;
    }
    const built = core.ur_overpass(overpassStart, index);
    overpassStart = -1;
    status.textContent = built ? 'Overpass opened.' : 'Choose a clear, straight crossing between streets at similar elevations.';
    if (built) {
      rebuildScene();
      draw(index);
    }
  });
  viewButton.disabled = false;
  viewButton.addEventListener('click', () => {
    relief = !relief;
    viewButton.setAttribute('aria-pressed', String(relief));
    hovered = -1;
    parcel.textContent = '';
    draw();
  });
  monthButton.addEventListener('click', () => advance(0));
  function select(button) {
    if (!button || button.disabled) return;
    tool = Number(button.dataset.tool);
    overpassStart = -1;
    if (tool === 4) status.textContent = 'Select a street at one end of the overpass.';
    for (const option of tools) option.setAttribute('aria-pressed', String(option === button));
  }
  for (const button of tools) button.addEventListener('click', () => select(button));
  const cameraControls = document.querySelector('#camera-controls');
  const panKeys = {
    ArrowLeft: [-1, 0], ArrowRight: [1, 0], ArrowUp: [0, -1], ArrowDown: [0, 1],
  };
  function redrawCamera() {
    hovered = -1;
    parcel.textContent = '';
    draw();
  }
  function zoomBy(factor) {
    camera.zoom = Math.max(10, Math.min(380, camera.zoom / factor));
    redrawCamera();
  }
  canvas.addEventListener('wheel', event => {
    if (!relief || event.ctrlKey || event.metaKey) return;
    event.preventDefault();
    if (event.deltaY) zoomBy(event.deltaY < 0 ? 1.14 : 1 / 1.14);
  }, { passive: false });
  viewButton.addEventListener('click', () => { cameraControls.hidden = !relief; });
  document.addEventListener('keydown', event => {
    if (event.ctrlKey || event.metaKey || event.altKey) return;
    select(tools.find(button => button.dataset.tool === event.key));
    if (!relief || document.activeElement !== canvas) return;
    const move = panKeys[event.key];
    if (move) {
      const [right, forward] = move;
      const c = Math.cos(camera.yaw), s = Math.sin(camera.yaw);
      const distance = camera.zoom * 0.07;
      camera.cx = Math.max(0, Math.min(width * 8, camera.cx + distance * (right * c + forward * s)));
      camera.cz = Math.max(0, Math.min(depth * 8, camera.cz + distance * (-right * s + forward * c)));
    } else if (event.key === 'q' || event.key === 'e') {
      camera.yaw += (event.key === 'q' ? -1 : 1) * Math.PI / 12;
    } else if (event.key === 'Home') {
      Object.assign(camera, initialCamera);
    } else if (['+', '=', '-'].includes(event.key)) {
      event.preventDefault();
      zoomBy(event.key === '-' ? 1 / 1.14 : 1.14);
      return;
    } else return;
    event.preventDefault();
    redrawCamera();
  });
  status.textContent = demo ? 'Explore the city or select a planning tool.'
    : 'Click a highlighted shoreline parcel to found your harbour.';
  animate(performance.now());
  draw();
  document.documentElement.dataset.ready = 'true';
  rebuildScene();
} catch (error) {
  status.textContent = error.message;
  console.error(error);
}

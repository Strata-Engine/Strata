// Ur.Scene and Strata's desktop shaders supply the mesh format, sky and fog.
struct Quad { words: vec4u, origin: vec4i }
struct Pose { positionScale: vec4f, rotation: vec4f }
struct Lamp { position: vec4f, direction: vec4f }
struct View {
  map: vec4f, eye: vec4f,
  cameraX: vec4f, cameraY: vec4f, cameraZ: vec4f,
  priorX: vec4f, priorY: vec4f, priorZ: vec4f, history: vec4f,
}
struct Light { sun: vec4f, color: vec4f, fog: vec4f }
@group(0) @binding(0) var<storage, read> quads: array<Quad>;
@group(0) @binding(1) var<uniform> view: View;
@group(0) @binding(2) var atlas: texture_2d_array<f32>;
@group(0) @binding(3) var tileSampler: sampler;
@group(0) @binding(4) var<storage, read> poses: array<Pose>;
@group(0) @binding(5) var<uniform> light: Light;
@group(0) @binding(6) var shadowMap: texture_depth_2d;
@group(0) @binding(7) var shadowSampler: sampler_comparison;
@group(0) @binding(8) var<storage, read> lamps: array<Lamp>;
@group(0) @binding(9) var<storage, read> emission: array<vec4f>;
@group(0) @binding(10) var<storage, read> lampCells: array<vec2u>;
@group(0) @binding(11) var<storage, read> lampList: array<vec4f>;
@group(0) @binding(12) var filteredTiles: sampler;
@group(0) @binding(13) var<storage, read> solidVoxels: array<u32>;
@group(0) @binding(14) var<storage, read> deckVoxels: array<u32>;
@group(0) @binding(15) var priorSun: texture_2d<u32>;

struct Surface { world: vec3f, normal: vec3f, uv: vec2f, material: u32, landing: u32 }
fn rotate(q: vec4f, p: vec3f) -> vec3f {
  let t = 2 * cross(q.xyz, p);
  return p + q.w * t + cross(q.xyz, t);
}

fn surface(v: u32, quad: Quad) -> Surface {
  let word = quad.words.x;
  let face = (word >> 25u) & 7u;
  let reverse = face == 0u || face == 2u || face == 5u;
  let corner = select(array(0u, 1u, 2u, 2u, 1u, 3u)[v],
    array(0u, 2u, 1u, 2u, 3u, 1u)[v], reverse);
  let h = f32(1u + ((word >> 20u) & 31u));
  let u = f32((corner & 1u) * (1u + ((word >> 15u) & 31u)));
  let t = f32(corner >> 1u) * h;
  let bits = (quad.words.y >> 16u) & 255u;
  let dipBits = select(bits, bits >> (2u * corner), face == 0u);
  let dip = select(0, f32(dipBits & 3u) / 8, word >> 31u != 0u);
  let drop = f32((word >> 28u) & 7u) / 8 + dip;
  let up = select(0, t - drop, corner >= 2u);
  let xyz = vec3f(f32(word & 31u), f32((word >> 5u) & 31u), f32((word >> 10u) & 31u));
  var local: vec3f;
  switch face {
    case 0u: { local = xyz + vec3f(u, 1 - drop, t); }
    case 1u: { local = xyz + vec3f(u, 0, t); }
    case 2u: { local = xyz + vec3f(1, up, u); }
    case 3u: { local = xyz + vec3f(0, up, u); }
    case 4u: { local = xyz + vec3f(u, up, 1); }
    default: { local = xyz + vec3f(u, up, 0); }
  }
  var normal = array(vec3f(0, 1, 0), vec3f(0, -1, 0), vec3f(1, 0, 0),
    vec3f(-1, 0, 0), vec3f(0, 0, 1), vec3f(0, 0, -1))[face];
  var world = (local + vec3f(quad.origin.xyz)) * 0.25;
  if quad.words.w != 0u {
    let body = poses[quad.words.w];
    world = body.positionScale.xyz + rotate(body.rotation, local * body.positionScale.w);
    normal = rotate(body.rotation, normal);
  }
  return Surface(world, normal, vec2f(u, select(h - up, t, face < 2u)),
    quad.words.y & 65535u, select(0u, quad.words.z, face == 0u));
}

fn sunClip(world: vec3f) -> vec3f {
  let sun = light.sun.xyz;
  let right = normalize(cross(vec3f(0, 1, 0), sun));
  let up = cross(sun, right);
  let delta = world - vec3f(view.map.x * 4, 16, view.map.y * 4);
  return vec3f(dot(right, delta) / 320, dot(up, delta) / 320, 0.5 - dot(sun, delta) / 640);
}

@vertex fn shadowVertex(@builtin(vertex_index) v: u32, @builtin(instance_index) i: u32) -> @builtin(position) vec4f {
  return vec4f(sunClip(surface(v, quads[i]).world), 1);
}

struct Vertex {
  @builtin(position) position: vec4f,
  @location(0) uv: vec2f,
  @location(1) @interpolate(flat) material: u32,
  @location(2) normal: vec3f,
  @location(3) @interpolate(flat) landing: u32,
  @location(4) world: vec3f,
  @location(5) @interpolate(flat) surfaceId: u32,
}
fn project(position: vec3f) -> vec4f {
  let world = vec4f(position, 1);
  return vec4f(dot(view.cameraX, world), dot(view.cameraY, world), dot(view.cameraZ, world), 1);
}

@vertex fn vertex(@builtin(vertex_index) v: u32, @builtin(instance_index) i: u32) -> Vertex {
  let quad = quads[i];
  let s = surface(v, quad);
  let face = ((quad.words.x >> 25) & 7u) + 1;
  return Vertex(project(s.world), s.uv, s.material, s.normal, s.landing, s.world,
    select(face, 0u, quad.origin.w >= 10601));
}

struct SeaVertex {
  @builtin(position) position: vec4f,
  @location(0) world: vec3f,
}
@vertex fn seaVertex(@builtin(vertex_index) v: u32) -> SeaVertex {
  // Ur.Scene.sea spans 4 × 4 level-five chunks, with its top at y=0.
  let corner = array(0u, 1u, 2u, 2u, 1u, 3u)[v];
  let world = vec3f(f32(corner & 1u) * 1024 - 256, 0, f32(corner >> 1u) * 1024 - 256);
  return SeaVertex(project(world), world);
}

fn skyBase(d: vec3f) -> vec3f {
  let sun = light.sun.xyz;
  let day = smoothstep(-0.15, 0.25, sun.y);
  let low = 1 - smoothstep(0, 0.45, sun.y);
  var zenith = mix(vec3f(0.004, 0.006, 0.02), vec3f(0.10, 0.24, 0.62), day);
  var horizon = mix(vec3f(0.01, 0.015, 0.03), vec3f(0.46, 0.62, 0.84), day);
  horizon = mix(horizon, vec3f(0.62, 0.40, 0.26) * day, low * 0.55);
  zenith = mix(zenith, zenith * vec3f(0.55, 0.60, 0.85), low);
  let toward = max(dot(d, sun), 0);
  return mix(zenith, horizon, pow(1 - max(d.y, 0), 4)) +
    vec3f(1, 0.42, 0.12) * low * day * pow(toward, 5) * 0.7 +
    light.color.rgb * 0.04 * pow(toward, 96);
}

fn skyColor(d: vec3f) -> vec3f {
  return skyBase(d) + light.color.rgb * 12 * smoothstep(0.99975, 0.99990, dot(d, light.sun.xyz));
}

fn skyFlat(n: vec3f) -> vec3f {
  let above = 0.5 * (skyBase(vec3f(0, 1, 0)) + skyBase(normalize(vec3f(n.x + 0.0001, 0.25, n.z))));
  return above * (0.25 + 0.35 * (0.5 + 0.5 * n.y));
}

fn headlight(world: vec3f, n: vec3f) -> vec3f {
  var color = vec3f(0);
  for (var i = 0u; i < u32(light.fog.z); i++) {
    let lamp = lamps[i];
    // The desktop lamp attenuation uses quarter-metre voxels.
    let toLamp = (lamp.position.xyz - world) * 4;
    let distance2 = dot(toLamp, toLamp);
    let facing = dot(n, toLamp);
    if distance2 < 256 && facing > 0 {
      let invDistance = inverseSqrt(max(distance2, 0.25));
      let aim = vec3f(lamp.direction.x * 0.97, -0.24, lamp.direction.z * 0.97);
      let cone = smoothstep(0.70, 0.94, dot(aim, -toLamp) * invDistance);
      let falloff = (1 - smoothstep(64, 256, distance2)) * 140 / (distance2 + 16);
      color += emission[u32(lamp.direction.w)].rgb * facing * invDistance * falloff * cone;
    }
  }
  return color;
}

fn streetLight(world: vec3f, n: vec3f) -> vec3f {
  let lot = vec2i(floor(world.xz / 8));
  let size = vec2i(view.map.xy);
  if any(lot < vec2i(0)) || any(lot >= size) { return vec3f(0); }
  let range = lampCells[lot.x + lot.y * size.x];
  var color = vec3f(0);
  for (var i = range.x; i < range.x + range.y; i++) {
    let lamp = lampList[i];
    let toLamp = (lamp.xyz - world) * 4;
    let distance2 = dot(toLamp, toLamp);
    let facing = dot(n, toLamp);
    if distance2 < 256 && facing > 0 {
      let falloff = (1 - smoothstep(64, 256, distance2)) * 140 / (distance2 + 16);
      color += emission[u32(lamp.w)].rgb * facing * inverseSqrt(max(distance2, 0.25)) * falloff;
    }
  }
  return color;
}

fn noZero(direction: vec3f) -> vec3f {
  return select(direction, vec3f(1e-6), abs(direction) < vec3f(1e-6));
}

fn boxExit(origin: vec3f, inv: vec3f, low: vec3f, size: vec3f) -> f32 {
  let t = (select(low, low + size, inv >= vec3f(0)) - origin) * inv;
  return min(t.x, min(t.y, t.z));
}

// Walk the desktop deck bitfield in its own voxel coordinates. As on
// desktop, ignore the first contact when starting on the body's surface.
fn deckBlocks(base: u32, origin: vec3f, direction: vec3f) -> bool {
  let body = poses[deckVoxels[base]];
  let scale = body.positionScale.w;
  let inverse = vec4f(-body.rotation.xyz, body.rotation.w);
  let o = rotate(inverse, origin * 0.25 - body.positionScale.xyz) / scale;
  let d = noZero(rotate(inverse, direction));
  let inv = 1 / d;
  let dims = vec3i(i32(deckVoxels[base + 1]), i32(deckVoxels[base + 2]), i32(deckVoxels[base + 3]));
  let a = -o * inv;
  let b = (vec3f(dims) - o) * inv;
  let lo = min(a, b);
  let hi = max(a, b);
  var t = max(0, max(lo.x, max(lo.y, lo.z)));
  let end = min(96 / scale, min(hi.x, min(hi.y, hi.z)));
  if t >= end { return false; }
  var v = clamp(vec3i(floor(o + d * (t + 0.0001))), vec3i(0), dims - 1);
  let step = vec3i(sign(d));
  let jump = abs(inv);
  var next = (vec3f(v) + max(vec3f(step), vec3f(0)) - o) * inv;
  for (var i = 0; i < 72; i++) {
    if t > end || any(v < vec3i(0)) || any(v >= dims) { break; }
    let row = deckVoxels[base + 4 + u32(v.z * 4 + v.y)];
    if t > 0.05 && (row & (1u << u32(v.x))) != 0 { return true; }
    if next.x < next.y && next.x < next.z {
      v.x += step.x; t = next.x; next.x += jump.x;
    } else if next.y < next.z {
      v.y += step.y; t = next.y; next.y += jump.y;
    } else {
      v.z += step.z; t = next.z; next.z += jump.z;
    }
  }
  return false;
}

// Ur uses opaque light classes for terrain and buildings. Empty 32-voxel
// layers and 8³ blocks can be crossed without walking their voxels.
fn voxelBlocks(origin: vec3f, direction: vec3f) -> bool {
  let d = noZero(direction);
  let inv = 1 / d;
  let size = vec2i(view.map.xy);
  var t = 0.0;
  var lastCell = -1;
  var base = 0u;
  for (var step = 0; step < 160; step++) {
    if t >= 384 { break; }
    let p = origin + d * t;
    let voxel = vec3i(floor(p));
    let chunk = voxel >> vec3u(5);
    if any(chunk.xz < vec2i(0)) || any(chunk.xz >= size) || chunk.y < 0 || chunk.y >= 4 { break; }
    let cell = (chunk.z * size.x + chunk.x) * 4 + chunk.y;
    if cell != lastCell {
      lastCell = cell;
      base = u32(cell) * 1092;
      // Desktop light rays stop when leaving the uploaded chunk volume.
      if solidVoxels[base + 1] == 0 { break; }
      let start = deckVoxels[u32(cell) * 2];
      let length = deckVoxels[u32(cell) * 2 + 1];
      for (var i = 0u; i < length; i++) {
        if deckBlocks(deckVoxels[start + i], origin, d) { return true; }
      }
    }
    let local = voxel & vec3i(31);
    let top = i32(solidVoxels[base]);
    var edge = vec3f(voxel);
    var span = vec3f(1);
    if top == 0 || (d.y >= 0 && local.y >= top) {
      edge = vec3f(chunk * 32);
      span = vec3f(32);
    } else {
      let group = (local.y / 8) * 16 + (local.z / 8) * 4 + local.x / 8;
      let top8 = i32(solidVoxels[base + 4 + u32(group)]);
      if top8 == 0 || (d.y >= 0 && (local.y & 7) >= top8) {
        edge = vec3f(voxel & vec3i(~7));
        span = vec3f(8);
      } else {
        let column = solidVoxels[base + 68 + u32(local.z * 32 + local.x)];
        if (column & (1u << u32(local.y))) != 0 { return true; }
        if d.y > 0 {
          let higher = column & (0xffffffffu << u32(local.y));
          // In this column we can cross directly to the next solid voxel
          // or the chunk ceiling. The x/z exits still bound the step.
          let nextY = select(32, i32(firstTrailingBit(higher)), higher != 0);
          span.y = f32(nextY - local.y);
        }
      }
    }
    t = max(boxExit(origin, inv, edge, span), t) + 0.001;
  }
  return false;
}

fn rasterVisibility(world: vec3f, normal: vec3f) -> f32 {
  let sunAt = sunClip(world + normal * 0.12);
  let uv = sunAt.xy * vec2f(0.5, -0.5) + 0.5;
  return textureSampleCompareLevel(shadowMap, shadowSampler, uv, sunAt.z);
}

// Match the desktop's hash and angular radius for a point on the sun disc.
fn sunToward(pixel: vec2u, frame: u32) -> vec3f {
  var h = pixel.x * 1973u + pixel.y * 9277u + frame * 26699u;
  h = (h ^ (h >> 16)) * 0x7feb352du;
  h = (h ^ (h >> 15)) * 0x846ca68bu;
  h ^= h >> 16;
  var k = h * 0x9e3779b9u + 0x85ebca6bu;
  k = (k ^ (k >> 16)) * 0x7feb352du;
  k ^= k >> 15;
  let sun = light.sun.xyz;
  let basis = select(vec3f(1, 0, 0), vec3f(0, 1, 0), abs(sun.y) < 0.9);
  let a = normalize(cross(sun, basis));
  let b = cross(sun, a);
  let radius = sqrt(f32(h & 65535u) / 65536) * 0.02;
  let angle = f32(k & 65535u) / 65536 * 6.2831853;
  return normalize(sun + (a * cos(angle) + b * sin(angle)) * radius);
}

fn sunlight(v: Vertex, normal: vec3f) -> vec2f {
  if dot(normal, light.sun.xyz) <= 0 || light.color.r <= 0 { return vec2f(0); }
  let frame = u32(view.map.w);
  let pixel = vec2u(v.position.xy);
  var old = vec2f(0);
  if view.history.x > 0 && v.surfaceId != 0u {
    var texel = vec2i(pixel);
    var depth = v.position.z;
    if view.history.z == 0 {
      let world = vec4f(v.world, 1);
      let clip = vec2f(dot(view.priorX, world), dot(view.priorY, world));
      let uv = clip * vec2f(0.5, -0.5) + 0.5;
      texel = vec2i(floor(uv * vec2f(textureDimensions(priorSun))));
      depth = dot(view.priorZ, world);
    }
    if all(texel >= vec2i(0)) && all(texel < vec2i(textureDimensions(priorSun))) {
      let previous = textureLoad(priorSun, texel, 0).x;
      let previousDepth = f32((previous >> 17) & 4095u) / 4095;
      if abs(previousDepth - depth) < 0.00045 && (previous >> 29) == v.surfaceId {
        old = vec2f(f32(previous & 1023u) / 1023, f32((previous >> 10) & 127u));
      }
    }
  }
  let tile = pixel >> vec2u(3);
  let selected = ((tile.x & 1u) + 2u * (tile.y & 1u)) == (frame & 3u);
  if old.y > 0 && !selected { return old; }
  let origin = v.world * 4 + normal * 0.02;
  let visible = select(1.0, 0.0, voxelBlocks(origin, sunToward(pixel, frame)));
  let kept = min(old.y, view.history.y);
  return vec2f(mix(old.x, visible, 1 / (kept + 1)), kept + 1);
}

fn sunHighlight(n: vec3f, towardEye: vec3f, material: u32, visibility: f32) -> vec3f {
  if dot(n, light.sun.xyz) <= 0 || visibility <= 0 { return vec3f(0); }
  let roughness = select(select(245.0, 116.0, material == 9u || material == 19u), 65.0, material == 3u) / 255;
  let gloss = 1 - roughness;
  let h = normalize(light.sun.xyz + towardEye);
  let power = exp2(1 + 11 * gloss);
  return light.color.rgb * visibility * pow(max(dot(n, h), 0), power) * (0.25 + 6 * gloss * gloss * gloss) * gloss;
}

fn displayColor(c: vec3f) -> vec3f {
  let over = max(c - 0.6, vec3f(0));
  let mapped = min(c, vec3f(0.6)) + 0.4 * (1 - exp(-over / 0.4));
  return select(mapped * 12.92, 1.055 * pow(mapped, vec3f(1 / 2.4)) - 0.055, mapped > vec3f(0.0031308));
}

fn tile(uv: vec2f, material: u32) -> vec3f {
  let dx = dpdx(uv);
  let dy = dpdy(uv);
  let footprint = max(dot(dx, dx), dot(dy, dy)) * 256;
  // WebGPU requires all three filters to be linear for anisotropy. Retain
  // the desktop sampler's nearest magnification with a separate sampler.
  if footprint <= 1 {
    return textureSampleLevel(atlas, tileSampler, uv, i32(material), 0).rgb;
  }
  return textureSampleGrad(atlas, filteredTiles, uv, i32(material), dx, dy).rgb;
}

fn fogged(lit: vec3f, delta: vec3f) -> vec3f {
  let distance = length(delta);
  let fog = clamp((distance - light.fog.x) / max(light.fog.y - light.fog.x, 0.001), 0, 1);
  return mix(lit, skyBase(normalize(vec3f(delta.x, distance * 0.04, delta.z))), fog * fog);
}

struct SkyVertex {
  @builtin(position) position: vec4f,
  @location(0) clip: vec2f,
}
@vertex fn skyVertex(@builtin(vertex_index) v: u32) -> SkyVertex {
  let clip = array(vec2f(-1, -1), vec2f(3, -1), vec2f(-1, 3))[v];
  return SkyVertex(vec4f(clip, 1, 1), clip);
}

@fragment fn skyFragment(v: SkyVertex) -> @location(0) vec4f {
  let right = view.cameraX.xyz;
  let up = view.cameraY.xyz;
  let forward = view.cameraZ.xyz;
  let ray = right * v.clip.x / dot(right, right) + up * v.clip.y / dot(up, up) +
    forward / dot(forward, forward) + normalize(forward) * 0.1;
  return vec4f(displayColor(skyColor(normalize(ray))), 1);
}

@fragment fn seaFragment(v: SeaVertex) -> @location(0) vec4f {
  let n = vec3f(0, 1, 0);
  let direct = light.color.rgb * max(light.sun.y, 0);
  let lit = tile(v.world.xz * 4, 3u) * (direct + skyFlat(n));
  return vec4f(displayColor(fogged(lit, v.world - view.eye.xyz)), 1);
}

fn shade(v: Vertex, n: vec3f, visibility: f32) -> vec4f {
  let albedo = tile(v.uv, v.material);
  let direct = light.color.rgb * max(dot(n, light.sun.xyz), 0) * visibility;
  let delta = v.world - view.eye.xyz;
  let lit = albedo * (direct + skyFlat(n) + headlight(v.world, n) + streetLight(v.world, n) + emission[v.material].rgb * 5)
    + sunHighlight(n, normalize(-delta), v.material, visibility);
  let color = displayColor(fogged(lit, delta));
  return vec4f(mix(color, vec3f(0.35, 0.75, 0.71), f32(v.landing) * 0.62), 1);
}

@fragment fn rasterFragment(v: Vertex) -> @location(0) vec4f {
  let n = normalize(v.normal);
  return shade(v, n, rasterVisibility(v.world, n));
}

struct LitFragment {
  @location(0) color: vec4f,
  @location(1) history: u32,
}
@fragment fn fragment(v: Vertex) -> LitFragment {
  let n = normalize(v.normal);
  let history = sunlight(v, n);
  // 10 bits of visibility, 7 of sample count, 12 of depth and 3 of face ID.
  let packed = u32(round(history.x * 1023)) | (u32(history.y) << 10) |
    (u32(round(v.position.z * 4095)) << 17) | (v.surfaceId << 29);
  return LitFragment(shade(v, n, history.x), packed);
}

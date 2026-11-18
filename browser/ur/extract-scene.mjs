// Keep the browser's meshes and poses tied to the desktop Scene implementation.
import assert from 'node:assert/strict';
import { readFileSync, writeFileSync } from 'node:fs';

const [input, output] = process.argv.slice(2);
const scene = readFileSync(input, 'utf8');
function definition(name) {
  const start = scene.search(new RegExp(`^  p?fn ${name}\\(`, 'm'));
  if (start < 0) throw new Error(`Ur.Scene is missing ${name}`);
  const rest = scene.slice(start + 1);
  const end = rest.search(/^  (?:p?fn |type |--)/m);
  return rest.slice(0, end < 0 ? undefined : end).trimEnd();
}
const shared = ['empty', 'face', 'box', 'colour', 'pixels', 'heights', 'wall', 'surface', 'occupied', 'high_ground'];
const names = [...scene.matchAll(/^  p?fn (\w+)\(/gm)].map(match => match[1]);
const functions = new Set();
function include(name) {
  if (functions.has(name)) return;
  functions.add(name);
  const source = definition(name);
  for (const called of source.matchAll(/(?<![.\w])(\w+)\(/g)) {
    if (names.includes(called[1])) include(called[1]);
  }
}
for (const name of [...shared, 'overpass', 'lighting_on', 'sign_parts', 'daylight', 'vehicles',
  'fleet', 'food_vehicles', 'citizen_vehicles', 'yield_to_vehicles']) include(name);
const adapters = {
  set_fog: 'fog', set_sun: 'publish_sun', headlight: 'headlight',
  body_begin: 'body_begin', chunk_begin: 'chunk_begin', chunk_part: 'chunk_part',
  chunk_commit: 'chunk_commit', slot_voxel: 'voxel', slot_pose: 'pose',
  body_shape: 'body_shape', body_voxel: 'body_voxel', body_place: 'body_place',
  chunk_free: 'chunk_free', footprint: 'footprint', footprint_blocked: 'footprint_blocked',
};
const emissive = [...definition('palette').matchAll(
  /^(?: +)(\d+) -> Strata\.Blocks\.with_emission\(d, (\d+), (\d+), (\d+), (\d+)\)/gm)];
assert.ok(emissive.length, 'Ur.Scene has no emissive materials');
const adapted = names.filter(name => functions.has(name)).map(name =>
  definition(name).replace(/Vk\.(\w+)/g, (_, call) => {
    assert.ok(adapters[call], `Ur.Scene.${name} uses unsupported Vk.${call}`);
    return adapters[call];
  }));
writeFileSync(output, `mod Ur.BrowserScene do
  needs IO.Foreign
  alias Strata.Quad as Q
  alias Ur.Sim as S
  alias Ur.Landscape as L
  type Lighting = { hour : Int, electric : Ur.Sim.UtilityDelivery }
  type CarPose = { x : Float, y : Float, z : Float, dx : Float, dz : Float }
  type Mesh = { words : NativeI32Arr, quads : Int }
  extern "browser_scene" : Cap(IO.Foreign) do
    fn mesh(x : Int, y : Int, z : Int, words : NativeI32Arr, count : Int) : Unit = "ur_scene_mesh"
    fn emission(id : Int, r : Int, g : Int, b : Int, strength : Int) : Unit = "ur_scene_emission"
    fn texture(id : Int, bytes : NativeU8Arr) : Unit = "ur_scene_texture"
    fn begin(slot : Int, x : Int, y : Int, z : Int, posed : Int) : Unit = "ur_scene_begin"
    fn part(words : NativeI32Arr, count : Int) : Unit = "ur_scene_part"
    fn voxel(slot : Int, scale : Float) : Unit = "ur_scene_voxel"
    fn pose(slot : Int, x : Float, y : Float, z : Float, qx : Float, qy : Float, qz : Float, qw : Float) : Unit = "ur_scene_pose"
    fn fog(near : Float, far : Float) : Unit = "ur_scene_fog"
    fn publish_sun(x : Float, y : Float, z : Float, r : Float, g : Float, b : Float) : Unit = "ur_scene_sun"
    fn lamp(x : Float, y : Float, z : Float, dx : Float, dz : Float, material : Int) : Unit = "ur_scene_lamp"
    fn volume_begin(x : Int, y : Int, z : Int, hs : NativeIntArr) : Unit = "ur_scene_volume_begin"
    fn volume_box(x : Int, low : Int, z : Int, w : Int, high : Int, d : Int) : Unit = "ur_scene_volume_box"
    fn capture_deck(slot : Int, shape : NativeU8Arr, x : Int, y : Int, z : Int) : Unit = "ur_scene_deck"
    fn changed_lot(lot : Int) : Unit = "ur_scene_changed_lot"
    fn static_lamp(x : Int, y : Int, z : Int, material : Int) : Unit = "ur_scene_static_lamp"
    fn footprint(slot : Int, x : Float, y : Float, z : Float, dx : Float, dz : Float, length : Float, width : Float) : Unit = "ur_scene_footprint"
    fn vehicle_footprint_hit(x0 : Float, y0 : Float, z0 : Float, x1 : Float, y1 : Float, z1 : Float, ignore : Int) : Int = "ur_scene_blocked"
    fn night() : Int = "ur_scene_night"
    fn failed() : Unit = "ur_scene_failed"
  end
  fn chunk_begin(slot : Int, x : Int, y : Int, z : Int, level : Int) : Bool do
    begin(slot, x, y, z, 0)
    true
  end
  fn body_begin(slot : Int) : Bool do
    begin(slot, 0, 0, 0, 1)
    true
  end
  fn chunk_part(partition : Int, words : NativeI32Arr, count : Int) : Bool do
    part(words, count)
    true
  end
  fn chunk_commit() : Bool do true end
  fn body_shape(slot : Int, shape : NativeU8Arr, x : Int, y : Int, z : Int) : Bool do
    capture_deck(slot, shape, x, y, z)
    true
  end
  fn body_voxel(slot : Int, scale : Float) : Unit do () end
  fn body_place(slot : Int, x : Float, y : Float, z : Float, qx : Float, qy : Float, qz : Float, qw : Float) : Unit do () end
  fn footprint_blocked(x0 : Float, y0 : Float, z0 : Float, x1 : Float, y1 : Float, z1 : Float, ignore : Int) : Bool do
    vehicle_footprint_hit(x0, y0, z0, x1, y1, z1, ignore) == 1
  end
  fn chunk_free(slot : Int) : Unit do () end
  fn headlight(x : Float, y : Float, z : Float, dx : Float, dz : Float, material : Int) : Bool do
    if night() == 0 do false else
      lamp(x, y, z, dx, dz, material)
      true
    end
  end
${adapted.join('\n').replaceAll(/Ur\.AppError\.stop\("[^"]+"\)/g, 'failed()')}
  fn palette() : Unit do
${emissive.map(([, id, r, g, b, strength]) => `    emission(${id}, ${r}, ${g}, ${b}, ${strength})`).join('\n')}
  end

  fn textures(id : Int) : Unit do
    if id >= 32 do () else
      let (r, g, b) = colour(id)
      texture(id, pixels(NativeArray.make_u8(1024, 255), id, 0, r, g, b))
      textures(id + 1)
    end
  end

  fn add_pieces(m : Mesh, parts : List(Ur.Architecture.Piece), x : Int, y : Int, z : Int, window : Bool, shop : Bool) : Mesh do
    match parts do
      Nil -> m
      Cons(p, rest) ->
        let low = S.clamp(p.y - y, 0, 32)
        let high = S.clamp(p.y + p.h - y, 0, 32)
        let m = if low < high do
          volume_box(p.x, low, p.z, p.w, high, p.d)
          box(m, p.x, low, p.z, p.w, high - low, p.d, p.material)
        else m end
        let use_window = low < high && p.material == 26 && !window
        let use_shop = low < high && p.material == 27 && !shop
        if low < high && (use_window || use_shop || p.material == 28) do
          static_lamp(x + p.x + p.w / 2, y + low + (high - low) / 2, z + p.z + p.d / 2, p.material)
        else () end
        add_pieces(m, rest, x, y, z, window || use_window, shop || use_shop)
    end
  end

  fn layers(x : Int, z : Int, hs : NativeIntArr, peak : Int, parts : List(Ur.Architecture.Piece), layer : Int) : Unit do
    if layer >= 4 do () else
      let y = layer * 32
      if peak > y || occupied(parts, y) do
        volume_begin(x, y, z, hs)
        let m = add_pieces(surface(empty(), hs, x, z, y, 0), parts, x, y, z, false, false)
        mesh(x, y, z, m.words, m.quads)
      else () end
      layers(x, z, hs, peak, parts, layer + 1)
    end
  end

  fn relight(c : Ur.Sim.City, power : Ur.Sim.UtilityDelivery, controls : NativeIntArr, before : Int, after : Int, before_tick : Int, after_tick : Int, lot : Int) : Unit do
    if !S.valid(lot) do () else
      let old = lighting_on(c, lot, { hour: before, electric: power })
      let now = lighting_on(c, lot, { hour: after, electric: power })
      let signal_changed = before_tick != after_tick && S.get(controls, lot) == 2 &&
        (S.signal_green(lot, 0, before_tick) != S.signal_green(lot, 0, after_tick) ||
         S.signal_green(lot, 2, before_tick) != S.signal_green(lot, 2, after_tick))
      if old != now || signal_changed do changed_lot(lot) else () end
      relight(c, power, controls, before, after, before_tick, after_tick, lot + 1)
    end
  end

  fn batch(c : Ur.Sim.City, power : Ur.Sim.UtilityDelivery, controls : NativeIntArr, hour : Int, tick : Int, i : Int, remaining : Int) : Unit do
    if remaining <= 0 || !S.valid(i) do () else
      let x = i % S.width() * 32
      let z = i / S.width() * 32
      let hs = heights(c, x, z, NativeArray.make_int(100, 0), 0)
      let lighting = lighting_on(c, i, { hour: hour, electric: power })
      let parts = sign_parts(c, i, S.get(controls, i), tick, 0, L.structures(c, i, lighting))
      layers(x, z, hs, high_ground(hs, 0, 0), parts, 0)
      let source = S.grade_source(c, i)
      if S.grade_kind(c, source) == 1 && S.grade_link(c, source, S.grade_peer(c, source)) do overpass(c, i, x, z) else () end
      batch(c, power, controls, hour, tick, i + 1, remaining - 1)
    end
  end
end
`);

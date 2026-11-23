// The sky, and the light it and the sun put on a surface. Colours are
// linear. The sun is u.sun.xyz (the direction to it) and u.sun_color.

// ── Weather (sv_set_weather) ────────────────────────────────────────────
// A clear sky's colour col in direction d under cloud: an overcast sky is a
// flat grey, a little lighter overhead and toward the sun, as bright as the
// day, a cool tint at dusk; a lightning flash lights it from inside.
vec3 weather_sky(vec3 col, vec3 d, float day, float low) {
    float oc = u.weather.x, flash = u.weather.z;
    if (oc <= 0.0 && flash <= 0.0) return col;
    float up = max(d.y, 0.0);
    float toward = max(dot(d, u.sun.xyz), 0.0);
    vec3 grey = mix(vec3(0.006, 0.007, 0.010), vec3(0.26, 0.28, 0.31), day);
    grey = mix(grey, grey * vec3(0.92, 0.86, 0.84), low * day);              // a dull warm cast as the sun sets behind it
    grey *= 0.78 + 0.22 * up + 0.25 * day * pow(toward, 3.0);                 // a brighter patch where the sun is
    col = mix(col, grey, smoothstep(0.0, 1.0, oc));
    return col + vec3(0.62, 0.66, 0.85) * flash * (0.6 + 0.4 * up);
}

// The sky's colour in unit direction d, without the sun's disc.
vec3 sky_base(vec3 d) {
    vec3 s = u.sun.xyz;
    float day = smoothstep(-0.15, 0.25, s.y);
    float low = 1.0 - smoothstep(0.0, 0.45, s.y);              // 1 with the sun on the horizon
    vec3 zenith = mix(vec3(0.004, 0.006, 0.02), vec3(0.10, 0.24, 0.62), day);
    vec3 horizon = mix(vec3(0.01, 0.015, 0.03), vec3(0.46, 0.62, 0.84), day);
    horizon = mix(horizon, vec3(0.62, 0.40, 0.26) * day, low * 0.55);
    zenith = mix(zenith, zenith * vec3(0.55, 0.60, 0.85), low);
    float up = max(d.y, 0.0);
    vec3 col = mix(zenith, horizon, pow(1.0 - up, 4.0));
    float toward = max(dot(d, s), 0.0);
    float clear = 1.0 - u.weather.x;
    col += vec3(1.0, 0.42, 0.12) * low * day * pow(toward, 5.0) * 0.7 * clear;   // the glow round a low sun
    col += u.sun_color.rgb * 0.04 * pow(toward, 96.0) * clear;                   // and the haze close to it
    return weather_sky(col, d, day, low);
}

// The sky with the sun's disc; cloud hides the disc.
vec3 sky_color(vec3 d) {
    return sky_base(d) + u.sun_color.rgb * 12.0 * (1.0 - u.weather.x) * smoothstep(0.99975, 0.99990, dot(d, u.sun.xyz));
}

// What distance fades to: the sky at the horizon, the way the eye is looking.
vec3 sky_fog(vec3 d) { return sky_base(normalize(vec3(d.x, 0.04, d.z))); }

// Sky light on a face with normal n when nothing is known about what it can
// see: more from above, a little bounced from below.
vec3 sky_flat(vec3 n) {
    vec3 above = 0.5 * (sky_base(vec3(0.0, 1.0, 0.0)) + sky_base(normalize(vec3(n.x, 0.25, n.z) + vec3(1.0e-4, 0.0, 0.0))));
    return above * (0.25 + 0.35 * (0.5 + 0.5 * n.y));
}

// ── Under water ─────────────────────────────────────────────────────────
// Is the eye under liquid?
bool submerged() { return u.water.w > 0.0 && u.light.x >= 1.0 && u.light.x < 9.0; }

// How much daylight reaches height y (metres) under the surface the eye is
// under: red goes first, then green, so depth turns blue.
vec3 depth_light(float y) {
    return exp(-max(u.water2.x - y, 0.0) * vec3(0.28, 0.085, 0.045));
}

// The colour distance fades to under water: the liquid's own, as bright as
// the day above it and as dim as the eye is deep.
vec3 water_haze() {
    return u.water.rgb * depth_light(u.eye.y) * (0.12 + 1.5 * smoothstep(-0.12, 0.35, u.sun.y));
}

// The sun's light rippling on what it reaches under water: two crossing
// wave patterns that drift, about 0.7 to 1.5.
float caustics(vec3 world) {
    float t = u.light.y;
    vec2 p = world.xz * 1.7;
    float a = sin(p.x * 1.3 + sin(p.y * 0.9 + t * 0.8) * 1.6 + t * 1.1);
    float b = sin(p.y * 1.7 + sin(p.x * 1.1 - t * 0.6) * 1.4 - t * 0.9);
    float c = a * b;
    return 0.75 + 0.75 * c * c;
}

// How much of the sun's light reaches p (metres) past the clouds' shadow
// blobs (sv_set_cloud_shadows): 1 in the clear, less under a blob's middle.
float cloud_shadow(vec3 p) {
    int n = int(u.cloud.x + 0.5);
    vec3 s = u.sun.xyz;
    if (n == 0 || s.y <= 0.02) return 1.0;
    float lit = 1.0;
    for (int k = 0; k < 64; k++) {
        if (k >= n) break;
        vec4 b = u.blobs[k];
        vec3 cp = b.xyz - p;
        float t = dot(cp, s);
        if (t <= 0.0) continue;
        float d2 = dot(cp, cp) - t * t, r2 = b.w * b.w;
        lit *= 1.0 - u.cloud.y * (1.0 - smoothstep(0.1 * r2, r2, d2));
    }
    return lit;
}

// Fade a lit colour into the sky with distance; under water, into the water.
vec3 fogged(vec3 col, vec3 world) {
    vec3 to = world - u.eye.xyz;
    float d = length(to);
    if (submerged()) return mix(water_haze(), col, exp(-d * u.water.w));
    float fogk = clamp((d - u.fog.x) / max(u.fog.y - u.fog.x, 0.001), 0.0, 1.0);
    return mix(col, sky_fog(to / max(d, 1.0e-4)), fogk * fogk);
}

// Two random numbers in 0..1 for a pixel, a frame and a purpose.
vec2 rand2(ivec2 px, int frame, int salt) {
    uint h = uint(px.x) * 1973u + uint(px.y) * 9277u + uint(frame) * 26699u + uint(salt) * 7919u;
    h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
    uint k = h * 0x9e3779b9u + 0x85ebca6bu;
    k ^= k >> 16; k *= 0x7feb352du; k ^= k >> 15;
    return vec2(float(h & 0xffffu), float(k & 0xffffu)) / 65536.0;
}

// A direction toward a point on the sun's disc: the centre for r = (0.5, 0.5).
vec3 sun_toward(vec2 r) {
    vec3 s = u.sun.xyz;
    vec3 a = normalize(cross(s, abs(s.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
    vec3 b = cross(s, a);
    float rad = sqrt(r.x) * 0.02, ang = r.y * 6.2831853;          // wider than the real sun, for a soft rim
    return normalize(s + (a * cos(ang) + b * sin(ang)) * rad);
}

// How much of the sun a point (voxel units) on a face with normal n sees
// along [toward]: 0 or 1.
float sun_visible(vec3 at, vec3 n, vec3 toward) {
    if (dot(n, u.sun.xyz) <= 0.0) return 0.0;
    Hit h;
    return trace(at + n * 0.02, toward, 384.0, false, h) ? 0.0 : 1.0;
}

// How bright a fully emitting block is, against a sun of about 1.
const float GLOW = 5.0;   // strata_vk.c's SV_GLOW (a figure's glow reach for history) must equal it

// Lamps near each cell of the light volume (strata_vk.c, lamps_to_frame):
// per cell (first, count); each lamp a glowing voxel (x, y, z voxels, class).
layout(std430, set = 0, binding = 32) readonly buffer LampCells { uvec2 cells[]; } lamp_cells;
layout(std430, set = 0, binding = 33) readonly buffer LampList { ivec4 lamps[]; } lamp_list;

// A lamp's fourth int: its light class in the low byte; bit 24 a moving lamp (its position in eighths of a voxel), bit 25 a beam
// (its heading in bytes 1 and 2), bit 26 a lamp the gather pass traces a ray to (strata_vk.c, SV_LAMP_*).
const int LAMP_MOVING = 1 << 24, LAMP_BEAM = 1 << 25, LAMP_SHADOW = 1 << 26;
const float LAMP_FIXTURE = 2.5;   // voxels round a lamp that never shadow it

// A cell's standing lamps (first, count), or its moving ones: those are listed after every cell's standing entry.
uvec2 lamp_entry(ivec3 cell, bool moving) {
    int i = cell.x + u.vol.x * (cell.y + u.vol.y * cell.z);
    return lamp_cells.cells[moving ? i + u.vol.x * u.vol.y * u.vol.z : i];
}

// What a lamp gives a point at [at] facing [normal] with nothing between: light mode 8's lamps, and the traced ones' (lamp_gather).
// It falls off with the square of the distance and fades out over the last half of [reach]; a beam lights a cone ahead of it.
vec3 lamp_direct(ivec4 lamp, vec3 at, vec3 normal, float reach, out vec3 dir, out float dist) {
    bool moving = (lamp.w & LAMP_MOVING) != 0;
    bool beam = (lamp.w & LAMP_BEAM) != 0;
    vec3 position = moving ? vec3(lamp.xyz) * 0.125 : vec3(lamp.xyz) + 0.5;
    vec3 to_lamp = position - at;
    float distance2 = dot(to_lamp, to_lamp), reach2 = reach * reach;
    float facing = dot(normal, to_lamp);
    dist = sqrt(distance2);
    dir = to_lamp / max(dist, 1.0e-4);
    if (distance2 >= reach2 || facing <= 0.0) return vec3(0.0);
    float inverse_distance = inversesqrt(max(distance2, 0.25));
    float cone = 1.0;
    if (beam) {
        vec2 heading = (vec2((lamp.w >> 8) & 255, (lamp.w >> 16) & 255) - 128.0) / 127.0;
        vec3 aim = vec3(heading.x * 0.97, -0.24, heading.y * 0.97);
        cone = smoothstep(0.70, 0.94, dot(aim, -to_lamp) * inverse_distance);
    }
    float power = moving ? (beam ? 140.0 : 48.0) : 140.0;
    float falloff = (1.0 - smoothstep(0.25 * reach2, reach2, distance2)) * power / (distance2 + 16.0);
    return class_emission(uint(lamp.w & 255)) * facing * inverse_distance * falloff * cone;
}

// Every lamp listed for [at]'s cell, with nothing between: light mode 8, which traces no rays to them.
vec3 lamps_unshadowed(vec3 at, vec3 normal) {
    ivec3 cell = wrap(ivec3(floor(at)) >> 5, u.vol.xyz);
    vec3 light = vec3(0.0), dir; float d;
    for (int part = 0; part < 2; part++) {
        uvec2 entry = lamp_entry(cell, part == 1);
        for (uint i = 0u; i < entry.y; ++i) light += lamp_direct(lamp_list.lamps[entry.x + i], at, normal, u.lamps.y, dir, d);
    }
    return light;
}

// The lamps' light at a point, gathered (gather.frag, when u.lamps.x is 1): every listed lamp that casts no shadow in full, and of
// those that do, one, chosen in proportion to what it would give, with a ray to see whether anything stands between; divided by
// the chance of choosing it, so that on average all of them are counted. [any_moving]: the cell lists a moving lamp.
vec3 lamp_gather(vec3 at, vec3 normal, ivec2 px, int frame, out bool any_moving) {
    ivec3 cell = wrap(ivec3(floor(at)) >> 5, u.vol.xyz);
    vec3 sum = vec3(0.0), held = vec3(0.0), held_dir = vec3(0.0), dir;
    float d, held_d = 0.0, held_w = 0.0, total = 0.0;
    float u0 = rand2(px, frame, 97).x;
    any_moving = false;
    for (int part = 0; part < 2; part++) {
        uvec2 entry = lamp_entry(cell, part == 1);
        if (part == 1 && entry.y > 0u) any_moving = true;
        for (uint i = 0u; i < entry.y; ++i) {
            ivec4 l = lamp_list.lamps[entry.x + i];
            vec3 g = lamp_direct(l, at, normal, u.lamps.y, dir, d);
            float w = g.r + g.g + g.b;
            if (w <= 0.0) continue;
            if ((l.w & LAMP_SHADOW) == 0) { sum += g; continue; }
            total += w;
            float p = w / total;
            if (u0 < p) { held = g; held_w = w; held_dir = dir; held_d = d; u0 = u0 / p; }
            else u0 = (u0 - p) / (1.0 - p);
        }
    }
    if (held_w > 0.0) {
        Hit h;
        // stop short of the lamp's fixture: a lamp is one glowing voxel, often set in a housing a few voxels across (a lamp
        // head, a window's frame) that the volume holds as solid, and the housing must not shadow its own lamp
        if (!(held_d > LAMP_FIXTURE + 0.1 && trace_steps(at + normal * 0.02, held_dir, held_d - LAMP_FIXTURE, false, 96, 1.0e9, -1000000, h))) sum += held * (total / held_w);
    }
    return sum;
}

// What lamp [l] would give a point at [at] facing [n] if nothing stood
// between, as sky_light counts light (cosine-weighted, over pi), and how far
// away it is and in what direction. A lamp's light reaches 16 voxels,
// fading out from 12, as lights in games do: past that it is a few
// hundredths of what it is close by, and not worth a ray.
vec3 lamp_gives(ivec4 l, vec3 at, vec3 n, out vec3 dir, out float d) {
    vec3 to = vec3(l.xyz) + 0.5 - at;
    float d2 = dot(to, to);
    d = sqrt(d2);
    dir = to / max(d, 1.0e-4);
    float c = dot(n, dir);
    if (c <= 0.0 || d >= 16.0) return vec3(0.0);
    // a voxel seen from d away covers about 1.2 / d^2 of the sphere; near, no more than a half
    float omega = min(1.2 / max(d2, 0.25), 3.14159);
    return class_emission(uint(l.w)) * GLOW * omega * c / 3.14159 * smoothstep(16.0, 12.0, d);
}

// Light from the lamps listed for [at]'s cell: one of them, chosen in
// proportion to what it would give (in one pass: each lamp in turn takes
// the place of the one held with the chance its share of the total so far
// gives it), and a ray to see whether anything stands between. Divided by
// the chance of choosing it, so that on average the sum over them all is
// counted, with little noise when one lamp gives most of it.
vec3 lamp_light(vec3 at, vec3 n, ivec2 px, int frame) {
    ivec3 cell = wrap(ivec3(floor(at)) >> 5, u.vol.xyz);
    uvec2 e = lamp_cells.cells[cell.x + u.vol.x * (cell.y + u.vol.y * cell.z)];
    if (e.y == 0u) return vec3(0.0);
    vec3 dir, held_dir = vec3(0.0); float d, held_d = 0.0;
    vec3 held = vec3(0.0); float held_w = 0.0, total = 0.0;
    vec2 r = rand2(px, frame, 97);
    float u0 = r.x;
    for (uint i = 0u; i < e.y; i++) {
        vec3 g = lamp_gives(lamp_list.lamps[e.x + i], at, n, dir, d);
        float w = g.r + g.g + g.b;
        if (w <= 0.0) continue;
        total += w;
        float p = w / total;
        // one uniform number serves every choice: it is stretched over what is left of it each time
        if (u0 < p) { held = g; held_w = w; held_dir = dir; held_d = d; u0 = u0 / p; }
        else u0 = (u0 - p) / (1.0 - p);
    }
    if (held_w <= 0.0) return vec3(0.0);
    Hit h;
    // stop short of the lamp's own voxel; a thin lamp never stops a ray, a solid one would
    if (held_d > 0.9 && trace_steps(at + n * 0.02, held_dir, held_d - 0.6, false, 32, 1.0e9, -1000000, h)) return vec3(0.0);
    return held * (total / held_w);
}

// The light arriving at a point (voxel units) on a face with normal n from
// everything but the sun: [rays] rays over the hemisphere, more of them near
// the normal. One that escapes brings the sky's colour; one that hits brings
// what the block (or traced figure's voxel) there emits and what it passes on from the sun and the sky.
// Where there are lamps listed, one more ray is aimed at one of them, and the
// hemisphere's rays leave lamps out: a small lamp is found by few of them,
// so its light would take many frames to settle. A traced figure's glowing
// voxels are in no lamp list, so the hemisphere's rays keep them.
vec3 sky_light(vec3 at, vec3 n, ivec2 px, int frame, int rays) {
    // where the gather pass gathers the lamps' light itself (lamp_gather), these rays leave lamps out and none is aimed at one
    bool gathered = u.lamps.x > 0.5;
    bool aimed = u.water2.z > 0.5 && !gathered;
    float own = aimed || (gathered && u.water2.z > 0.5) ? 0.0 : 1.0;
    vec3 a = normalize(cross(n, abs(n.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
    vec3 b = cross(n, a);
    vec3 sum = vec3(0.0);
    for (int i = 0; i < rays; i++) {
        vec2 r = rand2(px, frame, 1 + i);
        float rad = sqrt(r.x), ang = r.y * 6.2831853;
        vec3 dir = normalize(a * (rad * cos(ang)) + b * (rad * sin(ang)) + n * sqrt(max(1.0 - r.x, 0.0)));
        Hit h;
        if (!trace_steps(at + n * 0.02, dir, 48.0, false, 40, 12.0, -1000000, h)) sum += sky_base(dir) + h.glow * GLOW * own;
        else {
            vec3 hn = NORMALS[h.face];
            // a traced figure's voxel is in no lamp list: its light is found by these rays, aimed or not
            sum += hit_emission(h.cls) * GLOW * (h.cls >= PALETTE_HIT ? 1.0 : own) + h.glow * GLOW * own
                 + hit_color(h.cls) * (u.sun_color.rgb * max(dot(hn, u.sun.xyz), 0.0) * 0.5 + sky_flat(hn));
        }
    }
    return sum / float(max(rays, 1)) + (aimed ? lamp_light(at, n, px, frame) : vec3(0.0));
}

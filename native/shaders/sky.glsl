// The sky, and the light it and the sun put on a surface. Colours are
// linear. The sun is u.sun.xyz (the direction to it) and u.sun_color.

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
    col += vec3(1.0, 0.42, 0.12) * low * day * pow(toward, 5.0) * 0.7;       // the glow round a low sun
    col += u.sun_color.rgb * 0.04 * pow(toward, 96.0);                       // and the haze close to it
    return col;
}

// The sky with the sun's disc.
vec3 sky_color(vec3 d) {
    return sky_base(d) + u.sun_color.rgb * 12.0 * smoothstep(0.99975, 0.99990, dot(d, u.sun.xyz));
}

// What distance fades to: the sky at the horizon, the way the eye is looking.
vec3 sky_fog(vec3 d) { return sky_base(normalize(vec3(d.x, 0.04, d.z))); }

// Sky light on a face with normal n when nothing is known about what it can
// see: more from above, a little bounced from below.
vec3 sky_flat(vec3 n) {
    vec3 above = 0.5 * (sky_base(vec3(0.0, 1.0, 0.0)) + sky_base(normalize(vec3(n.x, 0.25, n.z) + vec3(1.0e-4, 0.0, 0.0))));
    return above * (0.25 + 0.35 * (0.5 + 0.5 * n.y));
}

// Fade a lit colour into the sky with distance.
vec3 fogged(vec3 col, vec3 world) {
    vec3 to = world - u.eye.xyz;
    float d = length(to);
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
const float GLOW = 5.0;

// The light arriving at a point (voxel units) on a face with normal n from
// everything but the sun: [rays] rays over the hemisphere, more of them near
// the normal. One that escapes brings the sky's colour; one that hits brings
// what the block there emits and what it passes on from the sun and the sky.
vec3 sky_light(vec3 at, vec3 n, ivec2 px, int frame, int rays) {
    vec3 a = normalize(cross(n, abs(n.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
    vec3 b = cross(n, a);
    vec3 sum = vec3(0.0);
    for (int i = 0; i < rays; i++) {
        vec2 r = rand2(px, frame, 1 + i);
        float rad = sqrt(r.x), ang = r.y * 6.2831853;
        vec3 dir = normalize(a * (rad * cos(ang)) + b * (rad * sin(ang)) + n * sqrt(max(1.0 - r.x, 0.0)));
        Hit h;
        if (!trace_steps(at + n * 0.02, dir, 48.0, false, 40, 12.0, h)) sum += sky_base(dir) + h.glow * GLOW;
        else {
            vec3 hn = NORMALS[h.face];
            sum += class_emission(h.cls) * GLOW + h.glow * GLOW
                 + class_color(h.cls) * (u.sun_color.rgb * max(dot(hn, u.sun.xyz), 0.0) * 0.5 + sky_flat(hn));
        }
    }
    return sum / float(max(rays, 1));
}

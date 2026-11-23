// Mirror reflections and the sun's highlight.

// The sun's highlight on a surface with normal n seen along [toward_eye],
// for a roughness 0..1: none when fully rough, small and bright when smooth.
vec3 sun_highlight(vec3 n, vec3 toward_eye, float rough, float sun_vis) {
    float gloss = 1.0 - rough;
    if (gloss <= 0.0 || sun_vis <= 0.0 || dot(n, u.sun.xyz) <= 0.0) return vec3(0.0);
    vec3 h = normalize(u.sun.xyz + toward_eye);
    float p = exp2(1.0 + 11.0 * gloss);
    return u.sun_color.rgb * sun_vis * pow(max(dot(n, h), 0.0), p) * (0.25 + 6.0 * gloss * gloss * gloss) * gloss;
}

// How much a surface mirrors at this angle: a little head-on, nearly all at a glance.
float mirror_share(vec3 n, vec3 toward_eye, float head_on) {
    float c = 1.0 - clamp(dot(n, toward_eye), 0.0, 1.0);
    float c2 = c * c;
    return head_on + (1.0 - head_on) * c2 * c2 * c;
}

// The mirror direction off a surface whose true normal is n and whose shading
// normal is m, kept from dipping back into the surface.
vec3 mirror_dir(vec3 toward_eye, vec3 m, vec3 n) {
    vec3 d = reflect(-toward_eye, m);
    float up = dot(d, n);
    return up < 0.03 ? normalize(d + n * (0.03 - up)) : d;
}

// What a mirror ray from [at] (voxel units) along unit [dir] sees. A hit that
// was on screen last frame takes its surface colour from the screen and its
// sun visibility from the light pass's memory; any other hit takes its class's
// (or a traced figure voxel's palette) colour and one shadow ray; a miss is sky.
// Either way the plain figures' shadow map darkens it too.
// [skip_y] is the layer of see-through voxels the ray may pass (the water it leaves), or a layer that does not exist.
vec3 reflected(vec3 at, vec3 dir, int skip_y) {
    Hit h;
    // exact for the first 40 voxels, every third voxel beyond: far reflections lose thin things
    if (!trace_steps(at, dir, 400.0, true, 200, 40.0, skip_y, h)) return sky_color(dir) + h.glow * GLOW;
    vec3 hv = at + dir * h.t;
    vec3 hp = hv * u.eye.w;
    vec3 hn = NORMALS[h.face];
    vec3 was = screen_of(u.prev_vp, hp);
    vec3 now = screen_of(u.vp, hp);
    vec3 albedo = hit_color(h.cls);
    float vis = -1.0;
    if (u.vol.w > 0 && on_screen(was) && on_screen(now)) {
        vec4 om = texture(meta_prev, was.xy);
        if (om.r > 0.0 && abs(om.g - was.z) < 0.03 * was.z + 0.4) {
            vis = texture(hist_prev, was.xy).a;
            albedo = texture(gb_albedo, now.xy).rgb;
        }
    }
    if (vis < 0.0) vis = sun_visible(hv, hn, u.sun.xyz);
    // the plain figures' shadow map, as on screen (the memory holds traced shadows only)
    if (u.fshadow.x > 0.5 && vis > 0.0) vis = min(vis, fig_shadow(hp, hn));
    vec3 col = albedo * (u.sun_color.rgb * max(dot(hn, u.sun.xyz), 0.0) * vis + sky_flat(hn)) + (hit_emission(h.cls) + h.glow) * GLOW;
    return fogged(col, hp);
}

// ── Rain on surfaces (sv_set_weather) ───────────────────────────────────
// Smooth value noise in 0..1 over the plane, one feature about every cell.
float wet_hash(vec2 c) { return fract(sin(dot(c, vec2(127.1, 311.7))) * 43758.5453); }
float wet_noise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(wet_hash(i), wet_hash(i + vec2(1.0, 0.0)), f.x), mix(wet_hash(i + vec2(0.0, 1.0)), wet_hash(i + vec2(1.0, 1.0)), f.x), f.y);
}
// How much of the ground at p (metres) stands under a puddle when it is [wet]: none
// below a third, then low spots fill first, the larger the wetter.
float puddle_at(vec2 p, float wet) {
    float n = 0.65 * wet_noise(p * 0.6) + 0.35 * wet_noise(p * 1.7 + 17.0);
    float level = 1.0 - smoothstep(0.3, 1.0, wet) * 0.45;
    return smoothstep(level, level + 0.05, n);
}
// Rings falling drops make on a puddle: 0..1, a few at a time, each a quarter
// of a second, while it rains.
float rain_rings(vec2 p, float rain) {
    if (rain <= 0.0) return 0.0;
    vec2 cell = floor(p * 2.5), f = fract(p * 2.5);
    float h = wet_hash(cell);
    float t = fract(u.rain.w * (1.2 + 2.0 * h) + h * 7.0);
    vec2 at = vec2(wet_hash(cell + 3.1), wet_hash(cell + 5.7)) * 0.6 + 0.2;
    float r = length(f - at);
    float ring = 1.0 - smoothstep(0.0, 0.06, abs(r - t * 0.4));
    return ring * (1.0 - t) * step(h, 0.25 + 0.6 * rain);
}
// A lit colour col at [world] (metres) with normal n, seen along [toward_eye],
// made wet: darker (wet stone and earth soak up light), glossy on what faces
// up, the sky mirrored in puddles, drops ringing them; and lit by a flash.
// [figure]: a moving figure, which takes the flash but no puddles. Green
// surfaces (grass, leaves) drink the rain: they darken but hold no puddles.
vec3 weathered(vec3 col, vec3 albedo, vec3 n, vec3 world, vec3 toward_eye, float sun_vis, bool figure) {
    float wet = u.weather.y, flash = u.weather.z;
    if (wet > 0.0) {
        float up = smoothstep(0.55, 0.95, n.y);
        col *= 1.0 - wet * (0.30 * up + 0.16 * (1.0 - up));
        float green = clamp((albedo.g - max(albedo.r, albedo.b)) * 6.0, 0.0, 1.0);
        float puddle = figure ? 0.0 : up * (1.0 - green) * puddle_at(world.xz, wet);
        float fres = mirror_share(n, toward_eye, 0.04);
        vec3 m = n;
        if (puddle > 0.0) {
            float ring = rain_rings(world.xz, u.weather.w);
            m = normalize(n + vec3(ring * 0.18, 0.0, ring * 0.11));
        }
        vec3 seen = sky_base(mirror_dir(toward_eye, m, n));
        col *= 1.0 - 0.25 * puddle;                                          // standing water is darker than the wet ground round it
        float share = max(up * wet * (1.0 - green) * (0.08 + 0.40 * fres), puddle * (0.30 + 0.60 * fres));
        col = mix(col, seen, share);
        col += sun_highlight(m, toward_eye, mix(0.45, 0.08, puddle), sun_vis) * up * wet;
    }
    if (flash > 0.0) col += albedo * vec3(0.75, 0.80, 1.0) * flash * (0.35 + 0.65 * max(n.y, 0.0)) * 1.4;
    return col;
}

// Translucent surfaces (water, glass): drawn after the light pass, over the
// lit image, shaded here and blended by their texture's alpha.
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec3 v_world;
layout(location = 2) flat in uint v_w1;
layout(location = 3) flat in uint v_face;
layout(location = 4) in vec2 v_tex;
layout(location = 5) flat in vec4 v_rot;
layout(location = 6) in vec3 v_keep;
layout(location = 7) flat in vec2 v_size;


layout(location = 0) out vec4 o_color;

// What water remembers between frames (see strata_vk.c, whist and wdist):
// last frame's, read; this frame's, written. rgb the mirror colour, a the
// sun's visibility; and the view depth of the surface they belong to.
layout(set = 0, binding = 28, rgba16f) uniform readonly image2D wh_prev;
layout(set = 0, binding = 29, r32f) uniform readonly image2D wd_prev;
layout(set = 0, binding = 30, rgba16f) uniform writeonly image2D wh_now;
layout(set = 0, binding = 31, r32f) uniform writeonly image2D wd_now;
layout(std430, set = 0, binding = 21) readonly buffer Movers { vec4 boxes[]; } movers;
// the sea map (sv_set_sea_map): how exposed each cell of open water is to the wind, 0..1
layout(std430, set = 0, binding = 44) readonly buffer SeaMap { float v[]; } sea_map_buf;


// ── Ocean ───────────────────────────────────────────────────────────────
// An ocean quad (Strata.Quad.ocean1: flags 249 on a top face) carries the
// water's depth at its four corners, in voxels; 63 is deep.
float ocean_hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float ocean_noise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(ocean_hash(i), ocean_hash(i + vec2(1, 0)), f.x), mix(ocean_hash(i + vec2(0, 1)), ocean_hash(i + vec2(1, 1)), f.x), f.y);
}
// How exposed open water at xz (metres) is to the wind: the sea map's cells
// bilinearly, clamped at its edges; fully exposed when there is no map.
float sea_exposure(vec2 xz) {
    int w = int(u.sea_grid.x), h = int(u.sea_grid.y);
    if (w < 1 || h < 1) return 1.0;
    vec2 g = (xz - u.sea_map.xy) / u.sea_map.z - 0.5;
    vec2 lo = clamp(floor(g), vec2(0.0), vec2(w - 1, h - 1));
    vec2 hi = min(lo + 1.0, vec2(w - 1, h - 1));
    vec2 f = clamp(g - lo, 0.0, 1.0);
    float a = sea_map_buf.v[int(lo.y) * w + int(lo.x)], b = sea_map_buf.v[int(lo.y) * w + int(hi.x)];
    float c = sea_map_buf.v[int(hi.y) * w + int(lo.x)], d = sea_map_buf.v[int(hi.y) * w + int(hi.x)];
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
// The depth in metres here, from the corners.
float ocean_depth() {
    vec2 f = clamp(v_uv / max(v_size, vec2(1.0)), 0.0, 1.0);
    vec4 c = vec4(uvec4(v_w1, v_w1 >> 6, v_w1 >> 12, v_w1 >> 18) & 63u);
    return mix(mix(c.x, c.y, f.x), mix(c.z, c.w, f.x), f.y) * u.eye.w;
}
// The water's own colour by depth: turquoise shallows, teal, deep blue.
vec3 ocean_colour(float d) {
    vec3 shallow = vec3(0.16, 0.74, 0.70), mid = vec3(0.05, 0.42, 0.55), deep = vec3(0.02, 0.13, 0.30);
    return d < 4.0 ? mix(shallow, mid, smoothstep(0.0, 4.0, d)) : mix(mid, deep, smoothstep(4.0, 12.0, d));
}
// Rolling waves: two crossing swells and a chop, on a quarter-voxel grid so they keep the world's grain.
// How rough the sea is here under the sea state (sv_set_sea), 0.15..1; -1 when there is none.
float sea_scale(vec2 xz) {
    if (u.sea.z <= 0.0) return -1.0;
    return clamp(u.sea.z / 3.0, 0.15, 1.0) * mix(0.25, 1.0, sea_exposure(xz));
}
// The wind's swell: its wavenumber and speed at roughness s (longer and faster as the sea builds).
vec2 swell(float s) { return vec2(6.2831 / mix(6.0, 9.0, s), 0.6 + 0.4 * s); }
// Where the wind's swell crests, 0..1, at q (metres, quarter-voxel stepped).
float sea_crest(vec2 q, float s) {
    vec2 k = swell(s);
    float bend = 3.0 * ocean_noise(q * 0.045 + vec2(7.0, 3.0));
    return smoothstep(0.88, 0.99, cos(dot(q, u.sea.xy) * k.x + bend - u.light.y * k.y));
}
// How the water's depth falls away here, metres per metre along x and z, from the corners.
vec2 ocean_slope() {
    vec2 f = clamp(v_uv / max(v_size, vec2(1.0)), 0.0, 1.0);
    vec4 c = vec4(uvec4(v_w1, v_w1 >> 6, v_w1 >> 12, v_w1 >> 18) & 63u);
    vec2 g = vec2(mix(c.y - c.x, c.w - c.z, f.y), mix(c.z - c.x, c.w - c.y, f.x)) / max(v_size, vec2(1.0));
    return g;   // depth voxels per voxel: the same in metres per metre
}

vec3 ocean_normal(vec3 world, float d) {
    vec2 q = floor(world.xz / u.eye.w * 4.0) / 4.0 * u.eye.w;
    float t = u.light.y;
    float s = sea_scale(world.xz);
    vec2 g;
    float amp;
    if (s < 0.0) {
        float k1 = 6.2831 / 6.0, k2 = 6.2831 / 2.5;
        vec2 a = vec2(0.8, 0.6), b = vec2(-0.45, 0.89);
        g = a * cos(dot(q, a) * k1 + t * 0.6) + 0.5 * b * cos(dot(q, b) * k2 + t * 1.1)
          + 0.25 * vec2(0.3, -0.95) * cos(dot(q, vec2(0.3, -0.95)) * 4.1 + t * 1.7);
        amp = 0.06 * smoothstep(0.0, 3.0, d);
    } else {
        // the wind's swell runs with it; two shorter trains 35 degrees either side break it up
        vec2 w = u.sea.xy;
        vec2 l = vec2(w.x * 0.819 - w.y * 0.574, w.x * 0.574 + w.y * 0.819);
        vec2 r = vec2(w.x * 0.819 + w.y * 0.574, -w.x * 0.574 + w.y * 0.819);
        vec2 k = swell(s);
        // crests bend and the swell comes in groups, so it never lines up into stripes
        float bend = 3.0 * ocean_noise(q * 0.045 + vec2(7.0, 3.0));
        float group = 0.45 + 0.55 * ocean_noise(q * 0.06 - w * t * 0.08);
        g = group * w * cos(dot(q, w) * k.x + bend - t * k.y)
          + 0.45 * l * cos(dot(q, l) * k.x * 1.9 + bend * 0.7 - t * k.y * 1.4)
          + 0.45 * r * cos(dot(q, r) * k.x * 2.3 - bend * 0.5 - t * k.y * 1.6);
        amp = (0.03 + 0.07 * s) * smoothstep(0.0, 3.0, d);
    }
    // each voxel's facet tilts its own way a little, slowly, so light breaks up rather than banding along the swells
    vec2 cell = floor(world.xz / u.eye.w);
    float phase = floor(t * 0.7);
    vec2 jitter = vec2(ocean_hash(cell + phase), ocean_hash(cell.yx + phase + 7.0)) - 0.5;
    g += jitter * 1.6;
    return normalize(vec3(-g.x * amp, 1.0, -g.y * amp));
}

void main() {
    // an ordered list is drawn only where the eye's rays run its way (quad.vert)
    if (any(lessThan(v_keep, vec3(0.0)))) discard;
    if (cut_hidden(v_world)) discard;
    uint face = v_face & 7u;
    bool far = v_face >= 8u;    // a chunk above level 0: lit by the sun and sky, mirroring the sky, without rays
    uint tex = v_w1 & 65535u;
    // Data overlays use the translucent pass but bypass water lighting and history.
    uint overlay = v_w1 >> 24;
    bool ocean = overlay == 249u && face == 0u;
    float ocean_d = ocean ? ocean_depth() : 0.0;
    if (overlay == 250u) {
        float zoom = 1.0 / max(length(vec3(u.vp[0][0], u.vp[1][0], u.vp[2][0])), 0.0001);
        float fade = mix(0.07, 0.96, smoothstep(28.0, 100.0, zoom));
        vec3 n = qrot(v_rot, NORMALS[min(face, 5u)]);
        // lit as the world is: the sun on the faces toward it and some through the cloud, the sky from above;
        // so they dim and warm with the day, and go grey when the sun is weak
        float toward_sun = max(dot(n, u.sun.xyz), 0.0);
        vec3 sky = sky_flat(n);
        sky = mix(vec3(dot(sky, vec3(0.3, 0.59, 0.11))), sky, 0.35);          // a cloud is grey: it takes the sky's light, not its blue
        vec3 light = u.sun_color.rgb * (0.45 + 0.55 * toward_sun) + sky * (1.6 + 0.6 * n.y);
        vec3 colour = tonemap(vec3(0.92, 0.93, 0.95) * light * (0.78 + 0.22 * n.y));
        // thinner over the middle of the view, where the player is looking
        vec4 clip = u.vp * vec4(v_world, 1.0);
        fade *= mix(0.35, 1.0, smoothstep(0.15, 0.65, length(clip.xy / clip.w)));
        // rain clouds are slate grey, storm clouds darker still, and lightning lights them from inside
        colour = mix(colour, colour * vec3(0.46, 0.49, 0.55), smoothstep(0.5, 1.0, u.weather.x) * (0.4 + 0.6 * u.weather.w));
        colour += vec3(0.70, 0.75, 0.95) * u.weather.z;
        o_color = vec4(colour, fade);
        return;
    }
    if (overlay >= 251u && overlay <= 253u && face == 0u) {
        // Four six-bit samples of a scalar field on an eight-voxel patch.
        vec2 uv = clamp(v_uv / 8.0, 0.0, 1.0);
        vec4 corners = vec4(uvec4(v_w1, v_w1 >> 6, v_w1 >> 12, v_w1 >> 18) & 63u) / 63.0;
        float concentration = mix(mix(corners.x, corners.y, uv.x),
                                  mix(corners.z, corners.w, uv.x), uv.y);
        vec3 low = vec3(1.0, 0.70, 0.29);
        vec3 high = vec3(0.92, 0.28, 0.13);
        if (overlay == 252u) {
            low = vec3(0.31, 0.81, 0.95);
            high = vec3(0.11, 0.37, 0.76);
        } else if (overlay == 251u) {
            low = vec3(0.72, 0.56, 0.86);
            high = vec3(0.46, 0.22, 0.65);
        }
        float alpha = 0.72 * (1.0 - exp(-3.0 * concentration));
        o_color = vec4(mix(low, high, smoothstep(0.18, 0.90, concentration)), alpha);
        return;
    }
    if (!ocean && (tex & 0xf000u) == 0xf000u && face == 0u) {
        uint strip = v_w1 >> 24;
        bool along_x = (strip & 1u) == 0u;
        float length = float(max(1u, strip >> 1));
        float along = along_x ? v_uv.x : v_uv.y;
        float across = along_x ? v_uv.y : v_uv.x;
        float start = float((tex >> 6) & 63u) / 63.0;
        float finish = float(tex & 63u) / 63.0;
        float heat = mix(start, finish, clamp(along / length, 0.0, 1.0));
        vec3 green = vec3(0.25, 0.76, 0.49);
        vec3 amber = vec3(0.96, 0.68, 0.23);
        vec3 red = vec3(0.94, 0.31, 0.25);
        vec3 colour = mix(mix(green, amber, clamp(heat * 2.0, 0.0, 1.0)),
                          red, clamp(heat * 2.0 - 1.0, 0.0, 1.0));
        float edge = smoothstep(0.5, 2.5, min(across, 10.0 - across));
        uint ends = (v_w1 >> 16) & 3u;
        if ((ends & 1u) != 0u) edge *= smoothstep(0.0, 3.0, along);
        if ((ends & 2u) != 0u) edge *= smoothstep(0.0, 3.0, length - along);
        o_color = vec4(colour, mix(0.40, 0.72, heat) * edge);
        return;
    }
    if (tex >= uint(u.misc.z)) tex = 0u;
    vec4 base = ocean ? vec4(ocean_colour(ocean_d), mix(0.35, 0.97, 1.0 - exp(-0.35 * ocean_d))) : texture(tiles, vec3(v_tex, float(tex)));
    uint ao = (v_w1 >> 16) & 255u;
    vec2 f = fract(v_uv);
    float a0 = float(ao & 3u), a1 = float((ao >> 2) & 3u), a2 = float((ao >> 4) & 3u), a3 = float((ao >> 6) & 3u);
    float occ = mix(mix(a0, a1, f.x), mix(a2, a3, f.x), f.y) / 3.0;
    vec3 n = qrot(v_rot, NORMALS[min(face, 5u)]);
    if (u.light.x < 1.0 || u.light.x >= 9.0) { o_color = vec4(shade_flat(base.rgb, n, occ, v_world), base.a); return; }
    vec3 toward = normalize(u.eye.xyz - v_world);
    if (submerged() && face == 0u && toward.y < 0.0) {
        // The surface seen from underneath. Straight overhead the sky shows
        // through, bent into a bright round window; beyond about 49 degrees
        // from straight up the surface is a mirror of the water below it.
        vec2 q = v_world.xz / u.eye.w;
        float t = u.light.y;
        vec3 up = normalize(-toward + 0.05 * vec3(sin(q.x * 2.3 + t * 1.3) + sin(q.y * 1.1 - t * 0.9), 0.0, sin(q.y * 2.9 + t * 1.6) + sin(q.x * 0.7 + t * 1.1)));
        float s2 = 1.7689 * (1.0 - up.y * up.y);                 // the sine of the angle in air, squared (water bends light 1.33 times)
        float c2 = sqrt(max(1.0 - s2, 0.0));
        vec3 air = normalize(vec3(up.x * 1.33, c2, up.z * 1.33));
        // the sky through the window, and the sun's glare where it stands in it
        vec3 window = sky_color(air) * 1.5 + u.sun_color.rgb * 0.6 * pow(max(dot(air, u.sun.xyz), 0.0), 60.0);
        vec3 seen = mix(window * mix(1.0, 0.45, s2 * s2), water_haze() * 0.85, smoothstep(0.86, 1.0, s2));
        o_color = vec4(tonemap(fogged(seen, v_world)), 0.95);
        return;
    }
    // What this surface point had last frame, if it was on screen and is the
    // same surface (the same distance from that frame's eye). A pixel with a
    // memory traces one frame in four, in 8-pixel blocks that take turns, as
    // the light pass does; one without traces now. An edit or a moving body
    // near it makes it trace every frame until things settle.
    bool memory = u.water2.y > 0.5;
    ivec2 px = ivec2(gl_FragCoord.xy);
    int frame = int(u.light.z);
    vec3 at = v_world / u.eye.w;
    float view_depth = (u.vp * vec4(v_world, 1.0)).w;
    bool have = false;
    vec4 old = vec4(0.0);
    if (memory && u.vol.w > 0) {
        vec4 pc = u.prev_vp * vec4(v_world, 1.0);
        ivec2 size = imageSize(wd_prev);
        ivec2 pp = ivec2(vec2(pc.x / pc.w * 0.5 + 0.5, 0.5 - pc.y / pc.w * 0.5) * vec2(size));
        if (pc.w > 0.0 && all(greaterThanEqual(pp, ivec2(0))) && all(lessThan(pp, size))) {
            float od = imageLoad(wd_prev, pp).r;
            if (od > 0.0 && abs(od - pc.w) < 0.02 * pc.w + 0.05) { have = true; old = imageLoad(wh_prev, pp); }
        }
    }
    // an edit to what glows or a colour alone (edit_hi.w 1) leaves the sun's shadow on the water as it was
    bool settling = u.vol.w <= 2 || u.edit_lo.w > 0.5 && u.edit_hi.w < 0.5 && all(greaterThan(at, u.edit_lo.xyz - 32.0)) && all(lessThan(at, u.edit_hi.xyz + 32.0));
    // the moving bodies' boxes, then the traced figures' (shadow and glow): any of them unsettles the remembered sun and mirror
    for (int b = 0; b < u.bodies.x && b < 144; b++)
        if (all(greaterThan(at, movers.boxes[2 * b].xyz)) && all(lessThan(at, movers.boxes[2 * b + 1].xyz))) settling = true;
    bool now = !have || settling || (((px.x >> 3) & 1) + 2 * ((px.y >> 3) & 1)) == (frame & 3);
    // one ray at the sun's centre
    float sun_mem = u.light.x < 2.0 || far ? 1.0 : (now ? sun_visible(at, n, u.sun.xyz) : old.a);
    // figures move every frame: their shadow is never remembered
    float sun_vis = sun_mem;
    if (u.fshadow.x > 0.5 && sun_vis > 0.0) sun_vis = min(sun_vis, fig_shadow(v_world, n));
    vec3 col = base.rgb * (u.sun_color.rgb * max(dot(n, u.sun.xyz), 0.0) * sun_vis * cloud_shadow(v_world) + sky_flat(n));
    float alpha = base.a;
    vec3 mirror = vec3(0.0);
    if (u.light.x >= 3.0) {
        uint cls = (v_w1 >> 24) & 255u;
        float rough = ocean ? 0.03 : float(class_roughness(cls)) / 255.0;
        if (rough < 0.25) {
            vec3 toward_eye = normalize(u.eye.xyz - v_world);
            // seen from underneath, the surface faces down
            vec3 m = dot(n, toward_eye) < 0.0 ? -n : n;
            // a smooth top face ripples: a small moving tilt, in voxel-sized steps so it keeps the world's grain
            if (ocean) m = ocean_normal(v_world, ocean_d);
            else if (face == 0u) {
                vec2 q = floor(v_world.xz / u.eye.w * 4.0) / 4.0;
                float t = u.light.y;
                m = normalize(m + 0.010 * vec3(sin(q.x * 2.3 + t * 1.3) + sin(q.y * 1.1 - t * 0.9), 0.0, sin(q.y * 2.9 + t * 1.6) + sin(q.x * 0.7 + t * 1.1)));
            }
            float share = mirror_share(m, toward_eye, 0.03) * (1.0 - rough);
            if (u.sun_color.w > 0.5) {
                mirror = far ? sky_color(mirror_dir(toward_eye, m, sign(dot(n, toward_eye)) * n)) : now ? reflected(at + sign(dot(n, toward_eye)) * n * 0.02, mirror_dir(toward_eye, m, sign(dot(n, toward_eye)) * n), int(floor(at.y - 0.001))) : old.rgb;
                col = mix(col, mirror, share);
            }
            col += ocean ? 0.45 * sun_highlight(m, toward_eye, 0.16, sun_vis) : sun_highlight(m, toward_eye, rough, sun_vis);
            alpha = mix(alpha, 1.0, share);
        }
    }
    if (ocean && u.light.x >= 3.0) {
        vec3 m = ocean_normal(v_world, ocean_d);
        vec3 r = reflect(-toward, m);
        // the sun glittering on the crests: voxel-sized sparkles that come and go, not a band
        vec2 cell = floor(v_world.xz / u.eye.w);
        float sparkle = step(0.72, ocean_hash(cell + floor(u.light.y * 3.0) * vec2(17.0, 31.0)));
        float glare = pow(max(dot(r, u.sun.xyz), 0.0), 160.0);
        col += u.sun_color.rgb * glare * (0.25 + 2.2 * sparkle) * sun_vis * cloud_shadow(v_world) * (1.0 - u.weather.x);
        // no moon in the sky to trace: a glint where one would stand, opposite the sun, fading in through dusk
        vec3 moon = normalize(vec3(-u.sun.x, 0.45, -u.sun.z));
        col += vec3(0.55, 0.62, 0.80) * 0.6 * smoothstep(0.05, -0.15, u.sun.y) * pow(max(dot(r, moon), 0.0), 220.0);
    }
    // water that never goes black or grey: at least a dim tint of its own colour, in any light or weather
    if (ocean) col = max(col, base.rgb * vec3(0.06, 0.12, 0.25) + vec3(0.010, 0.020, 0.040));
    float sea = ocean ? sea_scale(v_world.xz) : -1.0;
    if (ocean && sea > 0.0) {
        vec2 q = floor(v_world.xz / u.eye.w * 4.0) / 4.0 * u.eye.w;
        vec2 cell = floor(v_world.xz / (u.eye.w * 2.0));   // whitecaps and breakers fleck in 2-voxel blotches
        float lit = clamp(0.25 + 0.85 * max(u.sun.y, 0.0), 0.12, 1.0);
        vec3 white = vec3(0.93, 0.96, 0.97) * lit * (u.sun_color.rgb * 0.5 + 0.5);
        // whitecaps: flecks on the crests, more as the wind rises
        // a whitecap is a blotch on a crest that lasts a moment; the noise keeps them in patches, not lines
        float patchy = smoothstep(0.45, 0.8, ocean_noise(q * 0.12 + u.sea.xy * u.light.y * 0.3));
        float cap = sea_crest(q, sea) * patchy * step(1.0 - 0.5 * u.sea.w * sea, ocean_hash(cell + floor(u.light.y * 2.0) * vec2(13.0, 7.0)));
        col = mix(col, white, 0.8 * cap * smoothstep(1.0, 3.0, ocean_d));
        // breakers: bands of white water rolling shoreward, hardest where the shore faces the wind
        vec2 slope = ocean_slope();
        float steep = length(slope);
        if (steep > 0.002) {
            vec2 shoreward = -slope / steep;
            float facing = clamp(dot(shoreward, u.sea.xy), 0.0, 1.0);
            float roll = fract(dot(q, shoreward) / 5.0 - u.light.y * (0.25 + 0.2 * sea));
            // a band narrows and whitens as it reaches the shallows
            float width = mix(0.10, 0.22, 1.0 - smoothstep(0.5, 2.5, ocean_d));
            float band = (1.0 - smoothstep(0.0, width, roll)) * smoothstep(0.2, 0.6, ocean_d) * (1.0 - smoothstep(1.8, 3.0, ocean_d));
            float strength = sqrt(facing) * smoothstep(0.08, 0.45, sea);
            float breaker = band * strength * step(0.25, ocean_hash(cell + floor(u.light.y * 1.5)));
            col = mix(col, white, breaker);
            alpha = max(alpha, mix(alpha, 0.92, breaker));
        }
    }
    if (ocean) {
        // foam where the water thins against the shore, in voxel-sized flecks that wash in and out
        vec2 q = floor(v_world.xz / u.eye.w) * u.eye.w;
        float wash = ocean_noise(q * 1.3 + vec2(u.light.y * 0.4, -u.light.y * 0.3)) * 0.6 + 0.4 * ocean_noise(q * 3.1 - u.light.y * 0.25);
        float edge = 1.0 - smoothstep(0.15, 0.9, ocean_d);
        float foam = edge * step(0.45, wash + 0.35 * edge);
        float lit = clamp(0.25 + 0.85 * max(u.sun.y, 0.0), 0.12, 1.0);
        col = mix(col, vec3(0.93, 0.96, 0.97) * lit * (u.sun_color.rgb * 0.5 + 0.5), foam);
        alpha = max(alpha, mix(alpha, 0.9, foam));
    }
    if (memory) { imageStore(wh_now, px, vec4(mirror, sun_mem)); imageStore(wd_now, px, vec4(view_depth)); }
    o_color = vec4(tonemap(fogged(col, v_world)), alpha);
}

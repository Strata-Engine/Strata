// Translucent surfaces (water, glass): drawn after the light pass, over the
// lit image, shaded here and blended by their texture's alpha.
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec3 v_world;
layout(location = 2) flat in uint v_w1;
layout(location = 3) flat in uint v_face;
layout(location = 4) in vec2 v_tex;
layout(location = 5) flat in vec4 v_rot;


layout(location = 0) out vec4 o_color;

// What water remembers between frames (see strata_vk.c, whist and wdist):
// last frame's, read; this frame's, written. rgb the mirror colour, a the
// sun's visibility; and the view depth of the surface they belong to.
layout(set = 0, binding = 24, rgba16f) uniform readonly image2D wh_prev;
layout(set = 0, binding = 25, r32f) uniform readonly image2D wd_prev;
layout(set = 0, binding = 26, rgba16f) uniform writeonly image2D wh_now;
layout(set = 0, binding = 27, r32f) uniform writeonly image2D wd_now;
layout(std430, set = 0, binding = 21) readonly buffer Movers { vec4 boxes[]; } movers;

void main() {
    uint tex = v_w1 & 65535u;
    if (tex >= uint(u.misc.z)) tex = 0u;
    vec4 base = texture(tiles, vec3(v_tex, float(tex)));
    uint ao = (v_w1 >> 16) & 255u;
    vec2 f = fract(v_uv);
    float a0 = float(ao & 3u), a1 = float((ao >> 2) & 3u), a2 = float((ao >> 4) & 3u), a3 = float((ao >> 6) & 3u);
    float occ = mix(mix(a0, a1, f.x), mix(a2, a3, f.x), f.y) / 3.0;
    vec3 n = qrot(v_rot, NORMALS[min(v_face, 5u)]);
    if (u.light.x < 1.0 || u.light.x >= 9.0) { o_color = vec4(shade_flat(base.rgb, n, occ, v_world), base.a); return; }
    vec3 toward = normalize(u.eye.xyz - v_world);
    if (submerged() && v_face == 0u && toward.y < 0.0) {
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
    bool settling = u.vol.w <= 2 || u.edit_lo.w > 0.5 && all(greaterThan(at, u.edit_lo.xyz - 32.0)) && all(lessThan(at, u.edit_hi.xyz + 32.0));
    for (int b = 0; b < u.bodies.x && b < 16; b++)
        if (all(greaterThan(at, movers.boxes[2 * b].xyz)) && all(lessThan(at, movers.boxes[2 * b + 1].xyz))) settling = true;
    bool now = !have || settling || (((px.x >> 3) & 1) + 2 * ((px.y >> 3) & 1)) == (frame & 3);
    // one ray at the sun's centre
    float sun_vis = u.light.x < 2.0 ? 1.0 : (now ? sun_visible(at, n, u.sun.xyz) : old.a);
    vec3 col = base.rgb * (u.sun_color.rgb * max(dot(n, u.sun.xyz), 0.0) * sun_vis + sky_flat(n));
    float alpha = base.a;
    vec3 mirror = vec3(0.0);
    if (u.light.x >= 3.0) {
        uint cls = (v_w1 >> 24) & 255u;
        float rough = float(class_roughness(cls)) / 255.0;
        if (rough < 0.25) {
            vec3 toward_eye = normalize(u.eye.xyz - v_world);
            // seen from underneath, the surface faces down
            vec3 m = dot(n, toward_eye) < 0.0 ? -n : n;
            // a smooth top face ripples: a small moving tilt, in voxel-sized steps so it keeps the world's grain
            if (v_face == 0u) {
                vec2 q = floor(v_world.xz / u.eye.w * 4.0) / 4.0;
                float t = u.light.y;
                m = normalize(m + 0.010 * vec3(sin(q.x * 2.3 + t * 1.3) + sin(q.y * 1.1 - t * 0.9), 0.0, sin(q.y * 2.9 + t * 1.6) + sin(q.x * 0.7 + t * 1.1)));
            }
            float share = mirror_share(m, toward_eye, 0.03) * (1.0 - rough);
            if (u.sun_color.w > 0.5) {
                mirror = now ? reflected(at + sign(dot(n, toward_eye)) * n * 0.02, mirror_dir(toward_eye, m, sign(dot(n, toward_eye)) * n), int(floor(at.y - 0.001))) : old.rgb;
                col = mix(col, mirror, share);
            }
            col += sun_highlight(m, toward_eye, rough, sun_vis);
            alpha = mix(alpha, 1.0, share);
        }
    }
    if (memory) { imageStore(wh_now, px, vec4(mirror, sun_vis)); imageStore(wd_now, px, vec4(view_depth)); }
    o_color = vec4(tonemap(fogged(col, v_world)), alpha);
}

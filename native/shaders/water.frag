// Translucent surfaces (water, glass): drawn after the light pass, over the
// lit image, shaded here and blended by their texture's alpha.
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec3 v_world;
layout(location = 2) flat in uint v_w1;
layout(location = 3) flat in uint v_face;
layout(location = 4) in vec2 v_tex;
layout(location = 5) flat in vec4 v_rot;


layout(location = 0) out vec4 o_color;

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
    // one ray at the sun's centre: translucent surfaces keep no memory between frames
    float sun_vis = u.light.x >= 2.0 ? sun_visible(v_world / u.eye.w, n, u.sun.xyz) : 1.0;
    vec3 col = base.rgb * (u.sun_color.rgb * max(dot(n, u.sun.xyz), 0.0) * sun_vis + sky_flat(n));
    float alpha = base.a;
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
            if (u.sun_color.w > 0.5) col = mix(col, reflected(v_world / u.eye.w + sign(dot(n, toward_eye)) * n * 0.02, mirror_dir(toward_eye, m, sign(dot(n, toward_eye)) * n), int(floor(v_world.y / u.eye.w - 0.001))), share);
            col += sun_highlight(m, toward_eye, rough, sun_vis);
            alpha = mix(alpha, 1.0, share);
        }
    }
    o_color = vec4(tonemap(fogged(col, v_world)), alpha);
}

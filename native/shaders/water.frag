// Translucent surfaces (water, glass): drawn after the light pass, over the
// lit image, shaded here and blended by their texture's alpha.
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec3 v_world;
layout(location = 2) flat in uint v_w1;
layout(location = 3) flat in uint v_face;
layout(location = 4) in vec2 v_tex;


layout(location = 0) out vec4 o_color;

void main() {
    uint tex = v_w1 & 65535u;
    if (tex >= uint(u.misc.z)) tex = 0u;
    vec4 base = texture(tiles, vec3(v_tex, float(tex)));
    uint ao = (v_w1 >> 16) & 255u;
    vec2 f = fract(v_uv);
    float a0 = float(ao & 3u), a1 = float((ao >> 2) & 3u), a2 = float((ao >> 4) & 3u), a3 = float((ao >> 6) & 3u);
    float occ = mix(mix(a0, a1, f.x), mix(a2, a3, f.x), f.y) / 3.0;
    vec3 n = NORMALS[min(v_face, 5u)];
    if (u.light.x < 1.0 || u.light.x >= 9.0) { o_color = vec4(shade_flat(base.rgb, n, occ, v_world), base.a); return; }
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

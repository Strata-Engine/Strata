#version 450
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec3 v_world;
layout(location = 2) flat in uint v_w1;
layout(location = 3) flat in uint v_face;

layout(set = 0, binding = 0) uniform U {
    mat4 vp;
    vec4 sun;
    vec4 sky;
    vec4 fog;
    vec4 eye;
    vec4 pal[256];
} u;

layout(location = 0) out vec4 o_color;

const vec3 NORMALS[6] = vec3[6](vec3(0, 1, 0), vec3(0, -1, 0), vec3(1, 0, 0), vec3(-1, 0, 0), vec3(0, 0, 1), vec3(0, 0, -1));

float hash3(vec3 p) {
    p = fract(p * 0.3183099 + vec3(0.71, 0.113, 0.419));
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}

void main() {
    // Corner shading is the same on every unit face of a merged quad, so
    // interpolate inside the unit face the fragment is on.
    uint ao = (v_w1 >> 16) & 255u;
    vec2 f = fract(v_uv);
    float a0 = float(ao & 3u), a1 = float((ao >> 2) & 3u), a2 = float((ao >> 4) & 3u), a3 = float((ao >> 6) & 3u);
    float occ = mix(mix(a0, a1, f.x), mix(a2, a3, f.x), f.y) / 3.0;

    vec4 base = u.pal[v_w1 & 255u];
    vec3 n = NORMALS[min(v_face, 5u)];
    // a little per-voxel grain so flat colours read as blocks
    vec3 cell = floor(v_world / u.eye.w - n * 0.5);
    float grain = 0.94 + 0.12 * hash3(cell);

    float direct = max(dot(n, u.sun.xyz), 0.0);
    float skyl = 0.5 + 0.5 * n.y;
    float light = u.sun.w * (0.55 + 0.45 * skyl) + (1.0 - u.sun.w) * direct;
    light *= 0.45 + 0.55 * occ;

    vec3 col = base.rgb * grain * light;
    float d = distance(v_world, u.eye.xyz);
    float fogk = clamp((d - u.fog.x) / max(u.fog.y - u.fog.x, 0.001), 0.0, 1.0);
    col = mix(col, u.sky.rgb, fogk * fogk);
    o_color = vec4(col, base.a);
}

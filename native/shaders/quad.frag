#version 460
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec3 v_world;
layout(location = 2) flat in uint v_w1;
layout(location = 3) flat in uint v_face;
layout(location = 4) in vec2 v_tex;

layout(set = 0, binding = 0) uniform U {
    mat4 vp;
    vec4 sun;
    vec4 sky;
    vec4 fog;
    vec4 eye;
    ivec4 misc;     // x slots to cull, y commands per range, z texture layers
} u;
// One layer per texture id; repeat addressing, so a merged quad tiles its
// texture once per block.
layout(set = 0, binding = 6) uniform sampler2DArray tiles;
layout(push_constant) uniform PC { uint base; uint flags; } pc;   // flags bit 0: discard see-through pixels (cutout)

layout(location = 0) out vec4 o_color;

const vec3 NORMALS[6] = vec3[6](vec3(0, 1, 0), vec3(0, -1, 0), vec3(1, 0, 0), vec3(-1, 0, 0), vec3(0, 0, 1), vec3(0, 0, -1));

void main() {
    // a texture id past the array's last layer draws layer 0, never out of bounds
    uint tex = v_w1 & 65535u;
    if (tex >= uint(u.misc.z)) tex = 0u;
    vec4 base = texture(tiles, vec3(v_tex, float(tex)));
    if ((pc.flags & 1u) != 0u && base.a < 0.5) discard;

    // Corner shading is the same on every unit face of a merged quad, so
    // interpolate inside the unit face the fragment is on.
    uint ao = (v_w1 >> 16) & 255u;
    vec2 f = fract(v_uv);
    float a0 = float(ao & 3u), a1 = float((ao >> 2) & 3u), a2 = float((ao >> 4) & 3u), a3 = float((ao >> 6) & 3u);
    float occ = mix(mix(a0, a1, f.x), mix(a2, a3, f.x), f.y) / 3.0;

    vec3 n = NORMALS[min(v_face, 5u)];
    float direct = max(dot(n, u.sun.xyz), 0.0);
    float skyl = 0.5 + 0.5 * n.y;
    float light = u.sun.w * (0.55 + 0.45 * skyl) + (1.0 - u.sun.w) * direct;
    light *= 0.45 + 0.55 * occ;

    vec3 col = base.rgb * light;
    float d = distance(v_world, u.eye.xyz);
    float fogk = clamp((d - u.fog.x) / max(u.fog.y - u.fog.x, 0.001), 0.0, 1.0);
    col = mix(col, u.sky.rgb, fogk * fogk);
    o_color = vec4(col, base.a);
}

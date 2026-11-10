#version 450
// One packed quad per INSTANCE; the corner comes from gl_VertexIndex (0..3 of
// a triangle strip). Bit layout matches Strata.Quad:
//   w0: x 0-4, y 5-9, z 10-14, (w-1) 15-19, (h-1) 20-24, face 25-27
//   w1: tex 0-15, ao 16-23 (corner i at bits 2i), flags 24-31
// Faces: 0 +y, 1 -y, 2 +x, 3 -x, 4 +z, 5 -z. In-plane axes (u, v):
//   +-y: (x, z)   +-x: (z, y)   +-z: (x, y)
layout(location = 0) in uint w0;
layout(location = 1) in uint w1;

layout(set = 0, binding = 0) uniform U {
    mat4 vp;        // OpenGL clip conventions; converted below
    vec4 sun;       // xyz direction to the sun, w ambient
    vec4 sky;
    vec4 fog;       // x start, y end (metres)
    vec4 eye;       // xyz eye (metres), w metres per voxel
    vec4 pal[256];
} u;

layout(push_constant) uniform PC { vec4 origin; } pc;   // chunk low corner, voxels

layout(location = 0) out vec2 v_uv;           // in unit faces: 0..w, 0..h
layout(location = 1) out vec3 v_world;        // metres
layout(location = 2) flat out uint v_w1;
layout(location = 3) flat out uint v_face;

void main() {
    uint face = (w0 >> 25) & 7u;
    vec3 p = vec3(float(w0 & 31u), float((w0 >> 5) & 31u), float((w0 >> 10) & 31u));
    float w = float(((w0 >> 15) & 31u) + 1u);
    float h = float(((w0 >> 20) & 31u) + 1u);
    // u x v equals the outward normal on faces 1, 3, 4; elsewhere swap the
    // strip's middle corners so every face winds the same way.
    bool flip = (face == 0u || face == 2u || face == 5u);
    uint i = uint(gl_VertexIndex);
    uint cu = flip ? (i >> 1) : (i & 1u);
    uint cv = flip ? (i & 1u) : (i >> 1);
    float du = float(cu) * w, dv = float(cv) * h;
    vec3 q;
    if (face < 2u)      q = p + vec3(du, face == 0u ? 1.0 : 0.0, dv);
    else if (face < 4u) q = p + vec3(face == 2u ? 1.0 : 0.0, dv, du);
    else                q = p + vec3(du, dv, face == 4u ? 1.0 : 0.0);
    vec3 world = (pc.origin.xyz + q) * u.eye.w;
    v_uv = vec2(du, dv);
    v_world = world;
    v_w1 = w1;
    v_face = face;
    vec4 clip = u.vp * vec4(world, 1.0);
    clip.y = -clip.y;                         // Vulkan's y points down
    clip.z = (clip.z + clip.w) * 0.5;         // and its depth runs 0..1
    gl_Position = clip;
}

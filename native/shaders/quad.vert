// One packed quad per INSTANCE; the corner comes from gl_VertexIndex (0..3 of
// a triangle strip). Bit layout matches Strata.Quad:
//   w0: x 0-4, y 5-9, z 10-14, (w-1) 15-19, (h-1) 20-24, face 25-27
//   w1: tex 0-15, ao 16-23 (corner i at bits 2i), flags 24-31
// Faces: 0 +y, 1 -y, 2 +x, 3 -x, 4 +z, 5 -z. In-plane axes (u, v):
//   +-y: (x, z)   +-x: (z, y)   +-z: (x, y)
layout(location = 0) in uint w0;
layout(location = 1) in uint w1;


// The chunk table.
struct Chunk {
    ivec4 origin_level;     // xyz: low corner in voxels; w: bits 0-7 level, bit 8 in use
    uint first[6];
    uint count[6];
};
layout(std430, set = 0, binding = 1) readonly buffer Table { Chunk chunks[]; } table;
// Which slot each indirect command draws; [base] is where this call's range starts.
layout(std430, set = 0, binding = 4) readonly buffer DrawSlot { uint slot[]; } draw_slot;
layout(push_constant) uniform PC { uint base; uint flags; } pc;

layout(location = 0) out vec2 v_uv;           // in unit faces: 0..w, 0..h (corner shading interpolates in these)
layout(location = 4) out vec2 v_tex;          // texture coordinate: one tile per world voxel, image rows running downward
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
    ivec4 ol = table.chunks[draw_slot.slot[pc.base + uint(gl_DrawID)]].origin_level;
    // a level-L chunk's voxels are 2^L world voxels on a side
    float lod = float(1 << (ol.w & 255));
    vec3 world = (vec3(ol.xyz) + q * lod) * u.eye.w;
    // side faces have v running up the world; an image's rows run down
    v_tex = (face < 2u ? vec2(du, dv) : vec2(du, h - dv)) * lod;
    v_uv = vec2(du, dv);
    v_world = world;
    v_w1 = w1;
    v_face = face;
    gl_Position = to_clip(u.vp * vec4(world, 1.0));
}

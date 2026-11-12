// One packed quad per INSTANCE; the corner comes from gl_VertexIndex (0..3 of
// a triangle strip). Bit layout matches Strata.Quad:
//   w0: x 0-4, y 5-9, z 10-14, (w-1) 15-19, (h-1) 20-24, face 25-27
//   w1: tex 0-15, ao 16-23 (corner i at bits 2i), flags 24-31
// Faces: 0 +y, 1 -y, 2 +x, 3 -x, 4 +z, 5 -z. In-plane axes (u, v):
//   +-y: (x, z)   +-x: (z, y)   +-z: (x, y)
layout(location = 0) in uint w0;
layout(location = 1) in uint w1;


// The chunk table.
struct Chunk { ivec4 origin_level; uint first[6]; uint count[6]; vec4 pos; vec4 rot; };   // a body slot (w bit 9) is drawn at pos (metres), turned by rot
layout(std430, set = 0, binding = 1) readonly buffer Table { Chunk chunks[]; } table;
// Which slot each indirect command draws; [base] is where this call's range starts.
layout(std430, set = 0, binding = 4) readonly buffer DrawSlot { uint slot[]; } draw_slot;
// The block table, then each texture's top texture (strata_vk.c, sv_blocks_set): a far chunk's riser takes its block's top
struct Block { int tex[6]; int flags; int emission; };
layout(std430, set = 0, binding = 5) readonly buffer Blocks { Block rows[65536]; uint top_of[]; } blocks;
layout(push_constant) uniform PC { uint base; uint flags; } pc;

layout(location = 0) out vec2 v_uv;           // in unit faces: 0..w, 0..h (corner shading interpolates in these)
layout(location = 4) out vec2 v_tex;          // texture coordinate: one tile per world voxel, image rows running downward
layout(location = 1) out vec3 v_world;        // metres
layout(location = 2) flat out uint v_w1;
layout(location = 3) flat out uint v_face;        // 0..5; plus 8 for a chunk above level 0, lit without rays
layout(location = 5) flat out vec4 v_rot;        // the rotation of the body drawn; identity for the world

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
    // A partly full fluid stands [drop] eighths below its voxel's top: its
    // top face is lowered, and the upper edge of its sides with it.
    float drop = float((w0 >> 28) & 7u) / 8.0;
    // A fluid's quad carries no corner shading in that byte. On a top face
    // it holds how far each corner dips (0..3 eighths) to meet a lower
    // neighbour; on a side, how far its top edge is lowered to match.
    bool fluid = (w0 >> 31) != 0u;
    uint bits = (w1 >> 16) & 255u;
    float dip = fluid ? float((face == 0u ? (bits >> (2u * (cu + 2u * cv))) : bits) & 3u) / 8.0 : 0.0;
    float up = cv == 1u ? dv - drop - dip : 0.0;          // a side's height along v, as drawn
    vec3 q;
    if (face < 2u)      q = p + vec3(du, face == 0u ? 1.0 - drop - dip : 0.0, dv);
    else if (face < 4u) q = p + vec3(face == 2u ? 1.0 : 0.0, up, du);
    else                q = p + vec3(du, up, face == 4u ? 1.0 : 0.0);
    Chunk ch = table.chunks[draw_slot.slot[pc.base + uint(gl_DrawID)]];
    ivec4 ol = ch.origin_level;
    // a level-L chunk's voxels are 2^L world voxels on a side
    float lod = float(1 << (ol.w & 255));
    bool body = (ol.w & 512) != 0;
    vec3 world = body ? ch.pos.xyz + qrot(ch.rot, q * u.eye.w) : (vec3(ol.xyz) + q * lod) * u.eye.w;
    v_rot = body ? ch.rot : vec4(0.0, 0.0, 0.0, 1.0);
    // side faces have v running up the world; an image's rows run down
    v_tex = (face < 2u ? vec2(du, dv) : vec2(du, h - up)) * lod;
    v_uv = vec2(du, dv);
    v_world = world;
    // a side face of a chunk above level 0 takes its block's top texture: a riser there is many voxels tall,
    // and a grass side's dirt would brown the far hills
    uint w1t = ((ol.w & 255) > 0 && face >= 2u) ? ((w1 & 0xFFFF0000u) | blocks.top_of[w1 & 65535u]) : w1;
    v_w1 = fluid ? (w1t | (255u << 16)) : w1t;           // a fluid is not corner-shaded
    v_face = face | ((ol.w & 255) > 0 ? 8u : 0u);
    gl_Position = to_clip(u.vp * vec4(world, 1.0));
}

// A loose quad: four arbitrary corners, for model blocks. One 64-byte
// instance, drawn as a 4-vertex strip; the corner comes from gl_VertexIndex.
// Each corner is four 32-bit words:
//   word 0: x | y << 16        position in the chunk, in 1/256 voxel, offset by one voxel
//   word 1: z | u << 16        (so a model may reach a little outside its cell)
//   word 2: v | tex << 16      u, v in 1/256 tile, offset by 64 tiles
//   word 3: unused
layout(location = 0) in uvec4 c0;
layout(location = 1) in uvec4 c1;
layout(location = 2) in uvec4 c2;
layout(location = 3) in uvec4 c3;

struct Chunk { ivec4 origin_level; uint first[6]; uint count[6]; vec4 pos; vec4 rot; };   // a body slot (w bit 9) is drawn at pos (metres), turned by rot
layout(std430, set = 0, binding = 1) readonly buffer Table { Chunk chunks[]; } table;
layout(std430, set = 0, binding = 4) readonly buffer DrawSlot { uint slot[]; } draw_slot;
layout(push_constant) uniform PC { uint base; uint flags; } pc;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec3 v_world;
layout(location = 2) flat out uint v_w1;
layout(location = 3) flat out uint v_face;        // 0..5, plus 8 above level 0 (quad.vert)
layout(location = 5) flat out vec4 v_rot;
layout(location = 4) out vec2 v_tex;

vec3 pos_of(uvec4 c) { return vec3(float(c.x & 65535u), float(c.x >> 16), float(c.y & 65535u)) / 256.0 - 1.0; }

void main() {
    uvec4 c = gl_VertexIndex == 0 ? c0 : (gl_VertexIndex == 1 ? c1 : (gl_VertexIndex == 2 ? c2 : c3));
    vec3 p = pos_of(c);
    // the quad's facing, from its first three corners, snapped to the nearest
    // of the six directions the fragment shader lights by
    vec3 n = cross(pos_of(c1) - pos_of(c0), pos_of(c2) - pos_of(c0));
    vec3 a = abs(n);
    uint face = a.y >= a.x && a.y >= a.z ? (n.y >= 0.0 ? 0u : 1u) : (a.x >= a.z ? (n.x >= 0.0 ? 2u : 3u) : (n.z >= 0.0 ? 4u : 5u));
    Chunk ch = table.chunks[draw_slot.slot[pc.base + uint(gl_DrawID)]];
    ivec4 ol = ch.origin_level;
    bool body = (ol.w & 512) != 0;
    float lod = float(1 << (ol.w & 255));   // a level-L chunk's voxels are 2^L world voxels on a side
    vec3 world = body ? ch.pos.xyz + qrot(ch.rot, p * u.eye.w) : (vec3(ol.xyz) + p * lod) * u.eye.w;
    v_rot = body ? ch.rot : vec4(0.0, 0.0, 0.0, 1.0);
    v_uv = vec2(0.5);                       // no corner shading on a model
    v_tex = vec2(float(c.y >> 16), float(c.z & 65535u)) / 256.0 - 64.0;
    v_world = world;
    v_w1 = (c.z >> 16) | (255u << 16);
    v_face = face | ((ol.w & 255) > 0 ? 8u : 0u);
    gl_Position = to_clip(u.vp * vec4(world, 1.0));
}

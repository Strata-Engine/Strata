// A cross block: one 8-byte instance (Strata.Quad's instance words) drawn as
// two quads standing on the cell's diagonals, twelve vertices of a triangle
// list generated from gl_VertexIndex.
//   w0: x 0-4, y 5-9, z 10-14, (model + 1) 15-31     w1: block id
// The texture is the block's side texture, read from the block table.
layout(location = 0) in uint w0;
layout(location = 1) in uint w1;

struct Chunk { ivec4 origin_level; uint first[6]; uint count[6]; };
layout(std430, set = 0, binding = 1) readonly buffer Table { Chunk chunks[]; } table;
layout(std430, set = 0, binding = 4) readonly buffer DrawSlot { uint slot[]; } draw_slot;
struct Block { int tex[6]; int flags; int emission; };
layout(std430, set = 0, binding = 5) readonly buffer Blocks { Block rows[]; } blocks;
layout(push_constant) uniform PC { uint base; uint flags; } pc;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec3 v_world;
layout(location = 2) flat out uint v_w1;
layout(location = 3) flat out uint v_face;
layout(location = 4) out vec2 v_tex;

const int CORNER[6] = int[6](0, 1, 2, 2, 1, 3);

void main() {
    uint id = w1 & 65535u;
    if (id >= uint(u.misc.w)) {             // a block the table does not have: draw nothing
        gl_Position = vec4(0.0, 0.0, 2.0, 1.0);
        v_uv = vec2(0.0); v_world = vec3(0.0); v_w1 = 0u; v_face = 0u; v_tex = vec2(0.0);
        return;
    }
    vec3 p = vec3(float(w0 & 31u), float((w0 >> 5) & 31u), float((w0 >> 10) & 31u));
    int i = gl_VertexIndex;
    int c = CORNER[i % 6];
    float cu = float(c & 1), cv = float(c >> 1);
    // the first quad runs corner to corner one way, the second the other
    vec3 q = (i < 6) ? p + vec3(cu, cv, cu) : p + vec3(cu, cv, 1.0 - cu);
    ivec4 ol = table.chunks[draw_slot.slot[pc.base + uint(gl_DrawID)]].origin_level;
    vec3 world = (vec3(ol.xyz) + q) * u.eye.w;
    v_uv = vec2(0.5);                       // no corner shading on a plant
    v_tex = vec2(cu, 1.0 - cv);
    v_world = world;
    v_w1 = uint(blocks.rows[id].tex[2]) | (255u << 16);
    v_face = 0u;                            // lit as an upward surface
    gl_Position = to_clip(u.vp * vec4(world, 1.0));
}

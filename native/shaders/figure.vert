// A figure piece's quads, instanced over every figure showing its part: one
// draw per piece (strata_vk.c, fig_record). Instance i draws quad
// (first + i % n) of the piece for figure part (inst + i / n). The quads are
// read from the quad buffer as words (Strata.Quad's layout, as quad.vert
// reads it); the texture field holds a palette index.
//
// A quad sits at (piece corner - anchor + position) x voxel in its part's
// frame, which the part's instance places (origin, rotation) in the world.
struct FigInst { vec3 pos; uint flags; vec4 rot; };   // flags bit 0: the figure moved since the last frame
layout(std430, set = 0, binding = 25) readonly buffer FigInsts { FigInst inst[]; } figs;
layout(std430, set = 0, binding = 26) readonly buffer Quads { uint words[]; } quads;
layout(push_constant) uniform PC { uint base; uint flags; uint first; uint n; uint inst; uint rig; float ox; float oy; float oz; float voxel; } pc;

layout(location = 0) out vec2 v_uv;
layout(location = 4) out vec2 v_tex;
layout(location = 1) out vec3 v_world;
layout(location = 2) flat out uint v_w1;
layout(location = 3) flat out uint v_face;
layout(location = 5) flat out vec4 v_rot;

void main() {
    uint i = uint(gl_InstanceIndex);
    uint qi = pc.first + i % pc.n;
    FigInst f = figs.inst[pc.inst + i / pc.n];
    uint w0 = quads.words[qi * 2u], w1 = quads.words[qi * 2u + 1u];
    uint face = (w0 >> 25) & 7u;
    vec3 p = vec3(float(w0 & 31u), float((w0 >> 5) & 31u), float((w0 >> 10) & 31u));
    float w = float(((w0 >> 15) & 31u) + 1u);
    float h = float(((w0 >> 20) & 31u) + 1u);
    bool flip = (face == 0u || face == 2u || face == 5u);
    uint k = uint(gl_VertexIndex);
    uint cu = flip ? (k >> 1) : (k & 1u);
    uint cv = flip ? (k & 1u) : (k >> 1);
    float du = float(cu) * w, dv = float(cv) * h;
    vec3 q;
    if (face < 2u)      q = p + vec3(du, face == 0u ? 1.0 : 0.0, dv);
    else if (face < 4u) q = p + vec3(face == 2u ? 1.0 : 0.0, dv, du);
    else                q = p + vec3(du, dv, face == 4u ? 1.0 : 0.0);
    vec3 world = f.pos + qrot(f.rot, (q + vec3(pc.ox, pc.oy, pc.oz)) * pc.voxel);
    v_rot = f.rot;
    v_tex = vec2(du, dv);
    v_uv = vec2(du, dv);
    v_world = world;
    // the palette entry (the rig's 256, then the index) in the texture field; the corner shading kept; the top byte: bit 0 moving
    v_w1 = (pc.rig * 256u + (w1 & 255u)) | (w1 & 0x00FF0000u) | ((f.flags & 1u) << 24);
    v_face = face;
    gl_Position = to_clip(u.vp * vec4(world, 1.0));
}

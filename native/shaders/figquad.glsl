// A figure piece's quads, instanced over every figure showing its part: one
// draw per piece (strata_vk.c, fig_record and fig_shadow_record). Instance i
// draws quad (first + i % n) of the piece for figure part (inst + i / n). The
// quads are read from the quad buffer as words (Strata.Quad's layout, as
// quad.vert reads it); the texture field holds a palette index.
//
// A quad sits at (piece corner - anchor + position) x voxel in its part's
// frame, which the part's instance places (origin, rotation) in the world.
// Shared by figure.vert (the geometry pass) and figure_shadow.vert (the
// plain figures' sun shadow map).
struct FigInst { vec3 pos; uint flags; vec4 rot; };   // flags bit 0: the figure moved since the last frame
layout(std430, set = 0, binding = 25) readonly buffer FigInsts { FigInst inst[]; } figs;
layout(std430, set = 0, binding = 26) readonly buffer Quads { uint words[]; } quads;
layout(push_constant) uniform PC { uint base; uint flags; uint first; uint n; uint inst; uint rig; float ox; float oy; float oz; float voxel; } pc;

// This vertex's corner of its quad: q in the piece's voxels, (du, dv) along
// the quad, the quad's face and second word, and the part instance.
struct FigCorner { vec3 q; vec2 duv; uint face; uint w1; FigInst f; };
FigCorner fig_corner() {
    FigCorner c;
    uint i = uint(gl_InstanceIndex);
    uint qi = pc.first + i % pc.n;
    c.f = figs.inst[pc.inst + i / pc.n];
    uint w0 = quads.words[qi * 2u];
    c.w1 = quads.words[qi * 2u + 1u];
    uint face = (w0 >> 25) & 7u;
    vec3 p = vec3(float(w0 & 31u), float((w0 >> 5) & 31u), float((w0 >> 10) & 31u));
    float w = float(((w0 >> 15) & 31u) + 1u);
    float h = float(((w0 >> 20) & 31u) + 1u);
    bool flip = (face == 0u || face == 2u || face == 5u);
    uint k = uint(gl_VertexIndex);
    uint cu = flip ? (k >> 1) : (k & 1u);
    uint cv = flip ? (k & 1u) : (k >> 1);
    float du = float(cu) * w, dv = float(cv) * h;
    if (face < 2u)      c.q = p + vec3(du, face == 0u ? 1.0 : 0.0, dv);
    else if (face < 4u) c.q = p + vec3(face == 2u ? 1.0 : 0.0, dv, du);
    else                c.q = p + vec3(du, dv, face == 4u ? 1.0 : 0.0);
    c.duv = vec2(du, dv);
    c.face = face;
    return c;
}
// where a corner is in the world (metres)
vec3 fig_world(FigCorner c) { return c.f.pos + qrot(c.f.rot, (c.q + vec3(pc.ox, pc.oy, pc.oz)) * pc.voxel); }

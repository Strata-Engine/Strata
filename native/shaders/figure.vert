// A figure piece's quads in the geometry pass (figquad.glsl reads them).
layout(location = 0) out vec2 v_uv;
layout(location = 4) out vec2 v_tex;
layout(location = 1) out vec3 v_world;
layout(location = 2) flat out uint v_w1;
layout(location = 3) flat out uint v_face;
layout(location = 5) flat out vec4 v_rot;

void main() {
    FigCorner c = fig_corner();
    vec3 world = fig_world(c);
    v_rot = c.f.rot;
    v_tex = c.duv;
    v_uv = c.duv;
    v_world = world;
    // the palette entry (the rig's 256, then the index) in the texture field; the corner shading kept; the top byte: bit 0 moving
    v_w1 = (pc.rig * 256u + (c.w1 & 255u)) | (c.w1 & 0x00FF0000u) | ((c.f.flags & 1u) << 24);
    v_face = c.face;
    gl_Position = to_clip(u.vp * vec4(world, 1.0));
}

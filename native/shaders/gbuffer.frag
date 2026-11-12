// The geometry pass: solid surfaces write what the light pass needs to know
// about them, not a lit colour.
//   o_albedo  rgb the texture's colour
//   o_nm      r face (0..5) / 255, g 1 for a chunk above level 0 (lit without rays) else 0, b light class / 255, a corner shading 0..1;
//             a turned surface (a body) has r 16..255 and g: its true normal, octahedral (light.frag decodes)
// A figure (push constant flags bit 1) takes its colour, roughness and
// emission from its rig's palette, not a texture or a light class:
//   o_albedo  rgb the palette colour, a = (moving << 6 | emission in 0..63) / 255, so below 0.5 (the world writes 1)
//   o_nm      r, g its true normal (octahedral, always), b roughness 0..1, a corner shading
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec3 v_world;
layout(location = 2) flat in uint v_w1;
layout(location = 3) flat in uint v_face;
layout(location = 4) in vec2 v_tex;
layout(location = 5) flat in vec4 v_rot;

layout(push_constant) uniform PC { uint base; uint flags; } pc;   // flags bit 0: discard see-through pixels (cutout); bit 1: a figure
// The rigs' palettes: 256 entries a rig, two words each (strata_vk.c, sv_rig_palette):
// red and green in 11 bits and blue in 10 (linear); roughness and emission in 16 bits each.
layout(std430, set = 0, binding = 24) readonly buffer Palette { uvec2 e[]; } palette;

layout(location = 0) out vec4 o_albedo;
layout(location = 1) out vec4 o_nm;

void main() {
    if ((pc.flags & 2u) != 0u) {
        uint fao = (v_w1 >> 16) & 255u;
        vec2 ff = fract(v_uv);
        float f0 = float(fao & 3u), f1 = float((fao >> 2) & 3u), f2 = float((fao >> 4) & 3u), f3 = float((fao >> 6) & 3u);
        float focc = mix(mix(f0, f1, ff.x), mix(f2, f3, ff.x), ff.y) / 3.0;
        uvec2 pe = palette.e[v_w1 & 65535u];
        vec3 col = vec3(float(pe.x & 2047u) / 2047.0, float((pe.x >> 11) & 2047u) / 2047.0, float(pe.x >> 22) / 1023.0);
        float em = float(pe.y >> 16) / 65535.0;
        uint code = (((v_w1 >> 24) & 1u) << 6) | uint(floor(em * 63.0 + 0.5));
        o_albedo = vec4(col, float(code) / 255.0);
        vec2 fe = oct_enc(qrot(v_rot, NORMALS[min(v_face & 7u, 5u)]));
        o_nm = vec4((16.0 + floor(fe.x * 239.0 + 0.5)) / 255.0, fe.y, float(pe.y & 65535u) / 65535.0, focc);
        return;
    }
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

    o_albedo = vec4(base.rgb, 1.0);
    if (turned(v_rot)) {
        vec2 e = oct_enc(qrot(v_rot, NORMALS[min(v_face & 7u, 5u)]));
        o_nm = vec4((16.0 + floor(e.x * 239.0 + 0.5)) / 255.0, e.y, float((v_w1 >> 24) & 255u) / 255.0, occ);
    } else o_nm = vec4(float(min(v_face & 7u, 5u)) / 255.0, v_face >= 8u ? 1.0 : 0.0, float((v_w1 >> 24) & 255u) / 255.0, occ);
}

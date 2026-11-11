// The 2D layer's pixels. Kinds: 0 a plain colour, 1 a block texture layer,
// 2 a region of the interface atlas, 3 a glyph of the glyph atlas.
layout(set = 0, binding = 17) uniform sampler2D ui_art;
layout(set = 0, binding = 18) uniform sampler2DArray glyphs;   // distance fields: 0.5 on the outline, more inside

layout(location = 0) in vec2 v_uv;
layout(location = 1) flat in vec4 v_rgba;
layout(location = 2) flat in vec4 v_kind;

layout(location = 0) out vec4 o_color;

void main() {
    int kind = int(v_kind.x + 0.5);
    // colours are given as the screen shows them; the frame is blended in linear light
    vec4 tint = vec4(pow(v_rgba.rgb, vec3(2.2)), v_rgba.a);
    vec4 c = tint;
    if (kind == 1) c *= texture(tiles, vec3(v_uv, v_kind.y));
    else if (kind == 2) c *= texture(ui_art, v_uv / vec2(textureSize(ui_art, 0)));
    else if (kind == 3) {
        // v_kind.w is how many pixels the field's whole range spans at this
        // size, so the distance comes out in pixels and the edge is one
        // pixel wide whatever the size; v_kind.z widens the shape (an outline)
        float d = (texture(glyphs, vec3(v_uv / vec2(textureSize(glyphs, 0).xy), v_kind.y)).r - 0.5) * v_kind.w;
        float cover = clamp(d + 0.5 + v_kind.z, 0.0, 1.0);
        // the frame is blended in linear light, where a half-covered dark
        // pixel on a light ground looks nearly empty: dark text gets its
        // coverage lifted so thin strokes keep their weight
        float lum = dot(v_rgba.rgb, vec3(0.299, 0.587, 0.114));
        c.a *= pow(cover, mix(0.5, 1.0, lum));
    }
    if (c.a <= 0.0) discard;
    o_color = c;
}

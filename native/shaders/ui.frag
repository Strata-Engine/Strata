// The 2D layer's pixels. Kinds: 0 a plain colour, 1 a block texture layer,
// 2 a region of the interface atlas.
layout(set = 0, binding = 17) uniform sampler2D ui_art;

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
    if (c.a <= 0.0) discard;
    o_color = c;
}

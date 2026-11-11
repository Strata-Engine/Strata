// Spray: a textured square lit by the sun and a flat sky term, with no
// rays, fading out over the last third of its life.
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec3 v_world;
layout(location = 2) flat in float v_layer;
layout(location = 3) flat in float v_age;

layout(location = 0) out vec4 o_color;

void main() {
    float layer = v_layer >= float(u.misc.z) ? 0.0 : v_layer;
    vec4 base = texture(tiles, vec3(v_uv, layer));
    float fade = 1.0 - smoothstep(0.66, 1.0, v_age);
    if (base.a * fade < 0.02) discard;
    bool flat_light = u.light.x < 1.0 || u.light.x >= 9.0;
    vec3 col = flat_light ? base.rgb * 0.9 : tonemap(fogged(base.rgb * (u.sun_color.rgb * max(u.sun.y, 0.0) * 0.7 + sky_flat(vec3(0.0, 1.0, 0.0)) + 0.05), v_world));
    o_color = vec4(col, base.a * fade);
}

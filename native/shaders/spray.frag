// Spray: a textured square lit by the sun where the sun reaches it and by
// a flat sky term, dimmer under a roof; it fades out over the last third of
// its life.
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
    vec3 col = base.rgb * 0.9;
    if (!flat_light) {
        // a particle takes the sun only where the sun reaches it, and less sky under a roof
        vec3 at = v_world / u.eye.w;
        float sun = 1.0, open_sky = 1.0;
        if (u.light.x >= 2.0) {
            Hit h;
            sun = u.sun.y > 0.0 && !trace(at, u.sun.xyz, 384.0, false, h) ? 1.0 : 0.0;
            open_sky = trace(at, vec3(0.0, 1.0, 0.0), 24.0, false, h) ? 0.3 : 1.0;
        }
        col = tonemap(fogged(base.rgb * (u.sun_color.rgb * max(u.sun.y, 0.0) * 0.7 * sun + sky_flat(vec3(0.0, 1.0, 0.0)) * open_sky + 0.03), v_world));
    }
    o_color = vec4(col, base.a * fade);
}

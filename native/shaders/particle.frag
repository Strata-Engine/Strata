// Particles: a textured square. Shaded ones are blended over what is behind
// and lit by the sun and sky (per pixel, as spray always was, or once a
// particle, worked out in particle.vert); glowing ones are added, not lit,
// and fade with distance. Effect 0, spray, fades out over the last third of
// its life exactly as it did.
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec3 v_world;
layout(location = 2) flat in float v_layer;
layout(location = 3) flat in float v_age;
layout(location = 4) flat in vec4 v_color;
layout(location = 5) flat in vec4 v_light;
layout(location = 6) flat in int v_effect;

layout(push_constant) uniform PC { uint pass; } pc;

layout(location = 0) out vec4 o_color;

void main() {
    float layer = v_layer >= float(u.misc.z) ? 0.0 : v_layer;
    vec4 base = texture(tiles, vec3(v_uv, layer));
    bool flat_light = u.light.x < 1.0 || u.light.x >= 9.0;
    if (pc.pass == 1u) {
        // glowing: added; the sky's fog swallows it with distance
        float a = base.a * v_color.a;
        if (a < 0.01) discard;
        vec3 to = v_world - u.eye.xyz;
        float d = length(to);
        float fogk = clamp((d - u.fog.x) / max(u.fog.y - u.fog.x, 0.001), 0.0, 1.0);
        float keep = submerged() ? exp(-d * u.water.w) : 1.0 - fogk * fogk;
        o_color = vec4(base.rgb * v_color.rgb * a * keep, 0.0);
        return;
    }
    float fade = v_effect == 0 ? 1.0 - smoothstep(0.66, 1.0, v_age) : v_color.a;
    if (base.a * fade < 0.02) discard;
    vec3 albedo = base.rgb * v_color.rgb;
    vec3 col = albedo * 0.9;
    if (!flat_light) {
        vec3 lit;
        if (v_light.w > 0.5) lit = v_light.rgb;
        else {
            // a particle takes the sun only where the sun reaches it, and less sky under a roof
            vec3 at = v_world / u.eye.w;
            float sun = 1.0, open_sky = 1.0;
            if (u.light.x >= 2.0) {
                Hit h;
                sun = u.sun.y > 0.0 && !trace(at, u.sun.xyz, 384.0, false, h) ? 1.0 : 0.0;
                open_sky = trace(at, vec3(0.0, 1.0, 0.0), 24.0, false, h) ? 0.3 : 1.0;
            }
            lit = u.sun_color.rgb * max(u.sun.y, 0.0) * 0.7 * sun + sky_flat(vec3(0.0, 1.0, 0.0)) * open_sky + 0.03;
        }
        col = tonemap(fogged(albedo * lit, v_world));
    }
    o_color = vec4(col, base.a * fade);
}

// Light mode 0: the shading the renderer had before ray tracing. A sun
// term by face direction, a sky term, corner shading, and fog to a flat sky.
vec3 shade_flat(vec3 albedo, vec3 n, float occ, vec3 world) {
    float direct = max(dot(n, u.sun.xyz), 0.0);
    float skyl = 0.5 + 0.5 * n.y;
    float light = u.sun.w * (0.55 + 0.45 * skyl) + (1.0 - u.sun.w) * direct;
    light *= 0.45 + 0.55 * occ;
    vec3 col = albedo * light;
    float d = distance(world, u.eye.xyz);
    float fogk = clamp((d - u.fog.x) / max(u.fog.y - u.fog.x, 0.001), 0.0, 1.0);
    return mix(col, u.sky.rgb, fogk * fogk);
}

// The plain figures' sun shadow map (strata_vk.c, fig_shadow_record): depth
// seen from the sun, compared by the sampler (lit where the point is no
// farther from the sun than what the map holds; white past its edges).
layout(set = 0, binding = 27) uniform sampler2DShadow fig_map;
// How much of the sun the plain figures leave a point (metres) on a surface
// with normal n: 1 in the open, 0 in a figure's shadow. Four taps half a
// texel from the point, each a filtered 2 x 2 comparison: an edge softened
// over about two texels (12 cm across the sun; a low sun stretches it along
// its way). Read only while u.fshadow.x is 1.
float fig_shadow(vec3 world, vec3 n) {
    vec4 c = u.sun_vp * vec4(world + n * 0.04, 1.0);      // off the surface by most of a figure voxel
    vec2 uv = vec2(c.x * 0.5 + 0.5, 0.5 - c.y * 0.5);
    float d = (c.z + 1.0) * 0.5 - 0.02 * u.fshadow.z;      // and 2 cm toward the sun
    if (d >= 1.0) return 1.0;
    float t = 0.5 * u.fshadow.y;
    return 0.25 * (texture(fig_map, vec3(uv + vec2(-t, -t), d)) + texture(fig_map, vec3(uv + vec2(t, -t), d))
                 + texture(fig_map, vec3(uv + vec2(-t, t), d)) + texture(fig_map, vec3(uv + vec2(t, t), d)));
}

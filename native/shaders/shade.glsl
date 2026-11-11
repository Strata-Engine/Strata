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

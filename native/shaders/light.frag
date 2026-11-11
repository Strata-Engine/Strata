// The light pass: one fragment per screen pixel reads the geometry buffers
// and writes the lit colour.
layout(location = 0) in vec2 v_uv;

layout(set = 0, binding = 7) uniform sampler2D gb_albedo;
layout(set = 0, binding = 8) uniform sampler2D gb_nm;
layout(set = 0, binding = 9) uniform sampler2D gb_depth;

layout(location = 0) out vec4 o_lit;

// A colour for a light class, for looking at the volume.
vec3 class_tint(uint c) {
    vec3 p = fract(vec3(float(c)) * vec3(0.1031, 0.1030, 0.0973) + vec3(0.33, 0.71, 0.19));
    p += dot(p, p.yzx + 33.33);
    return 0.25 + 0.75 * fract((p.xxy + p.yzz) * p.zyx);
}

void main() {
    float depth = texture(gb_depth, v_uv).r;
    if (depth >= 1.0) { o_lit = vec4(u.sky.rgb, 1.0); return; }      // nothing drawn here: sky
    vec3 albedo = texture(gb_albedo, v_uv).rgb;
    vec4 nm = texture(gb_nm, v_uv);
    vec3 n = NORMALS[min(int(nm.r * 255.0 + 0.5), 5)];
    vec3 world = world_at(v_uv, depth);
    if (int(u.light.x) == 9) {
        // look at the light volume: step back off the surface, trace into it,
        // and show the class of the voxel found there (black where there is none)
        Hit h;
        vec3 ro = world / u.eye.w + n * 0.25;
        bool hit = trace(ro, -n, 1.5, true, h);
        o_lit = vec4(hit ? class_tint(h.cls) * (0.6 + 0.4 * max(dot(n, u.sun.xyz), 0.0)) : vec3(0.0), 1.0);
        return;
    }
    o_lit = vec4(shade_flat(albedo, n, nm.a, world), 1.0);
}

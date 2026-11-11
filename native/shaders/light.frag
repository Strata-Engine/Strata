// The light pass: one fragment per screen pixel reads the geometry buffers
// and writes the lit colour.
layout(location = 0) in vec2 v_uv;

layout(set = 0, binding = 7) uniform sampler2D gb_albedo;
layout(set = 0, binding = 8) uniform sampler2D gb_nm;
layout(set = 0, binding = 9) uniform sampler2D gb_depth;

layout(location = 0) out vec4 o_lit;

void main() {
    float depth = texture(gb_depth, v_uv).r;
    if (depth >= 1.0) { o_lit = vec4(u.sky.rgb, 1.0); return; }      // nothing drawn here: sky
    vec3 albedo = texture(gb_albedo, v_uv).rgb;
    vec4 nm = texture(gb_nm, v_uv);
    vec3 n = NORMALS[min(int(nm.r * 255.0 + 0.5), 5)];
    vec3 world = world_at(v_uv, depth);
    o_lit = vec4(shade_flat(albedo, n, nm.a, world), 1.0);
}

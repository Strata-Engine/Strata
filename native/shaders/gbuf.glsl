// The frame's geometry buffers, and what the light pass wrote for the frame
// before: hist_prev rgb sky light, a sun visibility; meta_prev r samples
// behind them, g view depth, b face.
layout(set = 0, binding = 7) uniform sampler2D gb_albedo;
layout(set = 0, binding = 8) uniform sampler2D gb_nm;
layout(set = 0, binding = 9) uniform sampler2D gb_depth;
layout(set = 0, binding = 15) uniform sampler2D hist_prev;
layout(set = 0, binding = 16) uniform sampler2D meta_prev;

// A world point's screen coordinate under a view-projection, and its view depth in z.
vec3 screen_of(mat4 vp, vec3 world) {
    vec4 c = vp * vec4(world, 1.0);
    return vec3(c.x / c.w * 0.5 + 0.5, 0.5 - c.y / c.w * 0.5, c.w);
}
bool on_screen(vec3 s) { return s.z > 0.0 && all(greaterThanEqual(s.xy, vec2(0.0))) && all(lessThan(s.xy, vec2(1.0))); }

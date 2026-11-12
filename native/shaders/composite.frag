// The lit image, to the screen.
layout(location = 0) in vec2 v_uv;
layout(set = 0, binding = 10) uniform sampler2D lit;
layout(location = 0) out vec4 o_color;

void main() {
    // under water the whole view sways a little
    vec2 uv = v_uv;
    if (u.water.w > 0.0 && u.light.x >= 1.0 && u.light.x < 9.0) uv = clamp(uv + 0.0035 * vec2(sin(uv.y * 31.0 + u.light.y * 1.9), sin(uv.x * 27.0 + u.light.y * 1.5)), 0.001, 0.999);
    vec3 c = texture(lit, uv).rgb;
    o_color = vec4(u.light.x >= 1.0 && u.light.x < 9.0 ? tonemap(c) : c, 1.0);
}

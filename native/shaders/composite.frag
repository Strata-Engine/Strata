// The lit image, to the screen.
layout(location = 0) in vec2 v_uv;
layout(set = 0, binding = 10) uniform sampler2D lit;
layout(location = 0) out vec4 o_color;

void main() {
    vec3 c = texture(lit, v_uv).rgb;
    o_color = vec4(u.light.x >= 1.0 && u.light.x < 9.0 ? tonemap(c) : c, 1.0);
}

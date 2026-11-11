// The lit image, to the screen.
layout(location = 0) in vec2 v_uv;
layout(set = 0, binding = 10) uniform sampler2D lit;
layout(location = 0) out vec4 o_color;

void main() { o_color = vec4(texture(lit, v_uv).rgb, 1.0); }

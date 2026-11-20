// The overlay's pixels: a flat colour, given as the screen shows it, its
// opacity scaled by the pass (1 where it is seen, less where drawn through).
layout(location = 0) flat in vec4 v_rgba;
layout(push_constant) uniform PC { float alpha; float unused; } pc;
layout(location = 0) out vec4 o_color;
void main() {
    float a = v_rgba.a * pc.alpha;
    if (a <= 0.0) discard;
    o_color = vec4(pow(v_rgba.rgb, vec3(2.2)), a);
}

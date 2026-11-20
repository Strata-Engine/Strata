// The overlay (Strata.Overlay): translucent quads and lines in metres over
// the finished frame, one item per INSTANCE, four vertices a strip. A line
// is a band of its width facing the eye. Every vertex is pulled 3 cm toward
// the eye so an item lying on a surface does not fight it for depth.
layout(location = 0) in vec4 a_head;    // kind (0 quad, 1 line), through, r, g
layout(location = 1) in vec4 a_head2;   // b, a, width, unused
layout(location = 2) in vec4 a_g0;      // geometry floats 0..3
layout(location = 3) in vec4 a_g1;      // 4..7
layout(location = 4) in vec4 a_g2;      // 8..11

layout(push_constant) uniform PC { float alpha; float unused; } pc;

layout(location = 0) flat out vec4 v_rgba;

void main() {
    int k = gl_VertexIndex & 3;
    vec3 p;
    if (a_head.x < 0.5) {
        vec3 c0 = a_g0.xyz, c1 = vec3(a_g0.w, a_g1.xy), c2 = vec3(a_g1.zw, a_g2.x), c3 = a_g2.yzw;
        p = k == 0 ? c0 : k == 1 ? c1 : k == 2 ? c2 : c3;
    } else {
        vec3 a = a_g0.xyz, b = vec3(a_g0.w, a_g1.xy);
        vec3 mid = 0.5 * (a + b);
        vec3 along = b - a;
        vec3 side = cross(along, u.eye.xyz - mid);
        float l = length(side);
        side = l > 1e-6 ? side / l : vec3(1.0, 0.0, 0.0);
        side *= 0.5 * a_head2.z;
        p = (k == 0 ? a - side : k == 1 ? a + side : k == 2 ? b - side : b + side);
    }
    vec3 to_eye = u.eye.xyz - p;
    float d = length(to_eye);
    if (d > 0.05) p += to_eye / d * 0.03;
    v_rgba = vec4(a_head.zw, a_head2.xy);
    gl_Position = to_clip(u.vp * vec4(p, 1.0));
}

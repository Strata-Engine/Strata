// The 2D layer: one quad per INSTANCE, given in device pixels from the
// window's top-left corner.
layout(location = 0) in vec4 a_rect;       // x0 y0 x1 y1, device pixels
layout(location = 1) in vec4 a_uv;         // u0 v0 u1 v1: 0..1 for a block texture, atlas pixels for the others
layout(location = 2) in vec4 a_rgba;       // sRGB, straight alpha
layout(location = 3) in vec4 a_kind;       // kind, layer, and two numbers the kind uses

layout(push_constant) uniform PC { vec2 screen; } pc;   // the frame's size in device pixels

layout(location = 0) out vec2 v_uv;
layout(location = 1) flat out vec4 v_rgba;
layout(location = 2) flat out vec4 v_kind;

void main() {
    vec2 c = vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1));
    vec2 p = mix(a_rect.xy, a_rect.zw, c);
    v_uv = mix(a_uv.xy, a_uv.zw, c);
    v_rgba = a_rgba;
    v_kind = a_kind;
    gl_Position = vec4(p / pc.screen * 2.0 - 1.0, 0.0, 1.0);
}

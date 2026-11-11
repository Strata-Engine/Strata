// Spray: one particle per INSTANCE, a camera-facing square flown from its
// birth along a fixed path. Nothing is simulated: the position at time t is
// start + velocity * t + gravity * t * t / 2, worked out here from the clock.
layout(location = 0) in vec4 a_start;      // xyz start (metres), w birth time (seconds)
layout(location = 1) in vec4 a_vel;        // xyz velocity (m/s), w life (seconds)
layout(location = 2) in vec2 a_look;       // x texture layer, y size (metres)

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec3 v_world;
layout(location = 2) flat out float v_layer;
layout(location = 3) flat out float v_age;   // 0 at birth, 1 at the end of its life

void main() {
    float t = u.light.y - a_start.w;
    if (t < 0.0 || t > a_vel.w) { gl_Position = vec4(2.0, 2.0, 2.0, 1.0); v_uv = vec2(0.0); v_world = vec3(0.0); v_layer = 0.0; v_age = 1.0; return; }
    vec3 pos = a_start.xyz + a_vel.xyz * t + vec3(0.0, -4.9, 0.0) * t * t;
    vec2 corner = vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1));
    vec4 clip = u.vp * vec4(pos, 1.0);
    // the projection's two focal lengths are the lengths of vp's first two rows
    vec2 focal = vec2(length(vec3(u.vp[0][0], u.vp[1][0], u.vp[2][0])), length(vec3(u.vp[0][1], u.vp[1][1], u.vp[2][1])));
    clip.xy += (corner - 0.5) * a_look.y * focal;
    v_uv = corner;
    v_world = pos;
    v_layer = a_look.x;
    v_age = t / max(a_vel.w, 1.0e-3);
    gl_Position = to_clip(clip);
}

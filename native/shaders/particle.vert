// Particles: one per INSTANCE, a camera-facing square flown from its birth
// along a path worked out here from the clock. Nothing is simulated: the
// record (strata_particles.h) holds the start, and the effect table how an
// effect's particles move, grow and colour over their life.
//
// The ring is drawn twice, shaded (pc.pass 0) then glowing (pc.pass 1);
// each pass sends the other look's particles off screen. Effect 0 is
// spray: its path, size and fade are kept exactly as they were.
layout(location = 0) in vec4 a_start;      // xyz start (metres), w birth time (seconds)
layout(location = 1) in vec4 a_vel;        // xyz velocity (m/s), w life (seconds)
layout(location = 2) in vec4 a_misc;       // x effect, y size scale, z seed 0..1, w rotation at birth
layout(location = 3) in vec4 a_more;       // x texture layer override (-1: the effect's)

layout(push_constant) uniform PC { uint pass; } pc;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec3 v_world;
layout(location = 2) flat out float v_layer;
layout(location = 3) flat out float v_age;   // 0 at birth, 1 at the end of its life
layout(location = 4) flat out vec4 v_color;  // the colour ramp at this age: rgb, opacity
layout(location = 5) flat out vec4 v_light;  // a shaded particle lit once: rgb the light it takes, w 1 when set
layout(location = 6) flat out int v_effect;

vec4 fxv(int e, int k) { return fx.v[e * 10 + k]; }

// the colour ramp: up to four keys (rgb, opacity) at ages
vec4 colour_at(int e, float age) {
    int n = clamp(int(fxv(e, 0).w), 1, 4);
    vec4 ages = fxv(e, 5);
    if (n == 1 || age <= ages.x) return fxv(e, 1);
    for (int i = 1; i < n; i++) {
        if (age <= ages[i]) return mix(fxv(e, i), fxv(e, 1 + i), clamp((age - ages[i - 1]) / max(ages[i] - ages[i - 1], 1.0e-4), 0.0, 1.0));
    }
    return fxv(e, n);
}
// the size ramp: up to four keys (metres) at ages
float size_at(int e, float age) {
    int n = clamp(int(fxv(e, 8).x), 1, 4);
    vec4 sizes = fxv(e, 6), ages = fxv(e, 7);
    if (n == 1 || age <= ages.x) return sizes.x;
    for (int i = 1; i < n; i++) {
        if (age <= ages[i]) return mix(sizes[i - 1], sizes[i], clamp((age - ages[i - 1]) / max(ages[i] - ages[i - 1], 1.0e-4), 0.0, 1.0));
    }
    return sizes[n - 1];
}

void off() { gl_Position = vec4(2.0, 2.0, 2.0, 1.0); v_uv = vec2(0.0); v_world = vec3(0.0); v_layer = 0.0; v_age = 1.0; v_color = vec4(0.0); v_light = vec4(0.0); v_effect = 0; }

void main() {
    int e = clamp(int(a_misc.x), 0, 63);
    float t = u.light.y - a_start.w;
    if (t < 0.0 || t > a_vel.w || uint(fxv(e, 0).x) != pc.pass) { off(); return; }
    vec4 dyn = fxv(e, 8);      // size keys, drag, wobble metres, wobble Hz
    vec4 acc = fxv(e, 9);      // ax, ay, az, spin
    float k = dyn.y;
    vec3 pos;
    if (k > 1.0e-6) pos = a_start.xyz + (a_vel.xyz - acc.xyz / k) * (1.0 - exp(-k * t)) / k + (acc.xyz / k) * t;
    else pos = a_start.xyz + a_vel.xyz * t + (0.5 * acc.xyz) * t * t;
    if (dyn.z > 0.0) {
        float ph = 6.2831853 * (dyn.w * t + a_misc.z);
        pos += vec3(sin(ph), 0.0, sin(ph + 1.7)) * dyn.z;
    }
    float age = t / max(a_vel.w, 1.0e-3);
    float size = size_at(e, age) * a_misc.y;
    vec2 corner = vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1));
    vec2 c = corner - 0.5;
    float turn = a_misc.w + acc.w * t;
    if (turn != 0.0) c = mat2(cos(turn), sin(turn), -sin(turn), cos(turn)) * c;
    vec4 clip = u.vp * vec4(pos, 1.0);
    // the projection's two focal lengths are the lengths of vp's first two rows
    vec2 focal = vec2(length(vec3(u.vp[0][0], u.vp[1][0], u.vp[2][0])), length(vec3(u.vp[0][1], u.vp[1][1], u.vp[2][1])));
    clip.xy += c * size * focal;
    v_uv = corner;
    v_world = pos;
    v_layer = a_more.x >= 0.0 ? a_more.x : fxv(e, 0).y;
    v_age = age;
    v_color = colour_at(e, age);
    v_effect = e;
    v_light = vec4(0.0);
    // a shaded effect lit once a particle: the sun where it reaches the particle's middle, less sky under a roof
    if (pc.pass == 0u && fxv(e, 0).z > 0.5) {
        vec3 l = vec3(1.0);
        if (u.light.x >= 1.0 && u.light.x < 9.0) {
            vec3 at = pos / u.eye.w;
            float sun = 1.0, open_sky = 1.0;
            if (u.light.x >= 2.0) {
                Hit h;
                sun = u.sun.y > 0.0 && !trace(at, u.sun.xyz, 384.0, false, h) ? 1.0 : 0.0;
                open_sky = trace(at, vec3(0.0, 1.0, 0.0), 24.0, false, h) ? 0.3 : 1.0;
            }
            l = u.sun_color.rgb * max(u.sun.y, 0.0) * 0.7 * sun + sky_flat(vec3(0.0, 1.0, 0.0)) * open_sky + 0.03;
        }
        v_light = vec4(l, 1.0);
    }
    gl_Position = to_clip(clip);
}

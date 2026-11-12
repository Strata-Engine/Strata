// Particles: one per INSTANCE, a camera-facing square flown from its birth
// along a path worked out here from the clock. Nothing is simulated: the
// record (strata_particles.h) holds the start, and the effect table how an
// effect's particles move, grow and colour over their life.
//
// A particle is 36 vertices: a cube about its middle (twelve triangles,
// counter-clockwise from outside, as strata_particles.h's pt_cube_vertex),
// or a square facing the eye (two triangles each way round, the rest
// off screen). The ring is drawn twice, shaded (pc.pass 0) then glowing
// (pc.pass 1); each pass sends the other look's particles off screen.
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
layout(location = 5) flat out vec4 v_light;  // a shaded particle lit once: x sun visibility, y open sky, w 1 when set
layout(location = 6) flat out int v_effect;
layout(location = 7) flat out vec4 v_normal; // xyz a cube face's outward normal, w 1 for a cube

const vec3 CN[6] = vec3[6](vec3(0, 1, 0), vec3(0, -1, 0), vec3(1, 0, 0), vec3(-1, 0, 0), vec3(0, 0, 1), vec3(0, 0, -1));
const vec3 CT1[6] = vec3[6](vec3(0, 0, 1), vec3(1, 0, 0), vec3(0, 1, 0), vec3(0, 0, 1), vec3(1, 0, 0), vec3(0, 1, 0));
const vec3 CT2[6] = vec3[6](vec3(1, 0, 0), vec3(0, 0, 1), vec3(0, 0, 1), vec3(0, 1, 0), vec3(0, 1, 0), vec3(1, 0, 0));
const float CS[6] = float[6](0.0, 1.0, 1.0, 0.0, 1.0, 0.0);
const float CTT[6] = float[6](0.0, 0.0, 1.0, 0.0, 1.0, 1.0);

// v turned by [a] radians about the unit axis k
vec3 turn_about(vec3 v, vec3 k, float a) { return v * cos(a) + cross(k, v) * sin(a) + k * dot(k, v) * (1.0 - cos(a)); }

vec4 fxv(int e, int k) { return fx.v[e * 11 + k]; }

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

void off() { gl_Position = vec4(2.0, 2.0, 2.0, 1.0); v_uv = vec2(0.0); v_world = vec3(0.0); v_layer = 0.0; v_age = 1.0; v_color = vec4(0.0); v_light = vec4(0.0); v_effect = 0; v_normal = vec4(0.0); }

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
    float turn = a_misc.w + acc.w * t;
    int vi = gl_VertexIndex;
    vec4 clip;
    vec3 at_vertex = pos;
    if (fxv(e, 10).x > 0.5) {
        // a cube: face vi / 6, tumbling about an axis the particle's seed picks
        int f = vi / 6, c = vi % 6;
        vec3 n = CN[f];
        vec3 q = 0.5 * n + (CS[c] - 0.5) * CT1[f] + (CTT[c] - 0.5) * CT2[f];
        if (turn != 0.0) {
            vec3 k = normalize(vec3(sin(6.2831853 * a_misc.z), 0.6, cos(6.2831853 * a_misc.z)));
            q = turn_about(q, k, turn);
            n = turn_about(n, k, turn);
        }
        at_vertex = pos + q * size;
        clip = u.vp * vec4(at_vertex, 1.0);
        v_uv = vec2(CS[c], 1.0 - CTT[c]);
        v_normal = vec4(n, 1.0);
    } else {
        // a square facing the eye: two triangles, then the same two the other way round
        if (vi >= 12) { off(); return; }
        int c = vi < 6 ? vi : 11 - vi;
        vec2 corner = vec2(CS[c], CTT[c]);
        vec2 sq = corner - 0.5;
        if (turn != 0.0) sq = mat2(cos(turn), sin(turn), -sin(turn), cos(turn)) * sq;
        clip = u.vp * vec4(pos, 1.0);
        // the projection's two focal lengths are the lengths of vp's first two rows
        vec2 focal = vec2(length(vec3(u.vp[0][0], u.vp[1][0], u.vp[2][0])), length(vec3(u.vp[0][1], u.vp[1][1], u.vp[2][1])));
        clip.xy += sq * size * focal;
        v_uv = corner;
        v_normal = vec4(0.0, 1.0, 0.0, 0.0);
    }
    v_world = at_vertex;
    v_layer = a_more.x >= 0.0 ? a_more.x : fxv(e, 0).y;
    v_age = age;
    v_color = colour_at(e, age);
    v_effect = e;
    v_light = vec4(0.0);
    // a shaded effect lit once a particle: the sun where it reaches the particle's middle, less sky under a roof
    // (traced at its first vertex only would differ per vertex; every vertex traces the same middle, so they agree)
    if (pc.pass == 0u && fxv(e, 0).z > 0.5) {
        float sun = 1.0, open_sky = 1.0;
        if (u.light.x >= 2.0 && u.light.x < 9.0) {
            vec3 at = pos / u.eye.w;
            Hit h;
            sun = u.sun.y > 0.0 && !trace(at, u.sun.xyz, 384.0, false, h) ? 1.0 : 0.0;
            open_sky = trace(at, vec3(0.0, 1.0, 0.0), 24.0, false, h) ? 0.3 : 1.0;
        }
        v_light = vec4(sun, open_sky, 0.0, 1.0);
    }
    gl_Position = to_clip(clip);
}

// The particles' effect table: 64 effects of eleven vec4s (strata_particles.h).
layout(std430, set = 0, binding = 34) readonly buffer Effects { vec4 v[]; } fx;

vec4 fxv(int e, int k) { return fx.v[e * 11 + k]; }

// The flight particle.vert draws and collide.comp walks: where a particle
// that left p0 at v0 is after t seconds under acceleration a and drag k.
vec3 pt_fly(vec3 p0, vec3 v0, vec3 a, float k, float t) {
    if (k > 1.0e-6) return p0 + (v0 - a / k) * (1.0 - exp(-k * t)) / k + (a / k) * t;
    return p0 + v0 * t + (0.5 * a) * t * t;
}
// ... and how fast it is going then
vec3 pt_vel(vec3 v0, vec3 a, float k, float t) {
    if (k > 1.0e-6) return a / k + (v0 - a / k) * exp(-k * t);
    return v0 + a * t;
}
// the sideways sway at t, phased by the particle's seed
vec3 pt_sway(float metres, float hz, float seed, float t) {
    float ph = 6.2831853 * (hz * t + seed);
    return vec3(sin(ph), 0.0, sin(ph + 1.7)) * metres;
}
// The faces a particle may meet, as trace.glsl numbers them: 0 +y, 1 -y, 2 +x, 3 -x, 4 +z, 5 -z.
const vec3 PT_FACE[6] = vec3[6](vec3(0, 1, 0), vec3(0, -1, 0), vec3(1, 0, 0), vec3(-1, 0, 0), vec3(0, 0, 1), vec3(0, 0, -1));
// v after a bounce off the face whose normal is n: the part along n turned
// back and scaled by [restitution], the rest scaled by [keep].
vec3 pt_bounced(vec3 v, vec3 n, float restitution, float keep) {
    float vn = dot(v, n);
    return (v - vn * n) * keep - n * (vn * restitution);
}

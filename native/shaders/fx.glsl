// The particles' effect table: 64 effects of ten vec4s (strata_particles.h).
layout(std430, set = 0, binding = 34) readonly buffer Effects { vec4 v[]; } fx;

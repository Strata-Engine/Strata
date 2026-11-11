// Mirror reflections and the sun's highlight.

// The sun's highlight on a surface with normal n seen along [toward_eye],
// for a roughness 0..1: none when fully rough, small and bright when smooth.
vec3 sun_highlight(vec3 n, vec3 toward_eye, float rough, float sun_vis) {
    float gloss = 1.0 - rough;
    if (gloss <= 0.0 || sun_vis <= 0.0 || dot(n, u.sun.xyz) <= 0.0) return vec3(0.0);
    vec3 h = normalize(u.sun.xyz + toward_eye);
    float p = exp2(1.0 + 11.0 * gloss);
    return u.sun_color.rgb * sun_vis * pow(max(dot(n, h), 0.0), p) * (0.25 + 6.0 * gloss * gloss * gloss) * gloss;
}

// How much a surface mirrors at this angle: a little head-on, nearly all at a glance.
float mirror_share(vec3 n, vec3 toward_eye, float head_on) {
    float c = 1.0 - clamp(dot(n, toward_eye), 0.0, 1.0);
    float c2 = c * c;
    return head_on + (1.0 - head_on) * c2 * c2 * c;
}

// The mirror direction off a surface whose true normal is n and whose shading
// normal is m, kept from dipping back into the surface.
vec3 mirror_dir(vec3 toward_eye, vec3 m, vec3 n) {
    vec3 d = reflect(-toward_eye, m);
    float up = dot(d, n);
    return up < 0.03 ? normalize(d + n * (0.03 - up)) : d;
}

// What a mirror ray from [at] (voxel units) along unit [dir] sees. A hit that
// was on screen last frame takes its surface colour from the screen and its
// sun visibility from the light pass's memory; any other hit takes its class's
// colour and one shadow ray; a miss is sky.
vec3 reflected(vec3 at, vec3 dir) {
    Hit h;
    // exact for the first 40 voxels, every third voxel beyond: far reflections lose thin things
    if (!trace_steps(at, dir, 400.0, true, 200, 40.0, h)) return sky_color(dir) + h.glow * GLOW;
    vec3 hv = at + dir * h.t;
    vec3 hp = hv * u.eye.w;
    vec3 hn = NORMALS[h.face];
    vec3 was = screen_of(u.prev_vp, hp);
    vec3 now = screen_of(u.vp, hp);
    vec3 albedo = class_color(h.cls);
    float vis = -1.0;
    if (on_screen(was) && on_screen(now)) {
        vec4 om = texture(meta_prev, was.xy);
        if (om.r > 0.0 && abs(om.g - was.z) < 0.03 * was.z + 0.4) {
            vis = texture(hist_prev, was.xy).a;
            albedo = texture(gb_albedo, now.xy).rgb;
        }
    }
    if (vis < 0.0) vis = sun_visible(hv, hn, u.sun.xyz);
    vec3 col = albedo * (u.sun_color.rgb * max(dot(hn, u.sun.xyz), 0.0) * vis + sky_flat(hn)) + (class_emission(h.cls) + h.glow) * GLOW;
    return fogged(col, hp);
}

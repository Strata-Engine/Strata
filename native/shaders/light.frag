// The light pass: one fragment per screen pixel reads the geometry buffers
// and writes the lit colour.
layout(location = 0) in vec2 v_uv;

layout(location = 0) out vec4 o_lit;
layout(location = 1) out vec4 o_hist;
layout(location = 2) out vec4 o_meta;

// A colour for a light class, for looking at the volume.
vec3 class_tint(uint c) {
    vec3 p = fract(vec3(float(c)) * vec3(0.1031, 0.1030, 0.0973) + vec3(0.33, 0.71, 0.19));
    p += dot(p, p.yzx + 33.33);
    return 0.25 + 0.75 * fract((p.xxy + p.yzz) * p.zyx);
}

void main() {
    o_hist = vec4(0.0); o_meta = vec4(0.0);
    float depth = texture(gb_depth, v_uv).r;
    int mode = int(u.light.x);
    if (depth >= 1.0) {                                               // nothing drawn here: sky
        // under water there is no sky to see, only water
        o_lit = vec4(mode >= 1 && mode < 9 ? (submerged() ? water_haze() : sky_color(normalize(world_at(v_uv, 1.0) - u.eye.xyz))) : u.sky.rgb, 1.0);
        return;
    }
    vec3 albedo = texture(gb_albedo, v_uv).rgb;
    vec4 nm = texture(gb_nm, v_uv);
    int r8 = int(nm.r * 255.0 + 0.5);
    vec3 n = r8 < 16 ? NORMALS[min(r8, 5)] : oct_dec(vec2(float(r8 - 16) / 239.0, nm.g));
    int face = r8 < 16 ? min(r8, 5) : nearest_face(n);
    vec3 world = world_at(v_uv, depth);
    if (mode == 9) {
        // look at the light volume: step back off the surface, trace into it,
        // and show the class of the voxel found there (black where there is none)
        Hit h;
        vec3 ro = world / u.eye.w + n * 0.25;
        bool hit = trace(ro, -n, 1.5, true, h);
        o_lit = vec4(hit ? class_tint(h.cls) * (0.6 + 0.4 * max(dot(n, u.sun.xyz), 0.0)) : vec3(0.0), 1.0);
        return;
    }
    if (mode == 0) { o_lit = vec4(shade_flat(albedo, n, nm.a, world), 1.0); return; }
    float ndl = max(dot(n, u.sun.xyz), 0.0);
    // What is under the surface the eye is under gets less light the deeper it lies, and the sun's ripples.
    bool wet = submerged() && world.y < u.water2.x;
    vec3 dim = wet ? depth_light(world.y) : vec3(1.0);
    float ripple = wet ? caustics(world) : 1.0;
    if (mode == 1) { o_lit = vec4(fogged(albedo * (u.sun_color.rgb * ndl * dim * ripple + sky_flat(n) * dim), world), 1.0); return; }

    // What the same surface point had last frame, if it was on screen and is
    // the same surface: same face, same distance from that frame's eye.
    ivec2 px = ivec2(gl_FragCoord.xy);
    int frame = int(u.light.z);
    vec3 at = world / u.eye.w;
    float view_depth = (u.vp * vec4(world, 1.0)).w;
    vec4 pc = u.prev_vp * vec4(world, 1.0);
    vec2 puv = vec2(pc.x / pc.w * 0.5 + 0.5, 0.5 - pc.y / pc.w * 0.5);
    float kept = 0.0;
    vec4 old = vec4(0.0);
    if (u.vol.w > 0 && pc.w > 0.0 && all(greaterThanEqual(puv, vec2(0.0))) && all(lessThan(puv, vec2(1.0)))) {
        vec4 om = texture(meta_prev, puv);
        if (abs(om.g - pc.w) < 0.02 * pc.w + 0.05 && int(om.b + 0.5) == face) {
            kept = min(om.r, float(u.vol.w));
            old = texture(hist_prev, puv);
        }
    }
    // A pixel with a settled memory takes a new sample one frame in four, in
    // a pattern that moves round each 2 x 2 block; a pixel with little
    // memory, and every pixel while an edit settles, samples every frame.
    // An edit unsettles the pixels it can reach: those within 32 voxels of the chunks it changed.
    // So does a sun that has just moved, everywhere.
    bool settling = u.vol.w <= 2 || u.edit_lo.w > 0.5 && all(greaterThan(at, u.edit_lo.xyz - 32.0)) && all(lessThan(at, u.edit_hi.xyz + 32.0));
    if (settling) kept = min(kept, 2.0);
    bool sample_now = kept < 4.0 || settling || (((px.x >> 3) & 1) + 2 * ((px.y >> 3) & 1)) == (frame & 3);
    float sun_vis = old.a;
    int rays = mode >= 4 ? int(u.light.w) : 0;
    vec3 sky = rays > 0 ? old.rgb : sky_flat(n);
    float count = kept;
    if (sample_now) {
        float sun_now = sun_visible(at, n, sun_toward(rand2(px, frame, 0)));
        // while an edit settles, a pixel whose new sample contradicts its memory starts again
        if (settling && abs(old.a - sun_now) > 0.6) kept = 0.0;
        // shadows lean on 64 past samples at most, so they follow a moving sun; sky light on all of them, since a small bright thing is found by few rays
        sun_vis = mix(old.a, sun_now, 1.0 / (min(kept, 64.0) + 1.0));
        if (rays > 0) sky = mix(old.rgb, sky_light(at, n, px, frame, rays), 1.0 / (kept + 1.0));
        count = kept + 1.0;
    }
    o_hist = vec4(sky, sun_vis);
    o_meta = vec4(count, view_depth, float(face), 0.0);

    // the pixel's light class: flat quads carry it; models and plants do not, and are looked up
    uint cls = uint(nm.b * 255.0 + 0.5);
    if (cls == 0u) cls = class_at(at - n * 0.01);
    vec3 col = albedo * (u.sun_color.rgb * ndl * sun_vis * dim * ripple + sky * dim);
    if (mode >= 4) col += albedo * class_emission(cls) * GLOW;
    if (mode >= 3) {
        // the sun's highlight on anything not fully rough; a mirror ray for what is smooth
        float rough = cls == 0u ? 1.0 : float(class_roughness(cls)) / 255.0;
        vec3 toward_eye = normalize(u.eye.xyz - world);
        if (rough < 0.25 && u.sun_color.w > 0.5) {
            float share = mirror_share(n, toward_eye, 0.12) * (1.0 - rough);
            col = mix(col, reflected(at + n * 0.02, mirror_dir(toward_eye, n, n), -1000000), share);
        }
        col += sun_highlight(n, toward_eye, rough, sun_vis);
    }
    o_lit = vec4(fogged(col, world), 1.0);
}

// The light pass: one fragment per screen pixel reads the geometry buffers
// and writes the lit colour.
layout(location = 0) in vec2 v_uv;

layout(location = 0) out vec4 o_lit;
layout(location = 1) out vec4 o_hist;
layout(location = 2) out vec4 o_meta;
// boxes (voxels, low and high corners) round bodies that moved this frame, then round traced figures that moved lately (strata_vk.c, fig_trace_prepare):
// the low corner's w 1 for a figure's shadow box (the sun term alone), 0 for the rest (every term)
layout(std430, set = 0, binding = 21) readonly buffer Movers { vec4 boxes[]; } movers;

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
    vec4 alb = texture(gb_albedo, v_uv);
    vec3 albedo = alb.rgb;
    // a figure's pixel: its albedo's alpha holds whether it moved and its emission (gbuffer.frag); the world's is 1
    int fcode = int(alb.a * 255.0 + 0.5);
    bool figure = fcode < 128;
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
    // a chunk above level 0 (the far field) is lit by the sun and sky alone, as mode 1 lights everything:
    // it is not in the light volume, and its rays would start inside the finer world under it
    bool far = !figure && r8 < 16 && nm.g > 0.5;
    if (mode == 1 || far) { o_lit = vec4(fogged(albedo * (u.sun_color.rgb * ndl * dim * ripple + sky_flat(n) * dim), world), 1.0); return; }

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
    // so does a body moving near it (its own surface, its shadow), or a glowing traced figure (the light it gives);
    // a moving traced figure's shadow box (w 1) samples the sun alone every frame, the sky keeping its schedule and memory
    bool shade_moves = false;
    for (int m = 0; m < u.bodies.x && m < 144; m++)
        if (all(greaterThan(at, movers.boxes[2 * m].xyz)) && all(lessThan(at, movers.boxes[2 * m + 1].xyz))) {
            if (movers.boxes[2 * m].w > 0.5) shade_moves = true; else settling = true;
        }
    // and a figure that moved since the last frame, on its own surface
    if (figure && (fcode & 64) != 0) settling = true;
    if (settling) kept = min(kept, 2.0);
    bool sample_now = kept < 4.0 || settling || (((px.x >> 3) & 1) + 2 * ((px.y >> 3) & 1)) == (frame & 3);
    // sky light changes slowly and costs most (its rays go far): a settled pixel traces it one frame in sixteen, in a pattern that moves round each
    // 4 x 4 block of 8-pixel tiles, against the sun's one in four
    // A pixel with little memory, or one settling, traces sky light one frame in four, a pixel of each 2 x 2 block in turn (so a quarter of the
    // pixels newly in view trace it each frame, not all of them: turning the view had made the light pass three times as slow); until its first
    // sample it is lit by the flat sky term. The sun's ray, which is cheaper, it still traces every frame.
    bool fresh = kept < 4.0 || settling;
    bool sky_now = fresh ? ((px.x & 1) + 2 * (px.y & 1)) == (frame & 3) : (((px.x >> 3) & 3) + 4 * ((px.y >> 3) & 3)) == (frame & 15);
    float sun_vis = old.a;
    int rays = mode >= 4 ? int(u.light.w) : 0;
    vec3 sky = rays > 0 ? (kept > 0.0 ? old.rgb : sky_flat(n)) : sky_flat(n);
    float count = kept;
    if (sample_now || shade_moves) {
        float sun_now = sun_visible(at, n, sun_toward(rand2(px, frame, 0)));
        // while an edit settles, a pixel whose new sample contradicts its memory starts again
        if (settling && abs(old.a - sun_now) > 0.6) kept = 0.0;
        // in a moving figure's shadow box the sun leans on 2 past samples, and none when the new one contradicts them
        float ks = shade_moves ? min(kept, 2.0) : kept;
        if (shade_moves && abs(old.a - sun_now) > 0.6) ks = 0.0;
        // shadows lean on 64 past samples at most, so they follow a moving sun; sky light on all of them, since a small bright thing is found by few rays
        sun_vis = mix(old.a, sun_now, 1.0 / (min(ks, 64.0) + 1.0));
        if (sample_now) {
            if (rays > 0 && sky_now) sky = mix(old.rgb, sky_light(at, n, px, frame, rays), 1.0 / (min(kept, 255.0) / 4.0 + 1.0));
            count = kept + 1.0;
        }
    }
    o_hist = vec4(sky, sun_vis);
    o_meta = vec4(count, view_depth, float(face), 0.0);
    // the plain figures' shadow, from this frame's map, the darker of it and the
    // traced shadow; never remembered, so a moving figure's shadow leaves no trail
    if (u.fshadow.x > 0.5 && ndl > 0.0) sun_vis = min(sun_vis, fig_shadow(world, n));

    // the pixel's light class: flat quads carry it; models and plants do not, and are looked up
    // a figure carries its roughness there instead, and its emission in the albedo's alpha
    uint cls = figure ? 0u : uint(nm.b * 255.0 + 0.5);
    if (cls == 0u && !figure) cls = class_at(at - n * 0.01);
    vec3 col = albedo * (u.sun_color.rgb * ndl * sun_vis * dim * ripple + sky * dim);
    if (mode >= 4) col += figure ? albedo * (float(fcode & 63) / 63.0) * GLOW : albedo * class_emission(cls) * GLOW;
    if (mode >= 3) {
        // the sun's highlight on anything not fully rough; a mirror ray for what is smooth
        float rough = figure ? nm.b : (cls == 0u ? 1.0 : float(class_roughness(cls)) / 255.0);
        vec3 toward_eye = normalize(u.eye.xyz - world);
        if (rough < 0.25 && u.sun_color.w > 0.5) {
            float share = mirror_share(n, toward_eye, 0.12) * (1.0 - rough);
            col = mix(col, reflected(at + n * 0.02, mirror_dir(toward_eye, n, n), -1000000), share);
        }
        col += sun_highlight(n, toward_eye, rough, sun_vis);
    }
    o_lit = vec4(fogged(col, world), 1.0);
    // light mode 5, for finding grain: lit as 4, but each pixel shows its memory: grey by how many past samples it leans on (white 64 and more),
    // red where it was made to start over this frame (an edit near it, a moving body, a moved sun), blue where a moving figure's shadow box is
    if (mode == 5) o_lit = vec4(settling ? vec3(0.9, 0.1, 0.1) : (shade_moves ? vec3(0.1, 0.3, 0.9) : vec3(min(count, 64.0) / 64.0)), 1.0);
}

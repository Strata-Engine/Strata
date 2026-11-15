// The light pass: one fragment per screen pixel reads the geometry buffers
// and what the gather pass traced, and writes the lit colour.
layout(location = 0) in vec2 v_uv;

layout(location = 0) out vec4 o_lit;
// what the gather pass wrote this frame (gather.frag): rgb sky light, a sun visibility; and r samples, g view depth, b face, a its state
layout(set = 0, binding = 35) uniform sampler2D hist_cur;
layout(set = 0, binding = 36) uniform sampler2D hmeta_cur;

// A colour for a light class, for looking at the volume.
vec3 class_tint(uint c) {
    vec3 p = fract(vec3(float(c)) * vec3(0.1031, 0.1030, 0.0973) + vec3(0.33, 0.71, 0.19));
    p += dot(p, p.yzx + 33.33);
    return 0.25 + 0.75 * fract((p.xxy + p.yzz) * p.zyx);
}

void main() {
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

    // The sky light and sun visibility the gather pass found (gather.frag), from the gathered pixels round this one that are the same
    // surface: same face, about the same distance from the eye, weighed by how near they are. A pixel none of them matches (an edge,
    // something thinner than a gathered pixel) traces the sun itself and takes the flat sky.
    vec3 at = world / u.eye.w;
    float view_depth = (u.vp * vec4(world, 1.0)).w;
    float s = u.trace.x;
    ivec2 hsize = textureSize(hist_cur, 0);
    vec2 hp = (gl_FragCoord.xy - 0.5) / s;                // where this pixel falls among the gathered ones (their blocks' top left pixels)
    ivec2 hc = ivec2(floor(hp + 0.5));
    // at one gathered pixel per screen pixel, just its own; else the 3 x 3 round the nearest, by a tent 1.5 gathered pixels wide,
    // which spreads each one's few samples over the screen pixels it stands for and its neighbours'
    int r = s > 1.0 ? 1 : 0;
    vec4 acc = vec4(0.0); float wsum = 0.0; vec4 meta = vec4(0.0); float wbest = 0.0;
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++) {
            ivec2 hq = clamp(hc + ivec2(dx, dy), ivec2(0), hsize - 1);
            vec2 d = abs(vec2(hq) - hp);
            float wb = max(1.5 - d.x, 0.0) * max(1.5 - d.y, 0.0);
            if (wb <= 0.0) continue;
            vec4 m = texelFetch(hmeta_cur, hq, 0);
            if (m.g <= 0.0 || int(m.b + 0.5) != face || abs(m.g - view_depth) > 0.03 * view_depth + 0.05) continue;
            acc += wb * texelFetch(hist_cur, hq, 0); wsum += wb;
            if (wb > wbest) { wbest = wb; meta = m; }
        }
    // none: a face thinner than a gathered pixel; look two further out, at any distance along the same face's plane
    if (wsum == 0.0 && r > 0)
        for (int dy = -2; dy <= 2; dy++)
            for (int dx = -2; dx <= 2; dx++) {
                ivec2 hq = clamp(hc + ivec2(dx, dy), ivec2(0), hsize - 1);
                vec4 m = texelFetch(hmeta_cur, hq, 0);
                if (m.g <= 0.0 || int(m.b + 0.5) != face || abs(m.g - view_depth) > 0.1 * view_depth + 0.5) continue;
                float wb = 1.0 / (1.0 + float(dx * dx + dy * dy));
                acc += wb * texelFetch(hist_cur, hq, 0); wsum += wb;
                if (wb > wbest) { wbest = wb; meta = m; }
            }
    int rays = mode >= 4 ? int(abs(u.light.w)) : 0;
    vec3 sky; float sun_vis;
    if (wsum > 0.0) { sky = acc.rgb / wsum; sun_vis = acc.a / wsum; }
    else { sky = sky_flat(n); sun_vis = sun_visible(at, n, sun_toward(rand2(ivec2(gl_FragCoord.xy), int(u.light.z), 0))); }
    if (rays == 0) sky = sky_flat(n);
    float count = meta.r;
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
    if (mode == 5) o_lit = vec4(meta.a > 1.5 ? vec3(0.1, 0.3, 0.9) : (meta.a > 0.5 ? vec3(0.9, 0.1, 0.1) : vec3(min(count, 64.0) / 64.0)), 1.0);
}

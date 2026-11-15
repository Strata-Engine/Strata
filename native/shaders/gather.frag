// The gather pass: the light pass's rays, traced for one screen pixel in
// u.trace.x along each side (strata_vk.c, STRATA_TRACE_FULL). It writes, for
// the pixel at the top left of its block, the sky light and sun visibility
// it has gathered over the frames (hist) and what they belong to (meta: r
// samples behind them, g view depth, b face, a 1 where an edit or a moving
// body made it start over, 2 in a moving figure's shadow box, for light mode 5).
// The light pass (light.frag) spreads them over the full-sized screen.
layout(location = 0) in vec2 v_uv;

layout(location = 0) out vec4 o_hist;
layout(location = 1) out vec4 o_meta;
// boxes (voxels, low and high corners) round bodies that moved this frame, then round traced figures that moved lately (strata_vk.c, fig_trace_prepare):
// the low corner's w 1 for a figure's shadow box (the sun term alone), 0 for the rest (every term)
layout(std430, set = 0, binding = 21) readonly buffer Movers { vec4 boxes[]; } movers;

void main() {
    o_hist = vec4(0.0); o_meta = vec4(0.0);
    int s = int(u.trace.x);
    ivec2 px = ivec2(gl_FragCoord.xy);                    // this pass's pixel: the schedules and random numbers run on it
    ivec2 full = textureSize(gb_depth, 0);
    // the full-sized pixel it traces for: each of its block's in turn, frame by frame, so that its memory holds all of them
    // (one pixel always would put a block's whole light on whatever that pixel hit, a voxel's very edge for some)
    int turn = int(u.light.z) & 3;
    ivec2 fp = min(px * s + (s > 1 ? ivec2(turn & 1, turn >> 1) : ivec2(0)), full - 1);
    vec2 uv = (vec2(fp) + 0.5) / vec2(full);
    float depth = texelFetch(gb_depth, fp, 0).r;
    int mode = int(u.light.x);
    if (depth >= 1.0 || mode < 2 || mode == 9) return;    // sky, or lit without rays: nothing to gather
    vec4 alb = texelFetch(gb_albedo, fp, 0);
    int fcode = int(alb.a * 255.0 + 0.5);
    bool figure = fcode < 128;
    vec4 nm = texelFetch(gb_nm, fp, 0);
    int r8 = int(nm.r * 255.0 + 0.5);
    if (!figure && r8 < 16 && nm.g > 0.5) return;         // the far field: lit by the sun and sky alone
    vec3 n = r8 < 16 ? NORMALS[min(r8, 5)] : oct_dec(vec2(float(r8 - 16) / 239.0, nm.g));
    int face = r8 < 16 ? min(r8, 5) : nearest_face(n);
    vec3 world = world_at(uv, depth);

    // What the same surface point had last frame, if it was on screen and is
    // the same surface: same face, same distance from that frame's eye.
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
    bool full_sky = u.light.w < 0.0;   // STRATA_SKY_FULL: the schedule of before (stored pictures)
    bool sky_now = full_sky ? sample_now : fresh ? ((px.x & 1) + 2 * (px.y & 1)) == (frame & 3) : (((px.x >> 3) & 3) + 4 * ((px.y >> 3) & 3)) == (frame & 15);
    float sun_vis = old.a;
    int rays = mode >= 4 ? int(abs(u.light.w)) : 0;
    vec3 sky = rays > 0 ? (kept > 0.0 || u.light.w < 0.0 ? old.rgb : sky_flat(n)) : sky_flat(n);
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
            if (rays > 0 && sky_now) sky = mix(old.rgb, sky_light(at, n, px, frame, rays), full_sky ? 1.0 / (kept + 1.0) : 1.0 / (min(kept, 255.0) / 4.0 + 1.0));
            count = kept + 1.0;
        }
    }
    o_hist = vec4(sky, sun_vis);
    o_meta = vec4(count, view_depth, float(face), settling ? 1.0 : (shade_moves ? 2.0 : 0.0));
}

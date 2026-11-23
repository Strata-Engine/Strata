// The gather pass: the light pass's rays, traced for one screen pixel in
// u.trace.x along each side (strata_vk.c, STRATA_TRACE_FULL). It writes, for
// the pixel at the top left of its block, the sky light and sun visibility
// it has gathered over the frames (hist) and what they belong to (meta: r
// sun samples behind them, g view depth, b face, a the sky samples behind
// them; in light mode 5, a is instead 1 where an edit or a moving body made
// it start over, 2 in a moving figure's shadow box).
// The light pass (light.frag) spreads them over the full-sized screen.
layout(location = 0) in vec2 v_uv;

layout(location = 0) out vec4 o_hist;
layout(location = 1) out vec4 o_meta;
layout(location = 2) out vec4 o_lamp;   // rgb the lamps' light, a the samples behind it (when u.lamps.x is 1)
layout(set = 0, binding = 39) uniform sampler2D lamp_prev;
// boxes (voxels, low and high corners) round bodies that moved this frame, then round traced figures that moved lately (strata_vk.c, fig_trace_prepare):
// the low corner's w 1 for a figure's shadow box (the sun term alone), 0 for the rest (every term)
layout(std430, set = 0, binding = 21) readonly buffer Movers { vec4 boxes[]; } movers;

void main() {
    o_hist = vec4(0.0); o_meta = vec4(0.0); o_lamp = vec4(0.0);
    int s = int(u.trace.x);
    ivec2 px = ivec2(gl_FragCoord.xy);                    // this pass's pixel: the schedules and random numbers run on it
    ivec2 full = textureSize(gb_depth, 0);
    ivec2 fp = min(px * s, full - 1);                     // the full-sized pixel it traces for: its block's top left
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
    float kept = 0.0, skyc = 0.0;   // samples behind the remembered sun visibility, and behind the remembered sky light
    bool had = false;
    vec4 old = vec4(0.0);
    if (u.vol.w > 0 && pc.w > 0.0 && all(greaterThanEqual(puv, vec2(0.0))) && all(lessThan(puv, vec2(1.0)))) {
        vec4 om = texture(meta_prev, puv);
        if (abs(om.g - pc.w) < 0.02 * pc.w + 0.05 && int(om.b + 0.5) == face) {
            kept = min(om.r, float(u.vol.w));
            skyc = mode == 5 ? kept * 0.25 : om.a;
            old = texture(hist_prev, puv);
            had = true;
        }
    }
    // A sun that moves (strata_vk.c): memory gathered under another sun is leaned on less, the less the faster the sun goes.
    // It is shortened, not thrown away, and the pixel keeps its turn: one ray in four frames still follows the sun.
    float sun_cap = u.trace.z, sky_cap = u.trace.w;
    bool moving = sun_cap < 64.0;
    if (moving) kept = min(kept, sun_cap);
    skyc = min(skyc, sky_cap);
    // A pixel with a settled memory takes a new sample one frame in four, in
    // a pattern that moves round each 2 x 2 block; a pixel with little
    // memory, and every pixel while an edit settles, samples every frame.
    // An edit unsettles the pixels it can reach: those within 32 voxels of the chunks it changed.
    // So does a sun that has just moved, everywhere.
    // An edit that changed only what glows or a colour (edit_hi.w 1) unsettles the sky light alone: the shadows stand.
    bool edited = u.edit_lo.w > 0.5 && all(greaterThan(at, u.edit_lo.xyz - 32.0)) && all(lessThan(at, u.edit_hi.xyz + 32.0));
    bool settling = u.vol.w <= 2 || edited && u.edit_hi.w < 0.5;
    bool sky_settling = edited && u.edit_hi.w > 0.5;
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
    if (settling || sky_settling) skyc = min(skyc, 0.5);
    // under a moving sun the turns go round single pixels, not 8-pixel tiles: neighbours of different ages blend (light.frag) where tiles would show
    bool turn = moving ? ((px.x & 1) + 2 * (px.y & 1)) == (frame & 3) : (((px.x >> 3) & 1) + 2 * ((px.y >> 3) & 1)) == (frame & 3);
    // little memory: under 4 samples; or, under a moving sun, under half of what it lets a pixel keep (half, because the cap
    // changes from frame to frame with the sun's pace: against the cap itself every pixel was short whenever it rose)
    bool sample_now = !had || kept < (moving ? 0.5 * min(4.0, sun_cap) : 4.0) || settling || turn;
    // sky light changes slowly and costs most (its rays go far): a settled pixel traces it one frame in sixteen, in a pattern that moves round each
    // 4 x 4 block of 8-pixel tiles, against the sun's one in four
    // A pixel with little memory, or one settling, traces sky light one frame in four, a pixel of each 2 x 2 block in turn (so a quarter of the
    // pixels newly in view trace it each frame, not all of them: turning the view had made the light pass three times as slow); until its first
    // sample it is lit by the flat sky term. The sun's ray, which is cheaper, it still traces every frame.
    // The sky's turn is its own, not a share of the sun's: taken only on the sun's turn, half the tiles' turns never came and their sky light
    // stood still until an edit (the two patterns meet in half the tiles).
    bool fresh = skyc < (sky_cap < 64.0 ? 0.5 * min(1.0, sky_cap) : 1.0) || settling || sky_settling;
    bool full_sky = u.light.w < 0.0;   // STRATA_SKY_FULL: the schedule of before (stored pictures)
    bool sky_turn = moving ? ((px.x & 3) + 4 * (px.y & 3)) == (frame & 15) : (((px.x >> 3) & 3) + 4 * ((px.y >> 3) & 3)) == (frame & 15);
    bool sky_now = full_sky ? sample_now : fresh ? ((px.x & 1) + 2 * (px.y & 1)) == (frame & 3) : sky_turn;
    float sun_vis = old.a;
    int rays = mode >= 4 && mode != 8 ? int(abs(u.light.w)) : 0;   // mode 8: no rays (light.frag, direct_lamps)
    vec3 sky = rays > 0 ? (had || full_sky ? old.rgb : sky_flat(n)) : sky_flat(n);
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
        if (sample_now) count = kept + 1.0;
    }
    if (rays > 0 && sky_now) {
        sky = mix(old.rgb, sky_light(at, n, px, frame, rays), full_sky ? 1.0 / (kept + 1.0) : 1.0 / (skyc + 1.0));
        skyc = min(skyc + 1.0, 64.0);
    }
    // The lamps' light, where it is gathered here: every lamp its cell lists, one of the shadow-casting ones by a ray (lamp_gather).
    // Its memory is its own. Where a lamp moves it leans on 3 past samples and takes one every frame, so a headlight's pool
    // follows the car; under standing lamps alone it settles like the sun's shadow and keeps the sun's turn.
    if (u.lamps.x > 0.5 && u.water2.z > 0.5) {
        ivec3 cell = wrap(ivec3(floor(at)) >> 5, u.vol.xyz);
        uint standing = lamp_entry(cell, false).y, roving = lamp_entry(cell, true).y;
        if (standing + roving > 0u) {
            vec4 lold = had ? texture(lamp_prev, puv) : vec4(0.0);
            float lc = lold.a;
            if (settling || sky_settling) lc = min(lc, 1.0);
            if (roving > 0u) lc = min(lc, 3.0);
            if (!had || lc < 4.0 || settling || sky_settling || turn) {
                bool any_moving;
                vec3 now = lamp_gather(at, n, px, frame, any_moving);
                o_lamp = vec4(mix(lold.rgb, now, 1.0 / (min(lc, 32.0) + 1.0)), min(lc + 1.0, 64.0));
            } else o_lamp = lold;
        }
    }
    o_hist = vec4(sky, sun_vis);
    o_meta = vec4(count, view_depth, float(face), mode == 5 ? (settling ? 1.0 : (shade_moves ? 2.0 : 0.0)) : skyc);
}

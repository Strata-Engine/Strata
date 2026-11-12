// A plain figure piece's quads into the sun's shadow map (strata_vk.c,
// fig_shadow_record): depth only, seen from the sun (u.sun_vp, orthographic).
void main() {
    gl_Position = to_clip(u.sun_vp * vec4(fig_world(fig_corner()), 1.0));
}

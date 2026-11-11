// The uniform block every shader shares. The C side keeps one copy per frame
// in flight and fills it at the start of the frame (strata_vk.c, UBO_*).
layout(set = 0, binding = 0) uniform U {
    mat4 vp;         // view-projection, OpenGL clip conventions; `to_clip` converts
    vec4 sun;        // xyz direction to the sun, w ambient share (light mode 0)
    vec4 sky;        // flat sky colour (light mode 0)
    vec4 fog;        // x start, y end (metres)
    vec4 eye;        // xyz eye (metres), w metres per voxel
    ivec4 misc;      // x slots to cull, y commands per range, z texture layers, w rows in the block table
    mat4 inv_vp;     // inverse of vp
    mat4 prev_vp;    // last frame's vp
    vec4 light;      // x light mode, y time in seconds, z frame number, w sky rays per pixel
    vec4 sun_color;  // rgb the sun's colour
    ivec4 vol;       // xyz the light volume's size in chunks; w how many past samples a pixel may lean on
} u;

// OpenGL clip space to Vulkan's: y points down, depth runs 0..1.
vec4 to_clip(vec4 gl) { return vec4(gl.x, -gl.y, (gl.z + gl.w) * 0.5, gl.w); }

// The world position (metres) of the surface at screen coordinate uv (0..1,
// y down) and depth-buffer value depth.
vec3 world_at(vec2 uv, float depth) {
    vec4 p = u.inv_vp * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth * 2.0 - 1.0, 1.0);
    return p.xyz / p.w;
}

const vec3 NORMALS[6] = vec3[6](vec3(0, 1, 0), vec3(0, -1, 0), vec3(1, 0, 0), vec3(-1, 0, 0), vec3(0, 0, 1), vec3(0, 0, -1));

// Linear colour to the 0..1 the screen takes (light modes above 0): as it
// is up to 0.6, then rolled off so that bright things keep their colour.
vec3 tonemap(vec3 c) {
    vec3 over = max(c - 0.6, 0.0);
    return min(c, 0.6) + 0.4 * (1.0 - exp(-over / 0.4));
}

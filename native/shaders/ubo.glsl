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
    vec4 sun_color;  // rgb the sun's colour; w 1 when mirror rays are on
    ivec4 vol;       // xyz the light volume's size in chunks; w how many past samples a pixel may lean on
    vec4 edit_lo;    // xyz the low corner (voxels) of what an edit just changed; w 1 while it settles
    vec4 edit_hi;    // xyz its high corner; w 1 when it changed only what glows or a colour, not what stops the sun
    ivec4 bodies;    // x moving bodies' boxes in the movers buffer, y bodies in the body table
    vec4 water;      // the liquid the eye is under: rgb its colour, w how fast it swallows light per metre (0: the eye is in air)
    vec4 water2;     // x the height of that liquid's surface (metres); y 1 when water may keep its memory between frames (storage images); z 1 when there are lamps for the light pass to aim at
    mat4 sun_vp;     // the plain figures' shadow map: world (metres) to its clip space, orthographic from the sun, OpenGL conventions
    vec4 fshadow;    // x 1 when the map holds figures this frame (else it is not read), y one texel in its 0..1 coordinates, z depth per metre toward the sun
    vec4 trace;      // x screen pixels along each side of one the gather pass traces for (1..4); y the motion clock; z, w how many past samples the sun's and the sky's light may lean on (64: all, a sun standing still)
    vec4 cut[9];     // the cutaway (Strata.Overlay.cut_floats): has height, y, x0, z0, x1, z1, capsules, 0, then four capsules of 7 floats
    vec4 lamps;      // x 1 when the gather pass gathers the lamps' light (sv_set_lamp_tracing); y the voxels a lamp's light reaches; z a cell's lamps at most, w cells to list (lamps.comp)
    uvec4 lamp_mask[2]; // a bit a light class: its standing lamps cast shadows (sv_set_lamp_shadow)
    vec4 weather;    // sv_set_weather: x overcast 0..1, y how wet surfaces are 0..1, z a lightning flash's brightness 0..1, w rain 0..1 (streaks drawn over the view)
    vec4 rain;       // xyz the direction rain falls (unit, metres), w the rain's own clock in seconds
    vec4 cloud;      // sv_set_cloud_shadows: x how many blobs, y how much of the sun a blob's middle takes 0..1
    vec4 blobs[64];  // the clouds' shadow blobs: xyz a centre (metres), w its radius
    vec4 sea;        // sv_set_sea: xy the wind's direction along x and z (unit), z the open sea's wave height in metres (0: no sea state), w whitecaps 0..1
    vec4 sea_map;    // sv_set_sea_map: xy its low corner, z its cell size (metres)
    vec4 sea_grid;   // its cells across and down (x 0: no map)
} u;

// The cutaway: is the surface at p (metres) hidden this frame? Above the
// height inside its box, or inside a capsule and above the capsule's lower
// end (so the floor round the point the capsule reaches stays).
float cutf(int i) { return u.cut[i >> 2][i & 3]; }
bool cut_hidden(vec3 p) {
    if (cutf(0) > 0.5 && p.y > cutf(1) && p.x >= cutf(2) && p.x <= cutf(4) && p.z >= cutf(3) && p.z <= cutf(5)) return true;
    int n = int(cutf(6) + 0.5);
    for (int k = 0; k < 4; k++) {
        if (k >= n) break;
        int b = 8 + 7 * k;
        vec3 a = vec3(cutf(b), cutf(b + 1), cutf(b + 2)), c = vec3(cutf(b + 3), cutf(b + 4), cutf(b + 5));
        float r = cutf(b + 6);
        if (p.y <= min(a.y, c.y)) continue;
        vec3 ab = c - a;
        float t = clamp(dot(p - a, ab) / max(dot(ab, ab), 1e-6), 0.0, 1.0);
        if (length(p - (a + t * ab)) < r) return true;
    }
    return false;
}

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

// A body's slot carries a pose: quads are turned by its rotation q about its low corner.
vec3 qrot(vec4 q, vec3 v) { vec3 t = 2.0 * cross(q.xyz, v); return v + q.w * t + cross(q.xyz, t); }
bool turned(vec4 q) { return abs(q.w) < 0.9999995; }
// A normal in two bytes (octahedral, y up), for the G-buffer's tilted surfaces.
vec2 oct_enc(vec3 n) {
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    vec2 p = n.y >= 0.0 ? n.xz : (1.0 - abs(n.zx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.z >= 0.0 ? 1.0 : -1.0);
    return p * 0.5 + 0.5;
}
vec3 oct_dec(vec2 e) {
    vec2 f = e * 2.0 - 1.0;
    vec3 n = vec3(f.x, 1.0 - abs(f.x) - abs(f.y), f.y);
    float t = max(-n.y, 0.0);
    n.x += n.x >= 0.0 ? -t : t;
    n.z += n.z >= 0.0 ? -t : t;
    return normalize(n);
}
// The nearest of the six face directions to n: 0 +y, 1 -y, 2 +x, 3 -x, 4 +z, 5 -z.
int nearest_face(vec3 n) {
    vec3 a = abs(n);
    return a.y >= a.x && a.y >= a.z ? (n.y >= 0.0 ? 0 : 1) : (a.x >= a.z ? (n.x >= 0.0 ? 2 : 3) : (n.z >= 0.0 ? 4 : 5));
}

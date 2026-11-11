// Rays through the light volume.
//
// The volume holds one byte per voxel, a light class (0 = empty), for every
// chunk the renderer has been given, addressed by voxel coordinate modulo its
// size. `owner` says, per chunk-sized cell, which chunk's voxels are in it,
// so a ray never mistakes a wrapped-round chunk for the one it is in: where
// the owner is not the chunk the ray is in, the ray has left the loaded
// world and sees nothing. `coarse` says how high each 8-voxel cell's contents
// reach, so empty space, and the air over the ground, is crossed eight voxels
// at a time.
layout(set = 0, binding = 11) uniform usampler3D vol;
layout(set = 0, binding = 12) uniform usampler3D coarse;
layout(set = 0, binding = 13) uniform isampler3D owner;
// Two rows of four ints per class: (kind, top texture, side texture,
// emission), (roughness, mean colour r, g, b scaled to 65535). Kinds: 0 empty, 1 solid, 2 cutout,
// 3 see-through, 4 thin.
layout(std430, set = 0, binding = 14) readonly buffer LightTable { ivec4 rows[]; } light_table;

int class_kind(uint c) { return light_table.rows[2u * c].x; }
ivec4 class_row(uint c) { return light_table.rows[2u * c]; }
int class_roughness(uint c) { return light_table.rows[2u * c + 1u].x; }
// A class's colour: the mean of its top texture, linear.
vec3 class_color(uint c) { return vec3(light_table.rows[2u * c + 1u].yzw) / 65535.0; }

// What a class emits: rgb scaled by its strength, each 0..1.
vec3 class_emission(uint c) {
    int e = light_table.rows[2u * c].w;
    vec3 rgb = vec3(float(e & 255), float((e >> 8) & 255), float((e >> 16) & 255)) / 255.0;
    return rgb * (float((e >> 24) & 255) / 255.0);
}

struct Hit {
    ivec3 voxel;     // the voxel hit
    int face;        // the face the ray entered it by: 0 +y, 1 -y, 2 +x, 3 -x, 4 +z, 5 -z
    float t;         // distance along the ray, in voxels
    uint cls;        // its light class
    vec3 glow;       // emission gathered from thin voxels the ray passed through
};

ivec3 wrap(ivec3 v, ivec3 size) { return ((v % size) + size) % size; }

// Is the texture of a cutout class solid where the ray enters this voxel?
bool cutout_blocks(uint c, vec3 p, int face) {
    ivec4 row = class_row(c);
    vec3 f = fract(p);
    vec2 uv = face < 2 ? f.xz : (face < 4 ? vec2(f.z, 1.0 - f.y) : vec2(f.x, 1.0 - f.y));
    int layer = face < 2 ? row.y : row.z;
    if (layer >= u.misc.z) layer = 0;
    return textureLod(tiles, vec3(uv, float(layer)), 0.0).a >= 0.5;
}

// A coarse cell holds how high its contents reach above its floor, 1..8, or 0
// for nothing. A ray [y] above the floor and not descending passes over them.
bool skip_cell(uint top, float y, float dy) { return top == 0u || (dy >= 0.0 && y >= float(top)); }

// Walk a ray from [ro] along unit [rd], both in voxel units, for at most
// [tmax] voxels. Solid voxels stop it. A run of cutout voxels of one class
// stops it where the texture is opaque at the point the ray enters the run:
// the faces between them are not drawn, so they do not block either. See-through voxels stop it only if [stop_glass]. Thin
// voxels never stop it; what they emit is added to h.glow. Returns false if
// nothing stopped it, including when it left the loaded world or ran out of
// [steps]: a ray that skims a surface spends a step per voxel. Past
// [fine_until] voxels from its start the ray looks at every third voxel only.
// See-through voxels in layer [skip_y] and the one above never stop it: a
// ray leaving a water surface starts inside that water's own layer, or
// just over it beside cells of the same surface that reach a layer higher,
// and the volume holds every one of those voxels as full.
bool trace_steps(vec3 ro, vec3 rd, float tmax, bool stop_glass, int steps, float fine_until, int skip_y, out Hit h) {
    h.glow = vec3(0.0); h.t = tmax; h.cls = 0u; h.face = 0; h.voxel = ivec3(0);
    ivec3 dims = u.vol.xyz;                       // the volume's size in chunks
    vec3 d = rd + (1.0 - step(1.0e-6, abs(rd))) * 1.0e-6;      // no exact zeros to divide by
    vec3 inv = 1.0 / d;
    float t = 0.0;
    int face = 0;
    ivec3 last_chunk = ivec3(-1000000);
    ivec3 shift = ivec3(0);                       // from a chunk coordinate to its cell in the volume
    float chunk_top = 0.0;                        // how high the contents of the chunk the ray is in reach, in voxels
    ivec3 last_thin = ivec3(-1000000);
    uint before = 0u;                             // the class of the voxel the ray was in last
    bool strode = false;                          // the last advance skipped voxels
    float fine = fine_until;
    for (int i = 0; i < steps; i++) {
        if (t >= tmax) return false;
        vec3 p = ro + d * t;
        ivec3 v = ivec3(floor(p));
        ivec3 ch = v >> 5;
        if (ch != last_chunk) {
            ivec3 cell = wrap(ch, dims);
            ivec4 own = texelFetch(owner, cell, 0);
            if (own.w == 0 || own.xyz != ch) return false;         // off the edge of what is loaded
            last_chunk = ch; shift = cell - ch; chunk_top = float(own.w - 1);
        }
        vec3 lo, hi;
        if (chunk_top == 0.0 || (d.y >= 0.0 && p.y - float(ch.y * 32) >= chunk_top)) {
            lo = vec3(ch * 32); hi = lo + 32.0;                      // nothing in this chunk on the ray's way: cross it whole
            before = 0u;
        } else if (skip_cell(texelFetch(coarse, (v >> 3) + shift * 4, 0).r, p.y - float((v.y >> 3) * 8), d.y)) {
            lo = vec3((v >> 3) * 8); hi = lo + 8.0;                  // nothing in this 8-voxel cell on the ray's way: cross it whole
            before = 0u;
        } else {
            uint c = texelFetch(vol, v + shift * 32, 0).r;
            if (c != 0u) {
                int kind = class_kind(c);
                bool stops = kind == 1 || (kind == 2 && c != before && cutout_blocks(c, p, face)) || (kind == 3 && stop_glass && v.y != skip_y && v.y != skip_y + 1);
                if (stops && strode) {
                    // found by a stride: go back and walk the last stretch exactly, for a true point and face
                    t -= 3.0; fine = t + 4.0; strode = false; before = 0u;
                    continue;
                }
                if (stops) { h.voxel = v; h.face = face; h.t = t; h.cls = c; return true; }
                // a thin block the ray starts beside lights it less: the block's own floor is not a hot square
                if (kind == 4 && v != last_thin) { h.glow += class_emission(c) * (t < 1.0 ? 0.3 : 1.0); last_thin = v; }
            }
            before = c;
            if (t > fine) {
                // far from where the ray began, look only every third voxel: what is
                // missed is thin, and the saving is most of a mirror ray's cost
                face = abs(d.y) > 0.5 ? (d.y > 0.0 ? 1 : 0) : (abs(d.x) > abs(d.z) ? (d.x > 0.0 ? 3 : 2) : (d.z > 0.0 ? 5 : 4));
                t += 3.0; strode = true;
                continue;
            }
            lo = vec3(v); hi = lo + 1.0;
        }
        strode = false;
        // leave the box by whichever face the ray reaches first
        vec3 tb = (mix(lo, hi, step(0.0, d)) - ro) * inv;
        float tn = min(tb.x, min(tb.y, tb.z));
        face = tn == tb.y ? (d.y > 0.0 ? 1 : 0) : (tn == tb.x ? (d.x > 0.0 ? 3 : 2) : (d.z > 0.0 ? 5 : 4));
        t = max(tn, t) + 1.0e-3;
    }
    return false;
}

// A ray with the usual step limit: enough for shadows, which leave the loaded world upward.
bool trace(vec3 ro, vec3 rd, float tmax, bool stop_glass, out Hit h) { return trace_steps(ro, rd, tmax, stop_glass, 160, 1.0e9, -1000000, h); }

// The light class of the voxel holding [p] (voxel units); 0 outside the loaded world.
uint class_at(vec3 p) {
    ivec3 v = ivec3(floor(p));
    ivec3 ch = v >> 5;
    ivec3 cell = wrap(ch, u.vol.xyz);
    ivec4 own = texelFetch(owner, cell, 0);
    if (own.w == 0 || own.xyz != ch) return 0u;
    return texelFetch(vol, v + (cell - ch) * 32, 0).r;
}

// Rays through the light volume.
//
// The volume holds one byte per voxel, a light class (0 = empty), for every
// chunk the renderer has been given, addressed by voxel coordinate modulo its
// size. `owner` says, per chunk-sized cell, which chunk's voxels are in it,
// so a ray never mistakes a wrapped-round chunk for the one it is in: where
// the owner is not the chunk the ray is in, the ray has left the loaded
// world and sees nothing. `coarse` marks 8-voxel cells that hold anything,
// so empty space is crossed eight voxels at a time.
layout(set = 0, binding = 11) uniform usampler3D vol;
layout(set = 0, binding = 12) uniform usampler3D coarse;
layout(set = 0, binding = 13) uniform isampler3D owner;
// Two rows of four ints per class: (kind, top texture, side texture,
// emission), (roughness, spare...). Kinds: 0 empty, 1 solid, 2 cutout,
// 3 see-through, 4 thin.
layout(std430, set = 0, binding = 14) readonly buffer LightTable { ivec4 rows[]; } light_table;

int class_kind(uint c) { return light_table.rows[2u * c].x; }
ivec4 class_row(uint c) { return light_table.rows[2u * c]; }
int class_roughness(uint c) { return light_table.rows[2u * c + 1u].x; }

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

// Walk a ray from [ro] along unit [rd], both in voxel units, for at most
// [tmax] voxels. Solid voxels stop it. A run of cutout voxels of one class
// stops it where the texture is opaque at the point the ray enters the run:
// the faces between them are not drawn, so they do not block either. See-through voxels stop it only if [stop_glass]. Thin
// voxels never stop it; what they emit is added to h.glow. Returns false if
// nothing stopped it, including when it left the loaded world.
bool trace(vec3 ro, vec3 rd, float tmax, bool stop_glass, out Hit h) {
    h.glow = vec3(0.0); h.t = tmax; h.cls = 0u; h.face = 0; h.voxel = ivec3(0);
    ivec3 dims = u.vol.xyz;                       // the volume's size in chunks
    vec3 d = rd + (1.0 - step(1.0e-6, abs(rd))) * 1.0e-6;      // no exact zeros to divide by
    vec3 inv = 1.0 / d;
    float t = 0.0;
    int face = 0;
    ivec3 last_chunk = ivec3(-1000000);
    ivec3 shift = ivec3(0);                       // from a chunk coordinate to its cell in the volume
    bool hollow = false;                          // the chunk the ray is in holds nothing
    ivec3 last_thin = ivec3(-1000000);
    uint before = 0u;                             // the class of the voxel the ray was in last
    for (int i = 0; i < 160; i++) {
        if (t >= tmax) return false;
        vec3 p = ro + d * t;
        ivec3 v = ivec3(floor(p));
        ivec3 ch = v >> 5;
        if (ch != last_chunk) {
            ivec3 cell = wrap(ch, dims);
            ivec4 own = texelFetch(owner, cell, 0);
            if (own.w == 0 || own.xyz != ch) return false;         // off the edge of what is loaded
            last_chunk = ch; shift = cell - ch; hollow = own.w == 2;
        }
        vec3 lo, hi;
        if (hollow) {
            lo = vec3(ch * 32); hi = lo + 32.0;                      // an empty chunk: cross it whole
            before = 0u;
        } else if (texelFetch(coarse, (v >> 3) + shift * 4, 0).r == 0u) {
            lo = vec3((v >> 3) * 8); hi = lo + 8.0;                  // nothing in this 8-voxel cell: cross it whole
            before = 0u;
        } else {
            uint c = texelFetch(vol, v + shift * 32, 0).r;
            if (c != 0u) {
                int kind = class_kind(c);
                bool stops = kind == 1 || (kind == 2 && c != before && cutout_blocks(c, p, face)) || (kind == 3 && stop_glass);
                if (stops) { h.voxel = v; h.face = face; h.t = t; h.cls = c; return true; }
                if (kind == 4 && v != last_thin) { h.glow += class_emission(c); last_thin = v; }
            }
            before = c;
            lo = vec3(v); hi = lo + 1.0;
        }
        // leave the box by whichever face the ray reaches first
        vec3 tb = (mix(lo, hi, step(0.0, d)) - ro) * inv;
        float tn = min(tb.x, min(tb.y, tb.z));
        face = tn == tb.y ? (d.y > 0.0 ? 1 : 0) : (tn == tb.x ? (d.x > 0.0 ? 3 : 2) : (d.z > 0.0 ? 5 : 4));
        t = max(tn, t) + 1.0e-3;
    }
    return false;
}

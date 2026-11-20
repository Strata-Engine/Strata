// Rays through the light volume.
//
// The volume holds one byte per voxel, a light class (0 = empty), for every
// chunk the renderer has been given whose voxels are not all one class, each
// in a 32-voxel page of a pool (16 x 16 pages a layer). `owner` says, per
// chunk-sized cell (a chunk's coordinate modulo the stream's box), which
// chunk is in it and where its voxels are: a page, or one class throughout.
// So a ray never mistakes a wrapped-round chunk for the one it is in: where
// the owner is not the chunk the ray is in, the ray has left the loaded
// world and sees nothing. `coarse` says how high each 8-voxel cell's contents
// reach, so empty space, and the air over the ground, is crossed eight voxels
// at a time. It and the owner each hold two heights: of everything, and of
// everything but see-through classes. A ray that passes through water and
// glass uses the second, so it crosses a lake as it crosses air.
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

// Bodies (Strata.Physics): each a grid of light classes, one byte a voxel, at
// a pose, its voxels the world's size (pos.w 0) or its own (pos.w metres: a fine body). A ray that enters a chunk looks at the bodies listed for that
// chunk's cell and steps through each one's grid in the body's own frame.
// Traced figures (Strata.Anim) are listed the same way, one group row a
// figure (pos.w -1; dims: its first part row, how many, its rig): each part
// row is a grid of its rig's palette indices at its own voxel size (pos.w).
#define SHAPE_BLOCK 4   // strata_vk.c SV_SHAPE_BLOCK: the coarse layer's block, in the shape's own voxels
struct BodyRow { vec4 pos; vec4 rot; ivec4 dims; vec4 sphere; };   // low corner (metres) and voxel size (0: the world's), rotation, size and first word of its classes, bounding sphere (voxels)
layout(std430, set = 0, binding = 17) readonly buffer BodyTable { BodyRow rows[]; } body_table;
layout(std430, set = 0, binding = 18) readonly buffer BodyBytes { uint words[]; } body_bytes;
layout(std430, set = 0, binding = 19) readonly buffer BodyCells { uvec2 cells[]; } body_cells;   // per cell: first entry, count
layout(std430, set = 0, binding = 20) readonly buffer BodyList { uint ids[]; } body_list;

struct Hit {
    ivec3 voxel;     // the voxel hit
    int face;        // the face the ray entered it by: 0 +y, 1 -y, 2 +x, 3 -x, 4 +z, 5 -z
    float t;         // distance along the ray, in voxels
    uint cls;        // its light class; a figure's voxel: PALETTE_HIT | rig x 256 + palette index
    vec3 glow;       // emission gathered from thin voxels the ray passed through
};

// The rigs' palettes (strata_vk.c, sv_rig_palette): 256 entries a rig, two
// words each: red and green in 11 bits, blue in 10 (linear); roughness and
// emission in 16 bits each. A figure voxel a ray hits takes its colour and
// emission from there, never from a light class.
layout(std430, set = 0, binding = 24) readonly buffer Palette { uvec2 e[]; } palette;
const uint PALETTE_HIT = 0x80000000u;
// The colour of what a ray hit: a class's mean colour, or a figure voxel's palette colour.
vec3 hit_color(uint c) {
    if (c < PALETTE_HIT) return class_color(c);
    uvec2 pe = palette.e[c & 65535u];
    return vec3(float(pe.x & 2047u) / 2047.0, float((pe.x >> 11) & 2047u) / 2047.0, float(pe.x >> 22) / 1023.0);
}
// What it emits: a class's emission, or a figure voxel's colour times its palette emission (as light.frag lights a figure's own pixels).
vec3 hit_emission(uint c) {
    if (c < PALETTE_HIT) return class_emission(c);
    return hit_color(c) * (float(palette.e[c & 65535u].y >> 16) / 65535.0);
}

ivec3 wrap(ivec3 v, ivec3 size) { return ((v % size) + size) % size; }

// Where a chunk's voxels start in the pool, from its owner texel's w; -1
// when they are all one class (w >> 13 is that class).
ivec3 page_of(int w) {
    if (((w >> 12) & 1) == 0) return ivec3(-1);
    int p = w >> 13;
    return ivec3((p % 16) * 32, ((p / 16) % 16) * 32, (p / 256) * 32);
}

// Is the texture of a cutout class solid where the ray enters this voxel?
bool cutout_blocks(uint c, vec3 p, int face) {
    ivec4 row = class_row(c);
    vec3 f = fract(p);
    vec2 uv = face < 2 ? f.xz : (face < 4 ? vec2(f.z, 1.0 - f.y) : vec2(f.x, 1.0 - f.y));
    int layer = face < 2 ? row.y : row.z;
    if (layer >= u.misc.z) layer = 0;
    return textureLod(tiles, vec3(uv, float(layer)), 0.0).a >= 0.5;
}

// The nearest stopping voxel of body [bi] along a ray (voxel units, world
// frame) before [best]: updates best and what was hit, in world terms. [pal]
// is 0 for a body of light classes, or PALETTE_HIT | rig x 256 for a figure
// part, every voxel of which stops the ray; a part's grid has voxels of its
// own size (pos.w metres), so it is walked in its own voxels and the
// distances converted.
void body_ray(uint bi, vec3 ro, vec3 rd, float tmax, bool stop_glass, uint pal, inout float best, inout uint bcls, inout int bface) {
    // the bounding sphere first: most rays miss it, and the rest is a rotation and a walk
    vec4 sp = body_table.rows[bi].sphere;
    vec3 oc = sp.xyz - ro;
    float tca = dot(oc, rd);
    float lim = min(tmax, best);
    if (dot(oc, oc) - tca * tca > sp.w * sp.w || tca + sp.w < 0.0 || tca - sp.w > lim) return;
    BodyRow b = body_table.rows[bi];
    float sc = b.pos.w > 0.0 ? b.pos.w : u.eye.w;     // metres a voxel of its grid
    float kv = b.pos.w > 0.0 ? sc / u.eye.w : 1.0;      // its voxel in world voxels: exactly 1 for a body of the world's voxels (a GPU's x / x need not be)
    vec4 qi = vec4(-b.rot.xyz, b.rot.w);
    vec3 o = qrot(qi, ro * u.eye.w - b.pos.xyz) / sc;
    vec3 d = qrot(qi, rd);
    d += (1.0 - step(1.0e-6, abs(d))) * 1.0e-6;
    vec3 inv = 1.0 / d;
    vec3 dims = vec3(b.dims.xyz);
    vec3 ta = -o * inv, tb = (dims - o) * inv;
    vec3 tlo = min(ta, tb), thi = max(ta, tb);
    float tin = max(max(tlo.x, tlo.y), max(tlo.z, 0.0));
    float lim2 = min(tmax, best);
    lim2 /= kv;
    float tout = min(min(thi.x, thi.y), min(thi.z, lim2));
    if (tin >= tout) return;
    // a ray that starts on the body's own surface does not stop on it at once
    int axis = tlo.x >= tlo.y && tlo.x >= tlo.z ? 0 : (tlo.y >= tlo.z ? 1 : 2);
    ivec3 stp = ivec3(sign(d));
    vec3 tstep = abs(inv);
    // Two levels: the blocks of SHAPE_BLOCK^3 voxels first (the coarse layer
    // after the voxels: 1 where a block holds anything), voxel by voxel only
    // inside a block that does. t is in the grid's voxels throughout.
    const int B = SHAPE_BLOCK;
    ivec3 bd = (b.dims.xyz + B - 1) / B;
    uint n = uint(b.dims.x * b.dims.y * b.dims.z);
    uint cw = uint(b.dims.w) + ((n + 3u) >> 2);
    vec3 p0 = o + d * (tin + 1.0e-4);
    ivec3 vb = clamp(ivec3(floor(p0 / float(B))), ivec3(0), bd - 1);
    vec3 tnb = ((vec3(vb) + step(0.0, d)) * float(B) - o) * inv;
    vec3 tsb = tstep * float(B);
    float t = tin;
    for (int kb = 0; kb < 200; kb++) {
        if (t > tout || any(lessThan(vb, ivec3(0))) || any(greaterThanEqual(vb, bd))) return;
        float tbo = min(min(tnb.x, tnb.y), min(tnb.z, tout));
        int bi2 = vb.x + bd.x * (vb.y + bd.y * vb.z);
        uint occ = (body_bytes.words[cw + uint(bi2 >> 2)] >> (8u * uint(bi2 & 3))) & 255u;
        if (occ != 0u) {
            // the voxels of this block the ray crosses, from t to tbo
            ivec3 lo = vb * B, hi = min(lo + B, b.dims.xyz) - 1;
            vec3 p = o + d * (t + 1.0e-4);
            ivec3 v = clamp(ivec3(floor(p)), lo, hi);
            vec3 tnext = (vec3(v) + step(0.0, d) - o) * inv;
            float tf = t;
            int ax = axis;
            for (int k = 0; k < 3 * SHAPE_BLOCK + 3; k++) {
                if (tf > tbo || any(lessThan(v, lo)) || any(greaterThan(v, hi))) break;
                int idx = v.x + b.dims.x * (v.y + b.dims.y * v.z);
                uint c = (body_bytes.words[uint(b.dims.w) + uint(idx >> 2)] >> (8u * uint(idx & 3))) & 255u;
                if (c != 0u && tf > 0.05) {
                    if (pal != 0u || class_kind(c) == 1 || class_kind(c) == 2 || (class_kind(c) == 3 && stop_glass)) {
                        vec3 nl = vec3(0.0); nl[ax] = -float(stp[ax]);
                        best = tf * kv; bcls = pal != 0u ? pal | c : c; bface = nearest_face(qrot(b.rot, nl));
                        return;
                    }
                }
                if (tnext.x < tnext.y && tnext.x < tnext.z) { v.x += stp.x; tf = tnext.x; tnext.x += tstep.x; ax = 0; }
                else if (tnext.y < tnext.z) { v.y += stp.y; tf = tnext.y; tnext.y += tstep.y; ax = 1; }
                else { v.z += stp.z; tf = tnext.z; tnext.z += tstep.z; ax = 2; }
            }
        }
        if (tnb.x < tnb.y && tnb.x < tnb.z) { vb.x += stp.x; t = tnb.x; tnb.x += tsb.x; axis = 0; }
        else if (tnb.y < tnb.z) { vb.y += stp.y; t = tnb.y; tnb.y += tsb.y; axis = 1; }
        else { vb.z += stp.z; t = tnb.z; tnb.z += tsb.z; axis = 2; }
    }
}

// A traced figure's group row [gi]: its sphere first, then each of its parts.
void figure_ray(uint gi, vec3 ro, vec3 rd, float tmax, inout float best, inout uint bcls, inout int bface) {
    vec4 sp = body_table.rows[gi].sphere;
    vec3 oc = sp.xyz - ro;
    float tca = dot(oc, rd);
    if (dot(oc, oc) - tca * tca > sp.w * sp.w || tca + sp.w < 0.0 || tca - sp.w > min(tmax, best)) return;
    ivec4 gd = body_table.rows[gi].dims;
    uint pal = PALETTE_HIT | (uint(gd.z) << 8);
    for (int j = 0; j < gd.y && j < 64; j++) body_ray(uint(gd.x + j), ro, rd, tmax, false, pal, best, bcls, bface);
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
    ivec3 page = ivec3(-1);                       // where the chunk's voxels start in the pool, or -1 when they are all one class
    uint whole = 0u;                              // ... that class
    float chunk_top = 0.0;                        // how high the contents of the chunk the ray is in reach, in voxels
    ivec3 last_thin = ivec3(-1000000);
    uint before = 0u;                             // the class of the voxel the ray was in last
    bool strode = false;                          // the last advance skipped voxels
    float fine = fine_until;
    // the nearest body voxel found so far, from the bodies listed in the chunks the ray has entered
    float bt = 1.0e30; uint bcls = 0u; int bface = 0;
#ifdef STRATA_TRACE_WORLD_ONLY
    const bool any_bodies = false;                // the blocks alone: collide.comp's particles meet no body or figure
#else
    bool any_bodies = u.bodies.y > 0;
#endif
    for (int i = 0; i < steps; i++) {
        if (t >= tmax || t >= bt) break;
        vec3 p = ro + d * t;
        ivec3 v = ivec3(floor(p));
        ivec3 ch = v >> 5;
        if (ch != last_chunk) {
            ivec3 cell = wrap(ch, dims);
            ivec4 own = texelFetch(owner, cell, 0);
            if (own.w == 0 || own.xyz != ch) break;                // off the edge of what is loaded
            last_chunk = ch; shift = cell - ch; chunk_top = stop_glass ? float((own.w & 63) - 1) : float((own.w >> 6) & 63);
            page = page_of(own.w); whole = uint(own.w >> 13);
            if (any_bodies) {
                uvec2 e = body_cells.cells[cell.x + dims.x * (cell.y + dims.y * cell.z)];
                for (uint k = 0u; k < e.y && k < 128u; k++) {
                    uint id = body_list.ids[e.x + k];
                    if (body_table.rows[id].pos.w < 0.0) figure_ray(id, ro, d, tmax, bt, bcls, bface);
                    else body_ray(id, ro, d, tmax, stop_glass, 0u, bt, bcls, bface);
                }
            }
        }
        vec3 lo, hi;
        if (chunk_top == 0.0 || (d.y >= 0.0 && p.y - float(ch.y * 32) >= chunk_top)) {
            lo = vec3(ch * 32); hi = lo + 32.0;                      // nothing in this chunk on the ray's way: cross it whole
            before = 0u;
        } else if (skip_cell(stop_glass ? texelFetch(coarse, (v >> 3) + shift * 4, 0).r & 15u : texelFetch(coarse, (v >> 3) + shift * 4, 0).r >> 4, p.y - float((v.y >> 3) * 8), d.y)) {
            lo = vec3((v >> 3) * 8); hi = lo + 8.0;                  // nothing in this 8-voxel cell on the ray's way: cross it whole
            before = 0u;
        } else {
            uint c = page.x >= 0 ? texelFetch(vol, page + (v & 31), 0).r : whole;
            if (c != 0u) {
                int kind = class_kind(c);
                bool stops = kind == 1 || (kind == 2 && c != before && cutout_blocks(c, p, face)) || (kind == 3 && stop_glass && v.y != skip_y && v.y != skip_y + 1);
                if (stops && strode) {
                    // found by a stride: go back and walk the last stretch exactly, for a true point and face
                    t -= 3.0; fine = t + 4.0; strode = false; before = 0u;
                    continue;
                }
                if (stops && t < bt) { h.voxel = v; h.face = face; h.t = t; h.cls = c; return true; }
                if (stops) break;
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
    if (bt < tmax) { h.voxel = ivec3(floor(ro + d * bt)); h.face = bface; h.t = bt; h.cls = bcls; return true; }
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
    ivec3 page = page_of(own.w);
    return page.x >= 0 ? texelFetch(vol, page + (v & 31), 0).r : uint(own.w >> 13);
}

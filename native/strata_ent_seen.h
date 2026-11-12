/* strata_ent_seen.h — the entity kernel's seen and unseen switch, and setting a walker on the ground as it comes into
 * view. Part of strata_ent.h, which includes it after strata_ent_move.h and before the tick; not a header to include
 * alone. Its state is the seen, waiting and frozen flags of each row (EN_F_*) and the world's seen and waiting counts.
 *
 * The switch runs once a tick, after movement and before the grid (en_tick), over each kind's dense array, with d the
 * kind's seen distance and c the world's chunk side, distances measured horizontally (x and z) to the nearest focus:
 *  - a seen entity farther than d + c from every focus becomes unseen: event 4, its grounded flag cleared and its parts
 *    released (en_release_parts: a figure or voices are the game's to free, and come back as it is seen again), then its
 *    kind's unseen rule: live, it goes on (moved as unseen); freeze, it neither moves nor ages until seen again;
 *    remove, it is removed at once (event 6, reason 2, straight after its event 4);
 *  - an unseen entity within d of a focus is a candidate. A flyer, a placed or a bodied one becomes seen at once,
 *    unmoved. A walker waits (stat 2, flag EN_F_WAITING) until every chunk its grounding reads is held, then is set at
 *    the nearest height where its box is clear and a voxel under it is solid, searching voxel by voxel up
 *    and down from its own (up first at equal distance) no farther than 16 m; with no such height it waits too and is
 *    tried again next tick. (Under its box, not under its centre: a box straddling a taller column stands on its edge.)
 *    A waiting walker is held in place: it neither follows its route nor is removed. Set down,
 *    it stands 1e-4 voxels above the ground (where the sweep leaves a box), its vertical velocity 0, grounded;
 *  - becoming seen writes event 3 at the entity's position (after it is set down) and clears frozen and waiting.
 * An entity between d and d + c keeps what it was, so one pacing at the edge does not flicker. With no focus every
 * seen entity becomes unseen and no unseen one is a candidate. */
#ifndef STRATA_ENT_SEEN_H
#define STRATA_ENT_SEEN_H
#ifndef STRATA_ENT_H
#error "strata_ent_seen.h is part of strata_ent.h; include that"
#endif

#define EN_GROUND_SEARCH 16.0      /* metres a walker may be moved up or down to set it on the ground */
#define EN_GROUND_CHUNKS 65536     /* chunks the grounding of one walker may read; a bigger box never grounds */

/* 1 if p is within dist of any focus, horizontally */
static inline int en_seen_by(const double *p, double dist, int nfocus, const double *focus) {
    double dd = dist * dist;
    for (int f = 0; f < nfocus; f++) {
        double dx = p[0] - focus[f * 3], dz = p[2] - focus[f * 3 + 2];
        if (dx * dx + dz * dz <= dd) return 1;
    }
    return 0;
}

/* 1 if entity id was seen at the end of the last tick */
static inline int en_seen(const EnWorld *w, int64_t id) {
    int r = en_row(w, id);
    return r >= 0 && (w->flags[r] & EN_F_SEEN) != 0;
}

/* floor(m / sc) as a voxel coordinate whose chunk can be held; 0 if it is out there or not finite */
static inline int en_voxel_of(double m, double sc, int *v) {
    double f = floor(m / sc), lim = (double)EN_CHUNK_COORD_MAX * 32;
    if (!(fabs(f) < lim)) return 0;
    *v = (int)f;
    return 1;
}

/* The voxels a grounding search of the box centred at p (half extents h) reads: x and z the box's, y from the voxel
 * under the lowest candidate to the top of the highest. 0 if any is out where no chunk is held. */
static inline int en_ground_range(const EnWorld *w, const double p[3], const double h[3], int lo[3], int hi[3]) {
    double sc = w->voxel_metres;
    if (!en_voxel_of(p[0] - h[0], sc, &lo[0]) || !en_voxel_of(p[0] + h[0], sc, &hi[0])) return 0;
    if (!en_voxel_of(p[2] - h[2], sc, &lo[2]) || !en_voxel_of(p[2] + h[2], sc, &hi[2])) return 0;
    if (!en_voxel_of(p[1] - h[1] - EN_GROUND_SEARCH, sc, &lo[1]) || !en_voxel_of(p[1] + h[1] + EN_GROUND_SEARCH, sc, &hi[1]))
        return 0;
    lo[1] -= 2; hi[1] += 1;   /* the voxel under the lowest candidate, and the rounding of the search's ends */
    return 1;
}

/* 1 if every chunk the grounding of the box centred at p reads is held */
static inline int en_ground_held(const EnWorld *w, const double p[3], const double h[3]) {
    int lo[3], hi[3];
    if (!en_ground_range(w, p, h, lo, hi)) return 0;
    int c0[3], c1[3];
    double n = 1;
    for (int a = 0; a < 3; a++) { c0[a] = en_floor32(lo[a]); c1[a] = en_floor32(hi[a]); n *= (double)(c1[a] - c0[a] + 1); }
    if (n > EN_GROUND_CHUNKS) return 0;
    for (int cy = c0[1]; cy <= c1[1]; cy++)
        for (int cz = c0[2]; cz <= c1[2]; cz++)
            for (int cx = c0[0]; cx <= c1[0]; cx++)
                if (!en_chunk_find(w, cx, cy, cz)) return 0;
    return 1;
}

/* the voxel range a box centred at q (half extents h) overlaps, strictly (a face only meeting a voxel does not
 * overlap it); 0 if any is out where no chunk is held */
static inline int en_box_voxels(double sc, const double q[3], const double h[3], int v0[3], int v1[3]) {
    for (int a = 0; a < 3; a++) {
        double hi = q[a] + h[a];
        if (!en_voxel_of(q[a] - h[a], sc, &v0[a]) || !en_voxel_of(hi, sc, &v1[a])) return 0;
        if (v1[a] > v0[a] && hi <= (double)v1[a] * sc) v1[a]--;   /* its far face only meets that voxel */
    }
    return 1;
}

/* 1 if a voxel of layer y under the box's horizontal extent v0..v1 is solid */
static inline int en_layer_solid(const EnWorld *w, EnChunkCache *cc, const int v0[3], const int v1[3], int y) {
    for (int z = v0[2]; z <= v1[2]; z++)
        for (int x = v0[0]; x <= v1[0]; x++)
            if (en_cached_solid(w, cc, x, y, z) == 1) return 1;
    return 0;
}

/* 1 if no solid voxel overlaps the box centred at q (strictly: a face only meeting a voxel does not), 0 if one does,
 * -1 if a voxel is not held; the lowest layer is looked at first, so a box inside a hill is answered at once */
static inline int en_box_clear(const EnWorld *w, EnChunkCache *cc, double sc, const double q[3], const double h[3]) {
    int v0[3], v1[3];
    if (!en_box_voxels(sc, q, h, v0, v1)) return -1;
    for (int y = v0[1]; y <= v1[1]; y++)
        for (int z = v0[2]; z <= v1[2]; z++)
            for (int x = v0[0]; x <= v1[0]; x++) {
                int s = en_cached_solid(w, cc, x, y, z);
                if (s != 0) return s < 0 ? -1 : 0;
            }
    return 1;
}

/* The height to set the box centred at p (half extents h) on the ground: candidates are the voxel faces within 16 m of
 * its base, nearest first, up before down at equal distance; the first where a voxel of the layer under the box's
 * horizontal extent is solid and the box is clear wins, its base 1e-4 voxels above that face. 1 and *y if found, 0 if
 * none. */
static inline int en_ground_find(const EnWorld *w, const double p[3], const double h[3], double *y) {
    double sc = w->voxel_metres, b = (p[1] - h[1]) / sc, reach = EN_GROUND_SEARCH / sc;
    int v0[3], v1[3];
    if (!en_box_voxels(sc, p, h, v0, v1) || !isfinite(b)) return 0;
    double fu = ceil(b), fd = floor(b);
    if (fu == fd) fd -= 1;
    EnChunkCache cc = {NULL, 0, 0, 0, 0};
    for (;;) {
        double du = fu - b, dd = b - fd;
        int up = du <= dd;
        if ((up ? du : dd) > reach) return 0;
        double fk = up ? fu : fd;
        if (up) fu += 1; else fd -= 1;
        int k = (int)fk;
        if (!en_layer_solid(w, &cc, v0, v1, k - 1)) continue;
        double q[3] = {p[0], fk * sc + sc * 0.0001 + h[1], p[2]};
        if (en_box_clear(w, &cc, sc, q, h) == 1) { *y = q[1]; return 1; }
    }
}

/* row r becomes seen: event 3 at its position, frozen and waiting cleared */
static inline void en_enter(EnWorld *w, int r) {
    const double *p = w->pos + r * 3;
    w->flags[r] = (uint8_t)((w->flags[r] & ~(EN_F_WAITING | EN_F_FROZEN)) | EN_F_SEEN);
    en_event(w, EN_E_ENTERED, en_id(w, r), 0, 0, 0, 0, 0, p[0], p[1], p[2]);
}

/* The switch over every live entity (see the top of this file); sets the seen and waiting counts. */
static inline void en_switch(EnWorld *w, int nfocus, const double *focus) {
    int seen = 0, waiting = 0;
    for (int k = 0; k < w->nkinds; k++) {
        const EnKind *kd = &w->kinds[k];
        const int32_t *dense = w->dense + (size_t)k * w->capacity;
        double d = kd->floats[EN_K_SEEN], h[3] = {kd->floats[EN_K_HX], kd->floats[EN_K_HY], kd->floats[EN_K_HZ]};
        int walker = kd->ints[EN_K_MOVER] == 2;
        for (int i = 0; i < kd->live;) {   /* a removal moves the kind's last entry into i: look at i again */
            int r = dense[i];
            double *p = w->pos + r * 3;
            if (w->flags[r] & EN_F_SEEN) {
                if (en_seen_by(p, d + w->chunk_metres, nfocus, focus)) { seen++; i++; continue; }
                int64_t id = en_id(w, r);
                w->flags[r] &= (uint8_t)~(EN_F_SEEN | EN_F_GROUNDED);
                en_release_parts(w, r);
                en_event(w, EN_E_LEFT, id, 0, 0, 0, 0, 0, p[0], p[1], p[2]);
                int rule = kd->ints[EN_K_UNSEEN];
                if (rule == 2) { en_remove(w, id, 2); continue; }
                if (rule == 1) w->flags[r] |= EN_F_FROZEN;
                i++;
                continue;
            }
            if (!en_seen_by(p, d, nfocus, focus)) { w->flags[r] &= (uint8_t)~EN_F_WAITING; i++; continue; }
            if (walker) {
                double y;
                if (!en_ground_held(w, p, h) || !en_ground_find(w, p, h, &y)) {
                    w->flags[r] |= EN_F_WAITING;
                    waiting++; i++;
                    continue;
                }
                p[1] = y;
                w->vel[r * 3 + 1] = 0;
                w->flags[r] |= EN_F_GROUNDED;
                memcpy(w->from + r * 3, p, 3 * sizeof *p);   /* set down, not moved: its touch box is where it stands */
            }
            en_enter(w, r);
            seen++; i++;
        }
    }
    w->seen = seen;
    w->waiting = waiting;
}

#endif

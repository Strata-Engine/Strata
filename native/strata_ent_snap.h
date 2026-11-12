/* strata_ent_snap.h — the entity kernel's snapshot and restore: the whole entity world as three flat arrays (int32,
 * float, double), and a world rebuilt from them. Part of strata_ent.h, which includes it last; not a header to include
 * alone. It keeps no state of its own.
 *
 * A snapshot holds everything en_hash covers and what the next tick needs for an unseen entity to behave as it would
 * have: the rows (kind by kind in dense order, so dense order is kept), every row's generation, the free-row stack in
 * order, each live row's flags, route, waypoint and slots, the route table, the pool's points of each held route and its
 * free blocks, the freed route ids, the tick counter, and the removals queued between ticks. It holds no masks, grid,
 * events, release list, part ids or focus points, and no stats.
 *
 * Seen state is not kept: a seen entity is written as becoming unseen would leave it (seen, grounded and lost-body
 * cleared, frozen set if its kind freezes unseen; a kind that removes unseen ones keeps it, unseen). A restored world
 * gives the near ones back their parts at the next tick.
 *
 * The layout. H = 3 + 2 x kinds.
 *   ints: [0] the version (1), [1] the capacity, [2] the kind count, then each kind's float and int slot counts;
 *         [H] route points, [H+1] free rows, [H+2] route ids handed out, [H+3] freed route ids, [H+4] free pool blocks,
 *         [H+5] removals queued, [H+6..H+7] the tick counter (low 32 bits, high 32 bits), then each kind's live count;
 *         the generation of every row (capacity); the free-row stack, bottom first; each live row kind by kind in dense
 *         order: its row, flags, route (-1 none), next waypoint, then its kind's int slots (low, high 32 bits each);
 *         every route id handed out: start, points, followers, state (all 0 for a free id); the freed route ids, the
 *         stack bottom first; the pool's free blocks in order: start, length; each queued removal: id (low, high), reason.
 *   floats: each live row as above: velocity x, y, z, facing, its kind's float slots, route speed, progress mark, stall.
 *   doubles: each live row as above: position x, y, z, age; each held route's points in id order (x, y, z each); each
 *            queued removal's position.
 *
 * Restoring needs an open world of the same capacity and route pool whose kinds have the same count and slot counts;
 * the kinds themselves (movers, sizes, rules) are the game's, registered before. It checks everything a restore reads
 * (sizes exact, numbers in range and finite, rows each live or free once, routes and their followers agreeing, the pool
 * adding up) before changing anything, so a snapshot that does not fit changes nothing; the check is against mistakes,
 * not proof against a crafted array. It is refused too while any entity holds a part (en_release_all first). */
#ifndef STRATA_ENT_SNAP_H
#define STRATA_ENT_SNAP_H
#ifndef STRATA_ENT_H
#error "strata_ent_snap.h is part of strata_ent.h; include that"
#endif

#define EN_SNAP_VERSION 1
#define EN_SNAP_FLAGS (EN_F_STUCK | EN_F_GROUNDED | EN_F_LOOPS | EN_F_BLOCKED | EN_F_WAITING | EN_F_FROZEN | EN_F_LOST)

/* the ints before the generations */
static inline int en_snap_head(int nkinds) { return 3 + 2 * nkinds + 8 + nkinds; }

/* the ints, floats and doubles a snapshot of w needs; 1 */
static inline int en_snapshot_sizes(const EnWorld *w, int sizes[3]) {
    int64_t ni = en_snap_head(w->nkinds) + (int64_t)w->capacity + w->nfree, nf = 0, nd = 0;
    for (int k = 0; k < w->nkinds; k++) {
        const EnKind *kd = &w->kinds[k];
        ni += (int64_t)kd->live * (4 + 2 * kd->ints[EN_K_ISLOTS]);
        nf += (int64_t)kd->live * (7 + kd->ints[EN_K_FSLOTS]);
        nd += (int64_t)kd->live * 4;
    }
    ni += 4 * (int64_t)w->routes_hw + w->nroute_free + 2 * (int64_t)w->nfx + 3 * (int64_t)w->nremoved;
    nd += 3 * (int64_t)w->route_pts_held + 3 * (int64_t)w->nremoved;
    sizes[0] = (int)ni; sizes[1] = (int)nf; sizes[2] = (int)nd;
    return 1;
}

/* a row's flags as a snapshot keeps them: as becoming unseen leaves them */
static inline uint8_t en_snap_flags(const EnWorld *w, int r) {
    uint8_t f = w->flags[r];
    if (f & EN_F_SEEN) {
        f &= (uint8_t)~(EN_F_SEEN | EN_F_GROUNDED | EN_F_LOST);
        if (w->kinds[w->kind[r]].ints[EN_K_UNSEEN] == 1) f |= EN_F_FROZEN;
    }
    return f;
}

/* Write the snapshot into ints, floats and doubles, each at least what en_snapshot_sizes says. 1. */
static inline int en_snapshot(const EnWorld *w, int32_t *ints, float *floats, double *doubles) {
    int nk = w->nkinds, a = 0, b = 0, c = 0;
    ints[a++] = EN_SNAP_VERSION; ints[a++] = w->capacity; ints[a++] = nk;
    for (int k = 0; k < nk; k++) { ints[a++] = w->kinds[k].ints[EN_K_FSLOTS]; ints[a++] = w->kinds[k].ints[EN_K_ISLOTS]; }
    ints[a++] = w->route_points; ints[a++] = w->nfree; ints[a++] = w->routes_hw; ints[a++] = w->nroute_free;
    ints[a++] = w->nfx; ints[a++] = w->nremoved;
    ints[a++] = (int32_t)(uint32_t)((uint64_t)w->tick_count & 0xffffffffu);
    ints[a++] = (int32_t)(uint32_t)((uint64_t)w->tick_count >> 32);
    for (int k = 0; k < nk; k++) ints[a++] = w->kinds[k].live;
    for (int r = 0; r < w->capacity; r++) ints[a++] = (int32_t)w->gen[r];
    for (int i = 0; i < w->nfree; i++) ints[a++] = w->free_rows[i];
    for (int k = 0; k < nk; k++) {
        const EnKind *kd = &w->kinds[k];
        const int32_t *dense = w->dense + (size_t)k * w->capacity;
        int nf = kd->ints[EN_K_FSLOTS], ni = kd->ints[EN_K_ISLOTS];
        for (int i = 0; i < kd->live; i++) {
            int r = dense[i];
            ints[a++] = r; ints[a++] = en_snap_flags(w, r); ints[a++] = w->rt_route[r]; ints[a++] = w->rt_next[r];
            for (int s = 0; s < ni; s++) {
                uint64_t v = (uint64_t)w->islot[(size_t)r * EN_SLOTS + s];
                ints[a++] = (int32_t)(uint32_t)(v & 0xffffffffu); ints[a++] = (int32_t)(uint32_t)(v >> 32);
            }
            for (int x = 0; x < 3; x++) floats[b++] = w->vel[r * 3 + x];
            floats[b++] = w->facing[r];
            for (int s = 0; s < nf; s++) floats[b++] = w->fslot[(size_t)r * EN_SLOTS + s];
            floats[b++] = w->rt_speed[r]; floats[b++] = w->rt_best[r]; floats[b++] = w->rt_stall[r];
            for (int x = 0; x < 3; x++) doubles[c++] = w->pos[r * 3 + x];
            doubles[c++] = w->age[r];
        }
    }
    for (int i = 0; i < w->routes_hw; i++) {
        const EnRoute *rt = &w->routes[i];
        int held = rt->state != 0;
        ints[a++] = held ? rt->start : 0; ints[a++] = held ? rt->n : 0; ints[a++] = held ? rt->users : 0; ints[a++] = rt->state;
        if (held) { memcpy(doubles + c, w->rpts + (size_t)rt->start * 3, (size_t)rt->n * 3 * sizeof *doubles); c += rt->n * 3; }
    }
    for (int i = 0; i < w->nroute_free; i++) ints[a++] = w->route_free[i];
    for (int i = 0; i < w->nfx; i++) { ints[a++] = w->fx_start[i]; ints[a++] = w->fx_len[i]; }
    for (int i = 0; i < w->nremoved; i++) {
        const EnRemoval *q = &w->removed[i];
        ints[a++] = (int32_t)(uint32_t)((uint64_t)q->id & 0xffffffffu); ints[a++] = (int32_t)(uint32_t)((uint64_t)q->id >> 32);
        ints[a++] = q->reason;
        for (int x = 0; x < 3; x++) doubles[c++] = q->pos[x];
    }
    return 1;
}

static inline int64_t en_snap_i64(const int32_t *p) { return (int64_t)(((uint64_t)(uint32_t)p[1] << 32) | (uint32_t)p[0]); }

/* The check: 1 if the arrays are a snapshot a restore into w accepts (parts held aside), else 0. Borrows a scratch
 * array for the call; 0 if it cannot. */
static inline int en_snapshot_fits(const EnWorld *w, int ni, const int32_t *ints, int nf, const float *floats,
                                   int nd, const double *doubles) {
    int nk = w->nkinds, cap = w->capacity, rp = w->route_points;
    if (!ints || ni < 3 || nf < 0 || nd < 0 || (nf > 0 && !floats) || (nd > 0 && !doubles)) return 0;
    if (ints[0] != EN_SNAP_VERSION || ints[1] != cap || ints[2] != nk) return 0;
    int h = en_snap_head(nk);
    if (ni < h) return 0;
    for (int k = 0; k < nk; k++)
        if (ints[3 + 2 * k] != w->kinds[k].ints[EN_K_FSLOTS] || ints[4 + 2 * k] != w->kinds[k].ints[EN_K_ISLOTS]) return 0;
    const int32_t *c = ints + 3 + 2 * nk;
    int nfree = c[1], hw = c[2], nrf = c[3], nfx = c[4], nrem = c[5];
    if (c[0] != rp || nfree < 0 || nfree > cap || hw < 0 || hw > rp || nrf < 0 || nrf > hw || nfx < 0 || nfx > rp + 1 ||
        nrem < 0 || nrem > EN_EVENTS || en_snap_i64(c + 6) < 0) return 0;
    int64_t live = 0, want_i = h + (int64_t)cap + nfree, want_f = 0, want_d = 0;
    for (int k = 0; k < nk; k++) {
        int lk = c[8 + k];
        if (lk < 0 || lk > cap) return 0;
        live += lk;
        want_i += (int64_t)lk * (4 + 2 * w->kinds[k].ints[EN_K_ISLOTS]);
        want_f += (int64_t)lk * (7 + w->kinds[k].ints[EN_K_FSLOTS]);
        want_d += (int64_t)lk * 4;
    }
    if (live + nfree != cap) return 0;
    want_i += 4 * (int64_t)hw + nrf + 2 * (int64_t)nfx + 3 * (int64_t)nrem;
    if (want_i != ni || want_f != nf || want_d > nd) return 0;
    /* scratch: a mark a row, then followers a route id and a mark a route id */
    int32_t *mark = (int32_t *)calloc((size_t)cap + 2 * (size_t)hw + 1, sizeof *mark);
    if (!mark) return 0;
    int32_t *users = mark + cap, *rfree = users + hw;
    int ok = 1, a = h;
    for (int r = 0; r < cap && ok; r++, a++) ok = ints[a] >= 1 && (uint32_t)ints[a] <= EN_GEN_MAX;
    for (int i = 0; i < nfree && ok; i++, a++) {
        int r = ints[a];
        ok = r >= 0 && r < cap && !mark[r];
        if (ok) mark[r] = 1;
    }
    int rows_at = a, fb = 0, db = 0;
    const int32_t *routes_at = ints + rows_at + (want_i - h - cap - nfree - 4 * (int64_t)hw - nrf - 2 * (int64_t)nfx - 3 * (int64_t)nrem);
    for (int k = 0; k < nk && ok; k++) {
        int fs = w->kinds[k].ints[EN_K_FSLOTS], is = w->kinds[k].ints[EN_K_ISLOTS];
        for (int i = 0; i < c[8 + k] && ok; i++) {
            int r = ints[a], fl = ints[a + 1], route = ints[a + 2], next = ints[a + 3];
            a += 4 + 2 * is;
            ok = r >= 0 && r < cap && !mark[r] && (fl & ~EN_SNAP_FLAGS) == 0 && route >= -1 && route < hw;
            if (!ok) break;
            mark[r] = 1;
            if (route >= 0) {
                const int32_t *rt = routes_at + 4 * (size_t)route;
                ok = (rt[3] == 1 || rt[3] == 2) && next >= 0 && next < rt[1];
                users[route]++;
            }
            for (int x = 0; x < 7 + fs && ok; x++) ok = isfinite(floats[fb + x]);
            ok = ok && floats[fb + 4 + fs] >= 0;   /* the route speed */
            fb += 7 + fs;
            for (int x = 0; x < 4 && ok; x++) ok = isfinite(doubles[db + x]);
            db += 4;
        }
    }
    int64_t pts = 0;
    for (int i = 0; i < hw && ok; i++, a += 4) {
        int start = ints[a], n = ints[a + 1], u = ints[a + 2], st = ints[a + 3];
        if (st == 0) { ok = start == 0 && n == 0 && u == 0 && users[i] == 0; continue; }
        ok = (st == 1 || st == 2) && n >= 1 && n <= EN_ROUTE_MAX && start >= 0 && start <= rp - n && u == users[i] && (st == 1 || u > 0);
        pts += n;
    }
    want_d += 3 * pts + 3 * (int64_t)nrem;
    ok = ok && want_d == nd;
    for (int i = 0; i < nrf && ok; i++, a++) {
        int id = ints[a];
        ok = id >= 0 && id < hw && !rfree[id] && routes_at[4 * (size_t)id + 3] == 0;
        if (ok) rfree[id] = 1;
    }
    if (ok) {   /* every free route id is on the stack */
        int zero = 0;
        for (int i = 0; i < hw; i++) zero += routes_at[4 * (size_t)i + 3] == 0;
        ok = zero == nrf;
    }
    int64_t freepts = 0;
    for (int i = 0, end = -1; i < nfx && ok; i++, a += 2) {
        int start = ints[a], len = ints[a + 1];
        ok = len >= 1 && start > end && start <= rp - len;
        end = start + len;
        freepts += len;
    }
    ok = ok && pts + freepts == rp;
    for (int i = 0; i < nrem && ok; i++, a += 3) ok = en_snap_i64(ints + a) >= 0 && ints[a + 2] >= 0 && ints[a + 2] <= 2;
    for (int64_t x = db; x < nd && ok; x++) ok = isfinite(doubles[x]);   /* the route points and the removals' positions */
    free(mark);
    return ok;
}

/* Restore w from a snapshot (see the top of this file): 1, or 0 with nothing changed but the refusal counted
 * (EN_R_PART while any entity holds a part, EN_R_SNAPSHOT for a snapshot that does not fit). Every entity comes back
 * unseen with no parts; the events and focus points are cleared and the grid rebuilt from the restored positions; the
 * masks, touch pairs, release list and stats are left as they were. */
static inline int en_restore(EnWorld *w, int ni, const int32_t *ints, int nf, const float *floats, int nd, const double *doubles) {
    for (size_t i = 0; i < (size_t)w->capacity * EN_PARTS; i++)
        if (w->part[i] >= 0 && w->kind[i / EN_PARTS] >= 0) { en_refuse(w, EN_R_PART); return 0; }
    if (!en_snapshot_fits(w, ni, ints, nf, floats, nd, doubles)) { en_refuse(w, EN_R_SNAPSHOT); return 0; }
    int nk = w->nkinds, cap = w->capacity;
    size_t n = (size_t)cap;
    const int32_t *c = ints + 3 + 2 * nk;
    memset(w->dense_at, 0, n * sizeof *w->dense_at);
    memset(w->pos, 0, n * 3 * sizeof *w->pos); memset(w->from, 0, n * 3 * sizeof *w->from);
    memset(w->vel, 0, n * 3 * sizeof *w->vel); memset(w->facing, 0, n * sizeof *w->facing); memset(w->age, 0, n * sizeof *w->age);
    memset(w->fslot, 0, n * EN_SLOTS * sizeof *w->fslot); memset(w->islot, 0, n * EN_SLOTS * sizeof *w->islot);
    memset(w->flags, 0, n);
    memset(w->rt_next, 0, n * sizeof *w->rt_next); memset(w->rt_speed, 0, n * sizeof *w->rt_speed);
    memset(w->rt_best, 0, n * sizeof *w->rt_best); memset(w->rt_stall, 0, n * sizeof *w->rt_stall);
    for (size_t i = 0; i < n * EN_PARTS; i++) w->part[i] = -1;
    int a = en_snap_head(nk), b = 0, d = 0;
    for (int r = 0; r < cap; r++) { w->kind[r] = -1; w->rt_route[r] = -1; w->gen[r] = (uint32_t)ints[a++]; }
    w->nfree = c[1];
    for (int i = 0; i < w->nfree; i++) w->free_rows[i] = ints[a++];
    w->live = 0;
    for (int k = 0; k < nk; k++) {
        EnKind *kd = &w->kinds[k];
        int32_t *dense = w->dense + (size_t)k * cap;
        int fs = kd->ints[EN_K_FSLOTS], is = kd->ints[EN_K_ISLOTS];
        kd->live = c[8 + k];
        w->live += kd->live;
        for (int i = 0; i < kd->live; i++) {
            int r = ints[a];
            dense[i] = r; w->dense_at[r] = i; w->kind[r] = (int16_t)k;
            w->flags[r] = (uint8_t)ints[a + 1]; w->rt_route[r] = ints[a + 2]; w->rt_next[r] = ints[a + 3];
            a += 4;
            for (int s = 0; s < is; s++, a += 2) w->islot[(size_t)r * EN_SLOTS + s] = en_snap_i64(ints + a);
            for (int x = 0; x < 3; x++) w->vel[r * 3 + x] = floats[b++];
            w->facing[r] = floats[b++];
            for (int s = 0; s < fs; s++) w->fslot[(size_t)r * EN_SLOTS + s] = floats[b++];
            w->rt_speed[r] = floats[b++]; w->rt_best[r] = floats[b++]; w->rt_stall[r] = floats[b++];
            for (int x = 0; x < 3; x++) w->pos[r * 3 + x] = w->from[r * 3 + x] = doubles[d++];
            w->age[r] = doubles[d++];
        }
    }
    memset(w->routes, 0, (size_t)w->route_points * sizeof *w->routes);
    w->routes_hw = c[2]; w->route_pts_held = 0; w->routes_held = 0;
    for (int i = 0; i < w->routes_hw; i++, a += 4) {
        EnRoute *rt = &w->routes[i];
        rt->start = ints[a]; rt->n = ints[a + 1]; rt->users = ints[a + 2]; rt->state = ints[a + 3];
        if (rt->state == 0) continue;
        memcpy(w->rpts + (size_t)rt->start * 3, doubles + d, (size_t)rt->n * 3 * sizeof *doubles);
        d += rt->n * 3;
        w->route_pts_held += rt->n; w->routes_held++;
    }
    w->nroute_free = c[3];
    for (int i = 0; i < w->nroute_free; i++) w->route_free[i] = ints[a++];
    w->nfx = c[4];
    for (int i = 0; i < w->nfx; i++, a += 2) { w->fx_start[i] = ints[a]; w->fx_len[i] = ints[a + 1]; }
    w->nremoved = c[5];
    for (int i = 0; i < w->nremoved; i++, a += 3) {
        EnRemoval *q = &w->removed[i];
        q->id = en_snap_i64(ints + a); q->reason = ints[a + 2];
        for (int x = 0; x < 3; x++) q->pos[x] = doubles[d++];
    }
    w->tick_count = en_snap_i64(c + 6);
    w->nev = 0; w->nfocus = 0; w->seen = 0; w->waiting = 0; w->in_tick = 0;
    en_grid_build(w);
    return 1;
}

/* Every part any entity holds goes on the release list (en_release_parts), as leaving view would send it: the game
 * frees them (en_releases) before restoring, or before closing the world. Past the list's room the rest are dropped and
 * counted in en_stat(5), so drain the list first. */
static inline void en_release_all(EnWorld *w) {
    for (int k = 0; k < w->nkinds; k++) {
        const int32_t *dense = w->dense + (size_t)k * w->capacity;
        for (int i = 0; i < w->kinds[k].live; i++) en_release_parts(w, dense[i]);
    }
}

#endif

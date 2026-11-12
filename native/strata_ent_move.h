/* strata_ent_move.h — the entity kernel's movers on the ground: the box sweep, walking, routes and unseen movement.
 * Part of strata_ent.h, which includes it after strata_ent_grid.h and before the tick; not a header to include
 * alone. Its state lives in EnWorld's route and walking fields, allocated by en_open.
 *
 * The sweep is Strata.Query.sweep, operation for operation, against the kernel's one-bit-a-voxel copy of the blocks
 * (every stopping block a whole cell): one axis at a time, y then x then z, each axis in steps no longer than the box
 * is thick (at least 0.05 voxels), stopping 1e-4 voxels short of the nearest face met. Contraction is off here as in
 * the rest of strata_ent.h, so on a world without model blocks the two agree to the last bit.
 *
 * Routes. A route is n points (1..EN_ROUTE_MAX), copied at en_route into one pool of route_points points. The pool is
 * first fit over a list of free blocks kept in order of start, neighbouring free blocks joined; nothing is ever moved,
 * so a route is refused when no single free block holds it, however many points are free in all. A route's id is a
 * slot of the route table; freed ids are reused last freed first. A route freed while entities follow it is held
 * (state 2, refusing new followers) until its last follower stops: follows -1 or another route, reaches the end of a
 * route that does not loop, or is removed.
 *
 * Walking, seen (en_walk): the kind's gravity into the vertical velocity, the fall capped at 60 m/s; the horizontal
 * velocity the game's, or, following, the route's speed toward the next waypoint (x and z); the box swept; a
 * horizontal axis blocked while grounded tried again raised by the step height and, if that goes farther, set down
 * again; landing grounds it, its vertical velocity 0. A waypoint is reached within 0.25 m horizontally. A follower
 * making less than 0.1 m of progress toward its waypoint for 1 s writes event 8 once, and again only after it has
 * made that progress and stalled again; it keeps pushing. A sweep that would enter a chunk not held changes nothing
 * that tick: no move, no fall, no waypoint passed, no turn, no event.
 *
 * Walking, unseen (en_walk_unseen): a follower moves at its speed, measured horizontally, along straight lines to
 * each waypoint in turn (y along with it), touching no blocks; one with no route stays. At the end of a route:
 * event 5, then it stops (velocity 0, following nothing), or starts again at the first point if the route loops.
 * Followers face their horizontal travel: facing atan2(dx, dz), +z turned about the vertical as Strata.Anim faces. */
#ifndef STRATA_ENT_MOVE_H
#define STRATA_ENT_MOVE_H
#ifndef STRATA_ENT_H
#error "strata_ent_move.h is part of strata_ent.h; include that"
#endif

#define EN_REACH 0.25              /* a waypoint is reached within this, horizontally, metres */
#define EN_FALL_MAX 60.0           /* m/s */
#define EN_PROGRESS 0.1            /* metres toward the waypoint that count as progress */
#define EN_STALL 1.0               /* seconds without progress before event 8 */
#define EN_SWEEP_VOXELS (1 << 22)  /* voxels a swept box may cover at once; a bigger box is refused (-1) */

/* ---- the sweep ---- */
typedef struct { const EnChunk *c; int kx, ky, kz, ok; } EnChunkCache;

/* 1 solid, 0 clear, -1 not held; the last chunk found is kept in cc */
static inline int en_cached_solid(const EnWorld *w, EnChunkCache *cc, int vx, int vy, int vz) {
    int kx = en_floor32(vx), ky = en_floor32(vy), kz = en_floor32(vz);
    if (!cc->ok || kx != cc->kx || ky != cc->ky || kz != cc->kz) {
        cc->c = en_chunk_find(w, kx, ky, kz); cc->kx = kx; cc->ky = ky; cc->kz = kz; cc->ok = 1;
    }
    return cc->c ? en_chunk_bit(cc->c, vx, vy, vz) : -1;
}

/* Strata.Query.obstacle: the nearest face along axis (forward: the low face of a voxel ahead, else the high face)
 * of a solid voxel the box at q overlaps. 1 found (*face), 0 none, -1 a voxel of the box's range is not held. */
static inline int en_obstacle(const EnWorld *w, EnChunkCache *cc, double sc, const double q[3], const double h[3],
                              int axis, int forward, double *face) {
    double lo[3], hi[3], lim = (double)EN_CHUNK_COORD_MAX * 32;
    int v0[3], n[3];
    for (int a = 0; a < 3; a++) {
        lo[a] = q[a] - h[a]; hi[a] = q[a] + h[a];
        double f0 = floor(lo[a] / sc), f1 = floor(hi[a] / sc);
        if (!(fabs(f0) < lim && fabs(f1) < lim)) return -1;   /* no chunk is held out there (also NaN) */
        v0[a] = (int)f0; n[a] = (int)f1 - v0[a] + 1;
    }
    if ((double)n[0] * n[1] * n[2] > EN_SWEEP_VOXELS) return -1;
    int have = 0; double best = 0;
    for (int z = 0; z < n[2]; z++)
        for (int y = 0; y < n[1]; y++)
            for (int x = 0; x < n[0]; x++) {
                int v[3] = {v0[0] + x, v0[1] + y, v0[2] + z};
                int s = en_cached_solid(w, cc, v[0], v[1], v[2]);
                if (s < 0) return -1;
                if (!s) continue;
                double o0[3], o1[3];
                for (int a = 0; a < 3; a++) { o0[a] = ((double)v[a] + 0.0) * sc; o1[a] = ((double)v[a] + 1.0) * sc; }
                if (lo[0] < o1[0] && hi[0] > o0[0] && lo[1] < o1[1] && hi[1] > o0[1] && lo[2] < o1[2] && hi[2] > o0[2]) {
                    double f = forward ? o0[axis] : o1[axis];
                    if (!have || (forward && f < best) || (!forward && f > best)) { have = 1; best = f; }
                }
            }
    if (have) *face = best;
    return have;
}

/* Strata.Query.move_axis: move q along axis by left; *blocked set if a face stopped it. 0, or -1 (not held). */
static inline int en_move_axis(const EnWorld *w, EnChunkCache *cc, double sc, double q[3], const double h[3], int axis,
                               double left, int *blocked) {
    for (int steps = 100000;; steps--) {
        if (fabs(left) < 0.000000000001 || steps <= 0) return 0;
        double cap0 = 2.0 * h[axis];
        double cap = cap0 < sc * 0.05 ? sc * 0.05 : cap0;
        double step = left > cap ? cap : left < 0.0 - cap ? 0.0 - cap : left;
        q[axis] = q[axis] + step;
        double face = 0;
        int r = en_obstacle(w, cc, sc, q, h, axis, step > 0.0, &face);
        if (r < 0) return -1;
        if (r) {
            q[axis] = step > 0.0 ? face - h[axis] - sc * 0.0001 : face + h[axis] + sc * 0.0001;
            *blocked = 1;
            return 0;
        }
        left = left - step;
    }
}

/* Move a box centred at p (metres) with half extents h by d, one axis at a time (y, then x, then z), against the
 * kernel's blocks: out its end, blocked[3] the axes a face stopped. 0; -1 if any voxel the box would reach is in a
 * chunk not held, a number is not finite, a half extent is negative, or the box covers more than EN_SWEEP_VOXELS
 * voxels at once, and then nothing moves (out = p, blocked all 0). */
static inline int en_sweep(const EnWorld *w, const double p[3], const double h[3], const double d[3], double out[3], int blocked[3]) {
    double q[3] = {p[0], p[1], p[2]};
    int b[3] = {0, 0, 0}, ok = 1;
    for (int a = 0; a < 3; a++) ok &= isfinite(p[a]) && isfinite(h[a]) && isfinite(d[a]) && h[a] >= 0;
    if (ok) {
        EnChunkCache cc = {NULL, 0, 0, 0, 0};
        double sc = w->voxel_metres;
        ok = en_move_axis(w, &cc, sc, q, h, 1, d[1], &b[1]) == 0 && en_move_axis(w, &cc, sc, q, h, 0, d[0], &b[0]) == 0 &&
             en_move_axis(w, &cc, sc, q, h, 2, d[2], &b[2]) == 0;
    }
    for (int a = 0; a < 3; a++) { out[a] = ok ? q[a] : p[a]; blocked[a] = ok ? b[a] : 0; }
    return ok ? 0 : -1;
}

/* ---- routes ---- */
/* give back pool points start..start+n, joining neighbouring free blocks */
static inline void en_pool_release(EnWorld *w, int start, int n) {
    int j = 0;
    while (j < w->nfx && w->fx_start[j] < start) j++;
    int prev = j > 0 && w->fx_start[j - 1] + w->fx_len[j - 1] == start;
    int next = j < w->nfx && start + n == w->fx_start[j];
    if (prev && next) {
        w->fx_len[j - 1] += n + w->fx_len[j];
        memmove(w->fx_start + j, w->fx_start + j + 1, (size_t)(w->nfx - j - 1) * sizeof *w->fx_start);
        memmove(w->fx_len + j, w->fx_len + j + 1, (size_t)(w->nfx - j - 1) * sizeof *w->fx_len);
        w->nfx--;
    } else if (prev) w->fx_len[j - 1] += n;
    else if (next) { w->fx_start[j] = start; w->fx_len[j] += n; }
    else {
        memmove(w->fx_start + j + 1, w->fx_start + j, (size_t)(w->nfx - j) * sizeof *w->fx_start);
        memmove(w->fx_len + j + 1, w->fx_len + j, (size_t)(w->nfx - j) * sizeof *w->fx_len);
        w->fx_start[j] = start; w->fx_len[j] = n; w->nfx++;
    }
}
/* the first free block holding n points: its start, taken; -1 if none */
static inline int en_pool_take(EnWorld *w, int n) {
    for (int j = 0; j < w->nfx; j++) {
        if (w->fx_len[j] < n) continue;
        int start = w->fx_start[j];
        w->fx_start[j] += n; w->fx_len[j] -= n;
        if (w->fx_len[j] == 0) {
            memmove(w->fx_start + j, w->fx_start + j + 1, (size_t)(w->nfx - j - 1) * sizeof *w->fx_start);
            memmove(w->fx_len + j, w->fx_len + j + 1, (size_t)(w->nfx - j - 1) * sizeof *w->fx_len);
            w->nfx--;
        }
        return start;
    }
    return -1;
}
static inline void en_route_release(EnWorld *w, int route) {
    EnRoute *rt = &w->routes[route];
    en_pool_release(w, rt->start, rt->n);
    w->route_pts_held -= rt->n; w->routes_held--;
    rt->state = 0; rt->users = 0;
    w->route_free[w->nroute_free++] = route;
}

/* A route of n points (3 doubles each, metres), copied into the pool. Its id, or -1: n outside 1..EN_ROUTE_MAX or a
 * point not finite (EN_R_VALUE), or no free block of the pool holds n points (EN_R_FULL). */
static inline int en_route(EnWorld *w, int n, const double *points) {
    if (n < 1 || n > EN_ROUTE_MAX || !points) return en_refuse(w, EN_R_VALUE);
    for (int i = 0; i < 3 * n; i++) if (!isfinite(points[i])) return en_refuse(w, EN_R_VALUE);
    int start = en_pool_take(w, n);
    if (start < 0) return en_refuse(w, EN_R_FULL);
    /* every held route holds a point, so a free id exists whenever the pool had room */
    int id = w->nroute_free > 0 ? w->route_free[--w->nroute_free] : w->routes_hw++;
    EnRoute *rt = &w->routes[id];
    rt->start = start; rt->n = n; rt->users = 0; rt->state = 1;
    memcpy(w->rpts + (size_t)start * 3, points, (size_t)n * 3 * sizeof *points);
    w->route_pts_held += n; w->routes_held++;
    return id;
}

/* Free a route: at once if nothing follows it, else once its last follower stops; new followers are refused from
 * now on. Refused (EN_R_ROUTE) for a route not held or already freed. */
static inline void en_route_free(EnWorld *w, int route) {
    if (route < 0 || route >= w->routes_hw || w->routes[route].state != 1) { en_refuse(w, EN_R_ROUTE); return; }
    if (w->routes[route].users == 0) en_route_release(w, route);
    else w->routes[route].state = 2;
}

/* row r stops following its route (if any); a freed route whose last follower this was is released */
static inline void en_unfollow(EnWorld *w, int r) {
    int route = w->rt_route[r];
    if (route < 0) return;
    EnRoute *rt = &w->routes[route];
    rt->users--;
    if (rt->state == 2 && rt->users == 0) en_route_release(w, route);
    w->rt_route[r] = -1;
    w->flags[r] &= (uint8_t)~(EN_F_LOOPS | EN_F_BLOCKED);
}

static inline const double *en_waypoint(const EnWorld *w, int r) {
    return w->rpts + ((size_t)w->routes[w->rt_route[r]].start + w->rt_next[r]) * 3;
}
static inline double en_flat_dist(const double *p, const double *q) {
    double dx = q[0] - p[0], dz = q[2] - p[2];
    return sqrt(dx * dx + dz * dz);
}
/* the follower's progress mark restarts from where it is, toward its current waypoint */
static inline void en_mark(EnWorld *w, int r) {
    w->rt_best[r] = (float)en_flat_dist(w->pos + r * 3, en_waypoint(w, r));
    w->rt_stall[r] = 0;
    w->flags[r] &= (uint8_t)~EN_F_BLOCKED;
}

/* Entity id follows route at speed metres a second (horizontally), from its first point, looping or not; route -1
 * stops following (the horizontal velocity, the kernel's while following, becomes 0). Refused: a dead id
 * (EN_R_ENTITY), a kind not walking (EN_R_MOVER), a route not held or freed (EN_R_ROUTE), a speed negative or not
 * finite, or loops not 0/1 (EN_R_VALUE). */
static inline void en_follow(EnWorld *w, int64_t id, int route, double speed, int loops) {
    int r = en_row(w, id);
    if (r < 0) { en_refuse(w, EN_R_ENTITY); return; }
    if (w->kinds[w->kind[r]].ints[EN_K_MOVER] != 2) { en_refuse(w, EN_R_MOVER); return; }
    if (route == -1) {
        if (w->rt_route[r] >= 0) { en_unfollow(w, r); w->vel[r * 3] = 0; w->vel[r * 3 + 2] = 0; }
        return;
    }
    if (route < 0 || route >= w->routes_hw || w->routes[route].state != 1) { en_refuse(w, EN_R_ROUTE); return; }
    if (!en_float_ok(speed) || speed < 0 || (loops | 1) != 1) { en_refuse(w, EN_R_VALUE); return; }
    en_unfollow(w, r);
    w->routes[route].users++;
    w->rt_route[r] = route; w->rt_next[r] = 0; w->rt_speed[r] = (float)speed;
    if (loops) w->flags[r] |= EN_F_LOOPS;
    en_mark(w, r);
}

/* 1 if the walker stood on something after its last seen step */
static inline int en_grounded(const EnWorld *w, int64_t id) {
    int r = en_row(w, id);
    return r >= 0 && (w->flags[r] & EN_F_GROUNDED) != 0;
}

/* The follower's waypoint moves on to the next point: at the end of a route that loops, the first; at the end of
 * one that does not, event 5 and the follower stops. 1 if it stopped. */
static inline int en_next_waypoint(EnWorld *w, int r) {
    int route = w->rt_route[r];
    if (++w->rt_next[r] < w->routes[route].n) { en_mark(w, r); return 0; }
    if (w->flags[r] & EN_F_LOOPS) { w->rt_next[r] = 0; en_mark(w, r); return 0; }
    const double *p = w->pos + r * 3;
    en_event(w, EN_E_ROUTE, en_id(w, r), 0, route, 0, 0, 0, p[0], p[1], p[2]);
    en_unfollow(w, r);
    w->vel[r * 3] = w->vel[r * 3 + 1] = w->vel[r * 3 + 2] = 0;
    return 1;
}

/* ---- walking ---- */
/* the horizontal distance moved from p to a, squared */
static inline double en_flat_moved(const double *p, const double *a) {
    double dx = a[0] - p[0], dz = a[2] - p[2];
    return dx * dx + dz * dz;
}

/* One seen walking step of row r over dt (> 0). The waypoints it passes and the way it turns are worked out first
 * without changing anything, and made so only once the sweep has answered: a walker whose sweep would enter a chunk
 * not held changes nothing that tick (position, velocity, route progress, facing, grounded, events). */
static inline void en_walk(EnWorld *w, int r, const EnKind *kd, double dt) {
    float *v = w->vel + r * 3;
    double *p = w->pos + r * 3;
    double vx = v[0], vz = v[2];
    int passes = 0, turns = 0;
    float face = 0;
    if (w->rt_route[r] >= 0) {
        const EnRoute *rt = &w->routes[w->rt_route[r]];
        const double *pts = w->rpts + (size_t)rt->start * 3;
        int nx = w->rt_next[r], stops = 0;
        for (int guard = 0; guard <= rt->n && en_flat_dist(p, pts + (size_t)nx * 3) <= EN_REACH; guard++) {
            passes++;   /* as en_next_waypoint will, once the step is taken */
            if (nx + 1 < rt->n) nx++;
            else if (w->flags[r] & EN_F_LOOPS) nx = 0;
            else { stops = 1; break; }
        }
        if (stops) vx = vz = 0;
        else {
            const double *q = pts + (size_t)nx * 3;
            double dx = q[0] - p[0], dz = q[2] - p[2], dh = sqrt(dx * dx + dz * dz), s = w->rt_speed[r];
            if (dh > 0) {
                if (s * dt > dh) s = dh / dt;   /* no farther than the waypoint */
                vx = dx / dh * s; vz = dz / dh * s;
                face = (float)atan2(dx, dz); turns = 1;
            } else vx = vz = 0;
        }
    }
    double vy = v[1];
    if (kd->ints[EN_K_GRAVITY]) vy = vy - kd->floats[EN_K_GRAVITY_MS2] * dt;
    if (vy < 0.0 - EN_FALL_MAX) vy = 0.0 - EN_FALL_MAX;
    double h[3] = {kd->floats[EN_K_HX], kd->floats[EN_K_HY], kd->floats[EN_K_HZ]};
    double d[3] = {vx * dt, vy * dt, vz * dt}, out[3];
    int b[3];
    if (en_sweep(w, p, h, d, out, b) < 0) return;   /* it would enter a chunk not held: held where it is, unchanged */
    for (int i = 0; i < passes; i++) if (en_next_waypoint(w, r)) break;   /* the last of them stops it, if it does */
    if (turns) w->facing[r] = face;
    double step = kd->floats[EN_K_STEP];
    if ((w->flags[r] & EN_F_GROUNDED) && (b[0] || b[2]) && step > 0) {
        double up[3] = {0, step, 0}, raised[3], across[3], down[3];
        int bu[3], ba[3], bd[3];
        if (en_sweep(w, p, h, up, raised, bu) == 0 && raised[1] > p[1]) {
            double flat[3] = {d[0], 0, d[2]};
            if (en_sweep(w, raised, h, flat, across, ba) == 0 && en_flat_moved(p, across) > en_flat_moved(p, out) + 1e-12) {
                double drop[3] = {0, (p[1] - raised[1]) + d[1], 0};
                if (en_sweep(w, across, h, drop, down, bd) == 0) {
                    for (int a = 0; a < 3; a++) out[a] = down[a];
                    b[0] = ba[0]; b[1] = bd[1]; b[2] = ba[2];
                }
            }
        }
    }
    for (int a = 0; a < 3; a++) p[a] = out[a];
    int landed = b[1] && vy <= 0;
    if (b[1]) vy = 0;
    w->flags[r] = (uint8_t)((w->flags[r] & ~EN_F_GROUNDED) | (landed ? EN_F_GROUNDED : 0));
    v[0] = (float)vx; v[1] = (float)vy; v[2] = (float)vz;
    if (w->rt_route[r] >= 0) {   /* progress toward the waypoint, or a stall */
        double dist = en_flat_dist(p, en_waypoint(w, r));
        if (dist <= w->rt_best[r] - EN_PROGRESS) en_mark(w, r);
        else {
            w->rt_stall[r] += (float)dt;
            if (w->rt_stall[r] >= EN_STALL && !(w->flags[r] & EN_F_BLOCKED)) {
                w->flags[r] |= EN_F_BLOCKED;
                en_event(w, EN_E_BLOCKED, en_id(w, r), 0, w->rt_route[r], w->rt_next[r], 0, 0, p[0], p[1], p[2]);
            }
        }
    }
}

/* One unseen walking step of row r over dt (> 0): along its route touching no blocks, or nowhere. */
static inline void en_walk_unseen(EnWorld *w, int r, double dt) {
    if (w->rt_route[r] < 0) return;
    double *p = w->pos + r * 3;
    float *v = w->vel + r * 3;
    double left = w->rt_speed[r] * dt;
    w->flags[r] &= (uint8_t)~EN_F_GROUNDED;   /* it stands on nothing the kernel knows */
    for (int guard = 0, n = w->routes[w->rt_route[r]].n; guard <= n; guard++) {
        const double *q = en_waypoint(w, r);
        double dx = q[0] - p[0], dy = q[1] - p[1], dz = q[2] - p[2], dh = sqrt(dx * dx + dz * dz);
        if (dh > 0) w->facing[r] = (float)atan2(dx, dz);
        if (dh <= left) {   /* reached it, with distance to spare */
            p[0] = q[0]; p[1] = q[1]; p[2] = q[2];
            left = left - dh;
            if (en_next_waypoint(w, r)) return;
            continue;
        }
        double f = left / dh, s = w->rt_speed[r];
        p[0] = p[0] + dx * f; p[1] = p[1] + dy * f; p[2] = p[2] + dz * f;
        v[0] = (float)(dx / dh * s); v[1] = 0; v[2] = (float)(dz / dh * s);
        w->rt_stall[r] = 0; w->rt_best[r] = (float)(dh - left);
        return;
    }
}

#endif

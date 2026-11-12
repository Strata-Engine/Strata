/* strata_ent_grid.h — the entity kernel's grid, and what is found with it:
 * near, the entity ray, and touch pairs. Part of strata_ent.h, which
 * includes it before the tick (en_tick ends by building the grid and
 * testing the touch pairs); not a header to include alone. Its state lives
 * in EnWorld's grid fields, allocated by en_open.
 *
 * The grid. At the end of each en_tick every live entity is put in a
 * uniform grid of EN_CELL (4 m) cells, built fresh: an entity sits in the
 * one cell holding its position. Cells are keyed by hash, bucketed by
 * (cell, kind), so a query for one kind never looks at another kind's
 * entities. The buckets are a counting sort: g_start[b] .. g_start[b+1] are
 * bucket b's entries in g_ent, at least twice as many buckets as entries.
 * A bucket can hold other cells' entities (a hash collision), so a query
 * keeps only the entries whose cell is the one it asked for, and so sees an
 * entity at most once. A query whose box spans more cells than the kind has
 * entities walks the kind's entries instead (g_byk, kind by kind in dense
 * order). Cell coordinates are clamped to +-EN_CELL_MAX, so a far position
 * still has a cell; clamping is monotone, so every query still finds it.
 *
 * Each entry keeps what the last tick knew: id, position, kind and its
 * touch box. near and en_ray_entities read those, not the live columns, and
 * skip an id that has since been removed; an entity added since is not in
 * the grid until the next tick ends.
 *
 * A touch box is the box the entity swept over the tick's step: the union of
 * its box (position +- the kind's half extents) where the step began and
 * where it ended. An entity that did not move has its plain box. Two boxes
 * touch when they overlap strictly on every axis; faces that only meet do
 * not touch. */
#ifndef STRATA_ENT_GRID_H
#define STRATA_ENT_GRID_H
#ifndef STRATA_ENT_H
#error "strata_ent_grid.h is part of strata_ent.h; include that"
#endif

#pragma STDC FP_CONTRACT OFF   /* distances and box tests unfused, so a test's brute force computes the same */

static inline int32_t en_gcell(double v) {
    double c = floor(v * (1.0 / EN_CELL));
    if (!(c > -EN_CELL_MAX)) c = -EN_CELL_MAX;   /* also -inf */
    if (c > EN_CELL_MAX) c = EN_CELL_MAX;
    return (int32_t)c;
}
static inline uint32_t en_grid_hash(int32_t x, int32_t y, int32_t z, int kind) {
    uint32_t h = (uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u ^ (uint32_t)z * 83492791u ^ (uint32_t)kind * 2654435761u;
    h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
    return h;
}
/* how many of kind's entities the grid holds */
static inline int en_grid_count(const EnWorld *w, int kind) { return w->g_kstart[kind + 1] - w->g_kstart[kind]; }

/* The cells holding positions in lo..hi (grown by a hair, so rounding at a cell's edge loses nothing) into c0..c1;
 * answers how many cells that is. */
static inline double en_grid_range(const double lo[3], const double hi[3], int32_t c0[3], int32_t c1[3]) {
    double n = 1;
    for (int a = 0; a < 3; a++) {
        double s0 = 0.000001 * (1 + fabs(lo[a])), s1 = 0.000001 * (1 + fabs(hi[a]));
        c0[a] = en_gcell(lo[a] - s0); c1[a] = en_gcell(hi[a] + s1);
        n *= (double)c1[a] - (double)c0[a] + 1;
    }
    return n;
}

/* Rebuild the grid from the live rows. w->from holds where each row's step began. */
static inline void en_grid_build(EnWorld *w) {
    int n = w->live, nb = 16;
    while (nb < 2 * n) nb <<= 1;
    memset(w->g_start, 0, (size_t)(nb + 1) * sizeof *w->g_start);
    int j = 0;
    for (int k = 0; k < EN_KINDS; k++) {   /* pass 1: each entry's bucket, counted */
        w->g_kstart[k] = j;
        if (k >= w->nkinds) continue;
        const int32_t *dense = w->dense + (size_t)k * w->capacity;
        for (int i = 0; i < w->kinds[k].live; i++, j++) {
            const double *p = w->pos + dense[i] * 3;
            uint32_t b = en_grid_hash(en_gcell(p[0]), en_gcell(p[1]), en_gcell(p[2]), k) & (uint32_t)(nb - 1);
            w->g_bucket[j] = (int32_t)b;
            w->g_start[b + 1]++;
        }
    }
    w->g_kstart[EN_KINDS] = j;
    for (int b = 0; b < nb; b++) w->g_start[b + 1] += w->g_start[b];
    j = 0;
    for (int k = 0; k < w->nkinds; k++) {   /* pass 2: each entry into its bucket's next place */
        const EnKind *kd = &w->kinds[k];
        const double h[3] = {kd->floats[EN_K_HX], kd->floats[EN_K_HY], kd->floats[EN_K_HZ]};
        double *reach = w->g_reach[k];
        reach[0] = reach[1] = reach[2] = 0;
        const int32_t *dense = w->dense + (size_t)k * w->capacity;
        for (int i = 0; i < kd->live; i++, j++) {
            int r = dense[i];
            int at = w->g_start[w->g_bucket[j]]++;
            EnGridEnt *e = &w->g_ent[at];
            w->g_byk[j] = at;
            e->id = en_id(w, r); e->kind = k;
            for (int a = 0; a < 3; a++) {
                double p = w->pos[r * 3 + a], f = w->from[r * 3 + a];
                e->p[a] = p; e->cell[a] = en_gcell(p);
                e->lo[a] = (f < p ? f : p) - h[a];
                e->hi[a] = (f > p ? f : p) + h[a];
                double out = p - e->lo[a] > e->hi[a] - p ? p - e->lo[a] : e->hi[a] - p;
                if (out > reach[a]) reach[a] = out;
            }
        }
    }
    for (int b = nb; b > 0; b--) w->g_start[b] = w->g_start[b - 1];   /* each start was moved to its bucket's end */
    w->g_start[0] = 0;
    w->g_n = n; w->g_nb = nb;
}

/* A walk over the entries of one kind whose positions may lie in a box: the cells' buckets, or the kind's whole
 * list when that is shorter. Each entry comes once. */
typedef struct {
    const EnWorld *w;
    int kind, linear, done;
    int32_t c0[3], c1[3], c[3];
    int i, end;
} EnGridWalk;
static inline void en_grid_bucket(EnGridWalk *g) {
    uint32_t b = en_grid_hash(g->c[0], g->c[1], g->c[2], g->kind) & (uint32_t)(g->w->g_nb - 1);
    g->i = g->w->g_start[b]; g->end = g->w->g_start[b + 1];
}
static inline void en_grid_walk(EnGridWalk *g, const EnWorld *w, int kind, const double lo[3], const double hi[3]) {
    g->w = w; g->kind = kind; g->done = 0;
    double cells = en_grid_range(lo, hi, g->c0, g->c1);
    g->linear = cells > en_grid_count(w, kind);
    if (g->linear) { g->i = w->g_kstart[kind]; g->end = w->g_kstart[kind + 1]; return; }
    memcpy(g->c, g->c0, sizeof g->c);
    en_grid_bucket(g);
}
/* the next entry, or NULL */
static inline const EnGridEnt *en_grid_next(EnGridWalk *g) {
    const EnWorld *w = g->w;
    if (g->linear) return g->i < g->end ? &w->g_ent[w->g_byk[g->i++]] : NULL;
    while (!g->done) {
        while (g->i < g->end) {
            const EnGridEnt *e = &w->g_ent[g->i++];
            if (e->kind == g->kind && e->cell[0] == g->c[0] && e->cell[1] == g->c[1] && e->cell[2] == g->c[2]) return e;
        }
        if (g->c[2] < g->c1[2]) g->c[2]++;
        else if (g->c[1] < g->c1[1]) { g->c[2] = g->c0[2]; g->c[1]++; }
        else if (g->c[0] < g->c1[0]) { g->c[2] = g->c0[2]; g->c[1] = g->c0[1]; g->c[0]++; }
        else { g->done = 1; break; }
        en_grid_bucket(g);
    }
    return NULL;
}

static inline int en_cmp_id(const void *a, const void *b) {
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return x < y ? -1 : x > y;
}

/* The ids of the entities within radius metres of p (centre to position) whose kind's bit is set in kind_mask,
 * sorted by id: the lowest max of them written to ids, answering how many were written; with ids NULL, answers
 * how many there are. Reads the last tick's grid (see above). A radius negative or NaN finds nothing. */
static inline int en_near(const EnWorld *w, const double p[3], double radius, uint64_t kind_mask, int max, int64_t *ids) {
    if (!p || !isfinite(p[0]) || !isfinite(p[1]) || !isfinite(p[2]) || !(radius >= 0)) return 0;
    if (max < 0) max = 0;
    double rr = radius * radius;
    double lo[3] = {p[0] - radius, p[1] - radius, p[2] - radius}, hi[3] = {p[0] + radius, p[1] + radius, p[2] + radius};
    int total = 0, held = 0;   /* ids[0..held) is a max-heap of the lowest ids found */
    for (int k = 0; k < w->nkinds; k++) {
        if (!((kind_mask >> k) & 1) || en_grid_count(w, k) == 0) continue;
        EnGridWalk g; en_grid_walk(&g, w, k, lo, hi);
        for (const EnGridEnt *e; (e = en_grid_next(&g));) {
            double dx = e->p[0] - p[0], dy = e->p[1] - p[1], dz = e->p[2] - p[2];
            if (dx * dx + dy * dy + dz * dz <= rr && en_row(w, e->id) >= 0) {
                total++;
                if (ids && max > 0) {
                    int64_t id = e->id;
                    int at;
                    if (held < max) {   /* sift up */
                        at = held++;
                        while (at > 0 && ids[(at - 1) / 2] < id) { ids[at] = ids[(at - 1) / 2]; at = (at - 1) / 2; }
                        ids[at] = id;
                    } else if (id < ids[0]) {   /* replace the largest, sift down */
                        at = 0;
                        for (;;) {
                            int c = 2 * at + 1;
                            if (c >= held) break;
                            if (c + 1 < held && ids[c + 1] > ids[c]) c++;
                            if (ids[c] <= id) break;
                            ids[at] = ids[c]; at = c;
                        }
                        ids[at] = id;
                    }
                }
            }
        }
    }
    if (!ids) return total;
    qsort(ids, (size_t)held, sizeof *ids, en_cmp_id);
    return held;
}

/* The nearest entity of a kind in kind_mask whose box (position +- the kind's half extents, in the last tick's grid)
 * the ray from o along d (any length) meets within max metres; a ray starting inside a box meets it at 0. Ties go to
 * the lower id. Its id, *t the distance in metres; -1 for none (also a zero direction, max <= 0 or a number not
 * finite), *t untouched. */
static inline int64_t en_ray_entities(const EnWorld *w, const double o[3], const double d[3], double max, uint64_t kind_mask, double *t) {
    if (!o || !d || !t) return -1;
    double len = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!isfinite(len) || !isfinite(max) || !isfinite(o[0]) || !isfinite(o[1]) || !isfinite(o[2])) return -1;
    if (len < 0.000000000001 || max <= 0) return -1;
    double u[3] = {d[0] / len, d[1] / len, d[2] / len};
    int64_t best = -1; double bt = 0;
    for (int k = 0; k < w->nkinds; k++) {
        if (!((kind_mask >> k) & 1) || en_grid_count(w, k) == 0) continue;
        const EnKind *kd = &w->kinds[k];
        const double h[3] = {kd->floats[EN_K_HX], kd->floats[EN_K_HY], kd->floats[EN_K_HZ]};
        double lo[3], hi[3];
        for (int a = 0; a < 3; a++) {
            double end = o[a] + u[a] * max;
            lo[a] = (end < o[a] ? end : o[a]) - h[a];
            hi[a] = (end > o[a] ? end : o[a]) + h[a];
        }
        if (best >= 0)   /* nothing farther than the best so far can win */
            for (int a = 0; a < 3; a++) {
                double end = o[a] + u[a] * bt;
                lo[a] = (end < o[a] ? end : o[a]) - h[a];
                hi[a] = (end > o[a] ? end : o[a]) + h[a];
            }
        EnGridWalk g; en_grid_walk(&g, w, k, lo, hi);
        for (const EnGridEnt *e; (e = en_grid_next(&g));) {
            double tn = 0, tf = max;
            int hit = 1;
            for (int a = 0; a < 3 && hit; a++) {
                double blo = e->p[a] - h[a], bhi = e->p[a] + h[a];
                if (u[a] == 0) { if (o[a] < blo || o[a] > bhi) hit = 0; continue; }
                double t1 = (blo - o[a]) / u[a], t2 = (bhi - o[a]) / u[a];
                if (t1 > t2) { double s = t1; t1 = t2; t2 = s; }
                if (t1 > tn) tn = t1;
                if (t2 < tf) tf = t2;
            }
            if (hit && tn <= tf && (best < 0 || tn < bt || (tn == bt && e->id < best)) && en_row(w, e->id) >= 0) {
                best = e->id; bt = tn;
            }
        }
    }
    if (best >= 0) *t = bt;
    return best;
}

/* Ask for touch events between kind_a and kind_b, in either order, from the next tick on. Declaring a pair again
 * changes nothing. Refused (EN_R_KIND) for no such kind. */
static inline void en_touching(EnWorld *w, int kind_a, int kind_b) {
    if (kind_a < 0 || kind_a >= w->nkinds || kind_b < 0 || kind_b >= w->nkinds) { en_refuse(w, EN_R_KIND); return; }
    int lo = kind_a < kind_b ? kind_a : kind_b, hi = kind_a < kind_b ? kind_b : kind_a;
    w->pairs[lo] |= (uint64_t)1 << hi;
}

/* One event 1 for each touching pair of declared kinds, from the grid just built: a the entity of the lower kind
 * (of one kind, the lower id), b the other, the point the centre of where their touch boxes overlap. Each declared
 * pair of kinds is tested once, in kind order; the kind with fewer entities is walked in dense order and each
 * entity asks the grid for the other kind's entities its touch box could reach. */
static inline void en_touches(EnWorld *w) {
    for (int i = 0; i < w->nkinds; i++) {
        if (!w->pairs[i] || en_grid_count(w, i) == 0) continue;
        for (int j = i; j < w->nkinds; j++) {
            if (!((w->pairs[i] >> j) & 1) || en_grid_count(w, j) == 0) continue;
            int ka = en_grid_count(w, j) < en_grid_count(w, i) ? j : i, kb = ka == i ? j : i;
            const double *reach = w->g_reach[kb];
            for (int ja = w->g_kstart[ka]; ja < w->g_kstart[ka + 1]; ja++) {
                const EnGridEnt *ea = &w->g_ent[w->g_byk[ja]];
                double lo[3], hi[3];
                for (int a = 0; a < 3; a++) { lo[a] = ea->lo[a] - reach[a]; hi[a] = ea->hi[a] + reach[a]; }
                EnGridWalk g; en_grid_walk(&g, w, kb, lo, hi);
                for (const EnGridEnt *eb; (eb = en_grid_next(&g));) {
                    if ((i != j || ea->id < eb->id) &&
                        ea->lo[0] < eb->hi[0] && eb->lo[0] < ea->hi[0] && ea->lo[1] < eb->hi[1] && eb->lo[1] < ea->hi[1] &&
                        ea->lo[2] < eb->hi[2] && eb->lo[2] < ea->hi[2]) {
                        const EnGridEnt *a = ka == i ? ea : eb, *b = ka == i ? eb : ea;
                        double c[3];
                        for (int x = 0; x < 3; x++) {
                            double l = a->lo[x] > b->lo[x] ? a->lo[x] : b->lo[x], h = a->hi[x] < b->hi[x] ? a->hi[x] : b->hi[x];
                            c[x] = (l + h) * 0.5;
                        }
                        en_event(w, EN_E_TOUCH, a->id, b->id, 0, 0, 0, 0, c[0], c[1], c[2]);
                    }
                }
            }
        }
    }
}

#pragma STDC FP_CONTRACT DEFAULT
#endif

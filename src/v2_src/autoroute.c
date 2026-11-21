/* autoroute.c - deterministic two-layer board router (see autoroute.h). Integer nanometres only. */
#include "autoroute.h"

#include "filesystem.h"

#include <stdio.h>
#include <string.h>

#define NL 2                      /* routing layers: 0 = F.Cu, 1 = B.Cu */
#define INF UINT64_MAX
#define NONE SIZE_MAX

typedef struct Grid { G2Nm minx, miny, maxx, maxy, pitch; size_t nx, ny, cells; } Grid;

typedef struct Items {
    BoardTrack *tracks; size_t nt, ct;
    BoardVia *vias; size_t nv, cv;
} Items;

typedef struct Router {
    Arena *a;
    const Board *b;
    const AutorouteRules *r;
    Grid g;
    G2Nm slack, via_r;
    uint8_t *blocked[NL];         /* per cell: 1 = no track of the current net may pass */
    uint8_t *via_blocked;         /* per cell: 1 = no via of the current net */
    uint8_t *in_tree;             /* per node (layer * cells + cell) */
    uint64_t *dist, *h;
    size_t *parent, *heap, *heap_pos;
    int32_t *target;              /* per node: pad slot + 1 of an unconnected pad, or 0 */
    size_t *touched; size_t touched_n, touched_cap;
    Items items;                  /* everything routed in the current pass */
    bool oom;
    char why_buf[256];            /* the last exact-check failure, in words */
} Router;

/* ------------------------------------------------------------------------------------------------- helpers */

static G2Nm half_up(G2Nm w) { return w / 2 + w % 2; }

static bool on_layer(const BoardPad *p, int l)
{
    BoardLayer bl = l == 0 ? BOARD_LAYER_F_CU : BOARD_LAYER_B_CU;
    return (p->layers & BOARD_LAYER_BIT(bl)) != 0;
}

static G2Point center(const Grid *g, size_t cell)
{
    G2Point p = {g->minx + (G2Nm)(cell % g->nx) * g->pitch, g->miny + (G2Nm)(cell / g->nx) * g->pitch};
    return p;
}

/* Cells whose centres lie within `box` grown by `thr`. False when none. */
static bool cell_range(const Grid *g, G2Aabb box, G2Nm thr, size_t *x0, size_t *x1, size_t *y0, size_t *y1)
{
    G2Nm lx = box.min.x - thr - g->minx, hx = box.max.x + thr - g->minx;
    G2Nm ly = box.min.y - thr - g->miny, hy = box.max.y + thr - g->miny;
    if (hx < 0 || hy < 0) return false;
    if (lx < 0) lx = 0;
    if (ly < 0) ly = 0;
    *x0 = (size_t)(lx / g->pitch);
    *y0 = (size_t)(ly / g->pitch);
    *x1 = (size_t)(hx / g->pitch);
    *y1 = (size_t)(hy / g->pitch);
    if (*x0 >= g->nx || *y0 >= g->ny) return false;
    if (*x1 >= g->nx) *x1 = g->nx - 1;
    if (*y1 >= g->ny) *y1 = g->ny - 1;
    return *x0 <= *x1 && *y0 <= *y1;
}

static G2Aabb seg_box(G2Point a, G2Point b)
{
    G2Aabb q = {{a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y}, {a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y}};
    return q;
}

/* After the call has run (a pointer, so the order of argument evaluation cannot matter); errors count as too close. */
static bool less(G2Status st, const G2Order *o) { return st != G2_OK || *o == G2_ORDER_LESS; }

static G2Nm net_width(const AutorouteRules *r, const Board *b, uint32_t net)
{
    const BoardNet *n = board_net_by_id(b, net);
    for (size_t i = 0; n && n->name && i < r->width_count; i++)
        if (r->widths[i].net && !strcmp(r->widths[i].net, n->name)) return r->widths[i].width_nm;
    return r->track_nm;
}

static void *grow(Router *R, void *old, size_t used, size_t *cap, size_t size)
{
    size_t c = *cap ? *cap * 2 : 64;
    void *n = arena_alloc_array(R->a, size, c);
    if (!n) { R->oom = true; return NULL; }
    if (used) memcpy(n, old, used * size);
    *cap = c;
    return n;
}

static bool add_track(Router *R, G2Point a, G2Point b, G2Nm w, int l, uint32_t net)
{
    if (a.x == b.x && a.y == b.y) return true;
    Items *it = &R->items;
    if (it->nt == it->ct) { BoardTrack *t = grow(R, it->tracks, it->nt, &it->ct, sizeof *t); if (!t) return false; it->tracks = t; }
    BoardTrack *t = &it->tracks[it->nt++];
    memset(t, 0, sizeof *t);
    t->a = a; t->b = b; t->width = w; t->layer = l == 0 ? BOARD_LAYER_F_CU : BOARD_LAYER_B_CU; t->net = net;
    return true;
}

static bool add_via(Router *R, G2Point p, uint32_t net)
{
    Items *it = &R->items;
    for (size_t i = 0; i < it->nv; i++) if (it->vias[i].net == net && it->vias[i].pos.x == p.x && it->vias[i].pos.y == p.y) return true;
    if (it->nv == it->cv) { BoardVia *v = grow(R, it->vias, it->nv, &it->cv, sizeof *v); if (!v) return false; it->vias = v; }
    BoardVia *v = &it->vias[it->nv++];
    memset(v, 0, sizeof *v);
    v->pos = p; v->size = R->r->via_size_nm; v->drill = R->r->via_drill_nm; v->from = BOARD_LAYER_F_CU; v->to = BOARD_LAYER_B_CU;
    v->net = net; v->plated = true; v->type = BOARD_VIA_THROUGH;
    return true;
}

/* ------------------------------------------------------------------------------------------------- obstacles */

static void mark_box(Router *R, int l, G2Aabb box, G2Nm thr_track, G2Nm thr_via)
{
    size_t x0, x1, y0, y1;
    G2Nm reach = thr_track > thr_via ? thr_track : thr_via;
    if (!cell_range(&R->g, box, reach, &x0, &x1, &y0, &y1)) return;
    for (size_t y = y0; y <= y1; y++)
        for (size_t x = x0; x <= x1; x++) {
            size_t c = y * R->g.nx + x;
            G2Point p = center(&R->g, c);
            G2Order o = G2_ORDER_LESS;
            if (l >= 0 && !R->blocked[l][c] && less(g2_compare_point_aabb_distance(p, box, thr_track, &o), &o)) R->blocked[l][c] = 1;
            if (thr_via >= 0 && !R->via_blocked[c] && less(g2_compare_point_aabb_distance(p, box, thr_via, &o), &o)) R->via_blocked[c] = 1;
        }
}

static void mark_segment(Router *R, int l, G2Point a, G2Point b, G2Nm thr_track, G2Nm thr_via)
{
    size_t x0, x1, y0, y1;
    G2Nm reach = thr_track > thr_via ? thr_track : thr_via;
    if (!cell_range(&R->g, seg_box(a, b), reach, &x0, &x1, &y0, &y1)) return;
    for (size_t y = y0; y <= y1; y++)
        for (size_t x = x0; x <= x1; x++) {
            size_t c = y * R->g.nx + x;
            G2Point p = center(&R->g, c);
            G2Order o = G2_ORDER_LESS;
            if (!R->blocked[l][c] && less(g2_compare_point_segment_distance(p, a, b, thr_track, &o), &o)) R->blocked[l][c] = 1;
            if (!R->via_blocked[c] && less(g2_compare_point_segment_distance(p, a, b, thr_via, &o), &o)) R->via_blocked[c] = 1;
        }
}

static void mark_circle(Router *R, G2Point c0, G2Nm thr_track, G2Nm thr_via)
{
    size_t x0, x1, y0, y1;
    G2Aabb box = {c0, c0};
    G2Nm reach = thr_track > thr_via ? thr_track : thr_via;
    if (!cell_range(&R->g, box, reach, &x0, &x1, &y0, &y1)) return;
    for (size_t y = y0; y <= y1; y++)
        for (size_t x = x0; x <= x1; x++) {
            size_t c = y * R->g.nx + x;
            G2Point p = center(&R->g, c);
            G2Order o = G2_ORDER_LESS;
            for (int l = 0; l < NL; l++)
                if (!R->blocked[l][c] && less(g2_compare_distance(p, c0, thr_track, &o), &o)) R->blocked[l][c] = 1;
            if (!R->via_blocked[c] && less(g2_compare_distance(p, c0, thr_via, &o), &o)) R->via_blocked[c] = 1;
        }
}

/* Obstacle grids for routing `net` with track width `w`. */
static void build_obstacles(Router *R, uint32_t net, G2Nm w)
{
    const Grid *g = &R->g;
    const AutorouteRules *r = R->r;
    G2Nm hw = half_up(w), clr = r->clearance_nm, s = R->slack, vr = R->via_r;
    for (int l = 0; l < NL; l++) memset(R->blocked[l], 0, g->cells);
    memset(R->via_blocked, 0, g->cells);
    /* Board edge: the routable rectangle is convex, so no slack is needed. */
    G2Nm et = r->edge_clearance_nm + hw, ev = r->edge_clearance_nm + vr;
    for (size_t c = 0; c < g->cells; c++) {
        G2Point p = center(g, c);
        G2Nm d = p.x - g->minx;
        if (g->maxx - p.x < d) d = g->maxx - p.x;
        if (p.y - g->miny < d) d = p.y - g->miny;
        if (g->maxy - p.y < d) d = g->maxy - p.y;
        if (d < et) R->blocked[0][c] = R->blocked[1][c] = 1;
        if (d < ev) R->via_blocked[c] = 1;
    }
    for (size_t i = 0; i < R->b->pad_count; i++) {
        const BoardPad *p = &R->b->pads[i];
        if (p->net == net) continue;
        G2Aabb box;
        if (board_pad_aabb(p, &box) != BOARD_OK) continue;   /* rejected earlier */
        bool any = false;
        for (int l = 0; l < NL; l++) if (on_layer(p, l)) { any = true; mark_box(R, l, box, hw + clr + s, vr + clr); }
        if (!any) mark_box(R, -1, box, -1, vr + clr);   /* a hole without copper on the routing layers */
    }
    for (size_t i = 0; i < R->items.nt; i++) {
        const BoardTrack *t = &R->items.tracks[i];
        if (t->net == net) continue;
        G2Nm tw = half_up(t->width);
        mark_segment(R, t->layer == BOARD_LAYER_F_CU ? 0 : 1, t->a, t->b, tw + hw + clr + s, tw + vr + clr);
    }
    for (size_t i = 0; i < R->items.nv; i++) {
        const BoardVia *v = &R->items.vias[i];
        if (v->net == net) continue;
        G2Nm vr2 = half_up(v->size);
        mark_circle(R, v->pos, vr2 + hw + clr + s, vr2 + vr + clr);
    }
    for (size_t i = 0; i < r->keepout_count; i++) {
        const AutorouteKeepout *k = &r->keepouts[i];
        if (k->front) mark_box(R, 0, k->box, hw + s, k->back ? vr : -1);
        if (k->back) mark_box(R, 1, k->box, hw + s, vr);
        if (k->front && !k->back) mark_box(R, -1, k->box, -1, vr);
    }
}

/* ------------------------------------------------------------------------------------------------- exact checks */

static void mm_text(char *buf, size_t cap, G2Nm v)
{
    bool neg = v < 0;
    uint64_t m = neg ? (uint64_t)(-(v + 1)) + 1u : (uint64_t)v;
    snprintf(buf, cap, "%s%llu.%03llu", neg ? "-" : "", (unsigned long long)(m / 1000000u), (unsigned long long)((m % 1000000u) / 1000u));
}

/* "track (x1,y1)-(x2,y2) on F.Cu is too close to pad R1.2" (l < 0: a via at a). */
static const char *pad_clash(Router *R, const BoardPad *p, G2Point a, G2Point b, int l)
{
    char ax[24], ay[24], bx[24], by[24];
    const char *ref = p->footprint < R->b->footprint_count && R->b->footprints[p->footprint].reference ? R->b->footprints[p->footprint].reference : "?";
    mm_text(ax, sizeof ax, a.x); mm_text(ay, sizeof ay, a.y); mm_text(bx, sizeof bx, b.x); mm_text(by, sizeof by, b.y);
    if (l < 0) snprintf(R->why_buf, sizeof R->why_buf, "a via at (%s, %s) mm is too close to pad %s.%s", ax, ay, ref, p->number ? p->number : "?");
    else snprintf(R->why_buf, sizeof R->why_buf, "a track (%s, %s)-(%s, %s) mm on %s is too close to pad %s.%s", ax, ay, bx, by, l == 0 ? "F.Cu" : "B.Cu", ref, p->number ? p->number : "?");
    return R->why_buf;
}

/* Is a track of `net`, width `w`, from a to b on layer l clear of all other copper, the edge and the keep-outs? */
static bool segment_ok(Router *R, uint32_t net, G2Nm w, int l, G2Point a, G2Point b, const char **why)
{
    const AutorouteRules *r = R->r;
    const Grid *g = &R->g;
    G2Nm hw = half_up(w), clr = r->clearance_nm, e = r->edge_clearance_nm + hw;
    G2Order o = G2_ORDER_LESS;
    if (a.x - g->minx < e || b.x - g->minx < e || g->maxx - a.x < e || g->maxx - b.x < e ||
        a.y - g->miny < e || b.y - g->miny < e || g->maxy - a.y < e || g->maxy - b.y < e) { *why = "board edge"; return false; }
    for (size_t i = 0; i < R->b->pad_count; i++) {
        const BoardPad *p = &R->b->pads[i];
        if (p->net == net || !on_layer(p, l)) continue;
        G2Aabb box;
        if (board_pad_aabb(p, &box) != BOARD_OK || less(g2_compare_segment_aabb_distance(a, b, box, hw + clr, &o), &o)) { *why = pad_clash(R, p, a, b, l); return false; }
    }
    for (size_t i = 0; i < R->items.nt; i++) {
        const BoardTrack *t = &R->items.tracks[i];
        if (t->net == net || (t->layer == BOARD_LAYER_F_CU ? 0 : 1) != l) continue;
        if (less(g2_compare_segment_segment_distance(a, b, t->a, t->b, hw + half_up(t->width) + clr, &o), &o)) { *why = "a track of another net"; return false; }
    }
    for (size_t i = 0; i < R->items.nv; i++) {
        const BoardVia *v = &R->items.vias[i];
        if (v->net == net) continue;
        G2Circle c = {v->pos, half_up(v->size)};
        if (less(g2_compare_segment_circle_distance(a, b, c, hw + clr, &o), &o)) { *why = "a via of another net"; return false; }
    }
    for (size_t i = 0; i < r->keepout_count; i++) {
        const AutorouteKeepout *k = &r->keepouts[i];
        if ((l == 0 && !k->front) || (l == 1 && !k->back)) continue;
        if (less(g2_compare_segment_aabb_distance(a, b, k->box, hw, &o), &o)) { *why = "a keep-out"; return false; }
    }
    return true;
}

static bool via_ok(Router *R, uint32_t net, G2Point p, const char **why)
{
    const AutorouteRules *r = R->r;
    const Grid *g = &R->g;
    G2Nm vr = R->via_r, clr = r->clearance_nm, e = r->edge_clearance_nm + vr;
    G2Order o = G2_ORDER_LESS;
    if (p.x - g->minx < e || g->maxx - p.x < e || p.y - g->miny < e || g->maxy - p.y < e) { *why = "board edge"; return false; }
    for (size_t i = 0; i < R->b->pad_count; i++) {
        const BoardPad *pd = &R->b->pads[i];
        if (pd->net == net) continue;
        G2Aabb box;
        if (board_pad_aabb(pd, &box) != BOARD_OK || less(g2_compare_point_aabb_distance(p, box, vr + clr, &o), &o)) { *why = pad_clash(R, pd, p, p, -1); return false; }
    }
    for (size_t i = 0; i < R->items.nt; i++) {
        const BoardTrack *t = &R->items.tracks[i];
        if (t->net == net) continue;
        if (less(g2_compare_point_segment_distance(p, t->a, t->b, vr + half_up(t->width) + clr, &o), &o)) { *why = "a track of another net"; return false; }
    }
    for (size_t i = 0; i < R->items.nv; i++) {
        const BoardVia *v = &R->items.vias[i];
        if (v->net == net) continue;
        if (less(g2_compare_distance(p, v->pos, vr + half_up(v->size) + clr, &o), &o)) { *why = "a via of another net"; return false; }
    }
    for (size_t i = 0; i < r->keepout_count; i++)
        if (less(g2_compare_point_aabb_distance(p, r->keepouts[i].box, vr, &o), &o)) { *why = "a keep-out"; return false; }
    return true;
}

/* ------------------------------------------------------------------------------------------------- search */

static bool heap_less(const Router *R, size_t a, size_t b)
{
    uint64_t fa = R->dist[a] + R->h[a], fb = R->dist[b] + R->h[b];
    return fa < fb || (fa == fb && (R->dist[a] > R->dist[b] || (R->dist[a] == R->dist[b] && a < b)));
}
static void heap_swap(Router *R, size_t i, size_t j)
{
    size_t t = R->heap[i]; R->heap[i] = R->heap[j]; R->heap[j] = t;
    R->heap_pos[R->heap[i]] = i; R->heap_pos[R->heap[j]] = j;
}
static void heap_up(Router *R, size_t i)
{
    while (i) { size_t p = (i - 1) / 2; if (!heap_less(R, R->heap[i], R->heap[p])) break; heap_swap(R, i, p); i = p; }
}
static void heap_down(Router *R, size_t n, size_t i)
{
    for (;;) {
        size_t l = 2 * i + 1, rr = l + 1, m = i;
        if (l < n && heap_less(R, R->heap[l], R->heap[m])) m = l;
        if (rr < n && heap_less(R, R->heap[rr], R->heap[m])) m = rr;
        if (m == i) return;
        heap_swap(R, i, m);
        i = m;
    }
}

static void touch(Router *R, size_t node)
{
    if (R->touched_n == R->touched_cap) { size_t *t = grow(R, R->touched, R->touched_n, &R->touched_cap, sizeof *t); if (!t) return; R->touched = t; }
    R->touched[R->touched_n++] = node;
}

/* A pad's terminal nodes: free cells on its layers whose centres are inside the pad; otherwise the nearest free
 * cell (within 3 cells) whose straight stub from the pad centre passes the exact check. */

static size_t terminals(Router *R, const BoardPad *p, uint32_t net, G2Nm w, size_t *out, size_t cap)
{
    G2Aabb box;
    size_t n = 0, x0, x1, y0, y1;
    if (board_pad_aabb(p, &box) != BOARD_OK) return 0;
    for (int l = 0; l < NL; l++) {
        if (!on_layer(p, l) || !cell_range(&R->g, box, 0, &x0, &x1, &y0, &y1)) continue;
        for (size_t y = y0; y <= y1 && n < cap; y++)
            for (size_t x = x0; x <= x1 && n < cap; x++) {
                size_t c = y * R->g.nx + x;
                bool inside = false;
                if (g2_aabb_contains_point(box, center(&R->g, c), &inside) == G2_OK && inside && !R->blocked[l][c]) out[n++] = (size_t)l * R->g.cells + c;
            }
    }
    if (n) return n;
    G2Aabb pt = {p->pos, p->pos};
    for (int l = 0; l < NL; l++) {
        if (!on_layer(p, l) || !cell_range(&R->g, pt, 3 * R->g.pitch, &x0, &x1, &y0, &y1)) continue;
        uint64_t best_d = INF;
        size_t best = NONE;
        for (size_t y = y0; y <= y1; y++)
            for (size_t x = x0; x <= x1; x++) {
                size_t c = y * R->g.nx + x;
                if (R->blocked[l][c]) continue;
                uint64_t d2 = 0;
                const char *why = NULL;
                if (g2_distance_squared(p->pos, center(&R->g, c), &d2) != G2_OK || d2 >= best_d) continue;
                if (!segment_ok(R, net, w, l, p->pos, center(&R->g, c), &why)) continue;
                best_d = d2;
                best = c;
            }
        if (best != NONE && n < cap) out[n++] = (size_t)l * R->g.cells + best;
    }
    return n;
}

static uint64_t octile(const Grid *g, size_t cell, G2Point goal)
{
    G2Point p = center(g, cell);
    uint64_t dx = (uint64_t)(p.x > goal.x ? p.x - goal.x : goal.x - p.x) / (uint64_t)g->pitch;
    uint64_t dy = (uint64_t)(p.y > goal.y ? p.y - goal.y : goal.y - p.y) / (uint64_t)g->pitch;
    uint64_t mn = dx < dy ? dx : dy, mx = dx > dy ? dx : dy;
    return mn * 14 + (mx - mn) * 10;
}

/* One multi-source search from the net's tree to the nearest unconnected pad. Returns the reached node or NONE. */
static size_t search(Router *R, const G2Point *goals, size_t ngoals)
{
    const Grid *g = &R->g;
    size_t nodes = NL * g->cells, hn = 0;
    for (size_t i = 0; i < R->touched_n; i++) {
        size_t v = R->touched[i];
        R->dist[v] = INF; R->parent[v] = NONE; R->heap_pos[v] = NONE; R->h[v] = INF;
    }
    R->touched_n = 0;
    for (size_t v = 0; v < nodes; v++) {
        if (!R->in_tree[v]) continue;
        R->dist[v] = 0; R->parent[v] = NONE;
        uint64_t best = INF;
        for (size_t k = 0; k < ngoals; k++) { uint64_t d = octile(g, v % g->cells, goals[k]); if (d < best) best = d; }
        R->h[v] = best;
        touch(R, v);
        R->heap[hn] = v; R->heap_pos[v] = hn; heap_up(R, hn); hn++;
    }
    static const int dx[8] = {1, -1, 0, 0, 1, 1, -1, -1}, dy[8] = {0, 0, 1, -1, 1, -1, 1, -1};
    while (hn && !R->oom) {
        size_t v = R->heap[0];
        R->heap_pos[v] = NONE;
        if (--hn) { R->heap[0] = R->heap[hn]; R->heap_pos[R->heap[0]] = 0; heap_down(R, hn, 0); }
        if (R->target[v]) return v;
        int l = (int)(v / g->cells);
        size_t c = v % g->cells, cx = c % g->nx, cy = c / g->nx;
        for (int k = 0; k < 9; k++) {
            size_t q;
            uint64_t step;
            if (k < 8) {
                int64_t qx = (int64_t)cx + dx[k], qy = (int64_t)cy + dy[k];
                if (qx < 0 || qy < 0 || (size_t)qx >= g->nx || (size_t)qy >= g->ny) continue;
                size_t qc = (size_t)qy * g->nx + (size_t)qx;
                if (R->blocked[l][qc]) continue;
                q = (size_t)l * g->cells + qc;
                bool horizontal = dy[k] == 0, vertical = dx[k] == 0;
                step = (horizontal || vertical) ? (((l == 0) == horizontal) ? 10 : 13) : 16;
            } else {
                int ol = 1 - l;
                if (R->via_blocked[c] || R->blocked[ol][c]) continue;
                q = (size_t)ol * g->cells + c;
                step = (uint64_t)R->r->via_cost * 10;
            }
            if (R->in_tree[q]) continue;
            if (R->h[q] == INF && R->dist[q] == INF && R->heap_pos[q] == NONE) {
                uint64_t best = INF;
                for (size_t m = 0; m < ngoals; m++) { uint64_t d = octile(g, q % g->cells, goals[m]); if (d < best) best = d; }
                R->h[q] = best;
                R->dist[q] = INF;
                R->parent[q] = NONE;
                touch(R, q);
            }
            uint64_t nd = R->dist[v] + step;
            if (nd < R->dist[q]) {
                R->dist[q] = nd;
                R->parent[q] = v;
                if (R->heap_pos[q] == NONE) { R->heap[hn] = q; R->heap_pos[q] = hn; hn++; }
                heap_up(R, R->heap_pos[q]);
            }
        }
    }
    return NONE;
}

/* ------------------------------------------------------------------------------------------------- one net */

typedef struct Path { size_t *nodes; size_t n; } Path;

/* Route `net`. Its copper is appended to R->items when every item passes the exact check; otherwise nothing is
 * kept and *why says what failed. */
static bool route_net(Router *R, uint32_t net, const size_t *pads, size_t npads, G2Nm w, size_t *segs, size_t *vias, const char **why)
{
    const Grid *g = &R->g;
    size_t nodes = NL * g->cells, keep_t = R->items.nt, keep_v = R->items.nv;
    *segs = *vias = 0;
    build_obstacles(R, net, w);
    memset(R->in_tree, 0, nodes);
    for (size_t v = 0; v < nodes; v++) R->target[v] = 0;
    size_t cap = 512;
    /* Pieces of one electrical pad (same footprint and number, overlapping copper, e.g. a module's exposed pad split
     * into parts) are one routing unit; its terminals are those of all its pieces. */
    size_t *unit_of = arena_alloc_array(R->a, sizeof *unit_of, npads);
    size_t *unit_pad = arena_alloc_array(R->a, sizeof *unit_pad, npads);
    if (!unit_of || !unit_pad) { R->oom = true; *why = "out of memory"; return false; }
    size_t nunits = 0;
    for (size_t i = 0; i < npads; i++) {
        const BoardPad *pi = &R->b->pads[pads[i]];
        G2Aabb bi, bj;
        unit_of[i] = NONE;
        if (board_pad_aabb(pi, &bi) == BOARD_OK)
            for (size_t j = 0; j < i && unit_of[i] == NONE; j++) {
                const BoardPad *pj = &R->b->pads[pads[j]];
                bool hit = false;
                if (pj->footprint == pi->footprint && pi->number && pj->number && !strcmp(pi->number, pj->number) &&
                    board_pad_aabb(pj, &bj) == BOARD_OK && g2_aabb_intersects(bi, bj, &hit) == G2_OK && hit) unit_of[i] = unit_of[j];
            }
        if (unit_of[i] == NONE) { unit_of[i] = nunits; unit_pad[nunits++] = pads[i]; }
    }
    size_t *terms = arena_alloc_array(R->a, sizeof *terms, cap * nunits);
    size_t *owner = arena_alloc_array(R->a, sizeof *owner, cap * nunits);
    size_t *nterm = arena_alloc_array(R->a, sizeof *nterm, nunits);
    bool *done = arena_alloc_array(R->a, sizeof *done, nunits);
    Path *paths = arena_alloc_array(R->a, sizeof *paths, nunits);
    size_t *stub_pad = arena_alloc_array(R->a, sizeof *stub_pad, 2 * nunits + 2), *stub_node = arena_alloc_array(R->a, sizeof *stub_node, 2 * nunits + 2);
    G2Point *goals = arena_alloc_array(R->a, sizeof *goals, nunits);
    size_t *tmp = arena_alloc_array(R->a, sizeof *tmp, cap);
    if (!terms || !owner || !nterm || !done || !paths || !stub_pad || !stub_node || !goals || !tmp) { R->oom = true; *why = "out of memory"; return false; }
    for (size_t u = 0; u < nunits; u++) { done[u] = false; nterm[u] = 0; }
    for (size_t i = 0; i < npads; i++) {
        size_t u = unit_of[i], got = terminals(R, &R->b->pads[pads[i]], net, w, tmp, cap);
        for (size_t k = 0; k < got && nterm[u] < cap; k++) {
            bool dupe = false;
            for (size_t m = 0; m < nterm[u] && !dupe; m++) dupe = terms[u * cap + m] == tmp[k];
            if (dupe) continue;
            terms[u * cap + nterm[u]] = tmp[k];
            owner[u * cap + nterm[u]++] = pads[i];
        }
    }
    for (size_t u = 0; u < nunits; u++) if (!nterm[u]) { *why = "a pad has no free grid cell to connect to"; return false; }
    npads = nunits;
    size_t npaths = 0, nstubs = 0;
    done[0] = true;
    for (size_t k = 0; k < nterm[0]; k++) R->in_tree[terms[k]] = 1;
    for (size_t connected = 1; connected < npads; connected++) {
        size_t ngoals = 0;
        for (size_t i = 0; i < npads; i++) {
            if (done[i]) continue;
            goals[ngoals++] = R->b->pads[unit_pad[i]].pos;
            for (size_t k = 0; k < nterm[i]; k++) if (!R->in_tree[terms[i * cap + k]]) R->target[terms[i * cap + k]] = (int32_t)(i + 1);
        }
        size_t hit = search(R, goals, ngoals);
        for (size_t i = 0; i < npads; i++) if (!done[i]) for (size_t k = 0; k < nterm[i]; k++) R->target[terms[i * cap + k]] = 0;
        if (hit == NONE) { *why = R->oom ? "out of memory" : "no free path on the grid"; return false; }
        size_t len = 0;
        for (size_t v = hit; v != NONE; v = R->parent[v]) len++;
        Path *P = &paths[npaths++];
        P->nodes = arena_alloc_array(R->a, sizeof *P->nodes, len);
        if (!P->nodes) { R->oom = true; *why = "out of memory"; return false; }
        P->n = len;
        size_t i = len;
        for (size_t v = hit; v != NONE; v = R->parent[v]) P->nodes[--i] = v;
        /* Which pad the path reached, and which pad (if any) it started from: stubs from those pad centres. */
        size_t reached = NONE;
        for (size_t pi = 0; pi < npads && reached == NONE; pi++)
            if (!done[pi]) for (size_t k = 0; k < nterm[pi]; k++) if (terms[pi * cap + k] == hit) { reached = pi; break; }
        if (reached == NONE) { *why = "internal: target pad not found"; return false; }
        for (size_t k = 0; k < nterm[reached]; k++) if (terms[reached * cap + k] == hit) { stub_pad[nstubs] = owner[reached * cap + k]; stub_node[nstubs++] = hit; break; }
        size_t start = P->nodes[0];
        for (size_t pi = 0; pi < npads; pi++)
            if (done[pi]) for (size_t k = 0; k < nterm[pi]; k++) if (terms[pi * cap + k] == start) { stub_pad[nstubs] = owner[pi * cap + k]; stub_node[nstubs++] = start; pi = npads; break; }
        done[reached] = true;
        for (size_t m = 0; m < len; m++) R->in_tree[P->nodes[m]] = 1;
        for (size_t k = 0; k < nterm[reached]; k++) R->in_tree[terms[reached * cap + k]] = 1;
    }
    /* Geometry: path cells -> straight runs, broken at path ends (junctions) and layer changes. */
    uint8_t *junction = R->via_blocked;        /* reuse as scratch: obstacles are rebuilt for the next net */
    memset(junction, 0, g->cells);
    for (size_t p = 0; p < npaths; p++) { junction[paths[p].nodes[0] % g->cells] = 1; junction[paths[p].nodes[paths[p].n - 1] % g->cells] = 1; }
    for (size_t p = 0; p < npaths; p++) {
        const Path *P = &paths[p];
        size_t run = 0;
        for (size_t m = 1; m <= P->n; m++) {
            bool end = m == P->n;
            if (!end) {
                size_t a = P->nodes[m - 1], b = P->nodes[m];
                if (a / g->cells != b / g->cells) {       /* via */
                    if (!add_track(R, center(g, P->nodes[run] % g->cells), center(g, a % g->cells), w, (int)(a / g->cells), net) ||
                        !add_via(R, center(g, a % g->cells), net)) { *why = "out of memory"; return false; }
                    run = m;
                    continue;
                }
                if (m + 1 < P->n && P->nodes[m + 1] / g->cells == b / g->cells && !junction[b % g->cells]) {
                    G2Point pa = center(g, a % g->cells), pb = center(g, b % g->cells), pc = center(g, P->nodes[m + 1] % g->cells);
                    if ((pb.x - pa.x) == (pc.x - pb.x) && (pb.y - pa.y) == (pc.y - pb.y)) continue;   /* collinear */
                }
            }
            size_t last = end ? P->nodes[P->n - 1] : P->nodes[m];
            size_t first = P->nodes[run];
            if (first / g->cells == last / g->cells &&
                !add_track(R, center(g, first % g->cells), center(g, last % g->cells), w, (int)(last / g->cells), net)) { *why = "out of memory"; return false; }
            run = m;
        }
    }
    for (size_t s = 0; s < nstubs; s++) {
        bool seen = false;
        for (size_t q = 0; q < s && !seen; q++) seen = stub_pad[q] == stub_pad[s] && stub_node[q] == stub_node[s];
        if (seen) continue;
        const BoardPad *pd = &R->b->pads[stub_pad[s]];   /* stub_pad holds board pad indices */
        if (!add_track(R, pd->pos, center(g, stub_node[s] % g->cells), w, (int)(stub_node[s] / g->cells), net)) { *why = "out of memory"; return false; }
    }
    /* Exact verification of everything this net added; a single failure removes the whole net. */
    for (size_t i = keep_t; i < R->items.nt; i++) {
        const BoardTrack *t = &R->items.tracks[i];
        if (!segment_ok(R, net, t->width, t->layer == BOARD_LAYER_F_CU ? 0 : 1, t->a, t->b, why)) {
            R->items.nt = keep_t; R->items.nv = keep_v;
            return false;
        }
    }
    for (size_t i = keep_v; i < R->items.nv; i++)
        if (!via_ok(R, net, R->items.vias[i].pos, why)) { R->items.nt = keep_t; R->items.nv = keep_v; return false; }
    *segs = R->items.nt - keep_t;
    *vias = R->items.nv - keep_v;
    return true;
}

/* ------------------------------------------------------------------------------------------------- entry */

static AutorouteStatus fail(AutorouteResult *out, AutorouteStatus st, const char *msg)
{
    out->status = st;
    snprintf(out->message, sizeof out->message, "%s", msg);
    return st;
}

AutorouteStatus autoroute_board(Arena *arena, const Board *b, const AutorouteRules *r, AutorouteResult *out)
{
    if (!arena || !b || !r || !out) return AUTOROUTE_ERR_ARGUMENT;
    memset(out, 0, sizeof *out);
    if (r->grid_nm <= 0 || r->track_nm <= 0 || r->clearance_nm < 0 || r->edge_clearance_nm < 0 || r->via_size_nm <= r->via_drill_nm ||
        r->via_drill_nm <= 0 || r->via_cost == 0 || r->passes == 0 || r->max_cells == 0)
        return fail(out, AUTOROUTE_INVALID_INPUT, "routing rules out of range");
    for (size_t i = 0; i < r->width_count; i++)
        if (r->widths[i].width_nm <= 0) return fail(out, AUTOROUTE_INVALID_INPUT, "a net width is not positive");
    if (b->track_count || b->via_count || b->arc_track_count) return fail(out, AUTOROUTE_INVALID_INPUT, "the board already has tracks or vias; Copper routes only unrouted boards");
    if (b->zone_count) return fail(out, AUTOROUTE_UNSUPPORTED, "the board has zones, which the router does not model yet");
    if (b->edge_count != 1 || b->edges[0].kind != BOARD_EDGE_RECT) return fail(out, AUTOROUTE_UNSUPPORTED, "the router needs exactly one rectangular board outline");
    for (size_t i = 0; i < b->pad_count; i++) {
        const BoardPad *p = &b->pads[i];
        G2Aabb box;
        if (p->shape == BOARD_PAD_CUSTOM || board_pad_aabb(p, &box) != BOARD_OK) return fail(out, AUTOROUTE_UNSUPPORTED, "a custom or unresolvable pad shape");
        for (int l = 1; l < BOARD_LAYER_B_CU; l++)
            if (p->layers & BOARD_LAYER_BIT(l)) {
                if (p->type == BOARD_PAD_SMD) return fail(out, AUTOROUTE_UNSUPPORTED, "a pad on an inner layer");
                break;
            }
    }
    Router R;
    memset(&R, 0, sizeof R);
    R.a = arena; R.b = b; R.r = r;
    G2Point e0 = b->edges[0].p0, e1 = b->edges[0].p1;
    R.g.minx = e0.x < e1.x ? e0.x : e1.x; R.g.maxx = e0.x > e1.x ? e0.x : e1.x;
    R.g.miny = e0.y < e1.y ? e0.y : e1.y; R.g.maxy = e0.y > e1.y ? e0.y : e1.y;
    R.g.pitch = r->grid_nm;
    R.g.nx = (size_t)((R.g.maxx - R.g.minx) / r->grid_nm) + 1;
    R.g.ny = (size_t)((R.g.maxy - R.g.miny) / r->grid_nm) + 1;
    if (R.g.nx < 2 || R.g.ny < 2 || R.g.nx > r->max_cells / R.g.ny) return fail(out, AUTOROUTE_INVALID_INPUT, "the board needs more grid cells than max_cells");
    R.g.cells = R.g.nx * R.g.ny;
    R.slack = (3 * r->grid_nm) / 4 + 1;
    R.via_r = half_up(r->via_size_nm);
    size_t nodes = NL * R.g.cells;
    for (int l = 0; l < NL; l++) R.blocked[l] = arena_alloc(arena, R.g.cells);
    R.via_blocked = arena_alloc(arena, R.g.cells);
    R.in_tree = arena_alloc(arena, nodes);
    R.dist = arena_alloc_array(arena, sizeof *R.dist, nodes);
    R.h = arena_alloc_array(arena, sizeof *R.h, nodes);
    R.parent = arena_alloc_array(arena, sizeof *R.parent, nodes);
    R.heap = arena_alloc_array(arena, sizeof *R.heap, nodes);
    R.heap_pos = arena_alloc_array(arena, sizeof *R.heap_pos, nodes);
    R.target = arena_alloc_array(arena, sizeof *R.target, nodes);
    if (!R.blocked[0] || !R.blocked[1] || !R.via_blocked || !R.in_tree || !R.dist || !R.h || !R.parent || !R.heap || !R.heap_pos || !R.target)
        return fail(out, AUTOROUTE_ERR_MEMORY, "out of memory for the routing grid");
    for (size_t v = 0; v < nodes; v++) { R.dist[v] = INF; R.h[v] = INF; R.parent[v] = NONE; R.heap_pos[v] = NONE; }

    /* Nets with pads, shortest (pad bounding box half-perimeter) first. */
    size_t nn = 0;
    uint32_t *order = arena_alloc_array(arena, sizeof *order, b->net_count + 1);
    uint64_t *span = arena_alloc_array(arena, sizeof *span, b->net_count + 1);
    out->nets = arena_alloc_array(arena, sizeof *out->nets, b->net_count + 1);
    size_t *pad_list = arena_alloc_array(arena, sizeof *pad_list, b->pad_count + 1);
    if (!order || !span || !out->nets || !pad_list) return fail(out, AUTOROUTE_ERR_MEMORY, "out of memory");
    for (size_t i = 0; i < b->net_count; i++) {
        uint32_t id = b->nets[i].id;
        if (id == 0) continue;
        size_t pads = 0;
        G2Aabb box = {{INT64_MAX, INT64_MAX}, {INT64_MIN, INT64_MIN}};
        for (size_t k = 0; k < b->pad_count; k++)
            if (b->pads[k].net == id) {
                pads++;
                G2Point p = b->pads[k].pos;
                if (p.x < box.min.x) box.min.x = p.x;
                if (p.y < box.min.y) box.min.y = p.y;
                if (p.x > box.max.x) box.max.x = p.x;
                if (p.y > box.max.y) box.max.y = p.y;
            }
        if (!pads) continue;
        AutorouteNetResult *nr = &out->nets[out->net_count++];
        memset(nr, 0, sizeof *nr);
        nr->name = b->nets[i].name; nr->net = id; nr->pads = pads; nr->width_nm = net_width(r, b, id); nr->message = "";
        if (pads < 2) { nr->status = AUTOROUTE_NET_SINGLE_PAD; continue; }
        /* Pads that share a number (one electrical pad split into pieces) still each need copper: all are routed. */
        order[nn] = id;
        span[nn++] = (uint64_t)(box.max.x - box.min.x) + (uint64_t)(box.max.y - box.min.y);
    }
    for (size_t i = 1; i < nn; i++)
        for (size_t j = i; j > 0 && (span[j] < span[j - 1] || (span[j] == span[j - 1] && order[j] < order[j - 1])); j--) {
            uint64_t ts = span[j]; span[j] = span[j - 1]; span[j - 1] = ts;
            uint32_t to = order[j]; order[j] = order[j - 1]; order[j - 1] = to;
        }

    size_t best_routed = 0, best_vias = SIZE_MAX;
    bool have_best = false;
    AutorouteNetResult *trial = arena_alloc_array(arena, sizeof *trial, out->net_count + 1);
    uint32_t *next = arena_alloc_array(arena, sizeof *next, nn + 1);
    if (!trial || !next) return fail(out, AUTOROUTE_ERR_MEMORY, "out of memory");
    for (uint32_t pass = 0; pass < r->passes; pass++) {
        R.items.nt = R.items.nv = 0;
        memcpy(trial, out->nets, out->net_count * sizeof *trial);
        size_t routed = 0, nf = 0, ns = 0;
        for (size_t oi = 0; oi < nn; oi++) {
            uint32_t id = order[oi];
            size_t np = 0;
            for (size_t k = 0; k < b->pad_count; k++) if (b->pads[k].net == id) pad_list[np++] = k;
            AutorouteNetResult *nr = NULL;
            for (size_t k = 0; k < out->net_count; k++) if (trial[k].net == id) nr = &trial[k];
            const char *why = "";
            size_t segs = 0, vias = 0;
            bool ok = route_net(&R, id, pad_list, np, nr->width_nm, &segs, &vias, &why);
            if (R.oom) return fail(out, AUTOROUTE_ERR_MEMORY, "out of memory while routing");
            nr->status = ok ? AUTOROUTE_NET_ROUTED : AUTOROUTE_NET_FAILED;
            nr->segments = segs; nr->vias = vias; nr->message = "";
            if (!ok && why) {   /* `why` may point into the router's scratch buffer: keep a copy */
                size_t wl = strlen(why);
                char *m = arena_alloc(arena, wl + 1);
                if (!m) return fail(out, AUTOROUTE_ERR_MEMORY, "out of memory");
                memcpy(m, why, wl + 1);
                nr->message = m;
            }
            if (ok) routed++;
        }
        /* failed nets first for the next pass, keeping their relative order */
        for (size_t oi = 0; oi < nn; oi++) for (size_t k = 0; k < out->net_count; k++)
            if (trial[k].net == order[oi] && trial[k].status == AUTOROUTE_NET_FAILED) next[nf++] = order[oi];
        ns = nf;
        for (size_t oi = 0; oi < nn; oi++) for (size_t k = 0; k < out->net_count; k++)
            if (trial[k].net == order[oi] && trial[k].status == AUTOROUTE_NET_ROUTED) next[ns++] = order[oi];
        if (!have_best || routed > best_routed || (routed == best_routed && R.items.nv < best_vias)) {
            have_best = true;
            best_routed = routed;
            best_vias = R.items.nv;
            out->tracks = arena_alloc_array(arena, sizeof *out->tracks, R.items.nt + 1);
            out->vias = arena_alloc_array(arena, sizeof *out->vias, R.items.nv + 1);
            if (!out->tracks || !out->vias) return fail(out, AUTOROUTE_ERR_MEMORY, "out of memory");
            if (R.items.nt) memcpy(out->tracks, R.items.tracks, R.items.nt * sizeof *out->tracks);
            if (R.items.nv) memcpy(out->vias, R.items.vias, R.items.nv * sizeof *out->vias);
            out->track_count = R.items.nt;
            out->via_count = R.items.nv;
            memcpy(out->nets, trial, out->net_count * sizeof *trial);
            out->pass_used = pass + 1;
        }
        if (routed == nn) break;
        memcpy(order, next, nn * sizeof *order);
    }
    out->nets_routed = best_routed;
    out->nets_failed = nn - best_routed;
    if (out->nets_failed) {
        snprintf(out->message, sizeof out->message, "%llu of %llu net(s) routed; %llu could not be routed with these rules", (unsigned long long)best_routed, (unsigned long long)nn, (unsigned long long)out->nets_failed);
        out->status = AUTOROUTE_PARTIAL;
    } else {
        snprintf(out->message, sizeof out->message, "all %llu net(s) routed: %llu track segment(s), %llu via(s)", (unsigned long long)nn, (unsigned long long)out->track_count, (unsigned long long)out->via_count);
        out->status = AUTOROUTE_ROUTED;
    }
    return out->status;
}

/* ------------------------------------------------------------------------------------------------- KiCad writer */

typedef struct Buf { Arena *a; char *p; size_t n, cap; bool oom; } Buf;

static void put(Buf *b, const char *s, size_t n)
{
    if (b->oom) return;
    if (b->n + n + 1 > b->cap) {
        size_t c = b->cap ? b->cap * 2 : 4096;
        while (c < b->n + n + 1) c *= 2;
        char *q = arena_alloc(b->a, c);
        if (!q) { b->oom = true; return; }
        if (b->n) memcpy(q, b->p, b->n);
        b->p = q; b->cap = c;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void puts_(Buf *b, const char *s) { put(b, s, strlen(s)); }

static const char *mm(char *buf, size_t cap, G2Nm v)
{
    bool neg = v < 0;
    uint64_t m = neg ? (uint64_t)(-(v + 1)) + 1u : (uint64_t)v;
    char frac[8];
    snprintf(frac, sizeof frac, "%06llu", (unsigned long long)(m % 1000000u));
    size_t f = strlen(frac);
    while (f && frac[f - 1] == '0') frac[--f] = 0;
    snprintf(buf, cap, "%s%llu%s%s", neg ? "-" : "", (unsigned long long)(m / 1000000u), f ? "." : "", frac);
    return buf;
}

static void uuid(char out[37], const char *seed, const char *kind, size_t i)
{
    char text[256];
    unsigned char d[FS_SHA256_SIZE];
    int n = snprintf(text, sizeof text, "copper-autoroute:%s:%s:%llu", seed ? seed : "", kind, (unsigned long long)i);
    fs_sha256_bytes(text, n > 0 ? (size_t)n : 0, d);
    d[6] = (unsigned char)((d[6] & 0x0f) | 0x50);   /* version 5 style */
    d[8] = (unsigned char)((d[8] & 0x3f) | 0x80);   /* RFC 4122 variant */
    static const char hex[] = "0123456789abcdef";
    size_t k = 0;
    for (int j = 0; j < 16; j++) {
        if (j == 4 || j == 6 || j == 8 || j == 10) out[k++] = '-';
        out[k++] = hex[d[j] >> 4];
        out[k++] = hex[d[j] & 15];
    }
    out[k] = 0;
}

AutorouteStatus autoroute_write_kicad(Arena *arena, const char *text, size_t len, const Board *board,
                                      const AutorouteResult *res, const char *seed, const char **out_text, size_t *out_len)
{
    if (!arena || !text || !res || !out_text || !out_len) return AUTOROUTE_ERR_ARGUMENT;
    (void)board;
    size_t end = len;
    while (end && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r' || text[end - 1] == '\n')) end--;
    if (!end || text[end - 1] != ')') return AUTOROUTE_INVALID_INPUT;
    end--;
    Buf b = {arena, NULL, 0, 0, false};
    put(&b, text, end);
    while (b.n && (b.p[b.n - 1] == ' ' || b.p[b.n - 1] == '\t' || b.p[b.n - 1] == '\r' || b.p[b.n - 1] == '\n')) b.p[--b.n] = 0;
    puts_(&b, "\n");
    char x[32], y[32], x2[32], y2[32], w[32], line[512], id[37];
    for (size_t i = 0; i < res->track_count; i++) {
        const BoardTrack *t = &res->tracks[i];
        uuid(id, seed, "segment", i);
        snprintf(line, sizeof line,
                 "\t(segment\n\t\t(start %s %s)\n\t\t(end %s %s)\n\t\t(width %s)\n\t\t(layer \"%s\")\n\t\t(net %u)\n\t\t(uuid \"%s\")\n\t)\n",
                 mm(x, sizeof x, t->a.x), mm(y, sizeof y, t->a.y), mm(x2, sizeof x2, t->b.x), mm(y2, sizeof y2, t->b.y),
                 mm(w, sizeof w, t->width), t->layer == BOARD_LAYER_F_CU ? "F.Cu" : "B.Cu", (unsigned)t->net, id);
        puts_(&b, line);
    }
    for (size_t i = 0; i < res->via_count; i++) {
        const BoardVia *v = &res->vias[i];
        uuid(id, seed, "via", i);
        snprintf(line, sizeof line,
                 "\t(via\n\t\t(at %s %s)\n\t\t(size %s)\n\t\t(drill %s)\n\t\t(layers \"F.Cu\" \"B.Cu\")\n\t\t(net %u)\n\t\t(uuid \"%s\")\n\t)\n",
                 mm(x, sizeof x, v->pos.x), mm(y, sizeof y, v->pos.y), mm(w, sizeof w, v->size), mm(x2, sizeof x2, v->drill), (unsigned)v->net, id);
        puts_(&b, line);
    }
    puts_(&b, ")\n");
    if (b.oom) return AUTOROUTE_ERR_MEMORY;
    *out_text = b.p;
    *out_len = b.n;
    return AUTOROUTE_ROUTED;
}

const char *autoroute_status_name(AutorouteStatus s)
{
    switch (s) {
    case AUTOROUTE_ROUTED: return "ROUTED";
    case AUTOROUTE_PARTIAL: return "PARTIALLY_ROUTED";
    case AUTOROUTE_INVALID_INPUT: return "INVALID_INPUT";
    case AUTOROUTE_UNSUPPORTED: return "UNSUPPORTED";
    case AUTOROUTE_ERR_ARGUMENT: return "ERR_ARGUMENT";
    case AUTOROUTE_ERR_MEMORY: return "ERR_MEMORY";
    }
    return "UNKNOWN";
}

const char *autoroute_net_status_name(AutorouteNetStatus s)
{
    switch (s) {
    case AUTOROUTE_NET_ROUTED: return "ROUTED";
    case AUTOROUTE_NET_FAILED: return "FAILED";
    case AUTOROUTE_NET_SINGLE_PAD: return "SINGLE_PAD";
    }
    return "UNKNOWN";
}

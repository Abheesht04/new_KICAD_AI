#include "router.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#define ROUTER_INF UINT64_MAX
#define ROUTER_NO_NODE SIZE_MAX

typedef struct {
  size_t *nodes;
  size_t *position;
  size_t count;
  const uint64_t *g;
  const uint64_t *h;
} MinHeap;

static uint64_t sat_add(uint64_t a, uint64_t b) {
  return a > UINT64_MAX - b ? UINT64_MAX : a + b;
}
static bool heap_less(const MinHeap *h, size_t a, size_t b) {
  uint64_t fa = sat_add(h->g[a], h->h[a]);
  uint64_t fb = sat_add(h->g[b], h->h[b]);
  return fa < fb || (fa == fb && (h->g[a] < h->g[b] ||
         (h->g[a] == h->g[b] && a < b)));
}
static void heap_swap(MinHeap *h, size_t a, size_t b) {
  size_t t = h->nodes[a]; h->nodes[a] = h->nodes[b]; h->nodes[b] = t;
  h->position[h->nodes[a]] = a; h->position[h->nodes[b]] = b;
}
static void heap_up(MinHeap *h, size_t i) {
  while (i) { size_t p = (i - 1) / 2; if (!heap_less(h, h->nodes[i], h->nodes[p])) break; heap_swap(h, i, p); i = p; }
}
static void heap_down(MinHeap *h, size_t i) {
  for (;;) { size_t l = i * 2 + 1, r = l + 1, m = i;
    if (l < h->count && heap_less(h, h->nodes[l], h->nodes[m])) m = l;
    if (r < h->count && heap_less(h, h->nodes[r], h->nodes[m])) m = r;
    if (m == i) break;
    heap_swap(h, i, m);
    i = m;
  }
}
static void heap_insert_or_decrease(MinHeap *h, size_t node) {
  size_t p = h->position[node];
  if (p == ROUTER_NO_NODE) { p = h->count++; h->nodes[p] = node; h->position[node] = p; }
  heap_up(h, p);
}
static size_t heap_pop(MinHeap *h) {
  size_t result = h->nodes[0]; h->position[result] = ROUTER_NO_NODE;
  --h->count;
  if (h->count) { h->nodes[0] = h->nodes[h->count]; h->position[h->nodes[0]] = 0; heap_down(h, 0); }
  return result;
}
static uint64_t octile(size_t ax, size_t ay, size_t bx, size_t by) {
  size_t dx = ax > bx ? ax - bx : bx - ax;
  size_t dy = ay > by ? ay - by : by - ay;
  size_t mn = dx < dy ? dx : dy, mx = dx > dy ? dx : dy;
  if (mx > UINT64_MAX / 10) return UINT64_MAX;
  return (uint64_t)mn * 14 + (uint64_t)(mx - mn) * 10;
}
static bool add_checked(G2Nm a, G2Nm b, G2Nm *out) {
  if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) return false;
  *out = a + b; return true;
}
static bool sub_checked(G2Nm a, G2Nm b, G2Nm *out) {
  if (b == INT64_MIN) return false;
  return add_checked(a, -b, out);
}
static G2Aabb segment_box(G2Point a, G2Point b, G2Nm expand) {
  G2Aabb q = {{a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y},
              {a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y}};
  q.min.x -= expand; q.min.y -= expand; q.max.x += expand; q.max.y += expand;
  return q;
}
static bool layer_mask_has(BoardLayerMask m, BoardLayer l) {
  return (unsigned)l < 64 && (m & BOARD_LAYER_BIT(l)) != 0;
}
static bool via_on_layer(const BoardVia *v, BoardLayer l) {
  if (v->from == l || v->to == l) return true;
  return board_layer_is_copper(l) && board_layer_is_copper(v->from) &&
         board_layer_is_copper(v->to) &&
         ((v->from < v->to && l > v->from && l < v->to) ||
          (v->to < v->from && l > v->to && l < v->from));
}
static bool segment_clear(const Board *b, G2Point a, G2Point z,
                          const RouterOptions *o, const G2Bvh *track_bvh,
                          const size_t *track_ids, size_t track_count,
                          size_t *scratch, char *why, size_t why_cap) {
  if (track_bvh && track_count) {
    G2Aabb query = segment_box(a, z, 0);
    size_t found = 0;
    if (g2_bvh_query(track_bvh, query, scratch, track_count, &found) != G2_OK) return false;
    for (size_t k = 0; k < found; ++k) {
      const BoardTrack *t = &b->tracks[track_ids[scratch[k]]];
      if (t->net == o->net) continue;
      G2Nm threshold = (t->width / 2 + t->width % 2) +
                       (o->trace_width_nm / 2 + o->trace_width_nm % 2) +
                       o->clearance_nm;
      G2Order order;
      if (g2_compare_segment_segment_distance(a, z, t->a, t->b, threshold, &order) != G2_OK) return false;
      if (order == G2_ORDER_LESS) return false;
    }
  }
  for (size_t i = 0; i < b->pad_count; ++i) {
    const BoardPad *p = &b->pads[i];
    if (!layer_mask_has(p->layers, o->layer) || p->net == o->net) continue;
    if (p->shape == BOARD_PAD_CUSTOM) { (void)snprintf(why, why_cap, "custom pad shape blocks routing verification"); return false; }
    G2Aabb box; G2Order order;
    if (board_pad_aabb(p, &box) != BOARD_OK ||
        g2_compare_segment_aabb_distance(a, z, box,
            o->trace_width_nm / 2 + o->trace_width_nm % 2 + o->clearance_nm, &order) != G2_OK) return false;
    if (order == G2_ORDER_LESS) return false;
  }
  for (size_t i = 0; i < b->via_count; ++i) {
    const BoardVia *v = &b->vias[i];
    if (!via_on_layer(v, o->layer) || v->net == o->net) continue;
    G2Circle c = {v->pos, v->size / 2 + v->size % 2}; G2Order order;
    if (g2_compare_segment_circle_distance(a, z, c,
            o->trace_width_nm / 2 + o->trace_width_nm % 2 + o->clearance_nm, &order) != G2_OK) return false;
    if (order == G2_ORDER_LESS) return false;
  }
  for (size_t i = 0; i < b->zone_count; ++i) {
    const BoardZone *zv = &b->zones[i];
    if (!layer_mask_has(zv->layers, o->layer)) continue;
    if (!zv->is_keepout) { (void)snprintf(why, why_cap, "filled copper zone geometry is not resolved; route is UNVERIFIED"); return false; }
    if (zv->no_tracks && zv->pt_count >= 3) {
      G2Polygon poly = {&b->zone_pts[zv->first_pt], zv->pt_count}; G2Order order;
      if (g2_compare_segment_polygon_distance(a, z, poly,
          o->trace_width_nm / 2 + o->trace_width_nm % 2 + o->clearance_nm, &order) != G2_OK) return false;
      if (order == G2_ORDER_LESS) return false;
    }
  }
  return true;
}
static RouterStatus fail(RouterResult *r, RouterStatus s, const char *m) {
  if (r) { r->status = s; (void)snprintf(r->message, sizeof(r->message), "%s", m ? m : ""); }
  return s;
}

RouterStatus copper_route_astar(const Board *b, G2Point start, G2Point goal,
                                const RouterOptions *o, Arena *arena,
                                RouterResult *out) {
  if (!b || !o || !arena || !out || o->grid_pitch_nm <= 0 ||
      o->trace_width_nm <= 0 || o->clearance_nm < 0 ||
      o->edge_clearance_nm < 0 || o->max_cells == 0 ||
      !board_layer_is_copper(o->layer) || o->net == 0)
    return fail(out, ROUTER_ERR_ARGUMENT, "invalid router arguments (a non-zero target net is required)");
  memset(out, 0, sizeof(*out));
  if (!board_net_by_id(b, o->net))
    return fail(out, ROUTER_ERR_ARGUMENT, "target net ID is not present on the board");
  if (b->arc_track_count) return fail(out, ROUTER_ERR_UNVERIFIED, "arc tracks are not yet included in router collision geometry");
  for (size_t i = 0; i < b->zone_count; ++i)
    if (!b->zones[i].is_keepout && layer_mask_has(b->zones[i].layers, o->layer))
      return fail(out, ROUTER_ERR_UNVERIFIED, "filled copper zone on target layer is not resolved");
  for (size_t i = 0; i < b->edge_count; ++i)
    if (b->edges[i].kind != BOARD_EDGE_RECT)
      return fail(out, ROUTER_ERR_UNVERIFIED, "A* currently requires a rectangular Edge.Cuts outline");
  if (b->edge_count != 1) return fail(out, ROUTER_ERR_UNVERIFIED, "A* currently requires exactly one rectangular Edge.Cuts outline");

  G2Nm margin = o->trace_width_nm / 2;
  if (!add_checked(margin, o->clearance_nm, &margin) ||
      !add_checked(margin, o->edge_clearance_nm, &margin)) return fail(out, ROUTER_ERR_RANGE, "edge margin overflow");
  G2Aabb bounds;
  if (board_bounds(b, &bounds) != BOARD_OK) return fail(out, ROUTER_ERR_GEOMETRY, "cannot calculate board bounds");
  G2Nm minx, miny, maxx, maxy;
  if (!add_checked(bounds.min.x, margin, &minx) || !add_checked(bounds.min.y, margin, &miny) ||
      !sub_checked(bounds.max.x, margin, &maxx) || !sub_checked(bounds.max.y, margin, &maxy) ||
      maxx <= minx || maxy <= miny) return fail(out, ROUTER_ERR_RANGE, "no routable board interior remains after edge clearance");
  uint64_t nx64 = (uint64_t)((maxx - minx) / o->grid_pitch_nm) + 1;
  uint64_t ny64 = (uint64_t)((maxy - miny) / o->grid_pitch_nm) + 1;
  if (nx64 == 0 || ny64 == 0 || nx64 > SIZE_MAX || ny64 > SIZE_MAX ||
      nx64 > UINT64_MAX / ny64 || nx64 * ny64 > o->max_cells || nx64 * ny64 > SIZE_MAX)
    return fail(out, ROUTER_ERR_RANGE, "grid exceeds configured max_cells");
  size_t nx = (size_t)nx64, ny = (size_t)ny64, count = nx * ny;
  if (start.x < minx || start.x > maxx || start.y < miny || start.y > maxy ||
      goal.x < minx || goal.x > maxx || goal.y < miny || goal.y > maxy)
    return fail(out, ROUTER_ERR_RANGE, "start or goal lies outside routable board interior");
  size_t sx = (size_t)(((start.x - minx) + o->grid_pitch_nm / 2) / o->grid_pitch_nm);
  size_t sy = (size_t)(((start.y - miny) + o->grid_pitch_nm / 2) / o->grid_pitch_nm);
  size_t gx = (size_t)(((goal.x - minx) + o->grid_pitch_nm / 2) / o->grid_pitch_nm);
  size_t gy = (size_t)(((goal.y - miny) + o->grid_pitch_nm / 2) / o->grid_pitch_nm);
  if (sx >= nx) sx = nx - 1;
  if (gx >= nx) gx = nx - 1;
  if (sy >= ny) sy = ny - 1;
  if (gy >= ny) gy = ny - 1;
  size_t source = sy * nx + sx, target = gy * nx + gx;

  size_t tc = 0;
  for (size_t i = 0; i < b->track_count; ++i) if (b->tracks[i].layer == o->layer && b->tracks[i].net != o->net) ++tc;
  G2Bvh bvh = {0}; size_t *track_ids = NULL;
  if (tc) {
    G2Aabb *tb = arena_alloc_array(arena, sizeof(*tb), tc);
    track_ids = arena_alloc_array(arena, sizeof(*track_ids), tc);
    if (!tb || !track_ids) return fail(out, ROUTER_ERR_MEMORY, "track BVH allocation failed");
    size_t k = 0;
    for (size_t i = 0; i < b->track_count; ++i) {
      const BoardTrack *t = &b->tracks[i]; if (t->layer != o->layer || t->net == o->net) continue;
      G2Nm ex = t->width / 2 + o->trace_width_nm / 2 + o->clearance_nm;
      tb[k] = segment_box(t->a, t->b, ex); track_ids[k++] = i;
    }
    if (g2_bvh_build(arena, tb, tc, 4, &bvh) != G2_OK) return fail(out, ROUTER_ERR_GEOMETRY, "track BVH construction failed");
  }
  uint64_t *gcost = arena_alloc_array(arena, sizeof(*gcost), count);
  uint64_t *heur = arena_alloc_array(arena, sizeof(*heur), count);
  size_t *parent = arena_alloc_array(arena, sizeof(*parent), count);
  size_t *heap_nodes = arena_alloc_array(arena, sizeof(*heap_nodes), count);
  size_t *heap_pos = arena_alloc_array(arena, sizeof(*heap_pos), count);
  uint8_t *closed = arena_alloc_array(arena, sizeof(*closed), count);
  size_t *scratch = tc ? arena_alloc_array(arena, sizeof(*scratch), tc) : NULL;
  if (!gcost || !heur || !parent || !heap_nodes || !heap_pos || !closed || (tc && !scratch))
    return fail(out, ROUTER_ERR_MEMORY, "A* workspace allocation failed");
  for (size_t y = 0; y < ny; ++y) for (size_t x = 0; x < nx; ++x) {
    size_t id = y * nx + x; gcost[id] = ROUTER_INF; parent[id] = ROUTER_NO_NODE;
    heap_pos[id] = ROUTER_NO_NODE; closed[id] = 0; heur[id] = octile(x, y, gx, gy);
  }
  MinHeap heap = {heap_nodes, heap_pos, 0, gcost, heur}; gcost[source] = 0;
  heap_insert_or_decrease(&heap, source);
  static const int8_t dx[8] = {1, 0, -1, 0, 1, -1, -1, 1};
  static const int8_t dy[8] = {0, 1, 0, -1, 1, 1, -1, -1};
  bool reached = false; char why[160] = "";
  while (heap.count) {
    size_t cur = heap_pop(&heap); if (closed[cur]) continue; closed[cur] = 1; ++out->expanded_nodes;
    if (cur == target) { reached = true; break; }
    size_t cx = cur % nx, cy = cur / nx;
    G2Point cp = {minx + (G2Nm)cx * o->grid_pitch_nm, miny + (G2Nm)cy * o->grid_pitch_nm};
    for (size_t d = 0; d < 8; ++d) {
      int64_t qx = (int64_t)cx + dx[d], qy = (int64_t)cy + dy[d];
      if (qx < 0 || qy < 0 || (uint64_t)qx >= nx || (uint64_t)qy >= ny) continue;
      size_t q = (size_t)qy * nx + (size_t)qx; if (closed[q]) continue;
      G2Point qp = {minx + qx * o->grid_pitch_nm, miny + qy * o->grid_pitch_nm};
      why[0] = '\0';
      if (!segment_clear(b, cp, qp, o, tc ? &bvh : NULL, track_ids, tc, scratch, why, sizeof(why))) {
        if (why[0]) return fail(out, ROUTER_ERR_UNVERIFIED, why);
        continue;
      }
      uint64_t step = (dx[d] && dy[d]) ? 14 : 10;
      uint64_t tentative = sat_add(gcost[cur], step);
      if (tentative < gcost[q]) { gcost[q] = tentative; parent[q] = cur; heap_insert_or_decrease(&heap, q); }
    }
  }
  if (!reached) return fail(out, ROUTER_ERR_NO_PATH, "no path exists on the configured grid");
  G2Point first_grid = {minx + (G2Nm)sx * o->grid_pitch_nm, miny + (G2Nm)sy * o->grid_pitch_nm};
  G2Point last_grid = {minx + (G2Nm)gx * o->grid_pitch_nm, miny + (G2Nm)gy * o->grid_pitch_nm};
  if (!segment_clear(b, start, first_grid, o, tc ? &bvh : NULL, track_ids, tc, scratch, why, sizeof(why)) ||
      !segment_clear(b, last_grid, goal, o, tc ? &bvh : NULL, track_ids, tc, scratch, why, sizeof(why)))
    return fail(out, why[0] ? ROUTER_ERR_UNVERIFIED : ROUTER_ERR_NO_PATH, why[0] ? why : "endpoint connector is blocked");
  G2Point *reverse = arena_alloc_array(arena, sizeof(*reverse), count);
  G2Point *path = arena_alloc_array(arena, sizeof(*path), count + 2);
  if (!reverse || !path) return fail(out, ROUTER_ERR_MEMORY, "route output allocation failed");
  size_t rn = 0, p = target;
  while (p != ROUTER_NO_NODE && rn < count) {
    reverse[rn++] = (G2Point){minx + (G2Nm)(p % nx) * o->grid_pitch_nm,
                              miny + (G2Nm)(p / nx) * o->grid_pitch_nm};
    if (p == source) break;
    p = parent[p];
  }
  if (rn == 0 || (size_t)(reverse[rn - 1].x - minx) / (size_t)o->grid_pitch_nm != sx ||
      (size_t)(reverse[rn - 1].y - miny) / (size_t)o->grid_pitch_nm != sy)
    return fail(out, ROUTER_ERR_GEOMETRY, "A* parent chain is invalid");
  size_t pn = 0; path[pn++] = start;
  for (size_t i = rn; i > 0; --i) {
    G2Point v = reverse[i - 1];
    if (v.x != path[pn - 1].x || v.y != path[pn - 1].y) path[pn++] = v;
  }
  if (goal.x != path[pn - 1].x || goal.y != path[pn - 1].y) path[pn++] = goal;
  out->points = path; out->point_count = pn; out->cost = gcost[target]; out->status = ROUTER_OK;
  (void)snprintf(out->message, sizeof(out->message), "route found with deterministic 8-neighbour A*");
  return ROUTER_OK;
}

#include "placer.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

typedef struct { G2Aabb box; size_t fp; } Occupied;

typedef struct { G2Aabb local; uint64_t area; size_t index; } FootprintGeom;

static PlacerStatus p_fail(PlacerResult *r, PlacerStatus s, const char *m) {
  if (r) { r->status = s; (void)snprintf(r->message, sizeof(r->message), "%s", m ? m : ""); }
  return s;
}
static bool safe_add(G2Nm a, G2Nm b, G2Nm *o) {
  if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) return false;
  *o = a + b; return true;
}
static bool safe_sub(G2Nm a, G2Nm b, G2Nm *o) {
  if (b == INT64_MIN) return false;
  return safe_add(a, -b, o);
}
static bool local_courtyard(const Board *b, size_t index, G2Aabb *out) {
  const BoardFootprint *fp = &b->footprints[index];
  if (!fp->courtyard_exact || fp->court_count < 3 ||
      (size_t)fp->first_court + fp->court_count > b->court_pt_count) return false;
  G2Nm minx = INT64_MAX, miny = INT64_MAX, maxx = INT64_MIN, maxy = INT64_MIN;
  for (size_t i = 0; i < fp->court_count; ++i) {
    G2Point p = b->court_pts[fp->first_court + i];
    if (p.x < minx) minx = p.x;
    if (p.y < miny) miny = p.y;
    if (p.x > maxx) maxx = p.x;
    if (p.y > maxy) maxy = p.y;
  }
  if (!safe_sub(minx, fp->pos.x, &out->min.x) ||
      !safe_sub(miny, fp->pos.y, &out->min.y) ||
      !safe_sub(maxx, fp->pos.x, &out->max.x) ||
      !safe_sub(maxy, fp->pos.y, &out->max.y)) return false;
  return out->max.x > out->min.x && out->max.y > out->min.y;
}
static bool translated(G2Aabb local, G2Point pos, G2Aabb *out) {
  return safe_add(local.min.x, pos.x, &out->min.x) &&
         safe_add(local.min.y, pos.y, &out->min.y) &&
         safe_add(local.max.x, pos.x, &out->max.x) &&
         safe_add(local.max.y, pos.y, &out->max.y);
}
static bool collides(G2Aabb box, const Occupied *occ, size_t count, G2Nm gap) {
  for (size_t i = 0; i < count; ++i) {
    G2Order order;
    if (g2_compare_aabb_aabb_distance(box, occ[i].box, gap, &order) != G2_OK ||
        order == G2_ORDER_LESS) return true;
  }
  return false;
}

PlacerStatus copper_place_grid(const Board *b, const PlacerOptions *o,
                               Arena *arena, PlacerResult *out) {
  if (!b || !o || !arena || !out || o->grid_pitch_nm <= 0 ||
      o->component_gap_nm < 0 || o->edge_clearance_nm < 0 ||
      o->max_candidates == 0) return p_fail(out, PLACER_ERR_ARGUMENT, "invalid placer arguments");
  memset(out, 0, sizeof(*out));
  if (b->edge_count != 1 || b->edges[0].kind != BOARD_EDGE_RECT)
    return p_fail(out, PLACER_ERR_UNVERIFIED, "grid placer currently requires one rectangular Edge.Cuts outline");
  G2Aabb board_box;
  if (board_bounds(b, &board_box) != BOARD_OK)
    return p_fail(out, PLACER_ERR_GEOMETRY, "cannot calculate board bounds");
  G2Nm minx, miny, maxx, maxy;
  if (!safe_add(board_box.min.x, o->edge_clearance_nm, &minx) ||
      !safe_add(board_box.min.y, o->edge_clearance_nm, &miny) ||
      !safe_add(board_box.max.x, -o->edge_clearance_nm, &maxx) ||
      !safe_add(board_box.max.y, -o->edge_clearance_nm, &maxy) || maxx <= minx || maxy <= miny)
    return p_fail(out, PLACER_ERR_GEOMETRY, "board interior is too small");

  size_t n = b->footprint_count;
  out->placements = n ? arena_alloc_array(arena, sizeof(*out->placements), n) : NULL;
  FootprintGeom *geom = n ? arena_alloc_array(arena, sizeof(*geom), n) : NULL;
  Occupied *occ = n ? arena_alloc_array(arena, sizeof(*occ), n) : NULL;
  bool *done = n ? arena_alloc_array(arena, sizeof(*done), n) : NULL;
  if (n && (!out->placements || !geom || !occ || !done))
    return p_fail(out, PLACER_ERR_MEMORY, "placer workspace allocation failed");
  memset(done, 0, n * sizeof(*done));
  out->placement_count = n;
  for (size_t i = 0; i < n; ++i) {
    const BoardFootprint *fp = &b->footprints[i]; G2Aabb local;
    if (!local_courtyard(b, i, &local))
      return p_fail(out, PLACER_ERR_UNVERIFIED, "one or more footprints lack a complete typed courtyard; refusing unsafe placement");
    G2Nm width_nm, height_nm;
    if (!safe_sub(local.max.x, local.min.x, &width_nm) ||
        !safe_sub(local.max.y, local.min.y, &height_nm) ||
        width_nm <= 0 || height_nm <= 0)
      return p_fail(out, PLACER_ERR_GEOMETRY, "footprint courtyard dimensions overflow or are invalid");
    uint64_t w = (uint64_t)width_nm, h = (uint64_t)height_nm;
    geom[i] = (FootprintGeom){local, (w && h > UINT64_MAX / w) ? UINT64_MAX : w * h, i};
    out->placements[i] = (PlacerPlacement){(uint32_t)i, fp->pos, fp->rot_mdeg, fp->locked, false};
  }
  size_t oc = 0;
  for (size_t i = 0; i < n; ++i) if (b->footprints[i].locked) {
    G2Aabb box;
    if (!translated(geom[i].local, b->footprints[i].pos, &box))
      return p_fail(out, PLACER_ERR_GEOMETRY, "locked footprint courtyard overflows coordinates");
    if (box.min.x < minx || box.min.y < miny || box.max.x > maxx || box.max.y > maxy)
      return p_fail(out, PLACER_ERR_GEOMETRY, "locked footprint courtyard violates board-edge clearance");
    if (collides(box, occ, oc, o->component_gap_nm))
      return p_fail(out, PLACER_ERR_GEOMETRY, "locked footprint courtyards overlap or violate component gap");
    occ[oc++] = (Occupied){box, i}; done[i] = true;
    out->placements[i].placed = true; ++out->placed_count;
  }
  /* Stable descending-area order, original footprint index breaks ties. */
  for (size_t placed = 0; placed < n; ++placed) {
    size_t best = SIZE_MAX;
    for (size_t i = 0; i < n; ++i) if (!done[i] &&
        (best == SIZE_MAX || geom[i].area > geom[best].area ||
         (geom[i].area == geom[best].area && i < best))) best = i;
    if (best == SIZE_MAX) break;
    G2Aabb local = geom[best].local; bool found = false;
    for (G2Nm y = miny; y <= maxy && !found;) {
      for (G2Nm x = minx; x <= maxx;) {
        if (++out->candidates_tested > o->max_candidates)
          return p_fail(out, PLACER_PARTIAL, "placement stopped at configured max_candidates");
        G2Point pos;
        G2Aabb candidate;
        if (safe_sub(x, local.min.x, &pos.x) && safe_sub(y, local.min.y, &pos.y) &&
            translated(local, pos, &candidate) && candidate.max.x <= maxx &&
            candidate.max.y <= maxy && !collides(candidate, occ, oc, o->component_gap_nm)) {
          out->placements[best].position = pos;
          out->placements[best].placed = true;
          occ[oc++] = (Occupied){candidate, best};
          ++out->placed_count; found = true; break;
        }
        G2Nm next_x;
        if (!safe_add(x, o->grid_pitch_nm, &next_x) || next_x > maxx) break;
        x = next_x;
      }
      if (found) break;
      G2Nm next_y;
      if (!safe_add(y, o->grid_pitch_nm, &next_y) || next_y > maxy) break;
      y = next_y;
    }
    done[best] = true;
    if (!found) { out->placements[best].placed = false; ++out->unplaced_count; }
  }
  if (out->unplaced_count) return p_fail(out, PLACER_PARTIAL, "some footprints could not be placed on the configured courtyard grid");
  out->status = PLACER_OK;
  (void)snprintf(out->message, sizeof(out->message), "deterministic first-fit courtyard placement proposals generated");
  return PLACER_OK;
}

#include "dfm_check.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

static uint64_t isqrt_ceil_u64(uint64_t n) {
  uint64_t lo = 0, hi = UINT64_C(1) << 32;
  while (lo < hi) {
    uint64_t mid = lo + (hi - lo) / 2;
    if (mid != 0 && mid > n / mid) hi = mid;
    else if (mid * mid < n) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}
static bool add_sat(G2Nm a, G2Nm b, G2Nm *out) {
  if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) return false;
  *out = a + b; return true;
}
static bool add_finding(CopperReport *r, const FabRule *rule, ReportStatus status,
                        const char *object, size_t index, bool has_location,
                        G2Point location, bool has_measurement,
                        G2Nm measured, const char *detail) {
  ReportFinding f;
  memset(&f, 0, sizeof(f));
  f.status = status; f.rule_id = rule ? rule->id : "NO_RULE";
  f.kind = rule ? rule->kind : "coverage"; f.object = object;
  f.object_index = index; f.has_location = has_location; f.location = location;
  f.has_measurement = has_measurement; f.measured_nm = measured;
  f.limit_nm = rule ? rule->value : 0;
  f.detail = detail ? detail : (rule ? rule->note : NULL);
  f.source_url = rule ? rule->source_url : NULL;
  f.retrieved = rule ? rule->retrieved : NULL;
  return report_add(r, &f);
}
static DfmStatus missing_rule(CopperReport *r, const char *kind) {
  return add_finding(r, NULL, REPORT_UNVERIFIED, "board", SIZE_MAX, false,
                     (G2Point){0,0}, false, 0,
                     kind) ? DFM_OK : DFM_ERR_REPORT_CAPACITY;
}
static DfmStatus check_track_width(const Board *b, const FabRule *rule,
                                   CopperReport *r) {
  bool failed = false, found = false;
  G2Nm minimum = INT64_MAX;
  G2Point minimum_at = {0, 0};
  size_t minimum_index = SIZE_MAX;
  const char *minimum_object = "tracks";
  for (size_t i = 0; i < b->track_count; ++i) {
    const BoardTrack *t = &b->tracks[i];
    found = true;
    if (t->width < minimum) { minimum = t->width; minimum_at = t->a; minimum_index = i; minimum_object = "track"; }
    if (t->width < rule->value) {
      failed = true;
      if (!add_finding(r, rule, REPORT_FAIL, "track", i, true, t->a,
                       true, t->width, "Track width is below fabricator minimum")) return DFM_ERR_REPORT_CAPACITY;
    }
  }
  for (size_t i = 0; i < b->arc_track_count; ++i) {
    const BoardArcTrack *a = &b->arc_tracks[i];
    found = true;
    if (a->width < minimum) { minimum = a->width; minimum_at = a->start; minimum_index = i; minimum_object = "arc_track"; }
    if (a->width < rule->value) {
      failed = true;
      if (!add_finding(r, rule, REPORT_FAIL, "arc_track", i, true, a->start,
                       true, a->width, "Arc track width is below fabricator minimum")) return DFM_ERR_REPORT_CAPACITY;
    }
  }
  if (!found)
    return add_finding(r, rule, REPORT_UNVERIFIED, "tracks", SIZE_MAX, false,
                       (G2Point){0,0}, false, 0, "No copper tracks present; check does not exercise any track")
               ? DFM_OK : DFM_ERR_REPORT_CAPACITY;
  if (!failed && !add_finding(r, rule, REPORT_PASS, minimum_object, minimum_index, true,
                               minimum_at, true, minimum,
                               "Minimum imported track width meets this rule")) return DFM_ERR_REPORT_CAPACITY;
  return DFM_OK;
}
static G2Aabb track_box(const BoardTrack *t, G2Nm expand) {
  G2Nm minx = t->a.x < t->b.x ? t->a.x : t->b.x;
  G2Nm maxx = t->a.x > t->b.x ? t->a.x : t->b.x;
  G2Nm miny = t->a.y < t->b.y ? t->a.y : t->b.y;
  G2Nm maxy = t->a.y > t->b.y ? t->a.y : t->b.y;
  return (G2Aabb){{minx - expand, miny - expand}, {maxx + expand, maxy + expand}};
}
static DfmStatus check_track_clearance(const Board *b, const BoardDiag *diag,
                                       const FabRule *rule, const DfmOptions *opts,
                                       Arena *arena, CopperReport *r) {
  if (b->track_count < 2) {
    if (!add_finding(r, rule, REPORT_UNVERIFIED, "tracks", SIZE_MAX, false,
                     (G2Point){0,0}, false, 0, "Fewer than two straight tracks; no pairwise clearance case to verify")) return DFM_ERR_REPORT_CAPACITY;
    return DFM_OK;
  }
  if (b->arc_track_count || b->zone_count || b->pad_count || b->via_count ||
      (diag && diag->unmodelled_geometry_records)) {
    if (!add_finding(r, rule, REPORT_UNVERIFIED, "board", SIZE_MAX, false,
                     (G2Point){0,0}, false, 0,
                     "Track clearance is only partially covered: pads, vias, arc tracks, zone fills, or unmodelled geometry require additional checks/native KiCad DRC")) return DFM_ERR_REPORT_CAPACITY;
  }
  G2Nm max_width = 0;
  for (size_t i = 0; i < b->track_count; ++i) if (b->tracks[i].width > max_width) max_width = b->tracks[i].width;
  G2Nm margin;
  if (!add_sat(rule->value, max_width, &margin)) return DFM_ERR_GEOMETRY;
  G2Aabb *boxes = (G2Aabb *)arena_alloc_array(arena, sizeof(G2Aabb), b->track_count);
  if (!boxes) return DFM_ERR_MEMORY;
  for (size_t i = 0; i < b->track_count; ++i)
    boxes[i] = track_box(&b->tracks[i], margin);
  G2Bvh tree;
  if (g2_bvh_build(arena, boxes, b->track_count, 8, &tree) != G2_OK) return DFM_ERR_GEOMETRY;
  size_t *ids = (size_t *)arena_alloc_array(arena, sizeof(size_t), b->track_count);
  if (!ids) return DFM_ERR_MEMORY;
  bool any_pair = false, failed = false;
  for (size_t i = 0; i < b->track_count; ++i) {
    const BoardTrack *a = &b->tracks[i];
    size_t n = 0;
    if (g2_bvh_query(&tree, boxes[i], ids, b->track_count, &n) != G2_OK) return DFM_ERR_GEOMETRY;
    for (size_t k = 0; k < n; ++k) {
      size_t j = ids[k];
      if (j <= i) continue;
      const BoardTrack *q = &b->tracks[j];
      if (a->layer != q->layer) continue;
      if (!(opts && opts->check_same_net_clearance) && a->net == q->net) continue;
      any_pair = true;
      G2Nm half = (a->width + q->width + 1) / 2, required;
      if (!add_sat(rule->value, half, &required)) return DFM_ERR_GEOMETRY;
      G2Order order;
      if (g2_compare_segment_segment_distance(a->a, a->b, q->a, q->b,
                                              required, &order) != G2_OK) return DFM_ERR_GEOMETRY;
      if (order == G2_ORDER_LESS) {
        failed = true;
        if (!add_finding(r, rule, REPORT_FAIL, "track_pair", i, true, a->a,
                         false, 0, "Different-net copper tracks violate clearance (BVH candidate + exact segment distance)")) return DFM_ERR_REPORT_CAPACITY;
      }
    }
  }
  if (!any_pair && !failed) {
    if (!add_finding(r, rule, REPORT_UNVERIFIED, "track_pairs", SIZE_MAX, false,
                     (G2Point){0,0}, false, 0, "No eligible different-net same-layer track pairs found")) return DFM_ERR_REPORT_CAPACITY;
  } else if (!failed && !add_finding(r, rule, REPORT_PASS, "track_pairs", SIZE_MAX, false,
                                     (G2Point){0,0}, true, rule->value,
                                     "All tested straight-track pairs satisfy the centreline + half-width clearance threshold"))
    return DFM_ERR_REPORT_CAPACITY;
  return DFM_OK;
}
static DfmStatus check_hole_minimum(const Board *b, const FabRule *rule,
                                    CopperReport *r, bool npth_only, bool via_only) {
  bool found = false, failed = false;
  G2Nm minimum = INT64_MAX;
  G2Point minimum_at = {0, 0};
  size_t minimum_index = SIZE_MAX;
  const char *minimum_object = "holes";
  if (!via_only) for (size_t i = 0; i < b->pad_count; ++i) {
    const BoardPad *p = &b->pads[i];
    if (p->drill.x <= 0 || p->drill.y <= 0) continue;
    if (npth_only && p->type != BOARD_PAD_NPTH) continue;
    if (!npth_only && p->type == BOARD_PAD_NPTH) continue;
    found = true;
    G2Nm d = p->drill.x < p->drill.y ? p->drill.x : p->drill.y;
    if (d < minimum) { minimum = d; minimum_at = p->pos; minimum_index = i; minimum_object = "pad_hole"; }
    if (d < rule->value) {
      failed = true;
      if (!add_finding(r, rule, REPORT_FAIL, "pad_hole", i, true, p->pos,
                       true, d, "Drill minor dimension is below fabricator minimum")) return DFM_ERR_REPORT_CAPACITY;
    }
  }
  if (!npth_only && !via_only) for (size_t i = 0; i < b->via_count; ++i) {
    found = true;
    G2Nm d = b->vias[i].drill;
    if (d < minimum) { minimum = d; minimum_at = b->vias[i].pos; minimum_index = i; minimum_object = "via_hole"; }
    if (d < rule->value) {
      failed = true;
      if (!add_finding(r, rule, REPORT_FAIL, "via_hole", i, true, b->vias[i].pos,
                       true, d, "Via drill is below fabricator minimum")) return DFM_ERR_REPORT_CAPACITY;
    }
  }
  if (!found) return add_finding(r, rule, REPORT_UNVERIFIED, "holes", SIZE_MAX, false,
                                  (G2Point){0,0}, false, 0, "No applicable holes were present")
                     ? DFM_OK : DFM_ERR_REPORT_CAPACITY;
  if (!failed && !add_finding(r, rule, REPORT_PASS, minimum_object, minimum_index, true,
                               minimum_at, true, minimum,
                               "Minimum measured drill dimension meets the limit")) return DFM_ERR_REPORT_CAPACITY;
  return DFM_OK;
}
static DfmStatus check_via_dimension(const Board *b, const FabRule *rule,
                                     CopperReport *r, int selector) {
  bool failed = false;
  G2Nm minimum = INT64_MAX;
  G2Point minimum_at = {0, 0};
  size_t minimum_index = SIZE_MAX;
  if (!b->via_count) return add_finding(r, rule, REPORT_UNVERIFIED, "vias", SIZE_MAX, false,
                                         (G2Point){0,0}, false, 0, "No vias on this board")
                                   ? DFM_OK : DFM_ERR_REPORT_CAPACITY;
  for (size_t i = 0; i < b->via_count; ++i) {
    const BoardVia *v = &b->vias[i];
    G2Nm measured = selector == 0 ? v->drill : selector == 1 ? v->size : (v->size - v->drill) / 2;
    if (measured < minimum) { minimum = measured; minimum_at = v->pos; minimum_index = i; }
    if (measured < rule->value) {
      failed = true;
      if (!add_finding(r, rule, REPORT_FAIL, "via", i, true, v->pos, true,
                       measured, selector == 2 ? "Via radial annular ring is below limit" :
                       selector == 1 ? "Via diameter is below limit" : "Via drill is below limit")) return DFM_ERR_REPORT_CAPACITY;
    }
  }
  if (!failed && !add_finding(r, rule, REPORT_PASS, "via", minimum_index, true,
                               minimum_at, true, minimum,
                               "Minimum measured via dimension meets this rule")) return DFM_ERR_REPORT_CAPACITY;
  return DFM_OK;
}
static DfmStatus check_pad_annular(const Board *b, const FabRule *rule,
                                   CopperReport *r) {
  bool found = false, failed = false;
  G2Nm minimum = INT64_MAX;
  G2Point minimum_at = {0, 0};
  size_t minimum_index = SIZE_MAX;
  for (size_t i = 0; i < b->pad_count; ++i) {
    const BoardPad *p = &b->pads[i];
    if (p->type != BOARD_PAD_THRU || p->drill.x <= 0 || p->drill.y <= 0) continue;
    found = true;
    G2Nm dx = (p->size.x - p->drill.x) / 2;
    G2Nm dy = (p->size.y - p->drill.y) / 2;
    G2Nm ring = dx < dy ? dx : dy;
    /* Eccentric drilled holes reduce the minimum remaining copper ring. */
    uint64_t offset_sq = 0;
    if (g2_distance_squared((G2Point){0,0}, p->drill_offset, &offset_sq) != G2_OK) return DFM_ERR_GEOMETRY;
    uint64_t offset_ceil = isqrt_ceil_u64(offset_sq);
    if (offset_ceil > (uint64_t)INT64_MAX) return DFM_ERR_GEOMETRY;
    G2Nm offset = (G2Nm)offset_ceil;
    ring = ring > offset ? ring - offset : 0;
    if (ring < minimum) { minimum = ring; minimum_at = p->pos; minimum_index = i; }
    if (ring < rule->value) {
      failed = true;
      if (!add_finding(r, rule, REPORT_FAIL, "pth_pad", i, true, p->pos,
                       true, ring, "Conservative minimum annular ring is below limit")) return DFM_ERR_REPORT_CAPACITY;
    }
  }
  if (!found) return add_finding(r, rule, REPORT_UNVERIFIED, "pth_pads", SIZE_MAX, false,
                                  (G2Point){0,0}, false, 0, "No plated through-hole pads found")
                     ? DFM_OK : DFM_ERR_REPORT_CAPACITY;
  if (!failed && !add_finding(r, rule, REPORT_PASS, "pth_pad", minimum_index, true,
                               minimum_at, true, minimum,
                               "Minimum conservative PTH annular ring meets this rule")) return DFM_ERR_REPORT_CAPACITY;
  return DFM_OK;
}
static DfmStatus check_hole_spacing(const Board *b, const FabRule *rule,
                                    CopperReport *r, bool via_mode) {
  bool any = false, failed = false, oval_unverified = false;
  if (!via_mode) {
    for (size_t i = 0; i < b->pad_count; ++i) {
      const BoardPad *p = &b->pads[i];
      if (p->drill.x > 0 && p->drill.y > 0 && p->drill.x != p->drill.y)
        oval_unverified = true;
    }
    if (oval_unverified &&
        !add_finding(r, rule, REPORT_UNVERIFIED, "oval_holes", SIZE_MAX, false,
                     (G2Point){0,0}, false, 0,
                     "Oval/slot hole spacing requires capsule geometry; the current centre-distance check cannot certify it"))
      return DFM_ERR_REPORT_CAPACITY;
  }
  if (via_mode) {
    for (size_t i = 0; i < b->via_count; ++i) for (size_t j = i + 1; j < b->via_count; ++j) {
      any = true; G2Nm rad = (b->vias[i].drill + b->vias[j].drill + 1) / 2, th;
      if (!add_sat(rad, rule->value, &th)) return DFM_ERR_GEOMETRY;
      G2Order o;
      if (g2_compare_distance(b->vias[i].pos, b->vias[j].pos, th, &o) != G2_OK) return DFM_ERR_GEOMETRY;
      if (o == G2_ORDER_LESS) { failed = true; if (!add_finding(r, rule, REPORT_FAIL, "via_hole_pair", i, true, b->vias[i].pos, false, 0, "Via hole-to-hole spacing is below limit")) return DFM_ERR_REPORT_CAPACITY; }
    }
  } else {
    for (size_t i = 0; i < b->pad_count; ++i) {
      const BoardPad *a = &b->pads[i]; if (a->drill.x <= 0 || a->drill.y <= 0) continue;
      for (size_t j = i + 1; j < b->pad_count; ++j) {
        const BoardPad *q = &b->pads[j]; if (q->drill.x <= 0 || q->drill.y <= 0) continue;
        any = true;
        G2Nm ra = (a->drill.x < a->drill.y ? a->drill.x : a->drill.y) / 2;
        G2Nm rb = (q->drill.x < q->drill.y ? q->drill.x : q->drill.y) / 2;
        G2Nm th; if (!add_sat(ra + rb, rule->value, &th)) return DFM_ERR_GEOMETRY;
        G2Order o;
        if (g2_compare_distance(a->pos, q->pos, th, &o) != G2_OK) return DFM_ERR_GEOMETRY;
        if (o == G2_ORDER_LESS) { failed = true; if (!add_finding(r, rule, REPORT_FAIL, "pad_hole_pair", i, true, a->pos, false, 0, "Pad hole-to-hole spacing is below limit")) return DFM_ERR_REPORT_CAPACITY; }
      }
    }
  }
  if (!any) return add_finding(r, rule, REPORT_UNVERIFIED, "hole_pairs", SIZE_MAX, false,
                                (G2Point){0,0}, false, 0, "No applicable hole pairs present")
                    ? DFM_OK : DFM_ERR_REPORT_CAPACITY;
  if (!failed && !add_finding(r, rule, REPORT_PASS, "hole_pairs", SIZE_MAX, false,
                               (G2Point){0,0}, true, rule->value,
                               "All tested hole pairs satisfy the edge-to-edge spacing rule")) return DFM_ERR_REPORT_CAPACITY;
  return DFM_OK;
}
static DfmStatus check_smd_min(const Board *b, const FabRule *rule, CopperReport *r) {
  bool found = false, failed = false;
  G2Nm minimum = INT64_MAX;
  G2Point minimum_at = {0, 0};
  size_t minimum_index = SIZE_MAX;
  for (size_t i = 0; i < b->pad_count; ++i) {
    const BoardPad *p = &b->pads[i]; if (p->type != BOARD_PAD_SMD) continue;
    found = true;
    G2Nm d = p->size.x < p->size.y ? p->size.x : p->size.y;
    if (d < minimum) { minimum = d; minimum_at = p->pos; minimum_index = i; }
    if (d < rule->value) { failed = true; if (!add_finding(r, rule, REPORT_FAIL, "smd_pad", i, true, p->pos, true, d, "Minimum SMD pad dimension is below limit")) return DFM_ERR_REPORT_CAPACITY; }
  }
  if (!found) return add_finding(r, rule, REPORT_UNVERIFIED, "smd_pads", SIZE_MAX, false,
                                  (G2Point){0,0}, false, 0, "No SMD pads found")
                     ? DFM_OK : DFM_ERR_REPORT_CAPACITY;
  if (!failed && !add_finding(r, rule, REPORT_PASS, "smd_pad", minimum_index, true,
                               minimum_at, true, minimum, "Minimum SMD pad dimension meets the limit")) return DFM_ERR_REPORT_CAPACITY;
  return DFM_OK;
}
static DfmStatus check_board_extent(const Board *b, const FabRule *rule,
                                    CopperReport *r, bool thickness) {
  if (thickness) {
    if (b->rules.board_thickness == BOARD_UNSET_NM)
      return add_finding(r, rule, REPORT_UNVERIFIED, "board_thickness", SIZE_MAX, false,
                         (G2Point){0,0}, false, 0, "Board thickness is absent from KiCad general section")
                 ? DFM_OK : DFM_ERR_REPORT_CAPACITY;
    bool pass = rule->kind[0] == 'm' && strstr(rule->kind, "max") != NULL
                    ? b->rules.board_thickness <= rule->value
                    : b->rules.board_thickness >= rule->value;
    return add_finding(r, rule, pass ? REPORT_PASS : REPORT_FAIL, "board_thickness", SIZE_MAX,
                       false, (G2Point){0,0}, true, b->rules.board_thickness,
                       pass ? "Board thickness meets limit" : "Board thickness violates limit")
               ? DFM_OK : DFM_ERR_REPORT_CAPACITY;
  }
  if (b->rules.board_thickness == BOARD_UNSET_NM ||
      b->rules.board_thickness < INT64_C(600000))
    return add_finding(r, rule, REPORT_UNVERIFIED, "board_outline", SIZE_MAX, false,
                       (G2Point){0,0}, false, 0,
                       "JLCPCB's 3 x 3 mm minimum dimension is published for board thickness >= 0.6 mm; thinner or unknown thickness requires review")
               ? DFM_OK : DFM_ERR_REPORT_CAPACITY;
  G2Aabb bounds;
  if (board_bounds(b, &bounds) != BOARD_OK)
    return add_finding(r, rule, REPORT_UNVERIFIED, "board_outline", SIZE_MAX, false,
                       (G2Point){0,0}, false, 0, "Cannot calculate board bounds from Edge.Cuts")
               ? DFM_OK : DFM_ERR_REPORT_CAPACITY;
  G2Nm w = bounds.max.x - bounds.min.x, h = bounds.max.y - bounds.min.y;
  G2Nm min = w < h ? w : h;
  bool pass = min >= rule->value;
  return add_finding(r, rule, pass ? REPORT_PASS : REPORT_FAIL, "board_outline", SIZE_MAX,
                     false, (G2Point){0,0}, true, min,
                     pass ? "Minimum board dimension meets limit" : "Minimum board dimension below limit")
             ? DFM_OK : DFM_ERR_REPORT_CAPACITY;
}
static DfmStatus check_edge_clearance(const Board *b, const FabRule *rule,
                                      CopperReport *r) {
  bool failed = false, tested = false;
  for (size_t i = 0; i < b->track_count; ++i) for (size_t j = 0; j < b->edge_count; ++j) {
    const BoardEdge *e = &b->edges[j];
    if (e->kind != BOARD_EDGE_LINE) continue;
    tested = true;
    G2Nm threshold, half = (b->tracks[i].width + (e->width > 0 ? e->width : 0) + 1) / 2;
    if (!add_sat(rule->value, half, &threshold)) return DFM_ERR_GEOMETRY;
    G2Order o;
    if (g2_compare_segment_segment_distance(b->tracks[i].a, b->tracks[i].b,
                                            e->p0, e->p1, threshold, &o) != G2_OK) return DFM_ERR_GEOMETRY;
    if (o == G2_ORDER_LESS) {
      failed = true;
      if (!add_finding(r, rule, REPORT_FAIL, "track_edge_pair", i, true,
                       b->tracks[i].a, false, 0, "Track copper is too close to a straight Edge.Cuts segment")) return DFM_ERR_REPORT_CAPACITY;
    }
  }
  bool nonlinear_edge = false;
  for (size_t j = 0; j < b->edge_count; ++j)
    if (b->edges[j].kind != BOARD_EDGE_LINE) nonlinear_edge = true;
  if (b->arc_track_count || b->via_count || b->pad_count || b->zone_count ||
      nonlinear_edge) {
    if (!add_finding(r, rule, REPORT_UNVERIFIED, "board_copper", SIZE_MAX, false,
                     (G2Point){0,0}, false, 0, "Edge-clearance coverage is incomplete for pads, vias, zones, arc tracks, and non-linear outlines")) return DFM_ERR_REPORT_CAPACITY;
  }
  if (!tested) return add_finding(r, rule, REPORT_UNVERIFIED, "edge_clearance", SIZE_MAX, false,
                                   (G2Point){0,0}, false, 0, "No straight Edge.Cuts segment was available")
                      ? DFM_OK : DFM_ERR_REPORT_CAPACITY;
  if (!failed && !add_finding(r, rule, REPORT_PASS, "track_edges", SIZE_MAX, false,
                               (G2Point){0,0}, true, rule->value,
                               "All tested track-to-straight-outline distances meet the limit")) return DFM_ERR_REPORT_CAPACITY;
  return DFM_OK;
}
static DfmStatus dispatch_rule(const Board *b, const BoardDiag *diag,
                               const FabRule *rule, const DfmOptions *o,
                               Arena *arena, CopperReport *report) {
  if (!strcmp(rule->kind, "min_track_width")) return check_track_width(b, rule, report);
  if (!strcmp(rule->kind, "min_clearance_copper")) return check_track_clearance(b, diag, rule, o, arena, report);
  if (!strcmp(rule->kind, "min_drill")) return check_hole_minimum(b, rule, report, false, false);
  if (!strcmp(rule->kind, "min_npth_drill")) return check_hole_minimum(b, rule, report, true, false);
  if (!strcmp(rule->kind, "min_via_drill")) return check_via_dimension(b, rule, report, 0);
  if (!strcmp(rule->kind, "min_via_diameter")) return check_via_dimension(b, rule, report, 1);
  if (!strcmp(rule->kind, "min_via_annular_ring")) return check_via_dimension(b, rule, report, 2);
  if (!strcmp(rule->kind, "min_annular_ring")) return check_pad_annular(b, rule, report);
  if (!strcmp(rule->kind, "min_hole_to_hole_via")) return check_hole_spacing(b, rule, report, true);
  if (!strcmp(rule->kind, "min_hole_to_hole_pad")) return check_hole_spacing(b, rule, report, false);
  if (!strcmp(rule->kind, "min_smd_pad_size")) return check_smd_min(b, rule, report);
  if (!strcmp(rule->kind, "min_copper_to_edge")) return check_edge_clearance(b, rule, report);
  if (!strcmp(rule->kind, "min_board_dimension")) return check_board_extent(b, rule, report, false);
  if (!strcmp(rule->kind, "min_board_thickness") || !strcmp(rule->kind, "max_board_thickness"))
    return check_board_extent(b, rule, report, true);
  return missing_rule(report, "Rule exists in dataset, but this DFM geometry check is not implemented yet");
}
DfmStatus dfm_check_board(const Board *b, const BoardDiag *diag,
                          const FabRuleSet *rules, const DfmOptions *options,
                          Arena *arena, CopperReport *report) {
  if (!b || !rules || !arena || !report) return DFM_ERR_ARGUMENT;
  if (!report->findings || report->capacity == 0) return DFM_ERR_REPORT_CAPACITY;
  const DfmOptions def = {0, NULL, false, NULL};
  const DfmOptions *o = options ? options : &def;
  if (o->mask_color && strcmp(o->mask_color, "standard") != 0 &&
      strcmp(o->mask_color, "black_white") != 0) return DFM_ERR_ARGUMENT;
  for (size_t i = 0; i < rules->rule_count; ++i) {
    const FabRule *rule = &rules->rules[i];
    if (rule->layers_min && b->rules.copper_layers < rule->layers_min) continue;
    if (rule->layers_max && b->rules.copper_layers > rule->layers_max) continue;
    if (rule->copper_oz_x10) {
      if (o->copper_oz_x10 == 0) {
        if (!add_finding(report, rule, REPORT_UNVERIFIED, "process_configuration", SIZE_MAX,
                         false, (G2Point){0,0}, false, 0,
                         "Copper weight was not selected; this fabrication limit cannot be chosen safely")) return DFM_ERR_REPORT_CAPACITY;
        continue;
      }
      if (rule->copper_oz_x10 != o->copper_oz_x10) continue;
    }
    if (rule->mask_color) {
      if (!o->mask_color) {
        if (!add_finding(report, rule, REPORT_UNVERIFIED, "process_configuration", SIZE_MAX,
                         false, (G2Point){0,0}, false, 0,
                         "Mask colour was not selected; colour-dependent bridge rule cannot be selected safely"))
          return DFM_ERR_REPORT_CAPACITY;
        continue;
      }
      if (strcmp(rule->mask_color, o->mask_color) != 0) continue;
    }
    if (rule->edge) {
      if (!o->edge_type) {
        if (!add_finding(report, rule, REPORT_UNVERIFIED, "process_configuration", SIZE_MAX,
                         false, (G2Point){0,0}, false, 0,
                         "Board edge process was not selected (routed or V-cut)")) return DFM_ERR_REPORT_CAPACITY;
        continue;
      }
      if (strcmp(rule->edge, o->edge_type) != 0) continue;
    }
    DfmStatus st = dispatch_rule(b, diag, rule, o, arena, report);
    if (st != DFM_OK) return st;
  }
  if (diag && diag->unmodelled_geometry_records) {
    if (!add_finding(report, NULL, REPORT_UNVERIFIED, "unmodelled_geometry", SIZE_MAX,
                     false, (G2Point){0,0}, false, 0,
                     "KiCad contains manufacturing-relevant geometry not represented in the typed board model; retain and run native KiCad DRC"))
      return DFM_ERR_REPORT_CAPACITY;
  }
  return DFM_OK;
}

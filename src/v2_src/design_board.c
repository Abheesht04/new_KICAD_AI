#include "design_board.h"

#include <stdio.h>
#include <string.h>

static const char *const k_layer_names[BOARD_LAYER_COUNT] = {
    "F.Cu",
    "In1.Cu", "In2.Cu", "In3.Cu", "In4.Cu", "In5.Cu", "In6.Cu",
    "In7.Cu", "In8.Cu", "In9.Cu", "In10.Cu", "In11.Cu", "In12.Cu",
    "In13.Cu", "In14.Cu", "In15.Cu", "In16.Cu", "In17.Cu", "In18.Cu",
    "In19.Cu", "In20.Cu", "In21.Cu", "In22.Cu", "In23.Cu", "In24.Cu",
    "In25.Cu", "In26.Cu", "In27.Cu", "In28.Cu", "In29.Cu", "In30.Cu",
    "B.Cu", "B.Adhes", "F.Adhes", "B.Paste", "F.Paste", "B.SilkS",
    "F.SilkS", "B.Mask", "F.Mask", "Dwgs.User", "Cmts.User",
    "Eco1.User", "Eco2.User", "Edge.Cuts", "Margin", "B.CrtYd", "F.CrtYd",
    "B.Fab", "F.Fab", "User.1", "User.2", "User.3", "User.4", "User.5",
    "User.6", "User.7", "User.8", "User.9"};

const char *board_layer_name(BoardLayer layer) {
  return (unsigned)layer < BOARD_LAYER_COUNT ? k_layer_names[layer] : "?";
}

BoardStatus board_layer_from_name(const char *name, BoardLayer *out) {
  if (!name || !out) return BOARD_ERR_ARGUMENT;
  for (unsigned i = 0; i < BOARD_LAYER_COUNT; ++i) {
    if (strcmp(name, k_layer_names[i]) == 0) {
      *out = (BoardLayer)i;
      return BOARD_OK;
    }
  }
  return BOARD_ERR_UNSUPPORTED;
}

bool board_layer_is_copper(BoardLayer layer) {
  return layer == BOARD_LAYER_F_CU ||
         (layer >= BOARD_LAYER_IN1_CU && layer <= BOARD_LAYER_IN30_CU) ||
         layer == BOARD_LAYER_B_CU;
}

BoardStatus board_init(Board *b, Arena *arena) {
  if (!b || !arena) return BOARD_ERR_ARGUMENT;
  memset(b, 0, sizeof(*b));
  b->arena = arena;
  b->rules.copper_layers = 2;
  b->rules.board_thickness = BOARD_UNSET_NM;
  b->rules.min_clearance = BOARD_UNSET_NM;
  b->rules.min_track_width = BOARD_UNSET_NM;
  b->rules.min_via_size = BOARD_UNSET_NM;
  b->rules.min_via_drill = BOARD_UNSET_NM;
  b->rules.min_hole_to_hole = BOARD_UNSET_NM;
  b->rules.min_annular = BOARD_UNSET_NM;
  b->rules.min_through_hole = BOARD_UNSET_NM;
  b->rules.edge_clearance = BOARD_UNSET_NM;
  b->rules.mask_margin = BOARD_UNSET_NM;
  b->rules.paste_margin = BOARD_UNSET_NM;
  return BOARD_OK;
}

const BoardNet *board_net_by_id(const Board *b, uint32_t id) {
  if (!b) return NULL;
  for (size_t i = 0; i < b->net_count; ++i)
    if (b->nets[i].id == id) return &b->nets[i];
  return NULL;
}


/* Q30 CORDIC constants. Angles are nanodegrees so the final iterations retain
 * useful precision. No floating point, libm, locale, or platform trig calls. */
#define TRIG_Q 1073741824LL
#define CORDIC_K_Q30 652032874LL
static const int64_t k_atan_nanodeg[31] = {
  45000000000LL,26565051177LL,14036243468LL,7125016349LL,
  3576334375LL,1789910608LL,895173710LL,447614171LL,
  223810500LL,111905677LL,55952892LL,27976453LL,
  13988227LL,6994114LL,3497057LL,1748528LL,
  874264LL,437132LL,218566LL,109283LL,54642LL,
  27321LL,13660LL,6830LL,3415LL,1708LL,854LL,427LL,213LL,107LL,53LL
};
static int64_t floor_div_pow2(int64_t v, unsigned shift) {
  const int64_t d = (int64_t)(UINT64_C(1) << shift);
  if (v >= 0) return v / d;
  return -(((-v) + d - 1) / d);
}
static bool trig_q30(int32_t angle_mdeg, int64_t *co, int64_t *si) {
  int64_t a = (int64_t)angle_mdeg % 360000LL;
  if (a < 0) a += 360000LL;
  if (a == 0) { *co = TRIG_Q; *si = 0; return true; }
  if (a == 90000) { *co = 0; *si = TRIG_Q; return true; }
  if (a == 180000) { *co = -TRIG_Q; *si = 0; return true; }
  if (a == 270000) { *co = 0; *si = -TRIG_Q; return true; }
  int cos_sign = 1;
  if (a > 180000) a -= 360000;
  if (a > 90000) { a = 180000 - a; cos_sign = -1; }
  else if (a < -90000) { a = -180000 - a; cos_sign = -1; }
  int64_t z = a * 1000000LL;
  int64_t x = CORDIC_K_Q30, y = 0;
  for (unsigned i = 0; i < 31; ++i) {
    int64_t xs = floor_div_pow2(x, i);
    int64_t ys = floor_div_pow2(y, i);
    if (z >= 0) { x -= ys; y += xs; z -= k_atan_nanodeg[i]; }
    else { x += ys; y -= xs; z += k_atan_nanodeg[i]; }
  }
  *co = cos_sign * x; *si = y;
  return true;
}
static bool checked_add_nm(G2Nm a, G2Nm b, G2Nm *out) {
  if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) return false;
  *out = a + b; return true;
}
static uint64_t ceil_div_u64(uint64_t n, uint64_t d) {
  return n / d + (n % d != 0);
}
BoardStatus board_rotate(G2Point local, int32_t rot_mdeg, G2Point *out,
                         bool *exact) {
  if (!out) return BOARD_ERR_ARGUMENT;
  int32_t a = rot_mdeg % 360000;
  if (a < 0) a += 360000;
  if (exact) *exact = true;
  if (a == 0) { *out = local; return BOARD_OK; }
  if (a == 90000) { out->x = local.y; if (local.x == INT64_MIN) return BOARD_ERR_RANGE; out->y = -local.x; return BOARD_OK; }
  if (a == 180000) { if (local.x == INT64_MIN || local.y == INT64_MIN) return BOARD_ERR_RANGE; out->x = -local.x; out->y = -local.y; return BOARD_OK; }
  if (a == 270000) { if (local.y == INT64_MIN) return BOARD_ERR_RANGE; out->x = -local.y; out->y = local.x; return BOARD_OK; }
  int64_t c, si; (void)trig_q30(a, &c, &si);
  /* The board coordinate convention has positive Y down. */
  /* Products are bounded by the board coordinate domain (±1 m). */
  /* KiCad board coordinates are bounded to ±1e9 nm, so each Q30
     product and the sum of two products fit signed 64-bit arithmetic. */
  if (local.x < -G2_COORD_MAX_NM || local.x > G2_COORD_MAX_NM ||
      local.y < -G2_COORD_MAX_NM || local.y > G2_COORD_MAX_NM)
    return BOARD_ERR_RANGE;
  int64_t x1 = local.x * c, x2 = local.y * si;
  int64_t y1 = -local.x * si, y2 = local.y * c;
  if ((x2 > 0 && x1 > INT64_MAX - x2) || (x2 < 0 && x1 < INT64_MIN - x2) ||
      (y2 > 0 && y1 > INT64_MAX - y2) || (y2 < 0 && y1 < INT64_MIN - y2))
    return BOARD_ERR_RANGE;
  int64_t sx = x1 + x2, sy = y1 + y2;
  /* Round to nearest nm, ties away from zero. */
  out->x = sx >= 0 ? (sx + TRIG_Q/2) / TRIG_Q : -(((-sx) + TRIG_Q/2) / TRIG_Q);
  out->y = sy >= 0 ? (sy + TRIG_Q/2) / TRIG_Q : -(((-sy) + TRIG_Q/2) / TRIG_Q);
  if (exact) *exact = false;
  return BOARD_OK;
}
BoardStatus board_pad_aabb(const BoardPad *p, G2Aabb *out) {
  if (!p || !out || p->size.x <= 0 || p->size.y <= 0)
    return BOARD_ERR_ARGUMENT;
  int32_t angle = p->rot_mdeg % 360000;
  if (angle < 0) angle += 360000;
  uint64_t ex, ey;
  if (angle == 0 || angle == 180000) {
    ex = ceil_div_u64((uint64_t)p->size.x, 2u);
    ey = ceil_div_u64((uint64_t)p->size.y, 2u);
  } else if (angle == 90000 || angle == 270000) {
    ex = ceil_div_u64((uint64_t)p->size.y, 2u);
    ey = ceil_div_u64((uint64_t)p->size.x, 2u);
  } else {
    int64_t c, si; (void)trig_q30(angle, &c, &si);
    uint64_t ac = (uint64_t)(c < 0 ? -c : c);
    uint64_t as = (uint64_t)(si < 0 ? -si : si);
    uint64_t sx = ac * (uint64_t)p->size.x + as * (uint64_t)p->size.y;
    uint64_t sy = as * (uint64_t)p->size.x + ac * (uint64_t)p->size.y;
    /* Divide projected dimensions by 2*Q30, rounding outward.
       Extra 2 nm covers bounded CORDIC approximation error. */
    uint64_t den = (uint64_t)TRIG_Q * 2u;
    ex = ceil_div_u64(sx, den) + 2u;
    ey = ceil_div_u64(sy, den) + 2u;
  }
  if (ex > (uint64_t)INT64_MAX || ey > (uint64_t)INT64_MAX)
    return BOARD_ERR_RANGE;
  G2Nm x = (G2Nm)ex, y = (G2Nm)ey;
  if (!checked_add_nm(p->pos.x, -x, &out->min.x) ||
      !checked_add_nm(p->pos.y, -y, &out->min.y) ||
      !checked_add_nm(p->pos.x, x, &out->max.x) ||
      !checked_add_nm(p->pos.y, y, &out->max.y))
    return BOARD_ERR_RANGE;
  return BOARD_OK;
}
static bool include_nm(G2Nm v, G2Nm half, G2Nm *mn, G2Nm *mx) {
  G2Nm lo, hi;
  if (!checked_add_nm(v, -half, &lo) || !checked_add_nm(v, half, &hi)) return false;
  if (lo < *mn) *mn = lo;
  if (hi > *mx) *mx = hi;
  return true;
}
BoardStatus board_bounds(const Board *b, G2Aabb *out) {
  if (!b || !out) return BOARD_ERR_ARGUMENT;
  if (b->edge_count == 0) return BOARD_ERR_INVALID;
  G2Nm minx = INT64_MAX, miny = INT64_MAX, maxx = INT64_MIN, maxy = INT64_MIN;
  for (size_t i = 0; i < b->edge_count; ++i) {
    const BoardEdge *e = &b->edges[i];
    if (e->kind == BOARD_EDGE_ARC || e->kind == BOARD_EDGE_CIRCLE)
      return BOARD_ERR_UNSUPPORTED; /* Never substitute an unsafe chord box. */
    G2Nm half = e->width > 0 ? e->width / 2 + e->width % 2 : 0;
    if (e->kind == BOARD_EDGE_POLYGON) {
      if ((size_t)e->first_pt + e->pt_count > b->edge_pt_count) return BOARD_ERR_REFERENCE;
      for (size_t k = 0; k < e->pt_count; ++k) {
        G2Point q = b->edge_pts[e->first_pt + k];
        if (!include_nm(q.x, half, &minx, &maxx) ||
            !include_nm(q.y, half, &miny, &maxy)) return BOARD_ERR_RANGE;
      }
    } else {
      G2Point q[2] = {e->p0, e->p1};
      for (size_t k = 0; k < 2; ++k)
        if (!include_nm(q[k].x, half, &minx, &maxx) ||
            !include_nm(q[k].y, half, &miny, &maxy)) return BOARD_ERR_RANGE;
    }
  }
  if (minx == INT64_MAX) return BOARD_ERR_INVALID;
  out->min = (G2Point){minx, miny}; out->max = (G2Point){maxx, maxy};
  return BOARD_OK;
}

BoardStatus board_hole_count(const Board *b, size_t *plated, size_t *unplated) {
  if (!b || !plated || !unplated) return BOARD_ERR_ARGUMENT;
  size_t pl = 0, un = 0;
  for (size_t i = 0; i < b->pad_count; ++i) {
    const BoardPad *p = &b->pads[i];
    if (p->drill.x <= 0 && p->drill.y <= 0) continue;
    if (p->type == BOARD_PAD_NPTH) ++un; else ++pl;
  }
  for (size_t i = 0; i < b->via_count; ++i)
    if (b->vias[i].plated) ++pl;
  *plated = pl; *unplated = un;
  return BOARD_OK;
}

static void diag_fail(BoardDiag *d, BoardStatus code, const char *message) {
  if (!d) return;
  d->status = code;
  (void)snprintf(d->message, sizeof(d->message), "%s", message ? message : "");
}

BoardStatus board_validate(const Board *b, BoardDiag *d) {
  if (!b) return BOARD_ERR_ARGUMENT;
  for (size_t i = 0; i < b->layer_count; ++i) {
    if ((unsigned)b->layers[i].id >= BOARD_LAYER_COUNT) {
      diag_fail(d, BOARD_ERR_INVALID, "board has invalid layer ID");
      return BOARD_ERR_INVALID;
    }
  }
  for (size_t i = 0; i < b->pad_count; ++i) {
    const BoardPad *p = &b->pads[i];
    if (p->footprint >= b->footprint_count) {
      diag_fail(d, BOARD_ERR_REFERENCE, "pad references missing footprint");
      return BOARD_ERR_REFERENCE;
    }
    if (p->net != 0 && !board_net_by_id(b, p->net)) {
      diag_fail(d, BOARD_ERR_REFERENCE, "pad references missing net");
      return BOARD_ERR_REFERENCE;
    }
    if (p->size.x <= 0 || p->size.y <= 0) {
      diag_fail(d, BOARD_ERR_INVALID, "pad with non-positive size");
      return BOARD_ERR_INVALID;
    }
    if ((p->drill.x < 0) || (p->drill.y < 0)) {
      diag_fail(d, BOARD_ERR_INVALID, "pad with negative drill size");
      return BOARD_ERR_INVALID;
    }
  }
  for (size_t i = 0; i < b->track_count; ++i) {
    const BoardTrack *t = &b->tracks[i];
    if (t->width <= 0 || (t->a.x == t->b.x && t->a.y == t->b.y)) {
      diag_fail(d, BOARD_ERR_INVALID, "track with non-positive width or zero length");
      return BOARD_ERR_INVALID;
    }
    if (!board_layer_is_copper(t->layer)) {
      diag_fail(d, BOARD_ERR_INVALID, "track on non-copper layer");
      return BOARD_ERR_INVALID;
    }
    if (t->net != 0 && !board_net_by_id(b, t->net)) {
      diag_fail(d, BOARD_ERR_REFERENCE, "track references missing net");
      return BOARD_ERR_REFERENCE;
    }
  }
  for (size_t i = 0; i < b->arc_track_count; ++i) {
    const BoardArcTrack *a = &b->arc_tracks[i];
    if (a->width <= 0 || !board_layer_is_copper(a->layer)) {
      diag_fail(d, BOARD_ERR_INVALID, "arc track has invalid width or layer");
      return BOARD_ERR_INVALID;
    }
    if (a->net != 0 && !board_net_by_id(b, a->net)) {
      diag_fail(d, BOARD_ERR_REFERENCE, "arc track references missing net");
      return BOARD_ERR_REFERENCE;
    }
  }
  for (size_t i = 0; i < b->via_count; ++i) {
    const BoardVia *v = &b->vias[i];
    if (v->drill <= 0 || v->size <= v->drill ||
        !board_layer_is_copper(v->from) || !board_layer_is_copper(v->to)) {
      diag_fail(d, BOARD_ERR_INVALID, "via drill/size/layer invalid");
      return BOARD_ERR_INVALID;
    }
    if (v->net != 0 && !board_net_by_id(b, v->net)) {
      diag_fail(d, BOARD_ERR_REFERENCE, "via references missing net");
      return BOARD_ERR_REFERENCE;
    }
  }
  for (size_t i = 0; i < b->zone_count; ++i) {
    const BoardZone *z = &b->zones[i];
    if ((size_t)z->first_pt + z->pt_count > b->zone_pt_count) {
      diag_fail(d, BOARD_ERR_REFERENCE, "zone point range out of bounds");
      return BOARD_ERR_REFERENCE;
    }
    if (z->net != 0 && !board_net_by_id(b, z->net)) {
      diag_fail(d, BOARD_ERR_REFERENCE, "zone references missing net");
      return BOARD_ERR_REFERENCE;
    }
  }
  for (size_t i = 0; i < b->footprint_count; ++i) {
    const BoardFootprint *f = &b->footprints[i];
    if ((size_t)f->first_pad + f->pad_count > b->pad_count ||
        (size_t)f->first_court + f->court_count > b->court_pt_count) {
      diag_fail(d, BOARD_ERR_REFERENCE, "footprint child range out of bounds");
      return BOARD_ERR_REFERENCE;
    }
  }
  for (size_t i = 0; i < b->edge_count; ++i) {
    const BoardEdge *e = &b->edges[i];
    if (e->width < 0 || (e->kind == BOARD_EDGE_POLYGON &&
        (size_t)e->first_pt + e->pt_count > b->edge_pt_count)) {
      diag_fail(d, BOARD_ERR_INVALID, "edge geometry invalid");
      return BOARD_ERR_INVALID;
    }
  }
  diag_fail(d, BOARD_OK, "");
  return BOARD_OK;
}

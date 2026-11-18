#include "design_board.h"

#include <stdio.h>
#include <string.h>


static const IRNode *head(const IRNode *n) { return ir_list_head(n); }
static bool is_list(const IRNode *n, const char *name) {
  const IRNode *h = head(n);
  return n && n->kind == IR_NODE_LIST && h && h->kind == IR_NODE_ATOM &&
         strcmp(h->text, name) == 0;
}
static const char *text_at(const IRNode *n, size_t i) {
  const IRNode *v = ir_child_at(n, i);
  return v && (v->kind == IR_NODE_ATOM || v->kind == IR_NODE_STRING) ? v->text : NULL;
}
static const IRNode *field(const IRNode *n, const char *name) {
  return ir_list_find(n, name);
}
static const char *field_text(const IRNode *n, const char *name) {
  return text_at(field(n, name), 1);
}
static bool atom_i64(const IRNode *n, int64_t *out) {
  return n && ir_atom_to_i64(n, out) == IR_STATUS_OK;
}
static bool atom_u64(const IRNode *n, uint64_t *out) {
  return n && ir_atom_to_u64(n, out) == IR_STATUS_OK;
}
static bool atom_nm(const IRNode *n, G2Nm *out) {
  return n && ir_atom_to_nm(n, out) == IR_STATUS_OK;
}
static bool node_nm(const IRNode *n, size_t i, G2Nm *out) {
  return atom_nm(ir_child_at(n, i), out);
}
/* Parse a decimal into a scaled integer without a binary floating-point path.
   Extra decimal digits are rounded half away from zero. Exponents are rejected,
   matching the numeric syntax emitted by KiCad. */
static bool atom_to_scaled_i64(const IRNode *node, uint64_t scale, int64_t *out) {
  if (!node || !out || node->kind != IR_NODE_ATOM || !node->text || scale == 0)
    return false;
  const unsigned char *p = (const unsigned char *)node->text;
  bool negative = false;
  if (*p == '+' || *p == '-') { negative = (*p == '-'); ++p; }
  unsigned digits = 0;
  uint64_t whole = 0;
  while (*p >= '0' && *p <= '9') {
    unsigned d = (unsigned)(*p++ - '0');
    if (whole > (UINT64_MAX - d) / 10u) return false;
    whole = whole * 10u + d; ++digits;
  }
  unsigned scale_digits = 0; uint64_t factor = 1;
  for (uint64_t q = scale; q > 1 && q % 10u == 0; q /= 10u) ++scale_digits, factor *= 10u;
  if (factor != scale) return false;
  uint64_t fraction = 0; unsigned seen = 0; bool round_up = false;
  if (*p == '.') {
    ++p;
    while (*p >= '0' && *p <= '9') {
      unsigned d = (unsigned)(*p++ - '0'); ++digits;
      if (seen < scale_digits) fraction = fraction * 10u + d;
      else if (seen == scale_digits) round_up = d >= 5u;
      ++seen;
    }
  }
  if (digits == 0 || *p != '\0') return false;
  while (seen < scale_digits) { fraction *= 10u; ++seen; }
  if (whole > UINT64_MAX / scale) return false;
  uint64_t magnitude = whole * scale;
  if (fraction > UINT64_MAX - magnitude) return false;
  magnitude += fraction;
  if (round_up) { if (magnitude == UINT64_MAX) return false; ++magnitude; }
  uint64_t limit = negative ? (uint64_t)INT64_MAX + 1u : (uint64_t)INT64_MAX;
  if (magnitude > limit) return false;
  if (negative && magnitude == (uint64_t)INT64_MAX + 1u) *out = INT64_MIN;
  else *out = negative ? -(int64_t)magnitude : (int64_t)magnitude;
  return true;
}
static bool node_angle(const IRNode *n, size_t i, int32_t *out) {
  int64_t value;
  if (!atom_to_scaled_i64(ir_child_at(n, i), 1000u, &value) ||
      value < INT32_MIN || value > INT32_MAX) return false;
  *out = (int32_t)value;
  return true;
}
static bool field_flag(const IRNode *n, const char *name) {
  const IRNode *f = field(n, name);
  if (!f) return false;
  const IRNode *v = ir_child_at(f, 1);
  if (!v) return true; /* marker form, e.g. (locked) */
  bool value = false;
  return ir_atom_to_bool(v, &value) == IR_STATUS_OK ? value : true;
}
static bool point_field(const IRNode *n, const char *name, G2Point *out) {
  const IRNode *f = field(n, name);
  if (!f) return false;
  return node_nm(f, 1, &out->x) && node_nm(f, 2, &out->y);
}
static void diag_skip(BoardDiag *d, const IRNode *n, bool geometry) {
  if (!d) return;
  ++d->unmodelled_records;
  if (geometry) ++d->unmodelled_geometry_records;
  if (d->first_unmodelled[0] == '\0') {
    const char *h = text_at(n, 0);
    if (h) (void)snprintf(d->first_unmodelled, sizeof(d->first_unmodelled), "%s", h);
  }
}
static BoardStatus diag_error(BoardDiag *d, BoardStatus s, const char *msg) {
  if (d) {
    d->status = s;
    (void)snprintf(d->message, sizeof(d->message), "%s", msg ? msg : "");
  }
  return s;
}
static size_t count_named(const IRNode *n, const char *name) {
  size_t c = 0;
  for (const IRNode *it = n ? n->first_child : NULL; it; it = it->next)
    if (is_list(it, name)) ++c;
  return c;
}
static size_t count_copper_layers(const IRNode *layers) {
  size_t c = 0;
  for (const IRNode *it = layers ? layers->first_child : NULL; it; it = it->next) {
    const char *name = text_at(it, 1);
    BoardLayer l;
    if (name && board_layer_from_name(name, &l) == BOARD_OK && board_layer_is_copper(l)) ++c;
  }
  return c;
}
static size_t count_fp_pads(const IRNode *fp) { return count_named(fp, "pad"); }
static bool layer_is_courtyard(const IRNode *n) {
  const char *name = field_text(n, "layer");
  return name && (strcmp(name, "F.CrtYd") == 0 || strcmp(name, "B.CrtYd") == 0);
}
static bool property_has_visible_fab_text(const IRNode *property) {
  const char *layer = field_text(property, "layer");
  if (!layer) return false;
  if (strcmp(layer, "F.SilkS") && strcmp(layer, "B.SilkS") &&
      strcmp(layer, "F.Fab") && strcmp(layer, "B.Fab")) return false;
  const IRNode *effects = field(property, "effects");
  return !field_flag(effects, "hide");
}
static size_t count_pts(const IRNode *pts) {
  size_t c = 0;
  for (const IRNode *it = pts ? pts->first_child : NULL; it; it = it->next)
    if (is_list(it, "xy")) ++c;
  return c;
}
static size_t count_court_points(const IRNode *fp) {
  size_t c = 0;
  for (const IRNode *it = fp ? fp->first_child : NULL; it; it = it->next) {
    if (!layer_is_courtyard(it)) continue;
    if (is_list(it, "fp_line")) c += 2;
    else if (is_list(it, "fp_rect")) c += 4;
    else if (is_list(it, "fp_circle")) c += 2;
    else if (is_list(it, "fp_arc")) c += 3;
    else if (is_list(it, "fp_poly")) c += count_pts(field(it, "pts"));
  }
  return c;
}
static size_t count_zone_points(const IRNode *z) {
  const IRNode *poly = field(z, "polygon");
  return count_pts(field(poly, "pts"));
}
static size_t count_edge_points(const IRNode *n) {
  if (is_list(n, "gr_poly")) return count_pts(field(n, "pts"));
  return 0;
}
static bool list_atom_is(const IRNode *n, size_t index, const char *value) {
  const char *t = text_at(n, index);
  return t && strcmp(t, value) == 0;
}
static bool get_net_id(const IRNode *n, uint32_t *out) {
  int64_t v;
  if (!atom_i64(ir_child_at(n, 1), &v) || v < 0 || v > UINT32_MAX) return false;
  *out = (uint32_t)v;
  return true;
}
static BoardLayerType parse_layer_type(const char *s) {
  if (!s) return BOARD_LAYER_TYPE_UNKNOWN;
  if (!strcmp(s, "signal")) return BOARD_LAYER_TYPE_SIGNAL;
  if (!strcmp(s, "power")) return BOARD_LAYER_TYPE_POWER;
  if (!strcmp(s, "mixed")) return BOARD_LAYER_TYPE_MIXED;
  if (!strcmp(s, "jumper")) return BOARD_LAYER_TYPE_JUMPER;
  if (!strcmp(s, "user")) return BOARD_LAYER_TYPE_USER;
  return BOARD_LAYER_TYPE_UNKNOWN;
}
static BoardLayerMask mask_for_pattern(const char *s, const Board *b) {
  BoardLayerMask mask = 0;
  if (!s) return 0;
  if (!strcmp(s, "*.Cu")) {
    if (b && b->layer_count) {
      for (size_t i = 0; i < b->layer_count; ++i)
        if (board_layer_is_copper(b->layers[i].id)) mask |= BOARD_LAYER_BIT(b->layers[i].id);
    } else {
      for (unsigned i = 0; i < BOARD_LAYER_COUNT; ++i)
        if (board_layer_is_copper((BoardLayer)i)) mask |= BOARD_LAYER_BIT(i);
    }
    return mask;
  }
  if (!strcmp(s, "*.Mask")) return BOARD_LAYER_BIT(BOARD_LAYER_F_MASK) | BOARD_LAYER_BIT(BOARD_LAYER_B_MASK);
  if (!strcmp(s, "*.Paste")) return BOARD_LAYER_BIT(BOARD_LAYER_F_PASTE) | BOARD_LAYER_BIT(BOARD_LAYER_B_PASTE);
  if (!strcmp(s, "*.SilkS")) return BOARD_LAYER_BIT(BOARD_LAYER_F_SILK) | BOARD_LAYER_BIT(BOARD_LAYER_B_SILK);
  if (!strcmp(s, "*.Adhes")) return BOARD_LAYER_BIT(BOARD_LAYER_F_ADHES) | BOARD_LAYER_BIT(BOARD_LAYER_B_ADHES);
  if (!strcmp(s, "*.CrtYd")) return BOARD_LAYER_BIT(BOARD_LAYER_F_CRTYD) | BOARD_LAYER_BIT(BOARD_LAYER_B_CRTYD);
  if (!strcmp(s, "*.Fab")) return BOARD_LAYER_BIT(BOARD_LAYER_F_FAB) | BOARD_LAYER_BIT(BOARD_LAYER_B_FAB);
  BoardLayer layer;
  if (board_layer_from_name(s, &layer) == BOARD_OK) {
    if (b && b->layer_count) {
      bool present = false;
      for (size_t i = 0; i < b->layer_count; ++i) if (b->layers[i].id == layer) present = true;
      if (!present) return 0;
    }
    mask |= BOARD_LAYER_BIT(layer);
  }
  return mask;
}
static BoardLayerMask layer_mask(const IRNode *node, const Board *b,
                                 BoardDiag *diag) {
  BoardLayerMask mask = 0;
  for (size_t i = 1; node && i < ir_list_length(node); ++i) {
    const char *s = text_at(node, i);
    if (!s) continue;
    BoardLayerMask add = mask_for_pattern(s, b);
    if (!add && diag) {
      ++diag->unsupported_layers;
      diag_skip(diag, node, true);
    }
    mask |= add;
  }
  return mask;
}
static BoardLayer first_layer(BoardLayerMask mask) {
  for (unsigned i = 0; i < BOARD_LAYER_COUNT; ++i)
    if (mask & BOARD_LAYER_BIT(i)) return (BoardLayer)i;
  return BOARD_LAYER_F_CU;
}
static bool get_point_at(const IRNode *n, size_t index, G2Point *p) {
  return node_nm(n, index, &p->x) && node_nm(n, index + 1, &p->y);
}
static bool checked_add(G2Nm a, G2Nm b, G2Nm *out) {
  if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) return false;
  *out = a + b; return true;
}
static bool transform_local(G2Point local, G2Point origin, int32_t rot_mdeg,
                            bool on_back, G2Point *out, bool *exact) {
  if (on_back) {
    if (local.y == INT64_MIN) return false;
    local.y = -local.y;
  }
  G2Point rotated;
  bool rotation_exact = true;
  if (board_rotate(local, rot_mdeg, &rotated, &rotation_exact) != BOARD_OK)
    return false;
  if (!checked_add(origin.x, rotated.x, &out->x) ||
      !checked_add(origin.y, rotated.y, &out->y)) return false;
  if (exact && !rotation_exact) *exact = false;
  return true;
}
static bool parse_at(const IRNode *n, G2Point *p, int32_t *angle) {
  const IRNode *at = field(n, "at");
  if (!at || !get_point_at(at, 1, p)) return false;
  *angle = 0;
  if (ir_list_length(at) > 3 && !node_angle(at, 3, angle)) return false;
  return true;
}
static G2Nm field_nm_or(const IRNode *n, const char *name, G2Nm def) {
  const IRNode *f = field(n, name);
  G2Nm v;
  return f && atom_nm(ir_child_at(f, 1), &v) ? v : def;
}
static bool parse_drill(const IRNode *pad, BoardPad *out) {
  const IRNode *drill = field(pad, "drill");
  if (!drill) { out->drill = (G2Point){0, 0}; out->drill_offset = (G2Point){0, 0}; return true; }
  G2Nm dims[2] = {0, 0}; size_t got = 0;
  for (size_t i = 1; i < ir_list_length(drill) && got < 2; ++i) {
    const IRNode *v = ir_child_at(drill, i);
    G2Nm nm;
    if (v && v->kind == IR_NODE_ATOM && atom_nm(v, &nm)) dims[got++] = nm;
  }
  if (got == 0) return false;
  if (got == 1) dims[1] = dims[0];
  out->drill = (G2Point){dims[0], dims[1]};
  out->drill_offset = (G2Point){0, 0};
  G2Point offset;
  if (point_field(drill, "offset", &offset)) out->drill_offset = offset;
  return true;
}
static BoardPadType parse_pad_type(const char *s) {
  if (s && !strcmp(s, "thru_hole")) return BOARD_PAD_THRU;
  if (s && !strcmp(s, "np_thru_hole")) return BOARD_PAD_NPTH;
  if (s && !strcmp(s, "connect")) return BOARD_PAD_CONNECT;
  return BOARD_PAD_SMD;
}
static BoardPadShape parse_pad_shape(const char *s) {
  if (s && !strcmp(s, "circle")) return BOARD_PAD_CIRCLE;
  if (s && !strcmp(s, "oval")) return BOARD_PAD_OVAL;
  if (s && !strcmp(s, "roundrect")) return BOARD_PAD_ROUNDRECT;
  if (s && !strcmp(s, "trapezoid")) return BOARD_PAD_TRAPEZOID;
  if (s && (!strcmp(s, "custom") || !strcmp(s, "chamfered_rect"))) return BOARD_PAD_CUSTOM;
  return BOARD_PAD_RECT;
}
static bool parse_pad(const IRNode *node, Board *b, BoardFootprint *fp,
                      uint32_t fp_index, BoardPad *out, BoardDiag *diag) {
  memset(out, 0, sizeof(*out));
  out->footprint = fp_index;
  out->number = text_at(node, 1);
  out->type = parse_pad_type(text_at(node, 2));
  const char *shape_name = text_at(node, 3);
  out->shape = parse_pad_shape(shape_name);
  if (!shape_name || (strcmp(shape_name, "circle") && strcmp(shape_name, "rect") &&
      strcmp(shape_name, "oval") && strcmp(shape_name, "roundrect") &&
      strcmp(shape_name, "trapezoid") && strcmp(shape_name, "custom") &&
      strcmp(shape_name, "chamfered_rect"))) {
    out->shape = BOARD_PAD_CUSTOM;
    if (diag) diag_skip(diag, node, true);
  }
  out->clearance = field_nm_or(node, "clearance", BOARD_UNSET_NM);
  out->mask_margin = field_nm_or(node, "solder_mask_margin", BOARD_UNSET_NM);
  out->paste_margin = field_nm_or(node, "solder_paste_margin", BOARD_UNSET_NM);
  out->roundrect_ratio_ppm = 0;
  out->rotation_exact = true;
  out->raw_node = node;
  G2Point local; int32_t local_angle;
  if (!parse_at(node, &local, &local_angle)) return false;
  const IRNode *size = field(node, "size");
  if (!size || !node_nm(size, 1, &out->size.x) || !node_nm(size, 2, &out->size.y)) return false;
  if (!parse_drill(node, out)) return false;
  int64_t ratio_ppm;
  const IRNode *rr = field(node, "roundrect_rratio");
  if (rr && atom_to_scaled_i64(ir_child_at(rr, 1), 1000000u, &ratio_ppm) &&
      ratio_ppm >= 0 && ratio_ppm <= 1000000)
    out->roundrect_ratio_ppm = (G2Nm)ratio_ppm;
  const IRNode *net = field(node, "net");
  if (net && !get_net_id(net, &out->net)) return false;
  const IRNode *layers = field(node, "layers");
  out->layers = layer_mask(layers, b, diag);
  bool exact = true;
  if (!transform_local(local, fp->pos, fp->rot_mdeg, fp->on_back, &out->pos, &exact)) return false;
  out->rotation_exact = exact;
  if (!exact && diag) ++diag->inexact_rotations;
  int32_t angle = fp->rot_mdeg + (fp->on_back ? -local_angle : local_angle);
  angle %= 360000; if (angle < 0) angle += 360000;
  out->rot_mdeg = angle;
  G2Point offset_abs;
  exact = true;
  if (!transform_local(out->drill_offset, (G2Point){0,0}, fp->rot_mdeg, fp->on_back,
                       &offset_abs, &exact)) return false;
  out->drill_offset = offset_abs;
  if (out->shape == BOARD_PAD_CUSTOM && diag) diag_skip(diag, node, true);
  out->locked = field_flag(node, "locked");
  return true;
}
static bool parse_layer_list_record(const IRNode *layers, Board *b, BoardDiag *diag) {
  size_t n = 0;
  for (const IRNode *it = layers ? layers->first_child : NULL; it; it = it->next)
    if (it->kind == IR_NODE_LIST) ++n;
  b->layers = n ? (BoardLayerInfo *)arena_alloc_array(b->arena, sizeof(BoardLayerInfo), n) : NULL;
  if (n && !b->layers) return false;
  for (const IRNode *it = layers ? layers->first_child : NULL; it; it = it->next) {
    if (it->kind != IR_NODE_LIST) continue;
    int64_t ordinal;
    const char *name = text_at(it, 1), *type = text_at(it, 2), *user = text_at(it, 3);
    BoardLayer layer;
    if (!atom_i64(ir_child_at(it, 0), &ordinal) || ordinal < 0 || ordinal > UINT32_MAX ||
        !name || board_layer_from_name(name, &layer) != BOARD_OK) {
      if (diag) { ++diag->unsupported_layers; diag_skip(diag, it, true); }
      continue;
    }
    b->layers[b->layer_count++] = (BoardLayerInfo){(uint32_t)ordinal, layer,
                                                    parse_layer_type(type), user};
  }
  return true;
}
static void parse_setup(Board *b, const IRNode *setup) {
  if (!setup) return;
  b->rules.mask_margin = field_nm_or(setup, "pad_to_mask_clearance", BOARD_UNSET_NM);
  b->rules.paste_margin = field_nm_or(setup, "pad_to_paste_clearance", BOARD_UNSET_NM);
  if (b->rules.min_track_width == BOARD_UNSET_NM)
    b->rules.min_track_width = field_nm_or(setup, "trace_min", BOARD_UNSET_NM);
  if (b->rules.min_clearance == BOARD_UNSET_NM)
    b->rules.min_clearance = field_nm_or(setup, "trace_clearance", BOARD_UNSET_NM);
  b->rules.min_via_size = field_nm_or(setup, "via_min_size", field_nm_or(setup, "via_size", BOARD_UNSET_NM));
  b->rules.min_via_drill = field_nm_or(setup, "via_min_drill", field_nm_or(setup, "via_drill", BOARD_UNSET_NM));
  b->rules.edge_clearance = field_nm_or(setup, "edge_clearance", BOARD_UNSET_NM);
}
static bool parse_edge(const IRNode *node, Board *b, size_t *poly_cursor,
                       BoardEdge *out) {
  memset(out, 0, sizeof(*out)); out->raw_node = node;
  const char *name = text_at(node, 0);
  out->width = field_nm_or(node, "width", BOARD_UNSET_NM);
  if (out->width == BOARD_UNSET_NM) {
    const IRNode *stroke = field(node, "stroke");
    out->width = field_nm_or(stroke, "width", 0);
  }
  if (!strcmp(name, "gr_line")) {
    out->kind = BOARD_EDGE_LINE;
    return point_field(node, "start", &out->p0) && point_field(node, "end", &out->p1);
  }
  if (!strcmp(name, "gr_arc")) {
    out->kind = BOARD_EDGE_ARC;
    return point_field(node, "start", &out->p0) && point_field(node, "mid", &out->mid) &&
           point_field(node, "end", &out->p1);
  }
  if (!strcmp(name, "gr_circle")) {
    out->kind = BOARD_EDGE_CIRCLE;
    return point_field(node, "center", &out->p0) && point_field(node, "end", &out->p1);
  }
  if (!strcmp(name, "gr_rect")) {
    out->kind = BOARD_EDGE_RECT;
    return point_field(node, "start", &out->p0) && point_field(node, "end", &out->p1);
  }
  if (!strcmp(name, "gr_poly")) {
    const IRNode *pts = field(node, "pts");
    out->kind = BOARD_EDGE_POLYGON;
    out->first_pt = (uint32_t)*poly_cursor;
    out->pt_count = (uint32_t)count_pts(pts);
    for (const IRNode *it = pts ? pts->first_child : NULL; it; it = it->next) {
      if (!is_list(it, "xy")) continue;
      G2Point p;
      if (!node_nm(it, 1, &p.x) || !node_nm(it, 2, &p.y)) return false;
      b->edge_pts[(*poly_cursor)++] = p;
    }
    return out->pt_count >= 3;
  }
  return false;
}
static bool parse_courtyard(const IRNode *node, BoardFootprint *fp, Board *b,
                            size_t *cursor, BoardDiag *diag) {
  const char *kind = text_at(node, 0);
  const IRNode *layer = field(node, "layer");
  const char *layer_name = text_at(layer, 1);
  bool back = layer_name && !strcmp(layer_name, "B.CrtYd");
  G2Point local[4]; size_t n = 0;
  if (!strcmp(kind, "fp_line") || !strcmp(kind, "fp_rect")) {
    if (!point_field(node, "start", &local[0]) || !point_field(node, "end", &local[1])) return false;
    n = 2;
    if (!strcmp(kind, "fp_rect")) {
      G2Point a = local[0], c = local[1];
      local[0] = a; local[1] = (G2Point){c.x, a.y};
      local[2] = c; local[3] = (G2Point){a.x, c.y}; n = 4;
    }
  } else if (!strcmp(kind, "fp_circle")) {
    if (!point_field(node, "center", &local[0]) || !point_field(node, "end", &local[1])) return false;
    n = 2;
  } else if (!strcmp(kind, "fp_arc")) {
    if (!point_field(node, "start", &local[0]) || !point_field(node, "mid", &local[1]) ||
        !point_field(node, "end", &local[2])) return false;
    n = 3;
  } else if (!strcmp(kind, "fp_poly")) {
    const IRNode *pts = field(node, "pts");
    for (const IRNode *it = pts ? pts->first_child : NULL; it; it = it->next) {
      if (!is_list(it, "xy")) continue;
      G2Point q, p; bool exact = true;
      if (!node_nm(it, 1, &q.x) || !node_nm(it, 2, &q.y) ||
          !transform_local(q, fp->pos, fp->rot_mdeg, fp->on_back, &p, &exact)) return false;
      if (!exact) {
        fp->courtyard_exact = false;
        if (diag) ++diag->inexact_rotations;
      }
      b->court_pts[(*cursor)++] = p;
    }
    (void)back;
    return true;
  } else return false;
  for (size_t i = 0; i < n; ++i) {
    G2Point p; bool exact = true;
    if (!transform_local(local[i], fp->pos, fp->rot_mdeg, fp->on_back, &p, &exact)) return false;
    if (!exact) {
      fp->courtyard_exact = false;
      if (diag) ++diag->inexact_rotations;
    }
    (void)back;
    b->court_pts[(*cursor)++] = p;
  }
  return true;
}
static bool parse_footprint(const IRNode *node, Board *b, size_t fp_index,
                            size_t *pad_cursor, size_t *court_cursor,
                            BoardDiag *diag) {
  BoardFootprint *fp = &b->footprints[fp_index];
  memset(fp, 0, sizeof(*fp));
  fp->raw_node = node;
  fp->reference = NULL; fp->value = NULL; fp->lib_id = text_at(node, 1);
  fp->uuid = field_text(node, "uuid"); fp->path = field_text(node, "path");
  if (!parse_at(node, &fp->pos, &fp->rot_mdeg)) {
    fp->pos = (G2Point){0,0}; fp->rot_mdeg = 0;
  }
  const char *layer_name = field_text(node, "layer");
  fp->layer = BOARD_LAYER_F_CU;
  if (layer_name && board_layer_from_name(layer_name, &fp->layer) != BOARD_OK) {
    if (diag) { ++diag->unsupported_layers; diag_skip(diag, node, true); }
  }
  fp->on_back = fp->layer == BOARD_LAYER_B_CU;
  fp->rotation_exact = (fp->rot_mdeg % 90000) == 0;
  fp->locked = field_flag(node, "locked");
  fp->placed = field_flag(node, "placed");
  const IRNode *attr = field(node, "attr");
  if (attr) {
    for (size_t i = 1; i < ir_list_length(attr); ++i) {
      const char *s = text_at(attr, i);
      if (s && !strcmp(s, "exclude_from_bom")) fp->exclude_from_bom = true;
      if (s && !strcmp(s, "exclude_from_pos_files")) fp->exclude_from_pos = true;
      if (s && !strcmp(s, "dnp")) fp->dnp = true;
    }
  }
  for (const IRNode *p = node->first_child; p; p = p->next) {
    if (!is_list(p, "property")) continue;
    if (property_has_visible_fab_text(p)) diag_skip(diag, p, true);
    const char *k = text_at(p, 1), *v = text_at(p, 2);
    if (!k || !v) continue;
    if (!strcmp(k, "Reference")) fp->reference = v;
    else if (!strcmp(k, "Value")) fp->value = v;
    else if (!strcmp(k, "Copper Part ID") || !strcmp(k, "Part ID")) fp->part_id = v;
    else if (!strcmp(k, "Manufacturer")) fp->manufacturer = v;
    else if (!strcmp(k, "MPN") || !strcmp(k, "Manufacturer Part Number")) fp->mpn = v;
    else if (!strcmp(k, "LCSC") || !strcmp(k, "LCSC Part") || !strcmp(k, "LCSC Part Number")) fp->lcsc_id = v;
  }
  for (const IRNode *p = node->first_child; p; p = p->next) {
    if (!is_list(p, "fp_text")) continue;
    const char *kind = text_at(p, 1), *v = text_at(p, 2);
    if (!kind || !v) continue;
    if (!strcmp(kind, "reference") && !fp->reference) fp->reference = v;
    if (!strcmp(kind, "value") && !fp->value) fp->value = v;
  }
  fp->first_pad = (uint32_t)*pad_cursor;
  fp->pad_count = (uint32_t)count_fp_pads(node);
  for (const IRNode *p = node->first_child; p; p = p->next) {
    if (!is_list(p, "pad")) continue;
    if (!parse_pad(p, b, fp, (uint32_t)fp_index, &b->pads[(*pad_cursor)++], diag))
      return false;
  }
  fp->first_court = (uint32_t)*court_cursor;
  fp->court_count = (uint32_t)count_court_points(node);
  fp->courtyard_exact = fp->court_count >= 3 && fp->rotation_exact;
  for (const IRNode *g = node->first_child; g; g = g->next) {
    if (!layer_is_courtyard(g)) continue;
    const char *kind = text_at(g, 0);
    if (kind && (!strcmp(kind, "fp_line") || !strcmp(kind, "fp_rect") ||
                 !strcmp(kind, "fp_circle") || !strcmp(kind, "fp_arc") ||
                 !strcmp(kind, "fp_poly"))) {
      if (kind && (!strcmp(kind, "fp_circle") || !strcmp(kind, "fp_arc")))
        fp->courtyard_exact = false;
      if (!parse_courtyard(g, fp, b, court_cursor, diag)) return false;
    } else if (kind && strncmp(kind, "fp_", 3) == 0) diag_skip(diag, g, true);
  }
  /* Count all footprint children not projected into the model. The original
     IR is retained; these counters stop downstream checks claiming full coverage. */
  for (const IRNode *g = node->first_child; g; g = g->next) {
    if (is_list(g, "pad") || is_list(g, "property") || is_list(g, "uuid") ||
        is_list(g, "path") || is_list(g, "at") || is_list(g, "layer") ||
        is_list(g, "attr") || is_list(g, "tstamp") || is_list(g, "descr") ||
        is_list(g, "tags") || is_list(g, "solder_mask_margin") ||
        is_list(g, "solder_paste_margin") || is_list(g, "solder_paste_ratio") ||
        is_list(g, "clearance") || is_list(g, "zone_connect") || is_list(g, "thermal_width") ||
        is_list(g, "thermal_gap") || is_list(g, "model")) continue;
    if (is_list(g, "fp_text") || is_list(g, "fp_text_box")) {
      diag_skip(diag, g, true);
      continue;
    }
    if (is_list(g, "fp_line") || is_list(g, "fp_rect") || is_list(g, "fp_circle") ||
        is_list(g, "fp_arc") || is_list(g, "fp_poly")) {
      if (!layer_is_courtyard(g)) diag_skip(diag, g, true);
      continue;
    }
    if (g->kind == IR_NODE_LIST) diag_skip(diag, g, true);
  }
  return true;
}
static bool parse_track(const IRNode *node, BoardTrack *out) {
  memset(out, 0, sizeof(*out));
  const IRNode *width = field(node, "width");
  return point_field(node, "start", &out->a) && point_field(node, "end", &out->b) &&
         width && atom_nm(ir_child_at(width, 1), &out->width);
}
static bool parse_arc_track(const IRNode *node, BoardArcTrack *out) {
  memset(out, 0, sizeof(*out));
  const IRNode *width = field(node, "width");
  return point_field(node, "start", &out->start) && point_field(node, "mid", &out->mid) &&
         point_field(node, "end", &out->end) && width &&
         atom_nm(ir_child_at(width, 1), &out->width);
}
static bool parse_via(const IRNode *node, BoardVia *out, Board *b, BoardDiag *diag) {
  memset(out, 0, sizeof(*out));
  if (!point_field(node, "at", &out->pos)) return false;
  out->size = field_nm_or(node, "size", 0);
  out->drill = field_nm_or(node, "drill", 0);
  const IRNode *layers = field(node, "layers");
  BoardLayerMask mask = layer_mask(layers, b, diag);
  out->from = first_layer(mask); out->to = out->from;
  for (unsigned i = 0; i < BOARD_LAYER_COUNT; ++i)
    if (mask & BOARD_LAYER_BIT(i)) out->to = (BoardLayer)i;
  const char *type = text_at(node, 1);
  if (type && !strcmp(type, "blind")) out->type = BOARD_VIA_BLIND_BURIED;
  else if (type && !strcmp(type, "micro")) out->type = BOARD_VIA_MICRO;
  else if (!type || !strcmp(type, "through")) out->type = BOARD_VIA_THROUGH;
  else out->type = BOARD_VIA_UNKNOWN;
  out->plated = true;
  out->locked = field_flag(node, "locked");
  out->uuid = field_text(node, "tstamp");
  for (const IRNode *it = node->first_child ? node->first_child->next : NULL; it; it = it->next) {
    if (it->kind != IR_NODE_LIST) continue;
    const char *name = text_at(it, 0);
    if (name && (!strcmp(name, "at") || !strcmp(name, "size") || !strcmp(name, "drill") ||
        !strcmp(name, "layers") || !strcmp(name, "net") || !strcmp(name, "tstamp") ||
        !strcmp(name, "locked"))) continue;
    diag_skip(diag, it, true);
  }
  const IRNode *net = field(node, "net");
  return !net || get_net_id(net, &out->net);
}
static bool parse_zone(const IRNode *node, Board *b, size_t *pt_cursor,
                       BoardZone *out, BoardDiag *diag) {
  memset(out, 0, sizeof(*out));
  out->raw_node = node;
  out->clearance = field_nm_or(node, "clearance", BOARD_UNSET_NM);
  out->min_thickness = field_nm_or(node, "min_thickness", BOARD_UNSET_NM);
  const IRNode *net = field(node, "net");
  if (net && !get_net_id(net, &out->net)) return false;
  BoardLayerMask lm = layer_mask(field(node, "layers"), b, diag);
  if (!lm) {
    const char *ln = field_text(node, "layer");
    BoardLayer id;
    if (ln && board_layer_from_name(ln, &id) == BOARD_OK) lm = BOARD_LAYER_BIT(id);
  }
  out->layers = lm; out->layer = first_layer(lm);
  out->locked = field_flag(node, "locked");
  out->first_pt = (uint32_t)*pt_cursor;
  const IRNode *pts = field(field(node, "polygon"), "pts");
  out->pt_count = (uint32_t)count_pts(pts);
  for (const IRNode *it = pts ? pts->first_child : NULL; it; it = it->next) {
    if (!is_list(it, "xy")) continue;
    G2Point p;
    if (!node_nm(it, 1, &p.x) || !node_nm(it, 2, &p.y)) return false;
    b->zone_pts[(*pt_cursor)++] = p;
  }
  const IRNode *keepout = field(node, "keepout");
  if (keepout) {
    out->is_keepout = true;
    out->no_tracks = list_atom_is(field(keepout, "tracks"), 1, "not_allowed");
    out->no_vias = list_atom_is(field(keepout, "vias"), 1, "not_allowed");
    out->no_pads = list_atom_is(field(keepout, "pads"), 1, "not_allowed");
    out->no_copper = list_atom_is(field(keepout, "copperpour"), 1, "not_allowed");
    out->no_footprints = list_atom_is(field(keepout, "footprints"), 1, "not_allowed");
  }
  if (out->pt_count < 3 || !out->is_keepout) diag_skip(diag, node, true);
  return true;
}
static bool top_level_known(const char *s) {
  static const char *const names[] = {
    "version", "generator", "uuid", "general", "paper", "page", "layers",
    "setup", "net", "footprint", "segment", "arc", "via", "zone",
    "gr_line", "gr_arc", "gr_circle", "gr_rect", "gr_poly", "gr_text",
    "gr_text_box", "gr_curve", "dimension", "target", "image", "group",
    "embedded_fonts", "property", "title_block", "pcbplotparams"
  };
  for (size_t i = 0; i < sizeof(names)/sizeof(names[0]); ++i)
    if (s && !strcmp(s, names[i])) return true;
  return false;
}
static bool is_edge_record(const IRNode *n) {
  const char *layer = field_text(n, "layer");
  return layer && !strcmp(layer, "Edge.Cuts") &&
         (is_list(n, "gr_line") || is_list(n, "gr_arc") ||
          is_list(n, "gr_circle") || is_list(n, "gr_rect") || is_list(n, "gr_poly"));
}

BoardStatus board_load_kicad(const char *path, Arena *arena, Board *out,
                             BoardDiag *diag) {
  if (!path || !arena || !out) return diag_error(diag, BOARD_ERR_ARGUMENT, "invalid argument");
  if (diag) memset(diag, 0, sizeof(*diag));
  BoardStatus st = board_init(out, arena);
  if (st != BOARD_OK) return diag_error(diag, st, "board initialization failed");
  IRError error; ir_error_clear(&error);
  IRNode *root = ir_parse_file(arena, path, &error);
  if (!root) return diag_error(diag, BOARD_ERR_PARSE, error.message ? error.message : "KiCad parse failed");
  if (!is_list(root, "kicad_pcb")) return diag_error(diag, BOARD_ERR_PARSE, "root is not kicad_pcb");
  out->source_root = root;
  uint64_t version = 0;
  const IRNode *ver = field(root, "version");
  if (ver && !atom_u64(ir_child_at(ver, 1), &version))
    return diag_error(diag, BOARD_ERR_PARSE, "invalid KiCad file version");
  out->kicad_file_version = version > UINT32_MAX ? 0 : (uint32_t)version;

  const IRNode *layers = field(root, "layers");
  if (!parse_layer_list_record(layers, out, diag)) return diag_error(diag, BOARD_ERR_MEMORY, "layer table allocation failed");
  size_t copper_count = count_copper_layers(layers);
  if (copper_count > 0 && copper_count <= UINT32_MAX) out->rules.copper_layers = (uint32_t)copper_count;
  const IRNode *general = field(root, "general");
  if (general) {
    out->rules.board_thickness = field_nm_or(general, "thickness", BOARD_UNSET_NM);
    for (const IRNode *g = general->first_child ? general->first_child->next : NULL; g; g = g->next)
      if (g->kind == IR_NODE_LIST && !is_list(g, "thickness")) diag_skip(diag, g, true);
  }
  const IRNode *setup = field(root, "setup");
  parse_setup(out, setup);
  if (setup) {
    static const char *const setup_fields[] = {"trace_min", "trace_clearance", "via_min_size", "via_size",
      "via_min_drill", "via_drill", "edge_clearance", "pad_to_mask_clearance", "pad_to_paste_clearance"};
    for (const IRNode *g = setup->first_child ? setup->first_child->next : NULL; g; g = g->next) {
      if (g->kind != IR_NODE_LIST) continue;
      const char *name = text_at(g, 0); bool known = false;
      for (size_t k = 0; name && k < sizeof(setup_fields)/sizeof(setup_fields[0]); ++k)
        if (!strcmp(name, setup_fields[k])) { known = true; break; }
      if (!known) diag_skip(diag, g, true);
    }
  }

  size_t nets = 0, fps = 0, pads = 0, tracks = 0, arcs = 0, vias = 0, zones = 0, edges = 0;
  size_t zone_pts = 0, edge_pts = 0, court_pts = 0;
  for (const IRNode *it = root->first_child ? root->first_child->next : NULL; it; it = it->next) {
    if (is_list(it, "net")) ++nets;
    else if (is_list(it, "footprint")) { ++fps; pads += count_fp_pads(it); court_pts += count_court_points(it); }
    else if (is_list(it, "segment")) ++tracks;
    else if (is_list(it, "arc")) ++arcs;
    else if (is_list(it, "via")) ++vias;
    else if (is_list(it, "zone")) { ++zones; zone_pts += count_zone_points(it); }
    else if (is_edge_record(it)) { ++edges; edge_pts += count_edge_points(it); }
    else if (!top_level_known(text_at(it, 0))) diag_skip(diag, it, true);
    else if ((is_list(it, "gr_line") || is_list(it, "gr_arc") || is_list(it, "gr_circle") ||
              is_list(it, "gr_rect") || is_list(it, "gr_poly") || is_list(it, "gr_text") ||
              is_list(it, "gr_text_box") || is_list(it, "gr_curve") || is_list(it, "dimension") ||
              is_list(it, "image") || is_list(it, "group") || is_list(it, "target")))
      diag_skip(diag, it, true);
    else if (is_list(it, "property") || is_list(it, "title_block") ||
             is_list(it, "pcbplotparams"))
      diag_skip(diag, it, false);
    else if (is_list(it, "version") || is_list(it, "generator") || is_list(it, "uuid") ||
             is_list(it, "paper") || is_list(it, "page") || is_list(it, "embedded_fonts"))
      diag_skip(diag, it, false);
  }
  out->net_count = nets; out->footprint_count = fps; out->pad_count = pads;
  out->track_count = tracks; out->arc_track_count = arcs; out->via_count = vias;
  out->zone_count = zones; out->edge_count = edges;
  out->zone_pt_count = zone_pts; out->edge_pt_count = edge_pts; out->court_pt_count = court_pts;
#define ALLOC_FIELD(field_name, count_name, type) do { \
  out->field_name = (count_name) ? (type *)arena_alloc_array(arena, sizeof(type), (count_name)) : NULL; \
  if ((count_name) && !out->field_name) return diag_error(diag, BOARD_ERR_MEMORY, "board array allocation failed"); \
} while (0)
  ALLOC_FIELD(nets, nets, BoardNet);
  ALLOC_FIELD(footprints, fps, BoardFootprint);
  ALLOC_FIELD(pads, pads, BoardPad);
  ALLOC_FIELD(tracks, tracks, BoardTrack);
  ALLOC_FIELD(arc_tracks, arcs, BoardArcTrack);
  ALLOC_FIELD(vias, vias, BoardVia);
  ALLOC_FIELD(zones, zones, BoardZone);
  ALLOC_FIELD(edges, edges, BoardEdge);
  ALLOC_FIELD(zone_pts, zone_pts, G2Point);
  ALLOC_FIELD(edge_pts, edge_pts, G2Point);
  ALLOC_FIELD(court_pts, court_pts, G2Point);
#undef ALLOC_FIELD

  size_t ni = 0, fi = 0, pi = 0, ti = 0, ai = 0, vi = 0, zi = 0, ei = 0;
  size_t zpc = 0, epc = 0, cpc = 0;
  for (const IRNode *it = root->first_child; it; it = it->next) {
    if (is_list(it, "net")) {
      int64_t id;
      const char *name = text_at(it, 2);
      if (!atom_i64(ir_child_at(it, 1), &id) || id < 0 || id > UINT32_MAX || !name)
        return diag_error(diag, BOARD_ERR_PARSE, "invalid net record");
      out->nets[ni++] = (BoardNet){(uint32_t)id, name};
    } else if (is_list(it, "footprint")) {
      if (!parse_footprint(it, out, fi++, &pi, &cpc, diag))
        return diag_error(diag, BOARD_ERR_PARSE, "invalid footprint or pad geometry");
    } else if (is_list(it, "segment")) {
      BoardTrack *t = &out->tracks[ti++];
      if (!parse_track(it, t)) return diag_error(diag, BOARD_ERR_PARSE, "invalid segment record");
      BoardLayer l;
      if (board_layer_from_name(field_text(it, "layer"), &l) != BOARD_OK) {
        diag_skip(diag, it, true); l = BOARD_LAYER_F_CU;
      }
      t->layer = l; t->locked = field_flag(it, "locked"); t->uuid = field_text(it, "tstamp");
      const IRNode *net = field(it, "net"); if (net && !get_net_id(net, &t->net)) return diag_error(diag, BOARD_ERR_PARSE, "invalid track net");
    } else if (is_list(it, "arc")) {
      BoardArcTrack *a = &out->arc_tracks[ai++];
      if (!parse_arc_track(it, a)) return diag_error(diag, BOARD_ERR_PARSE, "invalid arc track record");
      BoardLayer l;
      if (board_layer_from_name(field_text(it, "layer"), &l) != BOARD_OK) { diag_skip(diag, it, true); l = BOARD_LAYER_F_CU; }
      a->layer = l; a->locked = field_flag(it, "locked"); a->uuid = field_text(it, "tstamp");
      const IRNode *net = field(it, "net"); if (net && !get_net_id(net, &a->net)) return diag_error(diag, BOARD_ERR_PARSE, "invalid arc net");
    } else if (is_list(it, "via")) {
      if (!parse_via(it, &out->vias[vi++], out, diag)) return diag_error(diag, BOARD_ERR_PARSE, "invalid via record");
    } else if (is_list(it, "zone")) {
      if (!parse_zone(it, out, &zpc, &out->zones[zi++], diag)) return diag_error(diag, BOARD_ERR_PARSE, "invalid zone record");
    } else if (is_edge_record(it)) {
      if (!parse_edge(it, out, &epc, &out->edges[ei++])) return diag_error(diag, BOARD_ERR_PARSE, "invalid Edge.Cuts record");
    }
  }
  if (board_validate(out, diag) != BOARD_OK) return diag ? diag->status : BOARD_ERR_INVALID;
  if (diag) { diag->status = BOARD_OK; diag->message[0] = '\0'; }
  return BOARD_OK;
}

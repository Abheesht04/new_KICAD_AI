#include "copper.h"

#include "nmath.h"

/*
 * copper.c - see copper.h for the model.
 *
 * Everything that decides pass/fail is integer arithmetic through the
 * geom_cmp_* functions. The only double in this file is the report-only gap.
 */

/* Largest requirement we accept. Keeps required + ra + rb far from int64 limits. */
#define COPPER_MAX_REQUIRED_NM (INT64_C(1) << 32)

/* ------------------------------------------------------------------------ *
 * Constructors
 * ------------------------------------------------------------------------ */

CopperShape copper_shape_point(Pt center, Nm radius) {
  CopperShape s = {COPPER_SHAPE_POINT, center, center, radius};
  return s;
}

CopperShape copper_shape_segment(Pt p0, Pt p1, Nm radius) {
  CopperShape s = {COPPER_SHAPE_SEGMENT, p0, p1, radius};
  return s;
}

CopperShape copper_shape_rect(Pt min_corner, Pt max_corner, Nm corner_radius) {
  CopperShape s;
  s.kind = COPPER_SHAPE_RECT;
  s.a.x = min_corner.x + corner_radius;
  s.a.y = min_corner.y + corner_radius;
  s.b.x = max_corner.x - corner_radius;
  s.b.y = max_corner.y - corner_radius;
  s.radius = corner_radius;
  return s;
}

/* ------------------------------------------------------------------------ *
 * Layers
 * ------------------------------------------------------------------------ */

bool copper_layer_mask_single(unsigned layer_index, CopperLayerMask *out_mask) {
  if (!out_mask || layer_index >= COPPER_MAX_LAYERS) return false;
  *out_mask = UINT64_C(1) << layer_index;
  return true;
}

bool copper_layer_mask_span(unsigned first_layer_index,
                            unsigned last_layer_index,
                            CopperLayerMask *out_mask) {
  if (!out_mask || first_layer_index >= COPPER_MAX_LAYERS ||
      last_layer_index >= COPPER_MAX_LAYERS) {
    return false;
  }
  if (first_layer_index > last_layer_index) {
    unsigned t = first_layer_index;
    first_layer_index = last_layer_index;
    last_layer_index = t;
  }
  CopperLayerMask mask = 0;
  for (unsigned i = first_layer_index; i <= last_layer_index; ++i) {
    mask |= UINT64_C(1) << i;
  }
  *out_mask = mask;
  return true;
}

bool copper_layer_masks_overlap(CopperLayerMask a, CopperLayerMask b) {
  return (a & b) != 0;
}

/* ------------------------------------------------------------------------ *
 * Validation and bounding box
 * ------------------------------------------------------------------------ */

CopperResult copper_shape_validate(const CopperShape *s) {
  if (!s) return COPPER_RESULT_INVALID_INPUT;
  if (s->radius < 0 || s->radius > GEOM_COORD_MAX_NM) {
    return COPPER_RESULT_INVALID_INPUT;
  }
  switch (s->kind) {
  case COPPER_SHAPE_POINT:
    return pt_in_bounds(s->a) ? COPPER_RESULT_PASS : COPPER_RESULT_INVALID_INPUT;
  case COPPER_SHAPE_SEGMENT:
    return (pt_in_bounds(s->a) && pt_in_bounds(s->b))
               ? COPPER_RESULT_PASS
               : COPPER_RESULT_INVALID_INPUT;
  case COPPER_SHAPE_RECT:
    return (pt_in_bounds(s->a) && pt_in_bounds(s->b) && s->a.x <= s->b.x &&
            s->a.y <= s->b.y)
               ? COPPER_RESULT_PASS
               : COPPER_RESULT_INVALID_INPUT;
  case COPPER_SHAPE_ARC:
  case COPPER_SHAPE_POLYGON:
    return COPPER_RESULT_UNVERIFIED;
  default:
    return COPPER_RESULT_INVALID_INPUT;
  }
}

bool copper_shape_bbox(const CopperShape *s, Pt *out_min, Pt *out_max) {
  if (!out_min || !out_max || copper_shape_validate(s) != COPPER_RESULT_PASS) {
    return false;
  }
  Pt lo = s->a, hi = s->a;
  if (s->kind != COPPER_SHAPE_POINT) {
    lo.x = (s->a.x < s->b.x) ? s->a.x : s->b.x;
    lo.y = (s->a.y < s->b.y) ? s->a.y : s->b.y;
    hi.x = (s->a.x > s->b.x) ? s->a.x : s->b.x;
    hi.y = (s->a.y > s->b.y) ? s->a.y : s->b.y;
  }
  out_min->x = lo.x - s->radius;
  out_min->y = lo.y - s->radius;
  out_max->x = hi.x + s->radius;
  out_max->y = hi.y + s->radius;
  return true;
}

/* ------------------------------------------------------------------------ *
 * Core distance dispatch: sign(distance(core_a, core_b) - T), exact.
 * The lower-numbered kind is always passed first, which leaves six cases.
 * ------------------------------------------------------------------------ */

static int core_cmp(const CopperShape *a, const CopperShape *b, Nm T) {
  if (a->kind > b->kind) {
    const CopperShape *t = a;
    a = b;
    b = t;
  }
  switch (a->kind) {
  case COPPER_SHAPE_POINT:
    if (b->kind == COPPER_SHAPE_POINT) return geom_cmp_pt_pt(a->a, b->a, T);
    if (b->kind == COPPER_SHAPE_SEGMENT)
      return geom_cmp_pt_seg(a->a, b->a, b->b, T);
    return geom_cmp_pt_rect(a->a, b->a, b->b, T); /* RECT */
  case COPPER_SHAPE_SEGMENT:
    if (b->kind == COPPER_SHAPE_SEGMENT)
      return geom_cmp_seg_seg(a->a, a->b, b->a, b->b, T);
    return geom_cmp_seg_rect(a->a, a->b, b->a, b->b, T); /* RECT */
  default: /* RECT, RECT */
    return geom_cmp_rect_rect(a->a, a->b, b->a, b->b, T);
  }
}

/* Report-only squared core distance. */
static double core_dist2(const CopperShape *a, const CopperShape *b) {
  if (a->kind > b->kind) {
    const CopperShape *t = a;
    a = b;
    b = t;
  }
  switch (a->kind) {
  case COPPER_SHAPE_POINT:
    if (b->kind == COPPER_SHAPE_POINT) return (double)pt_dist2(a->a, b->a);
    if (b->kind == COPPER_SHAPE_SEGMENT)
      return pt_seg_dist2(a->a, b->a, b->b);
    return geom_dist2_pt_rect(a->a, b->a, b->b);
  case COPPER_SHAPE_SEGMENT:
    if (b->kind == COPPER_SHAPE_SEGMENT)
      return seg_seg_dist2(a->a, a->b, b->a, b->b);
    return geom_dist2_seg_rect(a->a, a->b, b->a, b->b);
  default:
    return geom_dist2_rect_rect(a->a, a->b, b->a, b->b);
  }
}

/* ------------------------------------------------------------------------ *
 * Items
 * ------------------------------------------------------------------------ */

/* Shared front gate: input validity and support, ignoring layers and nets. */
static CopperResult item_gate(const CopperItem *a, const CopperItem *b) {
  if (!a || !b || a->layers == 0 || b->layers == 0) {
    return COPPER_RESULT_INVALID_INPUT;
  }
  CopperResult ra = copper_shape_validate(&a->shape);
  CopperResult rb = copper_shape_validate(&b->shape);
  if (ra == COPPER_RESULT_INVALID_INPUT || rb == COPPER_RESULT_INVALID_INPUT) {
    return COPPER_RESULT_INVALID_INPUT;
  }
  if (ra == COPPER_RESULT_UNVERIFIED || rb == COPPER_RESULT_UNVERIFIED) {
    return COPPER_RESULT_UNVERIFIED;
  }
  return COPPER_RESULT_PASS;
}

CopperResult copper_pair_check(const CopperItem *a, const CopperItem *b,
                               Nm required_nm) {
  if (required_nm < 0 || required_nm > COPPER_MAX_REQUIRED_NM) {
    return COPPER_RESULT_INVALID_INPUT;
  }

  /* Questions 0-3 of the mental map. Invalid input beats everything; an
     unsupported shape is only reported if the pair could interact at all. */
  if (!a || !b || a->layers == 0 || b->layers == 0) {
    return COPPER_RESULT_INVALID_INPUT;
  }
  const CopperResult gate = item_gate(a, b);
  if (gate == COPPER_RESULT_INVALID_INPUT) return gate;

  if (!copper_layer_masks_overlap(a->layers, b->layers)) {
    return COPPER_RESULT_NOT_APPLICABLE;
  }
  if (a->net == b->net && a->net != COPPER_NET_NONE) {
    return COPPER_RESULT_NOT_APPLICABLE;
  }
  if (gate == COPPER_RESULT_UNVERIFIED) return COPPER_RESULT_UNVERIFIED;

  const Nm radii = a->shape.radius + b->shape.radius;

  /* Touching first, so required == 0 can never let a touching pair pass. */
  if (core_cmp(&a->shape, &b->shape, radii) <= 0) return COPPER_RESULT_SHORT;

  if (core_cmp(&a->shape, &b->shape, required_nm + radii) >= 0) {
    return COPPER_RESULT_PASS;
  }
  return COPPER_RESULT_TOO_CLOSE;
}

CopperResult copper_pair_touch(const CopperItem *a, const CopperItem *b,
                               bool *out_touch) {
  if (!out_touch) return COPPER_RESULT_INVALID_INPUT;
  *out_touch = false;
  const CopperResult gate = item_gate(a, b);
  if (gate == COPPER_RESULT_INVALID_INPUT) return gate;
  if (!copper_layer_masks_overlap(a->layers, b->layers)) {
    return COPPER_RESULT_NOT_APPLICABLE;
  }
  if (gate == COPPER_RESULT_UNVERIFIED) return gate;
  *out_touch =
      core_cmp(&a->shape, &b->shape, a->shape.radius + b->shape.radius) <= 0;
  return COPPER_RESULT_PASS;
}

bool copper_pair_gap_nm(const CopperItem *a, const CopperItem *b,
                        double *out_gap_nm) {
  if (!out_gap_nm) return false;
  if (item_gate(a, b) != COPPER_RESULT_PASS) return false;
  if (!copper_layer_masks_overlap(a->layers, b->layers)) return false;
  const double dist = nm_sqrt(core_dist2(&a->shape, &b->shape));
  *out_gap_nm = dist - (double)a->shape.radius - (double)b->shape.radius;
  return true;
}

const char *copper_item_kind_name(CopperItemKind kind) {
  switch (kind) {
  case COPPER_ITEM_TRACE: return "trace";
  case COPPER_ITEM_PAD: return "pad";
  case COPPER_ITEM_VIA: return "via";
  case COPPER_ITEM_ZONE: return "zone";
  default: return "unknown";
  }
}

const char *copper_result_name(CopperResult r) {
  switch (r) {
  case COPPER_RESULT_PASS: return "pass";
  case COPPER_RESULT_NOT_APPLICABLE: return "not_applicable";
  case COPPER_RESULT_TOO_CLOSE: return "too_close";
  case COPPER_RESULT_SHORT: return "short";
  case COPPER_RESULT_UNVERIFIED: return "unverified";
  case COPPER_RESULT_INVALID_INPUT: return "invalid_input";
  default: return "unknown";
  }
}

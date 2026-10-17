#ifndef COPPER_V2_H
#define COPPER_V2_H

/*
 * copper_v2.h - the copper model and the pairwise clearance check.
 *
 * MENTAL MAP
 * ----------
 * Think of the board as a building: each copper layer is a floor.
 * Every piece of copper (trace, pad, via, later arcs and zones) answers
 * exactly three questions:
 *
 *      WHAT shape is it?    -> CopperShape
 *      WHICH floors?        -> CopperLayerMask  (bit 0 = top copper, ...)
 *      WHICH family (net)?  -> CopperNet        (0 = no net)
 *
 * Two items are compared by asking, in this order, and stopping at the
 * first answer:
 *      1. share a floor?    no  -> COPPER_RESULT_NOT_APPLICABLE
 *      2. same net?         yes -> COPPER_RESULT_NOT_APPLICABLE
 *      3. shapes supported? no  -> COPPER_RESULT_UNVERIFIED
 *      4. gap >= required?  yes -> COPPER_RESULT_PASS
 *                           no  -> COPPER_RESULT_TOO_CLOSE or _SHORT
 *
 * THE ONE FORMULA
 * ---------------
 * Every shape is a CORE (a point, a segment or a rectangle) plus a RADIUS:
 *      round pad / via   = point   + radius (diameter / 2)
 *      trace             = segment + radius (width / 2)
 *      rectangular pad   = rectangle, radius 0
 *      rounded rect pad  = smaller rectangle + radius (the corner radius)
 *
 *      gap = distance(core_a, core_b) - radius_a - radius_b
 *
 * The check "gap >= required" is rewritten without a square root as
 *      distance(core_a, core_b) >= required + radius_a + radius_b
 * and decided with the exact integer functions in geom.h.
 *
 * ROUNDING POLICY (agreed): copper is rounded OUTWARD (bigger), requirements
 * are rounded UPWARD (stricter). Rounding may cause a false alarm but can
 * never hide a real violation. Conversion from mm happens in the adapter that
 * builds CopperItems, not in this module. Here everything is integer nm.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "geom.h"

/* ------------------------------------------------------------------------ *
 * Layers
 * ------------------------------------------------------------------------ */

/* Bit i set means "present on internal stackup layer i". Bit 0 is the top
 * copper layer and the highest used bit is the bottom copper layer. These are
 * NOT KiCad's layer IDs; an adapter translates those. A through-hole pad or
 * via simply has many bits set. Two items can interact only if
 * (a.layers & b.layers) != 0. */
typedef uint64_t CopperLayerMask;

#define COPPER_MAX_LAYERS 64u

/* ------------------------------------------------------------------------ *
 * Nets
 * ------------------------------------------------------------------------ */

/* A net is identified by a number. */
typedef uint32_t CopperNet;

/* "This copper is not assigned to any net". Matches KiCad, where net 0 is the
 * unconnected net. Real nets are 1, 2, 3, ...
 *
 * RULE: COPPER_NET_NONE is never "the same net" as anything, not even another
 * COPPER_NET_NONE. Unassigned copper that touches or nearly touches other
 * copper is reported instead of silently ignored. */
#define COPPER_NET_NONE ((CopperNet)0)

/* ------------------------------------------------------------------------ *
 * Shapes
 * ------------------------------------------------------------------------ */

typedef enum {
  COPPER_SHAPE_POINT = 0, /* core: one point            (round pad, via)   */
  COPPER_SHAPE_SEGMENT,   /* core: a line segment       (trace, oval pad)  */
  COPPER_SHAPE_RECT,      /* core: axis-aligned rectangle (rect pad)       */

  /* Placeholders. A shape of these kinds is accepted by the type system
   * but cannot be measured yet: the check returns COPPER_RESULT_UNVERIFIED.
   * Fields for them are added when we implement them. */
  COPPER_SHAPE_ARC,
  COPPER_SHAPE_POLYGON
} CopperShapeKind;

/*
 * One shape = core + radius. How `a` and `b` are used depends on `kind`:
 *
 *   COPPER_SHAPE_POINT    a = the point;                  b unused
 *   COPPER_SHAPE_SEGMENT  a, b = the two end points
 *   COPPER_SHAPE_RECT     a = lower-left corner (min x, min y)
 *                         b = upper-right corner (max x, max y)
 *
 * `radius` is in nm and must be >= 0. For a RECT, a rounded rectangle of
 * size w x h with corner radius c is stored as the rectangle inset by c on
 * every side (so b.x - a.x = w - 2c) with radius = c. If c is larger than half
 * of the shorter side the inset rectangle turns inside out (a.x > b.x), and
 * copper_shape_validate() rejects it by checking a <= b on both axes.
 */
typedef struct {
  CopperShapeKind kind;
  Pt a;
  Pt b;
  Nm radius;
} CopperShape;

/* Constructors. They only fill the struct; copper_shape_validate() judges it.
 * copper_shape_point/segment take the RADIUS (already rounded outward by the
 * adapter). */
CopperShape copper_shape_point(Pt center, Nm radius);
CopperShape copper_shape_segment(Pt p0, Pt p1, Nm radius);
/* min_corner / max_corner are the OUTER corners of the pad. The constructor
 * insets them by corner_radius and stores radius = corner_radius. */
CopperShape copper_shape_rect(Pt min_corner, Pt max_corner, Nm corner_radius);

/* ------------------------------------------------------------------------ *
 * Items
 * ------------------------------------------------------------------------ */

/* What kind of board object produced the copper. Used only for reporting. */
typedef enum {
  COPPER_ITEM_TRACE = 0,
  COPPER_ITEM_PAD,
  COPPER_ITEM_VIA,
  COPPER_ITEM_ZONE
} CopperItemKind;

typedef struct {
  CopperItemKind kind;
  uint32_t source_index; /* index in the caller's own array, so findings can
                            point back at the original object */
  CopperShape shape;
  CopperLayerMask layers;
  CopperNet net;
} CopperItem;

/* ------------------------------------------------------------------------ *
 * Results
 * ------------------------------------------------------------------------ */

typedef enum {
  /* The pair was measured and is fine. */
  COPPER_RESULT_PASS = 0,

  /* The rule does not apply to this pair (no common layer, or same net).
   * This is NOT a pass: nothing was measured. */
  COPPER_RESULT_NOT_APPLICABLE,

  /* Measured: the gap is positive but smaller than required. */
  COPPER_RESULT_TOO_CLOSE,

  /* Measured: the copper touches or overlaps and the nets differ
   * (or one of them has no net). A manufactured short. */
  COPPER_RESULT_SHORT,

  /* One of the shapes is a kind we cannot measure yet (arc, polygon).
   * The caller must treat the board as UNVERIFIED, never as passed. */
  COPPER_RESULT_UNVERIFIED,

  /* Bad input: a malformed shape, empty layer mask, out-of-range coordinate,
   * negative requirement. This is a bug in the caller, not a design fault. */
  COPPER_RESULT_INVALID_INPUT
} CopperResult;

/* ------------------------------------------------------------------------ *
 * Functions
 * ------------------------------------------------------------------------ */

/* Is the shape well formed? Returns COPPER_RESULT_PASS, _UNVERIFIED (arc or
 * polygon placeholder) or _INVALID_INPUT. Checks: radius >= 0, all coordinates
 * pass pt_in_bounds(), a RECT has min <= max on both axes (this is also what
 * catches a corner radius larger than half the short side). */
CopperResult copper_shape_validate(const CopperShape *shape);

/*
 * Check one pair of items against a minimum edge-to-edge gap `required_nm`
 * (>= 0). Follows the four questions in the mental map above.
 *
 * To tell TOO_CLOSE from SHORT, the check also tests whether the shapes touch
 * (core distance <= sum of radii).
 */
CopperResult copper_pair_check(const CopperItem *a, const CopperItem *b,
                               Nm required_nm);

/* Edge-to-edge gap in nm, for REPORTS only (it is a double). Negative means
 * overlap (when the cores intersect the value is -(ra + rb), the depth is not
 * resolved). Nets are ignored: any two valid, supported items on a common
 * layer can be measured. Returns false if there is no common layer, a shape is
 * unsupported or invalid. Never use this value for pass/fail. */
bool copper_pair_gap_nm(const CopperItem *a, const CopperItem *b,
                        double *out_gap_nm);

/*
 * Do the two items touch or overlap (gap <= 0) on a common layer? Nets are
 * ignored, so this is the primitive for connectivity (same-net copper that
 * touches is one island). Exact.
 *   COPPER_RESULT_PASS            measured; *out_touch is valid
 *   COPPER_RESULT_NOT_APPLICABLE  no common layer; *out_touch = false
 *   COPPER_RESULT_UNVERIFIED      arc/polygon, not measured
 *   COPPER_RESULT_INVALID_INPUT   bad input
 */
CopperResult copper_pair_touch(const CopperItem *a, const CopperItem *b,
                               bool *out_touch);

/* Bounding box of the shape INCLUDING its radius, nm. Returns false for
 * invalid or unsupported shapes. Used as a cheap broad-phase reject. */
bool copper_shape_bbox(const CopperShape *shape, Pt *out_min, Pt *out_max);

/* Layer-mask helpers. Indices are internal stackup indices (0 = top). */
bool copper_layer_mask_single(unsigned layer_index, CopperLayerMask *out_mask);
bool copper_layer_mask_span(unsigned first_layer_index,
                            unsigned last_layer_index,
                            CopperLayerMask *out_mask);
bool copper_layer_masks_overlap(CopperLayerMask a, CopperLayerMask b);

const char *copper_item_kind_name(CopperItemKind kind);
const char *copper_result_name(CopperResult result);

#endif

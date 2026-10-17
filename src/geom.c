#include "geom.h"
#include "nmath.h"
#include "u128.h"
#include <string.h>

/* Internal helpers for integer min/max */
static inline int64_t nm_min_i64(int64_t a, int64_t b) {
  return (a < b) ? a : b;
}
static inline int64_t nm_max_i64(int64_t a, int64_t b) {
  return (a > b) ? a : b;
}

/* ========================================================================= *
 *                    Units Conversion (mm <-> nm)
 * ========================================================================= */

/**
 * Converts millimeters to nanometers.
 * Rounds half away from zero to preserve integer accuracy.
 */
Nm nm_from_mm(double mm) {
  double val = mm * (double)NM_PER_MM;
  return (val >= 0.0) ? (Nm)(val + 0.5) : (Nm)(val - 0.5);
}

/**
 * Converts nanometers back to millimeters.
 */
double nm_to_mm(Nm nm) { return (double)nm / (double)NM_PER_MM; }

/* ========================================================================= *
 *                   Exact 2D Geometry Primitives
 * ========================================================================= */

/**
 * Helper to ensure coordinate points stay within the +/- 500mm limits.
 * Call this during board importing. Failing this check risks int64_t overflow
 * during cross-products and distance scaling calculations.
 */
bool pt_in_bounds(Pt p) {
  return (p.x >= -GEOM_COORD_MAX_NM && p.x <= GEOM_COORD_MAX_NM) &&
         (p.y >= -GEOM_COORD_MAX_NM && p.y <= GEOM_COORD_MAX_NM);
}

/**
 * Exact squared distance between two points (nm^2).
 */
int64_t pt_dist2(Pt a, Pt b) {
  int64_t dx = a.x - b.x;
  int64_t dy = a.y - b.y;
  return (dx * dx) + (dy * dy);
}

/**
 * Exact 2D cross product of vectors OA and OB.
 * > 0 for counter-clockwise, < 0 for clockwise, 0 if collinear.
 */
int64_t pt_cross(Pt o, Pt a, Pt b) {
  return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
}

/**
 * Helper: checks if point q lies on segment pr given p, q, r are collinear.
 */
static bool on_segment(Pt p, Pt q, Pt r) {
  return (q.x >= nm_min_i64(p.x, r.x) && q.x <= nm_max_i64(p.x, r.x) &&
          q.y >= nm_min_i64(p.y, r.y) && q.y <= nm_max_i64(p.y, r.y));
}

/**
 * Tests if line segment AB intersects line segment CD (touching counts).
 */
bool seg_intersect(Pt a, Pt b, Pt c, Pt d) {
  int64_t o1 = pt_cross(a, b, c);
  int64_t o2 = pt_cross(a, b, d);
  int64_t o3 = pt_cross(c, d, a);
  int64_t o4 = pt_cross(c, d, b);

  // General case: segments cross each other
  if (((o1 > 0 && o2 < 0) || (o1 < 0 && o2 > 0)) &&
      ((o3 > 0 && o4 < 0) || (o3 < 0 && o4 > 0))) {
    return true;
  }

  // Special Collinear Cases
  if (o1 == 0 && on_segment(a, c, b))
    return true;
  if (o2 == 0 && on_segment(a, d, b))
    return true;
  if (o3 == 0 && on_segment(c, a, d))
    return true;
  if (o4 == 0 && on_segment(c, b, d))
    return true;

  return false;
}

/* ========================================================================= *
 *                    Distance Calculations & Rules
 * ========================================================================= */

/**
 * Minimum squared distance from point P to line segment AB (nm^2).
 */
double pt_seg_dist2(Pt p, Pt a, Pt b) {
  int64_t l2 = pt_dist2(a, b);

  // Segment AB is a single point
  if (l2 == 0) {
    return (double)pt_dist2(p, a);
  }

  // Vector projection parameter t = (P - A) . (B - A) / |B - A|^2
  int64_t dot = (p.x - a.x) * (b.x - a.x) + (p.y - a.y) * (b.y - a.y);

  if (dot <= 0) {
    // Point projects outside segment, closer to A
    return (double)pt_dist2(p, a);
  }
  if (dot >= l2) {
    // Point projects outside segment, closer to B
    return (double)pt_dist2(p, b);
  }

  // Projection falls within segment: perp_dist^2 = cross(A, B, P)^2 / |B - A|^2
  double cross = (double)pt_cross(a, b, p);
  return (cross * cross) / (double)l2;
}

/**
 * Minimum squared distance between segment AB and segment CD (nm^2).
 */
double seg_seg_dist2(Pt a, Pt b, Pt c, Pt d) {
  // If segments intersect, distance is 0
  if (seg_intersect(a, b, c, d)) {
    return 0.0;
  }

  // Otherwise, check all 4 point-to-segment combinations
  double d1 = pt_seg_dist2(a, c, d);
  double d2 = pt_seg_dist2(b, c, d);
  double d3 = pt_seg_dist2(c, a, b);
  double d4 = pt_seg_dist2(d, a, b);

  return nm_min_f64(nm_min_f64(d1, d2), nm_min_f64(d3, d4));
}

/**
 * True if squared distance d2 satisfies d2 >= required_nm^2.
 */
bool dist2_at_least(double d2, Nm required_nm) {
  // A negative requirement is trivially satisfied (prevents math bugs on
  // squaring)
  if (required_nm < 0) {
    return true;
  }
  double req = (double)required_nm;
  return d2 >= (req * req);
}

/**
 * Distance in nanometers between segment AB and segment CD via nm_sqrt.
 */
double seg_seg_dist(Pt a, Pt b, Pt c, Pt d) {
  return nm_sqrt(seg_seg_dist2(a, b, c, d));
}


/* ========================================================================= *
 *             v2: exact threshold comparisons (no libm, no double)
 * =========================================================================
 *
 * Every function answers one question exactly:  sign(distance - T).
 *
 * Why squares?  For T >= 0 and distance >= 0:
 *     distance <  T   <=>   distance^2 <  T^2
 * so we never need a square root. Sizes (inputs within +-5e8 nm):
 *     coordinate difference           <= 1e9
 *     squared point distance          <= 2e18      fits int64
 *     T capped below 2^31             T^2 <= 2^62  fits uint64
 *     cross product                   <= 2e18      fits int64
 *     cross^2 and T^2 * |AB|^2        <= ~1e37     need 128 bits (u128.h)
 */

#define GEOM_T_CAP (INT64_C(1) << 31)

static int cmp_u64(uint64_t a, uint64_t b) { return (a < b) ? -1 : (a > b); }

/* sign(0 - T): the answer when the two shapes touch or overlap (distance 0). */
static int cmp_zero_dist(Nm T) { return (T > 0) ? -1 : (T == 0 ? 0 : 1); }

static int min_int(int a, int b) { return a < b ? a : b; }

int geom_cmp_pt_pt(Pt p, Pt q, Nm T) {
  if (T < 0) return 1;
  if (T >= GEOM_T_CAP) return -1;
  const uint64_t d2 = (uint64_t)pt_dist2(p, q);
  const uint64_t t2 = (uint64_t)T * (uint64_t)T;
  return cmp_u64(d2, t2);
}

int geom_cmp_pt_seg(Pt p, Pt a, Pt b, Nm T) {
  if (T < 0) return 1;
  if (T >= GEOM_T_CAP) return -1;

  const int64_t l2 = pt_dist2(a, b);
  if (l2 == 0) return geom_cmp_pt_pt(p, a, T); /* degenerate segment */

  /* Where does p project onto line AB?  dot <= 0: before A; dot >= l2: after B. */
  const int64_t dot = (p.x - a.x) * (b.x - a.x) + (p.y - a.y) * (b.y - a.y);
  if (dot <= 0) return geom_cmp_pt_pt(p, a, T);
  if (dot >= l2) return geom_cmp_pt_pt(p, b, T);

  /* Perpendicular case: dist^2 = cross^2 / l2. Compare without dividing:
   *     cross^2  vs  T^2 * l2        (both as exact 128-bit values)        */
  const int64_t cr = pt_cross(a, b, p);
  const uint64_t ac = (cr < 0) ? (uint64_t)(-cr) : (uint64_t)cr;
  const uint64_t t2 = (uint64_t)T * (uint64_t)T;
  return u128_cmp(u128_mul(ac, ac), u128_mul(t2, (uint64_t)l2));
}

/* Gap along one axis between value v and the interval [lo, hi]. */
static int64_t axis_gap(int64_t v, int64_t lo, int64_t hi) {
  if (v < lo) return lo - v;
  if (v > hi) return v - hi;
  return 0;
}

int geom_cmp_pt_rect(Pt p, Pt rmin, Pt rmax, Nm T) {
  if (T < 0) return 1;
  if (T >= GEOM_T_CAP) return -1;
  const int64_t dx = axis_gap(p.x, rmin.x, rmax.x);
  const int64_t dy = axis_gap(p.y, rmin.y, rmax.y);
  const uint64_t d2 = (uint64_t)(dx * dx + dy * dy);
  const uint64_t t2 = (uint64_t)T * (uint64_t)T;
  return cmp_u64(d2, t2);
}

int geom_cmp_seg_seg(Pt a, Pt b, Pt c, Pt d, Nm T) {
  if (T < 0) return 1;
  if (T >= GEOM_T_CAP) return -1;
  if (seg_intersect(a, b, c, d)) return cmp_zero_dist(T);
  /* Disjoint segments: closest approach involves an endpoint of one of them. */
  int r = geom_cmp_pt_seg(a, c, d, T);
  r = min_int(r, geom_cmp_pt_seg(b, c, d, T));
  r = min_int(r, geom_cmp_pt_seg(c, a, b, T));
  r = min_int(r, geom_cmp_pt_seg(d, a, b, T));
  return r; /* sign(min d_i - T) == min of sign(d_i - T) */
}

static bool pt_in_rect(Pt p, Pt rmin, Pt rmax) {
  return p.x >= rmin.x && p.x <= rmax.x && p.y >= rmin.y && p.y <= rmax.y;
}

int geom_cmp_seg_rect(Pt a, Pt b, Pt rmin, Pt rmax, Nm T) {
  if (T < 0) return 1;
  if (T >= GEOM_T_CAP) return -1;

  const Pt c[4] = {
      {rmin.x, rmin.y}, {rmax.x, rmin.y}, {rmax.x, rmax.y}, {rmin.x, rmax.y}};

  /* Segment touches the rectangle: an endpoint is inside, or it crosses an edge. */
  if (pt_in_rect(a, rmin, rmax) || pt_in_rect(b, rmin, rmax)) {
    return cmp_zero_dist(T);
  }
  for (int i = 0; i < 4; ++i) {
    if (seg_intersect(a, b, c[i], c[(i + 1) & 3])) return cmp_zero_dist(T);
  }

  /* Disjoint convex shapes: the closest pair involves a vertex of one of them. */
  int r = geom_cmp_pt_rect(a, rmin, rmax, T);
  r = min_int(r, geom_cmp_pt_rect(b, rmin, rmax, T));
  for (int i = 0; i < 4; ++i) {
    r = min_int(r, geom_cmp_pt_seg(c[i], a, b, T));
  }
  return r;
}

int geom_cmp_rect_rect(Pt amin, Pt amax, Pt bmin, Pt bmax, Nm T) {
  if (T < 0) return 1;
  if (T >= GEOM_T_CAP) return -1;
  /* Gap per axis is zero when the intervals overlap. */
  int64_t gx = nm_max_i64(0, nm_max_i64(amin.x - bmax.x, bmin.x - amax.x));
  int64_t gy = nm_max_i64(0, nm_max_i64(amin.y - bmax.y, bmin.y - amax.y));
  const uint64_t d2 = (uint64_t)(gx * gx + gy * gy);
  const uint64_t t2 = (uint64_t)T * (uint64_t)T;
  return cmp_u64(d2, t2);
}

/* ---- report-only distances (double) ------------------------------------ */

double geom_dist2_pt_rect(Pt p, Pt rmin, Pt rmax) {
  const int64_t dx = axis_gap(p.x, rmin.x, rmax.x);
  const int64_t dy = axis_gap(p.y, rmin.y, rmax.y);
  return (double)(dx * dx + dy * dy);
}

double geom_dist2_seg_rect(Pt a, Pt b, Pt rmin, Pt rmax) {
  const Pt c[4] = {
      {rmin.x, rmin.y}, {rmax.x, rmin.y}, {rmax.x, rmax.y}, {rmin.x, rmax.y}};
  if (pt_in_rect(a, rmin, rmax) || pt_in_rect(b, rmin, rmax)) return 0.0;
  for (int i = 0; i < 4; ++i) {
    if (seg_intersect(a, b, c[i], c[(i + 1) & 3])) return 0.0;
  }
  double best = nm_min_f64(geom_dist2_pt_rect(a, rmin, rmax),
                           geom_dist2_pt_rect(b, rmin, rmax));
  for (int i = 0; i < 4; ++i) {
    best = nm_min_f64(best, pt_seg_dist2(c[i], a, b));
  }
  return best;
}

double geom_dist2_rect_rect(Pt amin, Pt amax, Pt bmin, Pt bmax) {
  int64_t gx = nm_max_i64(0, nm_max_i64(amin.x - bmax.x, bmin.x - amax.x));
  int64_t gy = nm_max_i64(0, nm_max_i64(amin.y - bmax.y, bmin.y - amax.y));
  return (double)(gx * gx + gy * gy);
}

/* ---- mm -> nm, rounding up ---------------------------------------------- */

Nm nm_from_mm_ceil(double mm) {
  const double v = mm * (double)NM_PER_MM;
  Nm n = (Nm)v; /* truncates toward zero; v >= 0 by contract */
  const double frac = v - (double)n;
  /* frac is in [0,1). Below 1e-6 nm it is binary-representation noise
     (0.15 * 1e6 is not exactly 150000), not a real fraction of a nanometre. */
  if (frac > 1e-6) n += 1;
  return n;
}

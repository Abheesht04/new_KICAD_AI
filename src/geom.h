#ifndef GEOM_H
#define GEOM_H

#include <stdbool.h>
#include <stdint.h>

/* Coordinates are integer nanometres. All inputs must satisfy
   |x|, |y| <= GEOM_COORD_MAX_NM so that differences are <= 1e9 and every
   cross product (<= 2e18) and squared distance fits in int64_t. */
#define NM_PER_MM 1000000
#define GEOM_COORD_MAX_NM 500000000 /* 500 mm */

typedef int64_t Nm;
typedef struct {
  Nm x, y;
} Pt;

/* mm <-> nm. Rounds half away from zero; documented rule lives in geom.c. */
Nm nm_from_mm(double mm);
double nm_to_mm(Nm nm);

/* Exact integer results. */
int64_t pt_dist2(Pt a, Pt b);
int64_t pt_cross(Pt o, Pt a, Pt b);
bool seg_intersect(Pt a, Pt b, Pt c, Pt d); /* touching counts */

/* Squared distances (nm^2). Decisions use these, no root taken. */
double pt_seg_dist2(Pt p, Pt a, Pt b);
double seg_seg_dist2(Pt a, Pt b, Pt c, Pt d);

/* True if squared distance d2 corresponds to a distance >= required_nm. */
bool dist2_at_least(double d2, Nm required_nm);

/* Distance in nm via nm_sqrt. Reports only, not for pass/fail. */
double seg_seg_dist(Pt a, Pt b, Pt c, Pt d);

/* ---- v2 additions: exact predicates and conversions --------------------- */

/* True if |x|,|y| <= GEOM_COORD_MAX_NM. Every input to the functions below
   must satisfy this. */
bool pt_in_bounds(Pt p);

/* mm -> nm rounding UP (for requirements). Input must be finite and in
   [0, 501] mm. Values within 1e-6 nm of an integer are treated as that
   integer, so 0.15 mm gives 150000 nm and not 150001 nm. */
Nm nm_from_mm_ceil(double mm);

/*
 * Exact three-way comparison of a distance with a threshold T (nm):
 * returns -1 if dist < T, 0 if dist == T, +1 if dist > T.
 * Integer arithmetic only (uint64 and the 128-bit helpers in u128.h), so the
 * answer is exact for every input inside the coordinate bound.
 * T < 0 always gives +1; T >= 2^31 always gives -1 (the board diagonal is
 * below 1.5e9 nm, so no distance can reach it).
 *
 * "Rect" arguments are axis-aligned, given as (min corner, max corner) with
 * min <= max on both axes.
 */
int geom_cmp_pt_pt(Pt p, Pt q, Nm T);
int geom_cmp_pt_seg(Pt p, Pt a, Pt b, Nm T);
int geom_cmp_pt_rect(Pt p, Pt rmin, Pt rmax, Nm T);
int geom_cmp_seg_seg(Pt a, Pt b, Pt c, Pt d, Nm T);
int geom_cmp_seg_rect(Pt a, Pt b, Pt rmin, Pt rmax, Nm T);
int geom_cmp_rect_rect(Pt amin, Pt amax, Pt bmin, Pt bmax, Nm T);

/* Squared distances for REPORTS only (double). Never decide pass/fail on
   these; use the geom_cmp_* functions. */
double geom_dist2_pt_rect(Pt p, Pt rmin, Pt rmax);
double geom_dist2_seg_rect(Pt a, Pt b, Pt rmin, Pt rmax);
double geom_dist2_rect_rect(Pt amin, Pt amax, Pt bmin, Pt bmax);

#endif

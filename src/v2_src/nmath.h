#ifndef NMATH_H
#define NMATH_H

#include <stdint.h>

/* Numeric primitives. Product code never includes <math.h>. */

/* Exact floor(sqrt(n)), integer operations only. */
uint64_t nm_isqrt_u64(uint64_t n);

/* 1/sqrt(s) for s > 0: bit-pattern seed + fixed Newton iterations
   (iteration count fixed, never data-dependent). Target: ~1e-15 relative. */
double nm_rsqrt(double s);

/* sqrt(s) for s >= 0 (returns 0 for s == 0), computed as s * nm_rsqrt(s). */
double nm_sqrt(double s);

int64_t nm_abs_i64(int64_t v);
double nm_abs_f64(double v);
double nm_min_f64(double a, double b);
double nm_max_f64(double a, double b);

/* True if v is neither NaN nor +-infinity. No <math.h>. */
int nm_isfinite(double v);

#endif

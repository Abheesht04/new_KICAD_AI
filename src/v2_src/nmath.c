#include "nmath.h"
#include <string.h>

uint64_t nm_isqrt_u64(uint64_t n) {

  uint64_t res = 0;

  uint64_t bit = (uint64_t)1 << 62;

  while (bit > n) {
    bit >>= 2;
  }

  while (bit != 0) {
    if (n >= res + bit) {
      n -= res + bit;
      res = (res >> 1) + bit;
    } else {
      res >>= 1;
    }
    bit >>= 2;
  }

  return res;
}

double nm_rsqrt(double s) {
  if (s <= 0.0) {
    return 0.0;
  }

  // 1. Bit-pattern seed approximation
  uint64_t i;
  memcpy(&i, &s, sizeof(i));

  // 64-bit IEEE-754 magic constant optimized for initial rsqrt guess
  i = 0x5fe6ec85e7de30daULL - (i >> 1);

  double y;
  memcpy(&y, &i, sizeof(y));

  // 2. Fixed Newton-Raphson Iterations
  // Formula: y_{n+1} = y_n * (1.5 - 0.5 * s * y_n^2)
  const double threehalfs = 1.5;
  const double x2 = s * 0.5;

  y = y * (threehalfs - (x2 * y * y)); // Pass 1: ~16-bit accuracy
  y = y * (threehalfs - (x2 * y * y)); // Pass 2: ~32-bit accuracy
  y = y * (threehalfs - (x2 * y * y)); // Pass 3: ~52-bit accuracy (~1e-15)
  y = y * (threehalfs -
           (x2 * y * y)); // Pass 4: Refinement to machine precision limit

  return y;
}

/**
 * Computes sqrt(s) for s >= 0 using s * nm_rsqrt(s).
 */
double nm_sqrt(double s) {
  if (s <= 0.0) {
    return 0.0;
  }
  return s * nm_rsqrt(s);
}

/**
 * Absolute value for 64-bit signed integer.
 */
int64_t nm_abs_i64(int64_t v) { return (v < 0) ? -v : v; }

/**
 * Absolute value for 64-bit float, clearing the sign bit directly.
 */
double nm_abs_f64(double v) {
  uint64_t i;
  memcpy(&i, &v, sizeof(i));
  i &= 0x7FFFFFFFFFFFFFFFULL; // Clear bit 63 (the sign bit)
  double result;
  memcpy(&result, &i, sizeof(result));
  return result;
}

/**
 * Minimum of two doubles.
 */
double nm_min_f64(double a, double b) { return (a < b) ? a : b; }

/**
 * Maximum of two doubles.
 */
double nm_max_f64(double a, double b) { return (a > b) ? a : b; }

/**
 * Finite test without <math.h>: NaN fails v == v, and +-inf makes v - v NaN.
 */
int nm_isfinite(double v) { return (v == v) && ((v - v) == 0.0); }

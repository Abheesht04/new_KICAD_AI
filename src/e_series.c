// e_series.c
#include "e_series.h"
#include <math.h>
#include <stddef.h>

// E24 base mantissas, scaled by 10 (so 10 = 1.0, 91 = 9.1).
// These are the REAL IEC 60063 values -- not derived, just the standard.
static const int E24_BASE[] = {10, 11, 12, 13, 15, 16, 18, 20, 22, 24, 27, 30,
                               33, 36, 39, 43, 47, 51, 56, 62, 68, 75, 82, 91};

// E96 base mantissas, scaled by 100 (so 100 = 1.00, 976 = 9.76).
static const int E96_BASE[] = {
    100, 102, 105, 107, 110, 113, 115, 118, 121, 124, 127, 130, 133, 137,
    140, 143, 147, 150, 154, 158, 162, 165, 169, 174, 178, 182, 187, 191,
    196, 200, 205, 210, 215, 221, 226, 232, 237, 243, 249, 255, 261, 267,
    274, 280, 287, 294, 301, 309, 316, 324, 332, 340, 348, 357, 365, 374,
    383, 392, 402, 412, 422, 432, 442, 453, 464, 475, 487, 499, 511, 523,
    536, 549, 562, 576, 590, 604, 619, 634, 649, 665, 681, 698, 715, 732,
    750, 768, 787, 806, 825, 845, 866, 887, 909, 931, 953, 976};

static void series_table(ESeries series, const int **base, size_t *count,
                         int *scale) {
  if (series == E_SERIES_E24) {
    *base = E24_BASE;
    *count = sizeof(E24_BASE) / sizeof(E24_BASE[0]);
    *scale = 10; // base values represent mantissa * 10
  } else {
    *base = E96_BASE;
    *count = sizeof(E96_BASE) / sizeof(E96_BASE[0]);
    *scale = 100; // base values represent mantissa * 100
  }
}

// Reduce `value` to a mantissa in [1, 10) and an integer decade exponent,
// e.g. 4700.0 -> mantissa 4.7, decade_exp 3 (since 4.7 * 10^3 = 4700).
static double normalize(double value, int *decade_exp) {
  int exp = (int)floor(log10(value));
  double mantissa = value / pow(10.0, exp);
  *decade_exp = exp;
  return mantissa;
}

double e_series_nearest(ESeries series, double value) {
  if (value <= 0.0)
    return 0.0;

  const int *base;
  size_t count;
  int scale;
  series_table(series, &base, &count, &scale);

  int decade_exp;
  double mantissa = normalize(value, &decade_exp); // e.g. 4.7
  double scaled = mantissa * scale; // e.g. 47.0 (E24) or 470.0 (E96)

  // Linear scan for closest base entry -- table is tiny (24 or 96 entries),
  // this is plenty fast and stays simple.
  int best = base[0];
  double best_diff = fabs(scaled - base[0]);
  for (size_t i = 1; i < count; i++) {
    double diff = fabs(scaled - base[i]);
    if (diff < best_diff) {
      best_diff = diff;
      best = base[i];
    }
  }

  double snapped_mantissa = (double)best / scale;  // back to e.g. 4.7
  return snapped_mantissa * pow(10.0, decade_exp); // back to e.g. 4700.0
}

double e_series_at_least(ESeries series, double value) {
  if (!(value > 0.0) || !isfinite(value))
    return 0.0;

  const int *base;
  size_t count;
  int scale;
  series_table(series, &base, &count, &scale);

  int decade_exp;
  double scaled = normalize(value, &decade_exp) * scale;
  // 1 ppm slack: a value that already is a series value maps to itself despite float noise.
  for (size_t i = 0; i < count; i++)
    if (base[i] >= scaled * (1.0 - 1e-6))
      return (double)base[i] / scale * pow(10.0, decade_exp);
  return 1.0 * pow(10.0, decade_exp + 1); // past the last mantissa: the next decade's first value
}

bool e_series_contains(ESeries series, double value, double tolerance_ppm) {
  double nearest = e_series_nearest(series, value);
  double rel_diff = fabs(value - nearest) / value;
  double tolerance_fraction = tolerance_ppm / 1000000.0;
  return rel_diff <= tolerance_fraction;
}

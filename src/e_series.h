// e_series.h
#ifndef E_SERIES_H
#define E_SERIES_H

#include <stdbool.h>

typedef enum {
  E_SERIES_E24, // 5% tolerance, 24 values/decade
  E_SERIES_E96  // 1% tolerance, 96 values/decade
} ESeries;

// Is `value` (e.g. 10000.0 for 10k ohms) a real, standard value in this series?
// `tolerance_ppm` guards against float comparison noise (e.g. 1e-6 relative).
bool e_series_contains(ESeries series, double value, double tolerance_ppm);

// Snap an arbitrary value to the NEAREST real value in the series.
// Useful for "the user/LLM asked for 10.3k, what's the closest real part?"
double e_series_nearest(ESeries series, double value);

// The SMALLEST series value >= `value` (within 1 ppm), e.g. a series resistor that keeps a current at or
// below its target. Returns 0.0 for a non-positive or non-finite value.
double e_series_at_least(ESeries series, double value);

// The LARGEST series value <= `value` (within 1 ppm), e.g. the weakest pull-up that still meets a rise time.
// Returns 0.0 for a non-positive or non-finite value.
double e_series_at_most(ESeries series, double value);

#endif

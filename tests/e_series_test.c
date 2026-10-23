#include "e_series.h"

#include <assert.h>
#include <math.h>
#include <math.h>
#include <stdio.h>

int main(void)
{
    assert(e_series_contains(E_SERIES_E24, 4700.0, 0.0));
    assert(e_series_contains(E_SERIES_E24, 4701.0, 1000.0));
    assert(!e_series_contains(E_SERIES_E24, 4800.0, 1000.0));

    /* at_least: the smallest standard value not below the request. */
    assert(fabs(e_series_at_least(E_SERIES_E24, 280.0) - 300.0) < 1e-9);
    assert(fabs(e_series_at_least(E_SERIES_E24, 270.0) - 270.0) < 1e-9);
    assert(fabs(e_series_at_least(E_SERIES_E24, 270.0000001) - 270.0) < 1e-9); /* float noise stays put */
    assert(fabs(e_series_at_least(E_SERIES_E24, 9200.0) - 10000.0) < 1e-6);    /* past 9.1: next decade */
    assert(fabs(e_series_at_least(E_SERIES_E96, 281.0) - 287.0) < 1e-9);
    assert(e_series_at_least(E_SERIES_E24, 0.0) == 0.0 && e_series_at_least(E_SERIES_E24, -5.0) == 0.0);

    printf("E-series ppm semantics test PASSED\n");
    return 0;
}

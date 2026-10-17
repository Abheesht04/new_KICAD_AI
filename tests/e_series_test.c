#include "e_series.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    assert(e_series_contains(E_SERIES_E24, 4700.0, 0.0));
    assert(e_series_contains(E_SERIES_E24, 4701.0, 1000.0));
    assert(!e_series_contains(E_SERIES_E24, 4800.0, 1000.0));

    printf("E-series ppm semantics test PASSED\n");
    return 0;
}

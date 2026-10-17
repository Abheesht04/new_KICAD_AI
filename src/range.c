#include "range.h"

Range range_exact(double value)
{
    Range range = {value, value, value};
    return range;
}

Range range_percent(double nominal, double percent)
{
    double delta = nominal * (percent / 100.0);

    Range range;
    range.nominal = nominal;
    range.minimum = nominal - delta;
    range.maximum = nominal + delta;

    return range;
}

int range_contains(Range range, double value)
{
    return value >= range.minimum && value <= range.maximum;
}

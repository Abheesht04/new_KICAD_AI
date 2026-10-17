#ifndef RANGE_H
#define RANGE_H

typedef struct {
    double nominal;
    double minimum;
    double maximum;
} Range;

Range range_exact(double value);
Range range_percent(double nominal, double percent);
int range_contains(Range range, double value);

#endif

#ifndef CPLX_H
#define CPLX_H
/*
 * Portable complex arithmetic for AC analysis.
 *
 * A plain {re, im} pair instead of C99 _Complex, which MSVC does not support.
 * All functions are pure. Division reports a zero or non-finite denominator
 * instead of producing Inf/NaN.
 */

typedef struct {
    double re;
    double im;
} Cplx;

Cplx cplx(double re, double im);
Cplx cplx_add(Cplx a, Cplx b);
Cplx cplx_sub(Cplx a, Cplx b);
Cplx cplx_mul(Cplx a, Cplx b);
Cplx cplx_scale(Cplx a, double k);

/* a / b by Smith's method (no intermediate overflow for large/small parts).
 * Returns 1 and writes *out; returns 0 and leaves *out unchanged when b is
 * zero or either operand is non-finite. */
int cplx_div(Cplx a, Cplx b, Cplx *out);

double cplx_abs(Cplx a);        /* |a| via hypot: no overflow for large parts */
double cplx_arg_deg(Cplx a);    /* phase in degrees, (-180, 180]; 0 for a == 0 */
Cplx cplx_polar_deg(double magnitude, double phase_deg);
int cplx_isfinite(Cplx a);

#endif

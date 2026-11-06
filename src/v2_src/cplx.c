#include "cplx.h"

#include <math.h>

static const double PI = 3.14159265358979323846;

Cplx cplx(double re, double im) { return (Cplx){re, im}; }
Cplx cplx_add(Cplx a, Cplx b) { return (Cplx){a.re + b.re, a.im + b.im}; }
Cplx cplx_sub(Cplx a, Cplx b) { return (Cplx){a.re - b.re, a.im - b.im}; }
Cplx cplx_mul(Cplx a, Cplx b) { return (Cplx){a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re}; }
Cplx cplx_scale(Cplx a, double k) { return (Cplx){a.re * k, a.im * k}; }

int cplx_isfinite(Cplx a) { return isfinite(a.re) && isfinite(a.im); }

int cplx_div(Cplx a, Cplx b, Cplx *out)
{
    if (!cplx_isfinite(a) || !cplx_isfinite(b) || (b.re == 0.0 && b.im == 0.0)) {
        return 0;
    }
    Cplx q;
    if (fabs(b.re) >= fabs(b.im)) {
        double r = b.im / b.re, d = b.re + b.im * r;
        q = (Cplx){(a.re + a.im * r) / d, (a.im - a.re * r) / d};
    } else {
        double r = b.re / b.im, d = b.re * r + b.im;
        q = (Cplx){(a.re * r + a.im) / d, (a.im * r - a.re) / d};
    }
    if (!cplx_isfinite(q)) {
        return 0;
    }
    *out = q;
    return 1;
}

double cplx_abs(Cplx a) { return hypot(a.re, a.im); }

double cplx_arg_deg(Cplx a)
{
    if (a.re == 0.0 && a.im == 0.0) {
        return 0.0;
    }
    double deg = atan2(a.im, a.re) * 180.0 / PI;
    return deg <= -180.0 ? deg + 360.0 : deg;
}

Cplx cplx_polar_deg(double magnitude, double phase_deg)
{
    double rad = phase_deg * PI / 180.0;
    return (Cplx){magnitude * cos(rad), magnitude * sin(rad)};
}

/* Portable complex arithmetic: exact small cases, quadrant phases, and the
 * division / magnitude edge cases that naive formulas get wrong. */
#include "cplx.h"

#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>

static int near(double a, double b, double tol) { return fabs(a - b) <= tol * (1.0 + fabs(b)); }

int main(void)
{
    Cplx a = cplx(3, 4), b = cplx(1, -2), q = cplx(99, 99);

    assert(cplx_add(a, b).re == 4 && cplx_add(a, b).im == 2);
    assert(cplx_sub(a, b).re == 2 && cplx_sub(a, b).im == 6);
    assert(cplx_mul(a, b).re == 11 && cplx_mul(a, b).im == -2);    /* (3+4j)(1-2j) = 11-2j */
    assert(cplx_mul(cplx(0, 1), cplx(0, 1)).re == -1);             /* j*j = -1 */
    assert(cplx_div(a, b, &q) && near(q.re, -1.0, 1e-15) && near(q.im, 2.0, 1e-15)); /* (3+4j)/(1-2j) = -1+2j */
    assert(cplx_div(cplx(1, 0), cplx(0, 1), &q) && q.re == 0 && q.im == -1);           /* 1/j = -j */
    assert(cplx_abs(a) == 5.0);

    /* Zero and non-finite denominators fail and leave the output untouched. */
    q = cplx(7, 7);
    assert(!cplx_div(a, cplx(0, 0), &q) && q.re == 7 && q.im == 7);
    assert(!cplx_div(a, cplx(NAN, 1), &q) && !cplx_div(a, cplx(1, INFINITY), &q) && !cplx_div(cplx(NAN, 0), b, &q));
    assert(q.re == 7);

    /* Smith's division avoids overflow/underflow that |b|^2 would cause. */
    assert(cplx_div(cplx(1e300, 1e300), cplx(1e300, 1e300), &q) && near(q.re, 1.0, 1e-15) && fabs(q.im) < 1e-15);
    assert(cplx_div(cplx(1e-300, 1e-300), cplx(1e-300, -1e-300), &q) && fabs(q.re) < 1e-15 && near(q.im, 1.0, 1e-15));
    assert(cplx_div(cplx(1, 0), cplx(DBL_MAX, DBL_MAX), &q) && cplx_isfinite(q));
    assert(near(cplx_abs(cplx(1e300, 1e300)), sqrt(2.0) * 1e300, 1e-15));
    assert(near(cplx_abs(cplx(3e-320, 4e-320)), 5e-320, 1e-3));

    /* Phase in degrees, one point per quadrant and on each axis; 0 for 0. */
    assert(near(cplx_arg_deg(cplx(1, 1)), 45, 1e-14) && near(cplx_arg_deg(cplx(-1, 1)), 135, 1e-14));
    assert(near(cplx_arg_deg(cplx(-1, -1)), -135, 1e-14) && near(cplx_arg_deg(cplx(1, -1)), -45, 1e-14));
    assert(cplx_arg_deg(cplx(-1, 0)) == 180 && cplx_arg_deg(cplx(0, -2)) == -90 && cplx_arg_deg(cplx(0, 0)) == 0);

    Cplx p = cplx_polar_deg(2, 90);
    assert(fabs(p.re) < 1e-15 && near(p.im, 2, 1e-15));
    p = cplx_polar_deg(1, -45);
    assert(near(p.re, sqrt(0.5), 1e-15) && near(p.im, -sqrt(0.5), 1e-15));
    assert(cplx_isfinite(a) && !cplx_isfinite(cplx(NAN, 0)) && !cplx_isfinite(cplx(0, -INFINITY)));
    puts("cplx: PASS");
    return 0;
}

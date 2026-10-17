/* Oracle test: the exact integer comparators must agree with a plain
 * long-double reference (libm allowed here, tests only) everywhere except
 * within a tiny margin of the threshold, and must be exact on hand-made
 * boundary cases. */
#include "geom.h"
#include "nmath.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

static uint64_t rng_state = 0x9E3779B97F4A7C15ull;
static uint64_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}
static int64_t rnd_range(int64_t lo, int64_t hi)
{
    return lo + (int64_t)(rng() % (uint64_t)(hi - lo + 1));
}
static Pt rnd_pt(int64_t span)
{
    return (Pt){ rnd_range(-span, span), rnd_range(-span, span) };
}

/* ---- long double references ---- */
static long double ref_pt_seg(Pt p, Pt a, Pt b)
{
    long double vx = (long double)b.x - a.x, vy = (long double)b.y - a.y;
    long double wx = (long double)p.x - a.x, wy = (long double)p.y - a.y;
    long double l2 = vx * vx + vy * vy;
    long double t = (l2 == 0) ? 0 : (wx * vx + wy * vy) / l2;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    long double dx = wx - t * vx, dy = wy - t * vy;
    return sqrtl(dx * dx + dy * dy);
}
static long double ref_pt_rect(Pt p, Pt lo, Pt hi)
{
    long double dx = p.x < lo.x ? (long double)lo.x - p.x : (p.x > hi.x ? (long double)p.x - hi.x : 0);
    long double dy = p.y < lo.y ? (long double)lo.y - p.y : (p.y > hi.y ? (long double)p.y - hi.y : 0);
    return sqrtl(dx * dx + dy * dy);
}
static long double ref_seg_seg(Pt a, Pt b, Pt c, Pt d)
{
    if (seg_intersect(a, b, c, d)) return 0;
    long double m = ref_pt_seg(a, c, d);
    long double v;
    if ((v = ref_pt_seg(b, c, d)) < m) m = v;
    if ((v = ref_pt_seg(c, a, b)) < m) m = v;
    if ((v = ref_pt_seg(d, a, b)) < m) m = v;
    return m;
}
static long double ref_seg_rect(Pt a, Pt b, Pt lo, Pt hi)
{
    if (ref_pt_rect(a, lo, hi) == 0 || ref_pt_rect(b, lo, hi) == 0) return 0;
    Pt c[4] = {{lo.x, lo.y}, {hi.x, lo.y}, {hi.x, hi.y}, {lo.x, hi.y}};
    long double m = 1e30L;
    for (int i = 0; i < 4; ++i) {
        long double v = ref_seg_seg(a, b, c[i], c[(i + 1) & 3]);
        if (v < m) m = v;
    }
    return m;
}
static long double ref_rect_rect(Pt alo, Pt ahi, Pt blo, Pt bhi)
{
    long double gx = (long double)(alo.x - bhi.x);
    long double t = (long double)(blo.x - ahi.x);
    if (t > gx) gx = t;
    if (gx < 0) gx = 0;
    long double gy = (long double)(alo.y - bhi.y);
    t = (long double)(blo.y - ahi.y);
    if (t > gy) gy = t;
    if (gy < 0) gy = 0;
    return sqrtl(gx * gx + gy * gy);
}

/* Check cmp against reference at thresholds around the true distance. */
static void check(long double dist, int (*f)(const void *, Nm), const void *ctx)
{
    /* Clearly below / above must be exact. */
    Nm lo = (Nm)floorl(dist) - 2;
    Nm hi = (Nm)ceill(dist) + 2;
    if (lo >= 0 && lo < (INT64_C(1) << 31)) assert(f(ctx, lo) == 1);
    if (hi >= 0 && hi < (INT64_C(1) << 31)) assert(f(ctx, hi) == -1);
}

typedef struct { Pt p, a, b, c, d; } Ctx;
static int f_pp(const void *x, Nm T) { const Ctx *c = x; return geom_cmp_pt_pt(c->p, c->a, T); }
static int f_ps(const void *x, Nm T) { const Ctx *c = x; return geom_cmp_pt_seg(c->p, c->a, c->b, T); }
static int f_pr(const void *x, Nm T) { const Ctx *c = x; return geom_cmp_pt_rect(c->p, c->a, c->b, T); }
static int f_ss(const void *x, Nm T) { const Ctx *c = x; return geom_cmp_seg_seg(c->p, c->a, c->b, c->c, T); }
static int f_sr(const void *x, Nm T) { const Ctx *c = x; return geom_cmp_seg_rect(c->p, c->a, c->b, c->c, T); }
static int f_rr(const void *x, Nm T) { const Ctx *c = x; return geom_cmp_rect_rect(c->p, c->a, c->b, c->c, T); }

static Pt lo_of(Pt a, Pt b) { return (Pt){ a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y }; }
static Pt hi_of(Pt a, Pt b) { return (Pt){ a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y }; }

static void random_oracle(int64_t span, int rounds)
{
    for (int i = 0; i < rounds; ++i) {
        Ctx c = { rnd_pt(span), rnd_pt(span), rnd_pt(span), rnd_pt(span), rnd_pt(span) };
        long double dx = (long double)c.p.x - c.a.x, dy = (long double)c.p.y - c.a.y;
        check(sqrtl(dx * dx + dy * dy), f_pp, &c);
        check(ref_pt_seg(c.p, c.a, c.b), f_ps, &c);

        Ctx r = c;
        r.a = lo_of(c.a, c.b);
        r.b = hi_of(c.a, c.b);
        check(ref_pt_rect(r.p, r.a, r.b), f_pr, &r);

        check(ref_seg_seg(c.p, c.a, c.b, c.c), f_ss, &c);

        Ctx s = c; /* segment (p,a) vs rect from (b,c) */
        s.b = lo_of(c.b, c.c);
        s.c = hi_of(c.b, c.c);
        check(ref_seg_rect(s.p, s.a, s.b, s.c), f_sr, &s);

        Ctx q = c; /* rect (p,a) vs rect (b,c) */
        Pt alo = lo_of(c.p, c.a), ahi = hi_of(c.p, c.a);
        Pt blo = lo_of(c.b, c.c), bhi = hi_of(c.b, c.c);
        q.p = alo; q.a = ahi; q.b = blo; q.c = bhi;
        check(ref_rect_rect(alo, ahi, blo, bhi), f_rr, &q);
    }
}

static void exact_boundaries(void)
{
    /* 3-4-5 triangle: distance is exactly 5 nm. */
    assert(geom_cmp_pt_pt((Pt){0, 0}, (Pt){3, 4}, 5) == 0);
    assert(geom_cmp_pt_pt((Pt){0, 0}, (Pt){3, 4}, 4) == 1);
    assert(geom_cmp_pt_pt((Pt){0, 0}, (Pt){3, 4}, 6) == -1);
    /* point to a segment, perpendicular case: exactly 7 nm above a long segment. */
    assert(geom_cmp_pt_seg((Pt){500, 7}, (Pt){0, 0}, (Pt){1000, 0}, 7) == 0);
    assert(geom_cmp_pt_seg((Pt){500, 7}, (Pt){0, 0}, (Pt){1000, 0}, 6) == 1);
    assert(geom_cmp_pt_seg((Pt){500, 7}, (Pt){0, 0}, (Pt){1000, 0}, 8) == -1);
    /* A tilted segment where the perpendicular distance is not an integer:
       exactness must hold at the integer neighbours. A=(0,0) B=(3,4), P=(4,0):
       cross = 3*0-4*4 = -16, |AB| = 5, dist = 16/5 = 3.2 */
    assert(geom_cmp_pt_seg((Pt){4, 0}, (Pt){0, 0}, (Pt){3, 4}, 3) == 1);
    assert(geom_cmp_pt_seg((Pt){4, 0}, (Pt){0, 0}, (Pt){3, 4}, 4) == -1);
    /* touching and crossing segments are distance 0 */
    assert(geom_cmp_seg_seg((Pt){0, 0}, (Pt){10, 10}, (Pt){0, 10}, (Pt){10, 0}, 0) == 0);
    assert(geom_cmp_seg_seg((Pt){0, 0}, (Pt){10, 10}, (Pt){0, 10}, (Pt){10, 0}, 1) == -1);
    /* rectangles 5 apart on x */
    assert(geom_cmp_rect_rect((Pt){0, 0}, (Pt){10, 10}, (Pt){15, 0}, (Pt){20, 10}, 5) == 0);
    /* T edge cases */
    assert(geom_cmp_pt_pt((Pt){0, 0}, (Pt){0, 0}, -1) == 1);
    assert(geom_cmp_pt_pt((Pt){0, 0}, (Pt){0, 0}, 0) == 0);
    assert(geom_cmp_pt_pt((Pt){-500000000, -500000000}, (Pt){500000000, 500000000},
                          INT64_C(1) << 31) == -1);
    /* Extreme coordinates: no overflow, exact. Distance across the board is
       1e9 along x. */
    assert(geom_cmp_pt_seg((Pt){0, 500000000}, (Pt){-500000000, -500000000},
                           (Pt){500000000, -500000000}, 1000000000) == 0);
    puts("PASS: exact boundary cases");
}

static void conversions(void)
{
    assert(nm_from_mm_ceil(0.15) == 150000);
    assert(nm_from_mm_ceil(0.1500001) == 150001);
    assert(nm_from_mm_ceil(0.0) == 0);
    assert(nm_from_mm_ceil(1.000000001) == 1000001);
    assert(nm_from_mm(1.2345674) == 1234567);
    assert(nm_from_mm(-1.2345676) == -1234568);
    assert(nm_isfinite(1.0) && !nm_isfinite(INFINITY) && !nm_isfinite(NAN));
    puts("PASS: mm->nm conversions");
}

int main(void)
{
    exact_boundaries();
    conversions();
    random_oracle(1000, 20000);          /* dense: many near/touching cases */
    random_oracle(1000000, 20000);       /* ~1 mm */
    random_oracle(500000000, 20000);     /* full board: overflow stress */
    puts("GEOM TEST SUITE: PASS");
    return 0;
}

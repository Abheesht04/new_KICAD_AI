/* Nonlinear device models (Prompt 4): hand-calculated operating points in every
 * region, physical sanity (KCL, beta, signs), central finite-difference Jacobian
 * agreement, region-boundary continuity, PMOS/PNP checked on their own, solved
 * circuits verified by KCL, and Newton failure statuses. */
#include "mna.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

static const double VT = 1.380649e-23 * 300.0 / 1.602176634e-19;

static int close_rel(double got, double want, double rel, double abs_tol)
{
    return fabs(got - want) <= rel * fabs(want) + abs_tol;
}

/* ---------------- diode / limexp ---------------- */
static void test_diode_hardening(void)
{
    double s1, s2;
    /* limexp: value and slope continuous at the limit, linear beyond it. */
    double below = mna_limexp(40.0 - 1e-9, &s1), above = mna_limexp(40.0 + 1e-9, &s2);
    assert(close_rel(below, above, 1e-8, 0) && close_rel(s1, s2, 1e-8, 0));
    assert(close_rel(mna_limexp(41.0, &s1), exp(40.0) * 2.0, 1e-15, 0) && s1 == exp(40.0));
    assert(mna_limexp(-800.0, &s1) == 0.0 && s1 == 0.0);

    MnaDiode d = {1, 0, 1e-14, 1.0, 300.0};
    double i, g;
    mna_diode_evaluate(&d, 0.6, &i, &g);
    assert(close_rel(i, 1e-14 * (exp(0.6 / VT) - 1.0), 1e-13, 0) && close_rel(g, 1e-14 * exp(0.6 / VT) / VT, 1e-13, 0));
    mna_diode_evaluate(&d, -5.0, &i, &g);
    assert(close_rel(i, -1e-14, 1e-12, 0) && g >= 0.0 && g < 1e-40);
    /* The Jacobian matches the current everywhere, including beyond the exponent limit (audit I-10). */
    double v[] = {-5.0, -0.3, 0.0, 0.4, 0.7, 1.0, 1.0336, 1.2, 2.0};
    for (size_t k = 0; k < sizeof v / sizeof v[0]; ++k) {
        double h = 1e-7, ip, im, gp;
        mna_diode_evaluate(&d, v[k] + h, &ip, &gp);
        mna_diode_evaluate(&d, v[k] - h, &im, &gp);
        mna_diode_evaluate(&d, v[k], &i, &g);
        assert(close_rel((ip - im) / (2 * h), g, 1e-6, 1e-20));
    }
}

/* ---------------- BJT ---------------- */
static MnaBjt bjt(int polarity)
{
    MnaBjt b = {1, 2, 3, polarity, 1e-16, 100.0, 1.0, 1.0, 1.0, 300.0};
    return b;
}

static void check_bjt_jacobian(const MnaBjt *b, double vc, double vb, double ve)
{
    MnaBjtEval e, p, m;
    double v[3] = {vc, vb, ve};
    mna_bjt_evaluate(b, vc, vb, ve, &e);
    assert(fabs(e.ic + e.ib + e.ie) <= 1e-15 * (fabs(e.ic) + fabs(e.ib)) + 1e-30); /* KCL */
    for (int j = 0; j < 3; ++j) {
        double h = 1e-7 * fmax(1.0, fabs(v[j])), up[3] = {vc, vb, ve}, dn[3] = {vc, vb, ve};
        up[j] += h;
        dn[j] -= h;
        mna_bjt_evaluate(b, up[0], up[1], up[2], &p);
        mna_bjt_evaluate(b, dn[0], dn[1], dn[2], &m);
        double fd[3] = {(p.ic - m.ic) / (2 * h), (p.ib - m.ib) / (2 * h), (p.ie - m.ie) / (2 * h)};
        double scale = fabs(e.d[0][j]) + fabs(e.d[1][j]) + fabs(e.d[2][j]);
        for (int i = 0; i < 3; ++i) assert(fabs(fd[i] - e.d[i][j]) <= 1e-6 * scale + 1e-24);
    }
}

static void test_bjt(void)
{
    MnaBjt n = bjt(1), p = bjt(-1);
    MnaBjtEval e;
    double kr = 1.0 + 1.0 / n.beta_r;

    /* Forward active NPN: Vbe = 0.65, Vbc = -4.35. Ic = If - Ir (1 + 1/Br), Ib = If/Bf + Ir/Br. */
    double i_f = 1e-16 * (exp(0.65 / VT) - 1), i_r = 1e-16 * (exp(-4.35 / VT) - 1);
    mna_bjt_evaluate(&n, 5.0, 0.65, 0.0, &e);
    assert(close_rel(e.ic, i_f - i_r * kr, 1e-12, 0) && close_rel(e.ib, i_f / 100.0 + i_r, 1e-12, 0));
    assert(close_rel(e.ic / e.ib, 100.0, 1e-7, 0) && e.ie < 0); /* beta_F (reverse leakage ~ Is shifts it ~1e-9); emitter current flows out */

    /* Saturation: both junctions forward; the forced beta is below beta_F. */
    mna_bjt_evaluate(&n, 0.1, 0.75, 0.0, &e);
    assert(e.ic > 0 && e.ib > 0 && e.ic / e.ib < 100.0);

    /* Cutoff: both junctions reverse; only leakage of order Is flows. */
    mna_bjt_evaluate(&n, 5.0, -1.0, 0.0, &e);
    assert(fabs(e.ic) < 1e-15 && fabs(e.ib) < 1e-15);

    /* Reverse active: emitter and collector swap roles; Ic/Ib = -(1 + Br) / Br * Br = -(Br + 1). */
    mna_bjt_evaluate(&n, 0.0, 0.65, 5.0, &e);
    assert(close_rel(e.ic / e.ib, -(n.beta_r + 1.0), 1e-9, 0) && e.ic < 0);

    /* PNP forward active, checked on its own: Veb = 0.65, Vcb = -4.35. Currents flow OUT of the
     * collector and base, so Ic, Ib (into the terminals) are negative. */
    i_f = 1e-16 * (exp(0.65 / VT) - 1);
    i_r = 1e-16 * (exp(-4.35 / VT) - 1);
    mna_bjt_evaluate(&p, 0.0, 4.35, 5.0, &e);
    assert(close_rel(e.ic, -(i_f - i_r * kr), 1e-12, 0) && close_rel(e.ib, -(i_f / 100.0 + i_r), 1e-12, 0));
    assert(e.ic < 0 && e.ib < 0 && e.ie > 0 && close_rel(e.ic / e.ib, 100.0, 1e-7, 0));

    double points[][3] = {{5, 0.65, 0}, {0.1, 0.75, 0}, {5, -1, 0}, {0, 0.65, 5}, {0.2, 0.2, 0}, {3, 1.2, 0.1}};
    for (size_t k = 0; k < sizeof points / sizeof points[0]; ++k) {
        check_bjt_jacobian(&n, points[k][0], points[k][1], points[k][2]);
        check_bjt_jacobian(&p, -points[k][0], -points[k][1], -points[k][2]);
        check_bjt_jacobian(&p, 5 - points[k][0], 5 - points[k][1], 5 - points[k][2]);
    }
}

/* ---------------- MOSFET ---------------- */
static MnaMosfet mos(int polarity, double vto)
{
    MnaMosfet m = {1, 2, 3, polarity, vto, 1e-3, 1e-6, 1e-6, 0.02}; /* beta = 1 mA/V^2 */
    return m;
}

static void check_mos_jacobian(const MnaMosfet *m, double vd, double vg, double vs)
{
    MnaMosfetEval e, p, q;
    double v[3] = {vd, vg, vs};
    mna_mosfet_evaluate(m, vd, vg, vs, &e);
    double an[3] = {e.d_vd, e.d_vg, e.d_vs}, scale = fabs(e.d_vd) + fabs(e.d_vg) + fabs(e.d_vs);
    for (int j = 0; j < 3; ++j) {
        double h = 1e-7 * fmax(1.0, fabs(v[j])), up[3] = {vd, vg, vs}, dn[3] = {vd, vg, vs};
        up[j] += h;
        dn[j] -= h;
        mna_mosfet_evaluate(m, up[0], up[1], up[2], &p);
        mna_mosfet_evaluate(m, dn[0], dn[1], dn[2], &q);
        assert(fabs((p.id - q.id) / (2 * h) - an[j]) <= 1e-6 * scale + 1e-15);
    }
}

static void test_nmos(void)
{
    MnaMosfet n = mos(1, 1.0);
    MnaMosfetEval e;
    const double beta = 1e-3, lambda = 0.02;

    mna_mosfet_evaluate(&n, 5.0, 0.5, 0.0, &e); /* Vgs < Vto */
    assert(e.region == MNA_MOS_CUTOFF && e.id == 0 && e.d_vd == 0 && e.d_vg == 0 && e.d_vs == 0);
    mna_mosfet_evaluate(&n, 0.5, 3.0, 0.0, &e); /* Vov = 2 > Vds = 0.5 */
    assert(e.region == MNA_MOS_TRIODE && close_rel(e.id, beta * (2 * 0.5 - 0.125) * (1 + lambda * 0.5), 1e-14, 0));
    mna_mosfet_evaluate(&n, 5.0, 3.0, 0.0, &e); /* Vds >= Vov */
    assert(e.region == MNA_MOS_SATURATION && close_rel(e.id, 0.5 * beta * 4.0 * (1 + lambda * 5.0), 1e-14, 0));
    assert(close_rel(e.d_vg, beta * 2.0 * (1 + lambda * 5.0), 1e-14, 0) && close_rel(e.d_vd, 0.5 * beta * 4.0 * lambda, 1e-14, 0));

    /* Reverse: Vds = -0.5 conducts source to drain, mirror image of the triode case. */
    mna_mosfet_evaluate(&n, 0.0, 3.0, 0.5, &e);
    assert(e.reversed && e.region == MNA_MOS_TRIODE && close_rel(e.id, -beta * (2 * 0.5 - 0.125) * (1 + lambda * 0.5), 1e-14, 0));

    /* Continuity of Id, gm and gds at the triode/saturation boundary (Vds = Vov = 2) and at Vds = 0. */
    MnaMosfetEval a, b;
    mna_mosfet_evaluate(&n, 2.0 - 1e-9, 3.0, 0.0, &a);
    mna_mosfet_evaluate(&n, 2.0 + 1e-9, 3.0, 0.0, &b);
    assert(a.region == MNA_MOS_TRIODE && b.region == MNA_MOS_SATURATION);
    assert(close_rel(a.id, b.id, 1e-8, 0) && close_rel(a.d_vg, b.d_vg, 1e-7, 0) && fabs(a.d_vd - b.d_vd) < 1e-10);
    mna_mosfet_evaluate(&n, 1e-12, 3.0, 0.0, &a);
    mna_mosfet_evaluate(&n, -1e-12, 3.0, 0.0, &b);
    assert(fabs(a.id) < 1e-14 && fabs(b.id) < 1e-14 && close_rel(a.d_vd, b.d_vd, 1e-9, 0));
    mna_mosfet_evaluate(&n, 5.0, 1.0 + 1e-12, 0.0, &a); /* just above threshold: Id and gm -> 0 */
    assert(a.id < 1e-24 && a.d_vg < 1e-14);

    double points[][3] = {{5, 0.5, 0}, {0.5, 3, 0}, {5, 3, 0}, {0, 3, 0.5}, {3, 4, 1}, {1.5, 2.2, 0.3}, {-2, 0, -0.5}};
    for (size_t k = 0; k < sizeof points / sizeof points[0]; ++k) check_mos_jacobian(&n, points[k][0], points[k][1], points[k][2]);
}

static void test_pmos(void)
{
    /* Enhancement PMOS, Vtp = -1, checked with PMOS hand formulas:
     * saturation |Id| = beta/2 (Vsg - |Vtp|)^2 (1 + lambda Vsd), current flows out of the drain. */
    MnaMosfet p = mos(-1, -1.0);
    MnaMosfetEval e;
    const double beta = 1e-3, lambda = 0.02;

    mna_mosfet_evaluate(&p, 0.0, 4.5, 5.0, &e); /* Vsg = 0.5 < |Vtp| */
    assert(e.region == MNA_MOS_CUTOFF && e.id == 0);
    mna_mosfet_evaluate(&p, 0.0, 2.0, 5.0, &e); /* Vsg = 3, Vsd = 5: saturation */
    assert(e.region == MNA_MOS_SATURATION && close_rel(e.id, -0.5 * beta * 4.0 * (1 + lambda * 5.0), 1e-14, 0));
    mna_mosfet_evaluate(&p, 4.5, 2.0, 5.0, &e); /* Vsd = 0.5 < Vsg - |Vtp| = 2: triode */
    assert(e.region == MNA_MOS_TRIODE && close_rel(e.id, -beta * (2 * 0.5 - 0.125) * (1 + lambda * 0.5), 1e-14, 0));
    /* Raising the gate turns a PMOS off (opposite of NMOS). */
    MnaMosfetEval low, high;
    mna_mosfet_evaluate(&p, 0.0, 1.0, 5.0, &low);
    mna_mosfet_evaluate(&p, 0.0, 3.0, 5.0, &high);
    assert(low.id < high.id && high.id < 0 && low.d_vg > 0);

    double points[][3] = {{0, 4.5, 5}, {0, 2, 5}, {4.5, 2, 5}, {5, 2, 4.5}, {2, 0, 3}, {-1, -3, 0}};
    for (size_t k = 0; k < sizeof points / sizeof points[0]; ++k) check_mos_jacobian(&p, points[k][0], points[k][1], points[k][2]);
}

/* ---------------- solved circuits ---------------- */
static void solve_ok(MnaCircuit *c, MnaResult *r)
{
    MnaOptions o;
    MnaValidation v = {0};
    mna_options_default(&o);
    mna_solve(c, &o, r);
    assert(r->status == MNA_STATUS_SUCCESS);
    assert(mna_validate(c, r, &o, &v) && v.valid);
}

static void test_circuits(void)
{
    MnaCircuit c;
    MnaResult r = {0}, again = {0};
    MnaBjtEval be;
    MnaMosfetEval me;

    /* NPN common emitter: Vcc = 5 V (node 1), Rb = 470k to the base (2), Rc = 1k to the collector (3).
     * Ib ~ 9 uA, Ic ~ 0.9 mA, Vc ~ 4.1 V: forward active (100k would saturate it). */
    mna_circuit_init(&c, 3);
    MnaBjt q = {3, 2, 0, 1, 1e-16, 100.0, 1.0, 1.0, 1.0, 300.0};
    assert(mna_add_voltage_source(&c, 1, 0, 5.0) && mna_add_resistor(&c, 1, 2, 4.7e5) && mna_add_resistor(&c, 1, 3, 1e3));
    assert(mna_add_bjt(&c, &q));
    solve_ok(&c, &r);
    double vb = mna_node_voltage(&r, 2), vcol = mna_node_voltage(&r, 3);
    mna_bjt_evaluate(&q, vcol, vb, 0.0, &be);
    assert(close_rel((5.0 - vb) / 4.7e5, be.ib, 1e-9, 1e-15) && close_rel((5.0 - vcol) / 1e3, be.ic, 1e-9, 1e-15));
    assert(vb > 0.5 && vb < 0.8 && vcol > vb && close_rel(be.ic / be.ib, 100.0, 1e-6, 0)); /* forward active */
    solve_ok(&c, &again); /* deterministic: zero initial guess, identical result */
    assert(mna_node_voltage(&again, 3) == vcol && again.iterations == r.iterations);
    mna_result_free(&again);

    /* Too few iterations is NOT_CONVERGED, never success. */
    MnaOptions o;
    mna_options_default(&o);
    o.max_iterations = 2;
    mna_solve(&c, &o, &again);
    assert(again.status == MNA_STATUS_NOT_CONVERGED);
    mna_result_free(&again);
    mna_result_free(&r);
    mna_circuit_free(&c);

    /* NMOS with Rd = 1k from Vdd = 5 V (node 1) to the drain (3), gate held at 3 V (node 2). */
    mna_circuit_init(&c, 3);
    MnaMosfet n = {3, 2, 0, 1, 1.0, 1e-3, 1e-6, 1e-6, 0.02};
    assert(mna_add_voltage_source(&c, 1, 0, 5.0) && mna_add_voltage_source(&c, 2, 0, 3.0) && mna_add_resistor(&c, 1, 3, 1e3));
    assert(mna_add_mosfet(&c, &n));
    solve_ok(&c, &r);
    double vd = mna_node_voltage(&r, 3);
    mna_mosfet_evaluate(&n, vd, 3.0, 0.0, &me);
    assert(close_rel((5.0 - vd) / 1e3, me.id, 1e-9, 1e-15) && me.region == MNA_MOS_SATURATION);
    /* By hand: Vd = 5 - 1k * 0.5 mA/V^2 * 2^2 * (1 + 0.02 Vd)  =>  Vd = 3 / 1.04 >= Vov = 2. */
    assert(close_rel(vd, 3.0 / 1.04, 1e-9, 0));
    mna_result_free(&r);
    mna_circuit_free(&c);

    /* PMOS high-side: source at 5 V (node 1), gate at 2 V (node 2), Rd = 1k from the drain (3) to ground. */
    mna_circuit_init(&c, 3);
    MnaMosfet pm = {3, 2, 1, -1, -1.0, 1e-3, 1e-6, 1e-6, 0.02};
    assert(mna_add_voltage_source(&c, 1, 0, 5.0) && mna_add_voltage_source(&c, 2, 0, 2.0) && mna_add_resistor(&c, 3, 0, 1e3));
    assert(mna_add_mosfet(&c, &pm));
    solve_ok(&c, &r);
    vd = mna_node_voltage(&r, 3);
    mna_mosfet_evaluate(&pm, vd, 2.0, 5.0, &me);
    assert(close_rel(vd / 1e3, -me.id, 1e-9, 1e-15) && vd > 0.0 && vd < 5.0);
    mna_result_free(&r);
    mna_circuit_free(&c);

    /* Invalid parameters are rejected at construction. */
    mna_circuit_init(&c, 3);
    MnaBjt bad = q;
    bad.beta_f = 0;
    assert(!mna_add_bjt(&c, &bad));
    bad = q;
    bad.polarity = 0;
    assert(!mna_add_bjt(&c, &bad));
    MnaMosfet badm = n;
    badm.lambda = -0.1;
    assert(!mna_add_mosfet(&c, &badm));
    badm = n;
    badm.vto = NAN;
    assert(!mna_add_mosfet(&c, &badm));
    badm = n;
    badm.node_drain = 9;
    assert(!mna_add_mosfet(&c, &badm));
    mna_circuit_free(&c);
}

int main(void)
{
    test_diode_hardening();
    test_bjt();
    test_nmos();
    test_pmos();
    test_circuits();
    puts("nonlinear_models: PASS");
    return 0;
}

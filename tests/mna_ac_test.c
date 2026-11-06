/* AC small-signal analysis against independently written closed-form answers.
 * Expected values use real arithmetic on textbook transfer functions; the
 * production AC code is never used to compute its own expectations. */
#include "model_registry.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static const double PI = 3.14159265358979323846;
static ModelRegistry reg;

typedef struct {
    const char *model;
    size_t n0, n1;
    double param;   /* primary parameter (R, C, L, V, I); unused for the diode */
    Cplx ac;        /* source excitation */
} Inst;

static const DeviceModelSpec *spec(const char *id)
{
    const DeviceModelSpec *s = model_registry_find_device(&reg, id);
    assert(s);
    return s;
}

/* Builds and solves the AC system for the instances at angular frequency w. */
static MnaAcStatus solve(const Inst *inst, size_t count, size_t nodes, double w, const MnaResult *op, Cplx *x,
                         MnaAcSystem *out_sys)
{
    size_t branches = 0;
    for (size_t i = 0; i < count; ++i) branches += spec(inst[i].model)->ac_branch_count;
    MnaAcSystem sys;
    assert(mna_ac_init(&sys, nodes, branches));
    size_t branch = 0;
    for (size_t i = 0; i < count; ++i) {
        const DeviceModelSpec *s = spec(inst[i].model);
        size_t n[2] = {inst[i].n0, inst[i].n1};
        double params[2] = {inst[i].param, 1.0};
        if (!strcmp(inst[i].model, "shockley.diode")) { params[0] = 1e-14; params[1] = 1.0; }
        DeviceAcContext ctx = {w, op, inst[i].ac, branch};
        assert(device_model_supports(s, DEVICE_ANALYSIS_AC));
        assert(s->stamp_ac(&sys, n, params, &ctx));
        branch += s->ac_branch_count;
    }
    MnaAcStatus st = mna_ac_solve(&sys, x);
    if (out_sys) *out_sys = sys;
    else mna_ac_free(&sys);
    return st;
}

static int near(double a, double b, double tol) { return fabs(a - b) <= tol * (1.0 + fabs(b)); }
static double deg(double rad) { return rad * 180.0 / PI; }

static void test_resistor_divider_phase(void)
{
    /* 1 V at 30 degrees into 1k + 3k: Vout = 0.75 V at 30 degrees, for every frequency. */
    Inst c[] = {{"ideal.voltage_source", 1, 0, 0, {0}}, {"ideal.resistor", 1, 2, 1000, {0}}, {"ideal.resistor", 2, 0, 3000, {0}}};
    c[0].ac = cplx_polar_deg(1.0, 30.0);
    Cplx x[3];
    for (double f = 0; f <= 1e6; f = f ? f * 100 : 1) {
        assert(solve(c, 3, 2, 2 * PI * f, NULL, x, NULL) == MNA_AC_OK);
        assert(near(cplx_abs(x[1]), 0.75, 1e-12) && near(cplx_arg_deg(x[1]), 30.0, 1e-10));
        /* Source branch current flows + to - through the source: -Vin/4k, i.e. 0.25 mA at -150 degrees. */
        assert(near(cplx_abs(x[2]), 1.0 / 4000.0, 1e-12) && near(cplx_arg_deg(x[2]), -150.0, 1e-10));
    }
}

static void test_rc_low_pass(void)
{
    const double R = 1000.0, C = 1e-6, fc = 1.0 / (2 * PI * R * C);
    assert(fabs(fc - 159.154943) < 1e-6);
    Inst c[] = {{"ideal.voltage_source", 1, 0, 0, {1, 0}}, {"ideal.resistor", 1, 2, R, {0}}, {"ideal.capacitor", 2, 0, C, {0}}};
    Cplx x[3];
    assert(solve(c, 3, 2, 2 * PI * fc, NULL, x, NULL) == MNA_AC_OK);
    assert(near(cplx_abs(x[1]), 1.0 / sqrt(2.0), 1e-12) && near(cplx_arg_deg(x[1]), -45.0, 1e-10));
    double freqs[] = {0.1, 1, 10, 100, fc, 1e3, 1e4, 1e5, 1e7};
    for (size_t i = 0; i < sizeof freqs / sizeof freqs[0]; ++i) {
        double wrc = 2 * PI * freqs[i] * R * C;
        assert(solve(c, 3, 2, 2 * PI * freqs[i], NULL, x, NULL) == MNA_AC_OK);
        assert(near(cplx_abs(x[1]), 1.0 / sqrt(1.0 + wrc * wrc), 1e-11));    /* |H| = 1/sqrt(1+(wRC)^2) */
        assert(near(cplx_arg_deg(x[1]), -deg(atan(wrc)), 1e-9));            /* phase = -atan(wRC) */
    }
}

static void test_rc_high_pass(void)
{
    const double R = 2200.0, C = 47e-9;
    Inst c[] = {{"ideal.voltage_source", 1, 0, 0, {1, 0}}, {"ideal.capacitor", 1, 2, C, {0}}, {"ideal.resistor", 2, 0, R, {0}}};
    Cplx x[3];
    double freqs[] = {1, 100, 1539.2, 1e4, 1e6};
    for (size_t i = 0; i < sizeof freqs / sizeof freqs[0]; ++i) {
        double wrc = 2 * PI * freqs[i] * R * C;
        assert(solve(c, 3, 2, 2 * PI * freqs[i], NULL, x, NULL) == MNA_AC_OK);
        assert(near(cplx_abs(x[1]), wrc / sqrt(1.0 + wrc * wrc), 1e-11));  /* |H| = wRC/sqrt(1+(wRC)^2) */
        assert(near(cplx_arg_deg(x[1]), 90.0 - deg(atan(wrc)), 1e-9));     /* phase = 90 - atan(wRC) */
    }
}

static void test_rl(void)
{
    /* R in series, L to ground: H = jwL / (R + jwL); inductor current = Vin / (R + jwL). */
    const double R = 50.0, L = 1e-3;
    Inst c[] = {{"ideal.voltage_source", 1, 0, 0, {1, 0}}, {"ideal.resistor", 1, 2, R, {0}}, {"ideal.inductor", 2, 0, L, {0}}};
    Cplx x[4];
    double freqs[] = {10, 7957.75, 1e5, 1e7};
    for (size_t i = 0; i < sizeof freqs / sizeof freqs[0]; ++i) {
        double wl = 2 * PI * freqs[i] * L;
        assert(solve(c, 3, 2, 2 * PI * freqs[i], NULL, x, NULL) == MNA_AC_OK);
        assert(near(cplx_abs(x[1]), wl / sqrt(R * R + wl * wl), 1e-11));
        assert(near(cplx_arg_deg(x[1]), 90.0 - deg(atan(wl / R)), 1e-9));
        assert(near(cplx_abs(x[3]), 1.0 / sqrt(R * R + wl * wl), 1e-11)); /* branch unknown 1 = inductor */
        assert(near(cplx_arg_deg(x[3]), -deg(atan(wl / R)), 1e-9));
    }
}

static void test_series_rlc_resonance(void)
{
    const double R = 10.0, L = 10e-3, C = 1e-6, f0 = 1.0 / (2 * PI * sqrt(L * C));
    /* V -> R -> L -> C -> ground; nodes: 1 = source, 2 = R/L, 3 = L/C. */
    Inst c[] = {{"ideal.voltage_source", 1, 0, 0, {1, 0}}, {"ideal.resistor", 1, 2, R, {0}},
                {"ideal.inductor", 2, 3, L, {0}}, {"ideal.capacitor", 3, 0, C, {0}}};
    Cplx x[5];
    double best = 0;
    for (double f = f0 / 4; f < f0 * 4; f *= 1.01) {
        double w = 2 * PI * f, reactance = w * L - 1.0 / (w * C);
        assert(solve(c, 4, 3, w, NULL, x, NULL) == MNA_AC_OK);
        double current = cplx_abs(x[4]);                                   /* inductor branch current */
        assert(near(current, 1.0 / sqrt(R * R + reactance * reactance), 1e-10));
        if (current > best) best = current;
    }
    assert(solve(c, 4, 3, 2 * PI * f0, NULL, x, NULL) == MNA_AC_OK);
    assert(near(cplx_abs(x[4]), 1.0 / R, 1e-9) && fabs(cplx_arg_deg(x[4])) < 1e-6); /* purely resistive at f0 */
    assert(cplx_abs(x[4]) >= best);
    assert(near(cplx_abs(x[2]), 1.0 / (2 * PI * f0 * C * R), 1e-9));               /* |Vc| = Q * Vin */
}

static void test_superposition_and_sources(void)
{
    /* V1 = 1 V at 0 deg through R1, V2 = 2 V at 90 deg through R2, R3 to ground:
     * Vout = (V1/R1 + V2/R2) / (1/R1 + 1/R2 + 1/R3). */
    const double R1 = 1000, R2 = 2000, R3 = 500;
    Inst both[] = {{"ideal.voltage_source", 1, 0, 0, {1, 0}}, {"ideal.voltage_source", 2, 0, 0, {0, 2}},
                   {"ideal.resistor", 1, 3, R1, {0}}, {"ideal.resistor", 2, 3, R2, {0}}, {"ideal.resistor", 3, 0, R3, {0}}};
    Cplx x[5], a[5], b[5];
    double g = 1 / R1 + 1 / R2 + 1 / R3;
    assert(solve(both, 5, 3, 2 * PI * 1e3, NULL, x, NULL) == MNA_AC_OK);
    assert(near(x[2].re, (1 / R1) / g, 1e-12) && near(x[2].im, (2 / R2) / g, 1e-12));
    Inst only1[5], only2[5];
    memcpy(only1, both, sizeof both);
    memcpy(only2, both, sizeof both);
    only1[1].ac = cplx(0, 0);
    only2[0].ac = cplx(0, 0);
    assert(solve(only1, 5, 3, 2 * PI * 1e3, NULL, a, NULL) == MNA_AC_OK);
    assert(solve(only2, 5, 3, 2 * PI * 1e3, NULL, b, NULL) == MNA_AC_OK);
    assert(near(x[2].re, a[2].re + b[2].re, 1e-12) && near(x[2].im, a[2].im + b[2].im, 1e-12));

    /* A DC-only source contributes no AC: everything is zero. */
    Inst dc_only[] = {{"ideal.voltage_source", 1, 0, 5.0, {0, 0}}, {"ideal.resistor", 1, 0, 1000, {0}}};
    Cplx y[2];
    assert(solve(dc_only, 2, 1, 2 * PI * 50, NULL, y, NULL) == MNA_AC_OK && cplx_abs(y[0]) == 0 && cplx_abs(y[1]) == 0);

    /* Current source 1 mA at -60 deg flowing from ground through the source into node 1, 1k load: 1 V at -60 deg. */
    Inst cur[] = {{"ideal.current_source", 0, 1, 0, {0}}, {"ideal.resistor", 1, 0, 1000, {0}}};
    cur[0].ac = cplx_polar_deg(1e-3, -60);
    assert(solve(cur, 2, 1, 2 * PI * 50, NULL, y, NULL) == MNA_AC_OK);
    assert(near(cplx_abs(y[0]), 1.0, 1e-12) && near(cplx_arg_deg(y[0]), -60.0, 1e-10));
}

static void test_zero_frequency_and_singular(void)
{
    Cplx x[4];
    /* w = 0: capacitor open, inductor short. */
    Inst rc[] = {{"ideal.voltage_source", 1, 0, 0, {1, 0}}, {"ideal.resistor", 1, 2, 1000, {0}}, {"ideal.capacitor", 2, 0, 1e-6, {0}}};
    assert(solve(rc, 3, 2, 0.0, NULL, x, NULL) == MNA_AC_OK && near(x[1].re, 1.0, 1e-14) && x[1].im == 0.0);
    Inst rl[] = {{"ideal.voltage_source", 1, 0, 0, {1, 0}}, {"ideal.resistor", 1, 2, 50, {0}}, {"ideal.inductor", 2, 0, 1e-3, {0}}};
    assert(solve(rl, 3, 2, 0.0, NULL, x, NULL) == MNA_AC_OK && cplx_abs(x[1]) < 1e-15 && near(x[3].re, 1.0 / 50, 1e-12));
    /* A node joined only by capacitors floats at w = 0 (singular) but is defined at w > 0. */
    Inst cc[] = {{"ideal.voltage_source", 1, 0, 0, {1, 0}}, {"ideal.capacitor", 1, 2, 1e-6, {0}}, {"ideal.capacitor", 2, 0, 1e-6, {0}}};
    assert(solve(cc, 3, 2, 0.0, NULL, x, NULL) == MNA_AC_SINGULAR);
    assert(solve(cc, 3, 2, 2 * PI * 100, NULL, x, NULL) == MNA_AC_OK && near(x[1].re, 0.5, 1e-12));
    /* A floating node with no element at all is singular at every frequency. */
    Inst floating[] = {{"ideal.voltage_source", 1, 0, 0, {1, 0}}, {"ideal.resistor", 1, 0, 100, {0}}};
    assert(solve(floating, 2, 2, 2 * PI * 100, NULL, x, NULL) == MNA_AC_SINGULAR);
}

static void test_guards(void)
{
    Cplx x[2];
    MnaAcSystem sys;
    assert(mna_ac_init(&sys, 1, 0));
    mna_ac_stamp_admittance(&sys, 1, 0, cplx(1, 0));
    mna_ac_stamp_branch(&sys, 1, 0, 3); /* branch does not exist */
    assert(mna_ac_solve(&sys, x) == MNA_AC_NON_FINITE);
    mna_ac_clear(&sys);
    mna_ac_stamp_admittance(&sys, 1, 0, cplx(NAN, 0));
    assert(mna_ac_solve(&sys, x) == MNA_AC_NON_FINITE);
    mna_ac_free(&sys);
    Inst nan_source[] = {{"ideal.voltage_source", 1, 0, 0, {NAN, 0}}, {"ideal.resistor", 1, 0, 1, {0}}};
    assert(solve(nan_source, 2, 1, 1.0, NULL, x, NULL) == MNA_AC_NON_FINITE);
}

static void test_frequency_grid(void)
{
    double f[5], g[5];
    assert(mna_ac_frequency_grid(10, 1e5, 5, 1, f));
    assert(f[0] == 10 && f[4] == 1e5);
    for (int i = 0; i < 5; ++i) assert(near(f[i], 10 * pow(10, i), 1e-12));
    for (int i = 1; i < 5; ++i) assert(f[i] > f[i - 1]);
    assert(mna_ac_frequency_grid(0, 100, 5, 0, g) && g[0] == 0 && g[2] == 50 && g[4] == 100);
    assert(mna_ac_frequency_grid(50, 50, 1, 1, g) && g[0] == 50);
    assert(!mna_ac_frequency_grid(0, 100, 5, 1, g));     /* log needs start > 0 */
    assert(!mna_ac_frequency_grid(100, 10, 5, 1, g));    /* stop < start */
    assert(!mna_ac_frequency_grid(10, 100, 0, 1, g));
    assert(!mna_ac_frequency_grid(10, 100, 1, 1, g));    /* one point needs start == stop */
    assert(!mna_ac_frequency_grid(10, 10, 3, 1, g));
    assert(!mna_ac_frequency_grid(NAN, 100, 3, 1, g) && !mna_ac_frequency_grid(1, INFINITY, 3, 1, g));
    assert(!mna_ac_frequency_grid(1, 10, 100001, 1, g));
    /* Repeated sweeps are bit-identical. */
    double h[5];
    assert(mna_ac_frequency_grid(10, 1e5, 5, 1, h) && !memcmp(f, h, sizeof f));
    Inst rc[] = {{"ideal.voltage_source", 1, 0, 0, {1, 0}}, {"ideal.resistor", 1, 2, 1000, {0}}, {"ideal.capacitor", 2, 0, 1e-6, {0}}};
    Cplx x1[3], x2[3];
    assert(solve(rc, 3, 2, 1234.5, NULL, x1, NULL) == MNA_AC_OK && solve(rc, 3, 2, 1234.5, NULL, x2, NULL) == MNA_AC_OK);
    assert(!memcmp(x1, x2, sizeof x1));
}

/* Diode linearised at its DC operating point: Vout/Vin = 1 / (1 + R*gd), with
 * gd = Is/(n*Vt) * exp(Vd/(n*Vt)) recomputed here from the solved Vd. */
static void test_diode_small_signal(void)
{
    const double R = 1000.0, Is = 1e-14, vt = 1.380649e-23 * 300.0 / 1.602176634e-19;
    const DeviceModelSpec *v = spec("ideal.voltage_source"), *r = spec("ideal.resistor"), *d = spec("shockley.diode");
    DeviceStampContext dc = {DEVICE_ANALYSIS_DC, 0.0, NULL};
    MnaCircuit m;
    MnaResult op = {0};
    MnaOptions o;
    mna_options_default(&o);
    mna_circuit_init(&m, 2);
    size_t vin[2] = {1, 0}, res[2] = {1, 2}, diode[2] = {0, 2}; /* cathode at ground, anode at node 2 */
    double five = 5.0, rr = R, shockley[2] = {Is, 1.0};
    assert(v->stamp_dc(&m, vin, &five, &dc) && r->stamp_dc(&m, res, &rr, &dc) && d->stamp_dc(&m, diode, shockley, &dc));
    mna_solve(&m, &o, &op);
    assert(op.status == MNA_STATUS_SUCCESS);
    double vd = mna_node_voltage(&op, 2), gd = Is / vt * exp(vd / vt);
    assert(gd > 0.01 && gd < 1.0); /* forward biased, a few mA */
    Inst c[] = {{"ideal.voltage_source", 1, 0, 5.0, {1e-3, 0}}, {"ideal.resistor", 1, 2, R, {0}}, {"shockley.diode", 0, 2, 0, {0}}};
    Cplx x[3];
    assert(solve(c, 3, 2, 2 * PI * 1e3, &op, x, NULL) == MNA_AC_OK);
    double expected = 1e-3 / (1.0 + R * gd);
    assert(fabs(x[1].re - expected) <= 1e-9 * expected && fabs(x[1].im) < 1e-18);
    /* Without an operating point the diode cannot be linearised. */
    MnaAcSystem sys;
    assert(mna_ac_init(&sys, 2, 0));
    DeviceAcContext none = {1.0, NULL, {0, 0}, 0};
    size_t n[2] = {0, 2};
    assert(!d->stamp_ac(&sys, n, shockley, &none));
    mna_ac_free(&sys);
    mna_result_free(&op);
    mna_circuit_free(&m);
}

int main(void)
{
    char err[256];
    model_registry_init(&reg);
    assert(model_registry_register_builtin_devices(&reg, err, sizeof err));
    test_resistor_divider_phase();
    test_rc_low_pass();
    test_rc_high_pass();
    test_rl();
    test_series_rlc_resonance();
    test_superposition_and_sources();
    test_zero_frequency_and_singular();
    test_guards();
    test_frequency_grid();
    test_diode_small_signal();
    model_registry_free(&reg);
    puts("mna_ac: PASS");
    return 0;
}

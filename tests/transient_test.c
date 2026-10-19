/* Transient engine (Prompt 5): analytical responses for Backward Euler and
 * trapezoidal integration, exact discrete energy behaviour of an LC tank, a
 * nonlinear diode + capacitor case, the t = 0 discontinuity, transactional
 * state with rollback after injected stamp / solve / commit failures, and
 * input validation. Test-only specs are defined here and never registered
 * for projects. */
#include "transient.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static ModelRegistry reg;
static const double VT = 1.380649e-23 * 300.0 / 1.602176634e-19;

static const DeviceModelSpec *spec(const char *id)
{
    const DeviceModelSpec *s = model_registry_find_device(&reg, id);
    assert(s);
    return s;
}

static TransientDevice dev(const DeviceModelSpec *m, size_t n0, size_t n1, double p0, const char *label)
{
    TransientDevice d;
    memset(&d, 0, sizeof d);
    d.model = m;
    d.nodes[0] = n0;
    d.nodes[1] = n1;
    d.params[0] = p0;
    d.label = label;
    if (!strcmp(m->id, "shockley.diode")) {
        d.params[0] = 1e-14;
        d.params[1] = 1.0;
    }
    return d;
}

static int rel(double got, double want, double tol) { return fabs(got - want) <= tol * fmax(fabs(want), 1e-300); }

/* Runs `steps` steps, storing node `node` of each accepted solution in out[1..steps]. */
static void run_steps(TransientRun *run, size_t steps, size_t node, double *out)
{
    TransientFailure f;
    for (size_t k = 1; k <= steps; ++k) {
        MnaResult r = {0};
        assert(transient_step(run, &r, &f) == TRANSIENT_OK);
        if (out) out[k] = mna_node_voltage(&r, node);
        mna_result_free(&r);
    }
}

static void test_constant_current_charging(void)
{
    /* 1 mA from ground through the source into node 1, C = 1 uF: V(t) = I t / C exactly (both methods). */
    for (int m = 0; m < 2; ++m) {
        TransientDevice d[] = {dev(spec("ideal.current_source"), 0, 1, 1e-3, "I1"), dev(spec("ideal.capacitor"), 1, 0, 1e-6, "C1")};
        TransientRun run;
        double v[51];
        assert(transient_init(&run, d, 2, 1, m ? TRANSIENT_TRAPEZOIDAL : TRANSIENT_BACKWARD_EULER, 1e-4, NULL) == TRANSIENT_OK);
        run_steps(&run, 50, 1, v);
        for (int k = 1; k <= 50; ++k) assert(rel(v[k], 1e-3 * k * 1e-4 / 1e-6, 1e-12));
        assert(rel(run.accepted[1][1], 1e-3, 1e-10)); /* capacitor current state = I */
        transient_free(&run);
    }
}

static void test_rc_step(void)
{
    /* 1 V step into R = 1k, C = 1 uF (tau = 1 ms), dt = 10 us: a = dt / tau = 0.01. */
    const double a = 0.01;
    double be[101], tr[101];
    TransientDevice d[] = {dev(spec("ideal.voltage_source"), 1, 0, 1.0, "V1"), dev(spec("ideal.resistor"), 1, 2, 1000, "R1"),
                           dev(spec("ideal.capacitor"), 2, 0, 1e-6, "C1")};
    TransientRun run;
    assert(transient_init(&run, d, 3, 2, TRANSIENT_BACKWARD_EULER, 1e-5, NULL) == TRANSIENT_OK);
    run_steps(&run, 100, 2, be);
    transient_free(&run);
    assert(transient_init(&run, d, 3, 2, TRANSIENT_TRAPEZOIDAL, 1e-5, NULL) == TRANSIENT_OK);
    run_steps(&run, 100, 2, tr);
    transient_free(&run);

    double want_tr = a / (1 + a); /* first step is Backward Euler (t = 0+ discontinuity) */
    for (int k = 1; k <= 100; ++k) {
        assert(rel(be[k], 1.0 - pow(1 + a, -k), 1e-11));              /* BE: v_n = 1 - (1+a)^-n */
        if (k > 1) want_tr = ((1 - a / 2) * want_tr + a) / (1 + a / 2); /* TR recurrence */
        assert(rel(tr[k], want_tr, 1e-11));
    }
    double exact = 1.0 - exp(-1.0);
    assert(fabs(tr[100] - exact) < fabs(be[100] - exact) / 50); /* second order vs first order */
    assert(fabs(be[100] - exact) < 2e-3);
}

static void test_rl_step(void)
{
    /* 1 V into R = 10 ohm (node 1 -> 2) and L = 10 mH (node 2 -> ground): i = (1 - V2) / R, tau = 1 ms. */
    const double R = 10, L = 10e-3, dt = 1e-5, a = R * dt / L;
    TransientDevice d[] = {dev(spec("ideal.voltage_source"), 1, 0, 1.0, "V1"), dev(spec("ideal.resistor"), 1, 2, R, "R1"),
                           dev(spec("ideal.inductor"), 2, 0, L, "L1")};
    for (int m = 0; m < 2; ++m) {
        TransientRun run;
        double v[201], i_want = 0;
        assert(transient_init(&run, d, 3, 2, m ? TRANSIENT_TRAPEZOIDAL : TRANSIENT_BACKWARD_EULER, dt, NULL) == TRANSIENT_OK);
        run_steps(&run, 200, 2, v);
        for (int k = 1; k <= 200; ++k) {
            if (m == 0 || k == 1) i_want = (i_want + dt / L) / (1 + a);            /* BE */
            else i_want = ((1 - a / 2) * i_want + dt / L) / (1 + a / 2);           /* TR */
            assert(rel((1.0 - v[k]) / R, i_want, 1e-10));
        }
        assert(rel(run.accepted[2][0], i_want, 1e-10)); /* inductor state[0] = current */
        assert(fabs(i_want - (1 - exp(-2.0)) / R) < (m ? 1e-5 : 2e-3) / R);
        transient_free(&run);
    }
}

static void test_lc_energy(void)
{
    /* 1 V step into L = 1 mH (node 1 -> 2) and C = 1 uF (node 2 -> ground), no resistance.
     * With u = v_C - 1 and h = dt / sqrt(LC), E = C u^2 / 2 + L i^2 / 2 starts at C / 2.
     * Backward Euler: E_n = E_{n-1} / (1 + h^2) exactly (numerical damping).
     * Trapezoidal: energy preserving (Cayley transform), after its Backward Euler first step. */
    const double L = 1e-3, C = 1e-6, dt = 1e-6, h = dt / sqrt(L * C), e0 = C / 2;
    TransientDevice d[] = {dev(spec("ideal.voltage_source"), 1, 0, 1.0, "V1"), dev(spec("ideal.inductor"), 1, 2, L, "L1"),
                           dev(spec("ideal.capacitor"), 2, 0, C, "C1")};
    for (int m = 0; m < 2; ++m) {
        TransientRun run;
        TransientFailure f;
        assert(transient_init(&run, d, 3, 2, m ? TRANSIENT_TRAPEZOIDAL : TRANSIENT_BACKWARD_EULER, dt, NULL) == TRANSIENT_OK);
        for (int k = 1; k <= 2000; ++k) {
            MnaResult r = {0};
            assert(transient_step(&run, &r, &f) == TRANSIENT_OK);
            double u = run.accepted[2][0] - 1.0, i = run.accepted[1][0];
            double energy = 0.5 * C * u * u + 0.5 * L * i * i;
            double want = m ? e0 / (1 + h * h) : e0 * pow(1 + h * h, -k);
            assert(rel(energy, want, 1e-9));
            mna_result_free(&r);
        }
        transient_free(&run);
    }
}

static double shockley(double v) { return 1e-14 * (exp(v / VT) - 1.0); }

static void test_diode_capacitor(void)
{
    /* 5 V through 1k charges 1 uF; a diode across the capacitor clamps it. BE KCL at every step:
     * (5 - v_n)/R = C (v_n - v_{n-1}) / dt + Id(v_n). */
    const double R = 1000, C = 1e-6, dt = 1e-5;
    TransientDevice d[] = {dev(spec("ideal.voltage_source"), 1, 0, 5.0, "V1"), dev(spec("ideal.resistor"), 1, 2, R, "R1"),
                           dev(spec("ideal.capacitor"), 2, 0, C, "C1"), dev(spec("shockley.diode"), 0, 2, 0, "D1")};
    TransientRun run;
    double v[3001];
    v[0] = 0;
    assert(transient_init(&run, d, 4, 2, TRANSIENT_BACKWARD_EULER, dt, NULL) == TRANSIENT_OK);
    run_steps(&run, 3000, 2, v);
    for (int k = 1; k <= 3000; ++k) {
        double lhs = (5 - v[k]) / R, rhs = C * (v[k] - v[k - 1]) / dt + shockley(v[k]);
        assert(fabs(lhs - rhs) <= 1e-9 * fabs(lhs) + 1e-15);
        assert(v[k] >= v[k - 1] - 1e-15); /* monotonic charge */
    }
    /* 30 tau later it sits at the DC operating point of 5 V, 1k and the diode. */
    assert(fabs((5 - v[3000]) / R - shockley(v[3000])) < 1e-9 && v[3000] > 0.6 && v[3000] < 0.8);
    transient_free(&run);
}

static void test_discontinuity_and_methods(void)
{
    TransientDevice d[] = {dev(spec("ideal.voltage_source"), 1, 0, 1.0, "V1"), dev(spec("ideal.resistor"), 1, 2, 1000, "R1"),
                           dev(spec("ideal.capacitor"), 2, 0, 1e-6, "C1")};
    TransientRun run;
    double v[3];
    assert(transient_init(&run, d, 3, 2, TRANSIENT_TRAPEZOIDAL, 1e-5, NULL) == TRANSIENT_OK);
    run_steps(&run, 1, 2, v);
    assert(run.last.method == TRANSIENT_BACKWARD_EULER && rel(v[1], 0.01 / 1.01, 1e-12)); /* not 0.01 / 1.005 */
    run_steps(&run, 1, 2, v);
    assert(run.last.method == TRANSIENT_TRAPEZOIDAL && run.last.step == 2 && rel(run.last.time_s, 2e-5, 1e-15));
    transient_free(&run);
    assert(!strcmp(transient_method_name(TRANSIENT_TRAPEZOIDAL), "trapezoidal"));
}

/* ---- fault injection with test-only specs ---- */
static int fail_stamp, fail_solve, fail_commit;

static int flaky_stamp(MnaCircuit *m, const size_t *n, const double *p, const DeviceStampContext *c)
{
    (void)c;
    return !fail_stamp && mna_add_resistor(m, n[0], n[1], p[0]);
}

static size_t no_unknowns(const void *d) { (void)d; return 0; }
static int nan_load(void *data, const MnaDevice *device, MnaAssembly *a)
{
    (void)data, (void)device;
    return mna_assembly_add_residual(a, 0, fail_solve ? NAN : 0.0);
}
static void nothing(void *d) { (void)d; }
static const MnaDeviceOps NAN_OPS = {MNA_DEVICE_NONLINEAR, no_unknowns, nan_load, nothing, NULL};
static int nan_stamp(MnaCircuit *m, const size_t *n, const double *p, const DeviceStampContext *c)
{
    (void)n, (void)p, (void)c;
    return mna_add_device(m, &NAN_OPS, NULL);
}

static int counter_stamp(MnaCircuit *m, const size_t *n, const double *p, const DeviceStampContext *c)
{
    (void)c;
    return mna_add_resistor(m, n[0], n[1], p[0]);
}
static int counter_commit(const MnaResult *r, const size_t *n, const double *p, const DeviceStampContext *c, double *next)
{
    (void)r, (void)n, (void)p;
    next[0] = c->state[0] + 1.0;
    return !fail_commit;
}

static const DeviceTerminal TWO[] = {{"a", "a", NULL}, {"b", "b", NULL}};
static const DeviceParam R1[] = {{"r", DEVICE_UNIT_OHM, 0, 1e12, 1, 0, 0, "test resistance"}};
#define T_CAPS (DEVICE_CAP_DC | DEVICE_CAP_TRANSIENT)
static const DeviceModelSpec FLAKY = {"test.flaky", 1, "resistor", "test only", "test only", TWO, 2, R1, 1, 0, T_CAPS, 0, 0, NULL,
                                      flaky_stamp, flaky_stamp, NULL, NULL, 0, 0, 0};
static const DeviceModelSpec NANDEV = {"test.nan", 1, "resistor", "test only", "test only", TWO, 2, R1, 1, 0, T_CAPS, 0, 0, NULL,
                                       nan_stamp, nan_stamp, NULL, NULL, 0, 0, 0};
static const DeviceModelSpec COUNTER = {"test.counter", 1, "resistor", "test only", "test only", TWO, 2, R1, 1, 0, T_CAPS, 1,
                                        DEVICE_INTEGRATION_BACKWARD_EULER | DEVICE_INTEGRATION_TRAPEZOIDAL, "[0] accepted steps",
                                        counter_stamp, counter_stamp, counter_commit, NULL, 0, 0, 0};
static const DeviceModelSpec BE_ONLY = {"test.be_only", 1, "resistor", "test only", "test only", TWO, 2, R1, 1, 0, T_CAPS, 1,
                                        DEVICE_INTEGRATION_BACKWARD_EULER, "[0] accepted steps", counter_stamp, counter_stamp,
                                        counter_commit, NULL, 0, 0, 0};
static const DeviceModelSpec DC_ONLY = {"test.dc_only", 1, "resistor", "test only", "test only", TWO, 2, R1, 1, 0, DEVICE_CAP_DC, 0, 0,
                                        NULL, counter_stamp, NULL, NULL, NULL, 0, 0, 0};

/* The capacitor's commit runs before the failing device's: it must not be committed either. */
static void test_rollback(void)
{
    char err[256];
    assert(device_model_validate_spec(&FLAKY, err, sizeof err) && device_model_validate_spec(&COUNTER, err, sizeof err));
    TransientDevice d[] = {dev(spec("ideal.voltage_source"), 1, 0, 1.0, "V1"), dev(&FLAKY, 1, 2, 1000, "RF"),
                           dev(spec("ideal.capacitor"), 2, 0, 1e-6, "C1"), dev(&COUNTER, 2, 0, 1e9, "X1"),
                           dev(&NANDEV, 2, 0, 1, "N1")};
    TransientRun run, reference;
    TransientFailure f;
    double saved[5][DEVICE_MODEL_MAX_STATE], ref_v[6], v[6];
    assert(transient_init(&reference, d, 5, 2, TRANSIENT_TRAPEZOIDAL, 1e-5, NULL) == TRANSIENT_OK);
    run_steps(&reference, 5, 2, ref_v);

    assert(transient_init(&run, d, 5, 2, TRANSIENT_TRAPEZOIDAL, 1e-5, NULL) == TRANSIENT_OK);
    run_steps(&run, 3, 2, v);
    memcpy(saved, run.accepted, sizeof saved);
    TransientStep last = run.last;
    struct { int *flag; TransientStatus status; size_t device; } faults[] = {
        {&fail_stamp, TRANSIENT_STAMP_FAILED, 1}, {&fail_solve, TRANSIENT_SOLVE_FAILED, SIZE_MAX},
        {&fail_commit, TRANSIENT_COMMIT_FAILED, 3}};
    for (size_t k = 0; k < sizeof faults / sizeof faults[0]; ++k) {
        MnaResult r = {0};
        *faults[k].flag = 1;
        assert(transient_step(&run, &r, &f) == faults[k].status);
        *faults[k].flag = 0;
        assert(f.status == faults[k].status && f.step == 4 && rel(f.time_s, 4e-5, 1e-15) && f.device == faults[k].device);
        assert(f.message[0] != '\0' && r.x == NULL);
        if (faults[k].status == TRANSIENT_SOLVE_FAILED) assert(f.mna_status == MNA_STATUS_DEVICE_FAILURE && f.iterations <= 1);
        assert(!memcmp(run.accepted, saved, sizeof saved));                  /* accepted state untouched */
        assert(run.last.step == last.step && run.last.time_s == last.time_s); /* time not advanced */
    }
    /* After the faults clear, the run continues exactly as if they never happened. */
    run_steps(&run, 2, 2, v + 3);
    for (int k = 4; k <= 5; ++k) assert(v[k] == ref_v[k]);
    assert(!memcmp(run.accepted, reference.accepted, sizeof saved) && run.accepted[3][0] == 5.0);
    transient_free(&run);
    transient_free(&reference);
}

static void test_invalid_requests(void)
{
    TransientDevice ok[] = {dev(spec("ideal.voltage_source"), 1, 0, 1.0, "V1"), dev(spec("ideal.resistor"), 1, 0, 1.0, "R1")};
    TransientRun run;
    TransientFailure f;
    double bad_dt[] = {0.0, -1e-6, NAN, INFINITY};
    for (size_t k = 0; k < 4; ++k) assert(transient_init(&run, ok, 2, 1, TRANSIENT_BACKWARD_EULER, bad_dt[k], &f) == TRANSIENT_INVALID_ARGUMENT);
    assert(transient_init(&run, ok, 2, 1, (TransientMethod)7, 1e-6, &f) == TRANSIENT_INVALID_ARGUMENT);
    TransientDevice far[] = {dev(spec("ideal.resistor"), 1, 5, 1.0, "R9")};
    assert(transient_init(&run, far, 1, 1, TRANSIENT_BACKWARD_EULER, 1e-6, &f) == TRANSIENT_INVALID_ARGUMENT && f.device == 0);
    TransientDevice nan_param[] = {dev(spec("ideal.resistor"), 1, 0, NAN, "R9")};
    assert(transient_init(&run, nan_param, 1, 1, TRANSIENT_BACKWARD_EULER, 1e-6, &f) == TRANSIENT_INVALID_ARGUMENT);
    TransientDevice dc_only[] = {dev(&DC_ONLY, 1, 0, 1.0, "X")};
    assert(transient_init(&run, dc_only, 1, 1, TRANSIENT_BACKWARD_EULER, 1e-6, &f) == TRANSIENT_UNSUPPORTED && strstr(f.message, "X"));
    TransientDevice be_only[] = {dev(spec("ideal.voltage_source"), 1, 0, 1.0, "V1"), dev(&BE_ONLY, 1, 0, 1.0, "B")};
    assert(transient_init(&run, be_only, 2, 1, TRANSIENT_TRAPEZOIDAL, 1e-6, &f) == TRANSIENT_UNSUPPORTED && strstr(f.message, "trapezoidal"));
    assert(transient_init(&run, be_only, 2, 1, TRANSIENT_BACKWARD_EULER, 1e-6, &f) == TRANSIENT_OK);
    transient_free(&run);
}

/* DC and transient are distinct stamps: a capacitor is open and an inductor a short at DC,
 * while in transient both are companion models. */
static void test_dc_versus_companion(void)
{
    DeviceStampContext dc = {DEVICE_ANALYSIS_DC, 0.0, NULL, 0};
    MnaCircuit m;
    MnaResult r = {0};
    MnaOptions o;
    mna_options_default(&o);
    size_t src[2] = {1, 0}, cap[2] = {1, 2}, gnd_cap[2] = {2, 0};
    double one = 1.0, c = 1e-6;
    mna_circuit_init(&m, 2);
    assert(spec("ideal.voltage_source")->stamp_dc(&m, src, &one, &dc) && spec("ideal.capacitor")->stamp_dc(&m, cap, &c, &dc) &&
           spec("ideal.capacitor")->stamp_dc(&m, gnd_cap, &c, &dc));
    mna_solve(&m, &o, &r);
    assert(r.status == MNA_STATUS_SINGULAR_MATRIX); /* node 2 joined only by capacitors floats at DC */
    mna_result_free(&r);
    mna_circuit_free(&m);
    TransientDevice d[] = {dev(spec("ideal.voltage_source"), 1, 0, 1.0, "V1"), dev(spec("ideal.capacitor"), 1, 2, 1e-6, "C1"),
                           dev(spec("ideal.capacitor"), 2, 0, 1e-6, "C2")};
    TransientRun run;
    double v[2];
    assert(transient_init(&run, d, 3, 2, TRANSIENT_BACKWARD_EULER, 1e-6, NULL) == TRANSIENT_OK);
    run_steps(&run, 1, 2, v);
    assert(rel(v[1], 0.5, 1e-12)); /* capacitive divider in transient */
    transient_free(&run);
}

int main(void)
{
    char err[256];
    model_registry_init(&reg);
    assert(model_registry_register_builtin_devices(&reg, err, sizeof err));
    test_constant_current_charging();
    test_rc_step();
    test_rl_step();
    test_lc_energy();
    test_diode_capacitor();
    test_discontinuity_and_methods();
    test_rollback();
    test_invalid_requests();
    test_dc_versus_companion();
    model_registry_free(&reg);
    puts("transient: PASS");
    return 0;
}

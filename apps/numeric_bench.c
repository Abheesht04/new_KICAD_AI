/* numeric_bench: measures the current MNA kernels against analytical oracles (docs/engineering/NUMERICAL_ROADMAP.md).
 *
 *   numeric_bench [--quick]   -> one JSON document on stdout
 *
 * Every accuracy number compares the solver with a closed-form answer computed here, never with the
 * solver itself. Timings are best-of-N processor times and vary by machine; they are for growth rates, not limits.
 * This is a measurement tool, not a test: it reports what happens, including failures.
 */
#include "mna.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Processor time (portable C17; MinGW's runtime has no timespec_get). Small solves are repeated. */
static double now_s(void) {
    return (double)clock() / CLOCKS_PER_SEC;
}

/* JSON number, or null when the value is undefined (a failed solve); rotating buffers for one printf. */
static const char *J(double v) {
    static char buf[8][40];
    static int next;
    char *b = buf[next++ % 8];
    if (isfinite(v)) snprintf(b, sizeof buf[0], "%.10g", v);
    else snprintf(b, sizeof buf[0], "null");
    return b;
}

static const char *status_name(MnaStatus s) {
    switch (s) {
    case MNA_STATUS_SUCCESS: return "success";
    case MNA_STATUS_INVALID_ARGUMENT: return "invalid_argument";
    case MNA_STATUS_ALLOCATION_FAILURE: return "allocation_failure";
    case MNA_STATUS_SINGULAR_MATRIX: return "singular_matrix";
    case MNA_STATUS_NOT_CONVERGED: return "not_converged";
    default: return "device_failure";
    }
}

/* Chain of n equal resistors from a 1 V source to ground: V(k) = (n - k + 1) / n at node k = 1..n. */
static int ladder(MnaCircuit *m, size_t n, double r) {
    mna_circuit_init(m, n);
    if (!mna_add_voltage_source(m, 1, 0, 1.0)) return 0;
    for (size_t k = 1; k < n; ++k)
        if (!mna_add_resistor(m, k, k + 1, r)) return 0;
    return mna_add_resistor(m, n, 0, r);
}

static void bench_ladder(int quick) {
    static const size_t sizes[] = {10, 50, 100, 200, 400, 800, 1600};
    size_t count = quick ? 4 : sizeof sizes / sizeof sizes[0];
    printf("\"ladder\":[");
    for (size_t s = 0; s < count; ++s) {
        size_t n = sizes[s], unknowns = n + 1;
        int reps = n <= 50 ? 400 : n <= 100 ? 50 : n <= 400 ? 3 : 1;
        double total = 0, err = 0;
        MnaResult r = {0};
        MnaStatus status = MNA_STATUS_SUCCESS;
        for (int rep = 0; rep < reps; ++rep) {
            MnaCircuit m = {0};
            MnaOptions o;
            mna_options_default(&o);
            if (!ladder(&m, n, 1000.0)) { printf("{\"n\":%zu,\"error\":\"build\"}", n); return; }
            mna_result_free(&r);
            double t0 = now_s();
            mna_solve(&m, &o, &r);
            total += now_s() - t0;
            status = r.status;
            mna_circuit_free(&m);
        }
        for (size_t k = 1; k <= n && status == MNA_STATUS_SUCCESS; ++k) {
            double e = fabs(mna_node_voltage(&r, k) - (double)(n - k + 1) / (double)n);
            if (e > err) err = e;
        }
        /* Structural nonzeros of the MNA matrix: tridiagonal node block plus the source's two couplings. */
        size_t nnz = n + 2 * (n - 1) + 2;
        printf("%s{\"nodes\":%zu,\"unknowns\":%zu,\"dense_entries\":%zu,\"structural_nonzeros\":%zu,\"fill_fraction\":%.6g,"
               "\"dense_bytes\":%zu,\"mean_solve_s\":%.6g,\"status\":\"%s\",\"max_error_v\":%.3g,\"max_residual\":%.3g}",
               s ? "," : "", n, unknowns, unknowns * unknowns, nnz, (double)nnz / (double)(unknowns * unknowns),
               unknowns * unknowns * sizeof(double), total / reps, status_name(status), err, r.maximum_residual);
        mna_result_free(&r);
    }
    printf("]");
}

/* Two equal resistors R from a 1 V source: the midpoint is 0.5 V for every R. */
static void bench_scaling(void) {
    static const double rs[] = {1e-3, 1.0, 1e3, 1e6, 1e9, 1e10, 1e11, 1e12};
    printf(",\"divider_scaling\":[");
    for (size_t i = 0; i < sizeof rs / sizeof rs[0]; ++i) {
        MnaCircuit m = {0};
        MnaOptions o;
        MnaResult r = {0};
        mna_options_default(&o);
        mna_circuit_init(&m, 2);
        mna_add_voltage_source(&m, 1, 0, 1.0);
        mna_add_resistor(&m, 1, 2, rs[i]);
        mna_add_resistor(&m, 2, 0, rs[i]);
        mna_solve(&m, &o, &r);
        printf("%s{\"resistance_ohm\":%.3g,\"status\":\"%s\",\"error_v\":%s}", i ? "," : "", rs[i], status_name(r.status),
               J(r.status == MNA_STATUS_SUCCESS ? fabs(mna_node_voltage(&r, 2) - 0.5) : NAN));
        mna_result_free(&r);
        mna_circuit_free(&m);
    }
    /* Ten-decade mix in series: 1 mOhm over 1 GOhm, V(mid) = 1e9 / (1e9 + 1e-3). */
    MnaCircuit m = {0};
    MnaOptions o;
    MnaResult r = {0};
    mna_options_default(&o);
    mna_circuit_init(&m, 2);
    mna_add_voltage_source(&m, 1, 0, 1.0);
    mna_add_resistor(&m, 1, 2, 1e-3);
    mna_add_resistor(&m, 2, 0, 1e9);
    mna_solve(&m, &o, &r);
    printf("],\"mixed_scale\":{\"r_top_ohm\":1e-3,\"r_bottom_ohm\":1e9,\"status\":\"%s\",\"error_v\":%s}", status_name(r.status),
           J(r.status == MNA_STATUS_SUCCESS ? fabs(mna_node_voltage(&r, 2) - 1e9 / (1e9 + 1e-3)) : NAN));
    mna_result_free(&r);
    mna_circuit_free(&m);
    /* A chain of 1 TOhm resistors: correctly scaled but every conductance is 1e-12 S. */
    printf(",\"teraohm_chain\":[");
    static const size_t ns[] = {2, 4, 8};
    for (size_t i = 0; i < 3; ++i) {
        MnaCircuit c = {0};
        MnaResult rr = {0};
        mna_options_default(&o);
        ladder(&c, ns[i], 1e12);
        mna_solve(&c, &o, &rr);
        double err = 0;
        for (size_t k = 1; k <= ns[i] && rr.status == MNA_STATUS_SUCCESS; ++k)
            err = fmax(err, fabs(mna_node_voltage(&rr, k) - (double)(ns[i] - k + 1) / (double)ns[i]));
        printf("%s{\"nodes\":%zu,\"status\":\"%s\",\"max_error_v\":%.3g}", i ? "," : "", ns[i], status_name(rr.status), err);
        mna_result_free(&rr);
        mna_circuit_free(&c);
    }
    printf("]");
}

/* Source conflicts and floating nodes: what the solver reports. */
static void bench_degenerate(void) {
    struct { const char *name; int kind; } cases[] = {{"parallel_sources_equal", 0}, {"parallel_sources_conflict", 1},
                                                      {"floating_node", 2}, {"source_loop_through_zero_ohm_limit", 3}};
    printf(",\"degenerate\":[");
    for (size_t i = 0; i < 4; ++i) {
        MnaCircuit m = {0};
        MnaOptions o;
        MnaResult r = {0};
        mna_options_default(&o);
        mna_circuit_init(&m, 2);
        if (cases[i].kind <= 1) {
            mna_add_voltage_source(&m, 1, 0, 1.0);
            mna_add_voltage_source(&m, 1, 0, cases[i].kind ? 2.0 : 1.0);
            mna_add_resistor(&m, 1, 2, 1e3);
            mna_add_resistor(&m, 2, 0, 1e3);
        } else if (cases[i].kind == 2) {
            mna_add_voltage_source(&m, 1, 0, 1.0);
            mna_add_resistor(&m, 1, 0, 1e3); /* node 2 touches nothing */
        } else {
            mna_add_voltage_source(&m, 1, 0, 1.0);
            mna_add_resistor(&m, 1, 2, 1e-9); /* the smallest resistance a part may state is > 0 */
            mna_add_resistor(&m, 2, 0, 1e-9);
        }
        mna_solve(&m, &o, &r);
        printf("%s{\"case\":\"%s\",\"status\":\"%s\"}", i ? "," : "", cases[i].name, status_name(r.status));
        mna_result_free(&r);
        mna_circuit_free(&m);
    }
    printf("]");
}

/* Diode + resistor from V: I solves V = I R + n Vt ln(I / Is + 1). Oracle: Lambert W in log space,
 * w + ln w = ln(a) + x with a = Is R / (n Vt), x = (V + Is R) / (n Vt); I = (n Vt / R) w - Is. */
static double lambert_w_exp(double lnz) {
    double w = lnz > 1 ? lnz - log(lnz) : exp(lnz); /* W(z) ~ z for small z, ~ ln z - ln ln z for large */
    for (int k = 0; k < 100; ++k) {
        double f = w + log(w) - lnz, step = f / (1 + 1 / w);
        w -= step;
        if (w <= 0) w = 1e-300;
        if (fabs(step) <= 1e-15 * fabs(w)) break;
    }
    return w;
}

static void bench_diode(int quick) {
    static const double vs[] = {0.3, 0.7, 1, 5, 10, 20, 24, 25, 26, 30, 50, 100, 1000};
    const double is = 1e-14, n = 1.0, r = 1000.0, vt = 1.380649e-23 * 300.0 / 1.602176634e-19;
    size_t count = quick ? 8 : sizeof vs / sizeof vs[0];
    printf(",\"diode_newton\":{\"is_a\":%g,\"ideality\":%g,\"r_ohm\":%g,\"max_iterations\":100,\"max_step_v\":0.25,\"cases\":[", is, n, r);
    for (size_t i = 0; i < count; ++i) {
        double nvt = n * vt, lnz = log(is * r / nvt) + (vs[i] + is * r) / nvt;
        double exact_i = nvt / r * lambert_w_exp(lnz) - is;
        MnaCircuit m = {0};
        MnaOptions o;
        MnaResult res = {0};
        mna_options_default(&o);
        mna_circuit_init(&m, 2);
        mna_add_voltage_source(&m, 1, 0, vs[i]);
        mna_add_resistor(&m, 1, 2, r);
        mna_add_diode(&m, 2, 0, is, n, 300.0);
        mna_solve(&m, &o, &res);
        double got = res.status == MNA_STATUS_SUCCESS ? (vs[i] - mna_node_voltage(&res, 2)) / r : NAN;
        printf("%s{\"supply_v\":%g,\"status\":\"%s\",\"iterations\":%zu,\"current_a\":%s,\"oracle_current_a\":%s,\"relative_error\":%s}",
               i ? "," : "", vs[i], status_name(res.status), res.iterations, J(got), J(exact_i), J(fabs(got - exact_i) / exact_i));
        mna_result_free(&res);
        mna_circuit_free(&m);
    }
    printf("]}");
}

int main(int argc, char **argv) {
    int quick = argc > 1 && !strcmp(argv[1], "--quick");
    printf("{\"tool\":\"numeric_bench\",\"solver\":\"dense Gaussian elimination, partial pivoting, absolute pivot threshold 1e-12\",");
    bench_ladder(quick);
    bench_scaling();
    bench_degenerate();
    bench_diode(quick);
    printf("}\n");
    return 0;
}

#include "mna_ac.h"

#include "matrix.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

int mna_ac_init(MnaAcSystem *sys, size_t node_count, size_t branch_count)
{
    memset(sys, 0, sizeof *sys);
    sys->node_count = node_count;
    sys->branch_count = branch_count;
    sys->size = node_count + branch_count;
    if (sys->size == 0) {
        return 1;
    }
    sys->a = calloc(sys->size * sys->size, sizeof *sys->a);
    sys->b = calloc(sys->size, sizeof *sys->b);
    if (!sys->a || !sys->b) {
        mna_ac_free(sys);
        return 0;
    }
    return 1;
}

void mna_ac_free(MnaAcSystem *sys)
{
    free(sys->a);
    free(sys->b);
    memset(sys, 0, sizeof *sys);
}

void mna_ac_clear(MnaAcSystem *sys)
{
    if (sys->size) {
        memset(sys->a, 0, sys->size * sys->size * sizeof *sys->a);
        memset(sys->b, 0, sys->size * sizeof *sys->b);
    }
}

/* An out-of-range index poisons the system so that solve reports it. */
static void poison(MnaAcSystem *sys)
{
    if (sys->size) {
        sys->b[0] = cplx(NAN, NAN);
    }
}

static void add(MnaAcSystem *sys, size_t row, size_t col, Cplx v)
{
    if (row >= sys->size || col >= sys->size) {
        poison(sys);
        return;
    }
    Cplx *cell = &sys->a[row * sys->size + col];
    *cell = cplx_add(*cell, v);
}

void mna_ac_stamp_admittance(MnaAcSystem *sys, size_t node_a, size_t node_b, Cplx y)
{
    if (node_a) add(sys, node_a - 1, node_a - 1, y);
    if (node_b) add(sys, node_b - 1, node_b - 1, y);
    if (node_a && node_b) {
        add(sys, node_a - 1, node_b - 1, cplx_scale(y, -1.0));
        add(sys, node_b - 1, node_a - 1, cplx_scale(y, -1.0));
    }
}

void mna_ac_stamp_node_entry(MnaAcSystem *sys, size_t row_node, size_t col_node, Cplx v)
{
    if (row_node && col_node) add(sys, row_node - 1, col_node - 1, v);
}

void mna_ac_stamp_branch(MnaAcSystem *sys, size_t node_a, size_t node_b, size_t branch)
{
    size_t k = sys->node_count + branch;
    if (node_a) { add(sys, node_a - 1, k, cplx(1, 0)); add(sys, k, node_a - 1, cplx(1, 0)); }
    if (node_b) { add(sys, node_b - 1, k, cplx(-1, 0)); add(sys, k, node_b - 1, cplx(-1, 0)); }
    if (branch >= sys->branch_count) poison(sys);
}

void mna_ac_add_branch_entry(MnaAcSystem *sys, size_t branch_row, size_t branch_col, Cplx v)
{
    if (branch_row >= sys->branch_count || branch_col >= sys->branch_count) {
        poison(sys);
        return;
    }
    add(sys, sys->node_count + branch_row, sys->node_count + branch_col, v);
}

void mna_ac_add_branch_rhs(MnaAcSystem *sys, size_t branch, Cplx v)
{
    if (branch >= sys->branch_count) {
        poison(sys);
        return;
    }
    Cplx *cell = &sys->b[sys->node_count + branch];
    *cell = cplx_add(*cell, v);
}

/* A current source whose `current` leaves node_a, passes through the source and
 * enters node_b, matching the DC current device (residual +i at a, -i at b):
 * with A x + s = 0 the right-hand side gets -i at a and +i at b. */
void mna_ac_inject_current(MnaAcSystem *sys, size_t node_a, size_t node_b, Cplx current)
{
    if (node_a > sys->node_count || node_b > sys->node_count) {
        poison(sys);
        return;
    }
    if (node_a) sys->b[node_a - 1] = cplx_sub(sys->b[node_a - 1], current);
    if (node_b) sys->b[node_b - 1] = cplx_add(sys->b[node_b - 1], current);
}

MnaAcStatus mna_ac_solve(const MnaAcSystem *sys, Cplx *x)
{
    size_t n = sys->size, m = 2 * n;
    if (n == 0) {
        return MNA_AC_OK;
    }
    if (!sys->a || !sys->b || !x) {
        return MNA_AC_INVALID_ARGUMENT;
    }
    for (size_t i = 0; i < n * n; ++i) {
        if (!cplx_isfinite(sys->a[i])) return MNA_AC_NON_FINITE;
    }
    for (size_t i = 0; i < n; ++i) {
        if (!cplx_isfinite(sys->b[i])) return MNA_AC_NON_FINITE;
    }
    Matrix block = {0};
    double *rhs = malloc(m * sizeof *rhs), *sol = malloc(m * sizeof *sol);
    if (!rhs || !sol || !matrix_create(&block, m, m)) {
        free(rhs);
        free(sol);
        matrix_free(&block);
        return MNA_AC_ALLOCATION_FAILURE;
    }
    for (size_t r = 0; r < n; ++r) {
        for (size_t c = 0; c < n; ++c) {
            Cplx v = sys->a[r * n + c];
            block.data[r * m + c] = v.re;
            block.data[r * m + n + c] = -v.im;
            block.data[(n + r) * m + c] = v.im;
            block.data[(n + r) * m + n + c] = v.re;
        }
        rhs[r] = sys->b[r].re;
        rhs[n + r] = sys->b[r].im;
    }
    MnaAcStatus status = MNA_AC_OK;
    if (!matrix_solve(&block, rhs, sol)) {
        status = MNA_AC_SINGULAR;
    } else {
        for (size_t i = 0; i < n; ++i) {
            x[i] = cplx(sol[i], sol[n + i]);
            if (!cplx_isfinite(x[i])) status = MNA_AC_SINGULAR;
        }
    }
    free(rhs);
    free(sol);
    matrix_free(&block);
    return status;
}

const char *mna_ac_status_name(MnaAcStatus status)
{
    switch (status) {
        case MNA_AC_OK:                 return "ok";
        case MNA_AC_INVALID_ARGUMENT:   return "invalid_argument";
        case MNA_AC_ALLOCATION_FAILURE: return "allocation_failure";
        case MNA_AC_NON_FINITE:         return "non_finite";
        case MNA_AC_SINGULAR:           return "singular";
        default:                        return "unknown";
    }
}

Cplx mna_ac_node_voltage(const MnaAcSystem *sys, const Cplx *x, size_t node)
{
    return node == 0 || node > sys->node_count ? cplx(0, 0) : x[node - 1];
}

int mna_ac_frequency_grid(double start_hz, double stop_hz, size_t points, int logarithmic, double *out)
{
    if (!out || !isfinite(start_hz) || !isfinite(stop_hz) || points == 0 || points > 100000 || stop_hz < start_hz ||
        start_hz < 0 || (logarithmic && start_hz <= 0) || (points == 1 && start_hz != stop_hz) ||
        (points > 1 && start_hz == stop_hz)) {
        return 0;
    }
    for (size_t i = 0; i < points; ++i) {
        double t = points == 1 ? 0.0 : (double)i / (double)(points - 1);
        out[i] = logarithmic ? start_hz * pow(stop_hz / start_hz, t) : start_hz + (stop_hz - start_hz) * t;
    }
    out[points - 1] = stop_hz;
    return 1;
}

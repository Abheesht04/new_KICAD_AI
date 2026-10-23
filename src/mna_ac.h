#ifndef MNA_AC_H
#define MNA_AC_H
/*
 * Small-signal AC system  A(jw) x = b(jw)  for linearised MNA.
 *
 * Unknown order matches DC MNA: node voltages for nodes 1..N (row = node - 1;
 * node 0 is ground and has no row), followed by branch currents. Each device
 * that needs a branch current (voltage source, inductor) is given a branch
 * index by the caller. Sign conventions match the DC engine: a node row sums
 * currents leaving that node; a branch current flows from the positive node
 * through the device to the negative node; a voltage source's branch row reads
 * V(+) - V(-) = E.
 *
 * Devices write only through the stamp helpers. The system owns its complex
 * arrays; it does not know any device equations.
 *
 * Solving: the complex system is rewritten as the real 2N system
 *     [Ar -Ai] [xr]   [br]
 *     [Ai  Ar] [xi] = [bi]
 * and solved by the existing dense matrix_solve (Gaussian elimination with
 * partial pivoting). This reuses the one tested linear solver instead of adding
 * a second complex LU, at 4x memory and ~2x flops, which is fine for the dense
 * circuit sizes supported today. It inherits matrix_solve's absolute pivot
 * threshold (1e-12, audit issue I-01): admittances far below that in an
 * otherwise empty column are reported singular. There is no scaling.
 */
#include <stddef.h>

#include "cplx.h"

typedef enum {
    MNA_AC_OK = 0,
    MNA_AC_INVALID_ARGUMENT,
    MNA_AC_ALLOCATION_FAILURE,
    MNA_AC_NON_FINITE,  /* a stamp or the excitation was not finite */
    MNA_AC_SINGULAR     /* no unique solution at this frequency */
} MnaAcStatus;

typedef struct {
    size_t node_count;   /* non-ground nodes */
    size_t branch_count; /* branch-current unknowns */
    size_t size;         /* node_count + branch_count */
    Cplx *a;             /* size x size, row-major */
    Cplx *b;             /* size */
} MnaAcSystem;

int mna_ac_init(MnaAcSystem *sys, size_t node_count, size_t branch_count); /* 1 on success */
void mna_ac_free(MnaAcSystem *sys);
void mna_ac_clear(MnaAcSystem *sys);

/* Stamp helpers. Nodes use the MNA numbering (0 = ground, skipped). branch is
 * 0-based among branch unknowns. Out-of-range indices are not applied and make
 * mna_ac_solve report MNA_AC_NON_FINITE. */
void mna_ac_stamp_admittance(MnaAcSystem *sys, size_t node_a, size_t node_b, Cplx y);
/* Adds v at (row node, column node); either being ground (0) is skipped. For multi-terminal
 * linearisations such as transistor conductance matrices. */
void mna_ac_stamp_node_entry(MnaAcSystem *sys, size_t row_node, size_t col_node, Cplx v);
void mna_ac_stamp_branch(MnaAcSystem *sys, size_t node_a, size_t node_b, size_t branch);
void mna_ac_add_branch_entry(MnaAcSystem *sys, size_t branch_row, size_t branch_col, Cplx v);
void mna_ac_add_branch_rhs(MnaAcSystem *sys, size_t branch, Cplx v);
void mna_ac_inject_current(MnaAcSystem *sys, size_t node_a, size_t node_b, Cplx current);

/* Solves into x (size entries). Does not modify the system. */
MnaAcStatus mna_ac_solve(const MnaAcSystem *sys, Cplx *x);
const char *mna_ac_status_name(MnaAcStatus status);

/* Node voltage phasor from a solution (0 for ground). */
Cplx mna_ac_node_voltage(const MnaAcSystem *sys, const Cplx *x, size_t node);

/* Frequencies for a sweep, in increasing order. logarithmic: start > 0, equal
 * ratio steps; linear: start >= 0, equal steps. points == 1 requires
 * start == stop. The last point is exactly stop. Returns 1, or 0 for invalid
 * arguments (non-finite, stop < start, points 0 or > 100000). */
int mna_ac_frequency_grid(double start_hz, double stop_hz, size_t points, int logarithmic, double *out);

#endif

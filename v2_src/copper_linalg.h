#ifndef COPPER_LINALG_H
#define COPPER_LINALG_H

#include "arena.h"
#include "cplx.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CLA_OK = 0,
    CLA_ERR_ARGUMENT,
    CLA_ERR_MEMORY,
    CLA_ERR_RANGE,
    CLA_ERR_SINGULAR,
    CLA_ERR_NONFINITE
} ClaStatus;

typedef struct {
    double pivot_relative_tolerance; /* relative to the active row scale */
    uint32_t refinement_iterations;  /* iterative-refinement correction steps */
    double refinement_tolerance;     /* relative infinity-norm target */
} ClaOptions;

typedef struct {
    double relative_residual;
    double pivot_growth_estimate;
    uint32_t refinement_steps;
} ClaStats;

typedef enum {
    CLA_WORKSPACE_REAL = 1,
    CLA_WORKSPACE_COMPLEX = 2
} ClaWorkspaceKind;

/* Scratch arrays are allocated once from an Arena and can be reused by
 * repeated Newton solves of the same dimension. The workspace is not
 * re-entrant or thread-safe; use one per concurrent solve. */
typedef struct {
    size_t dimension;
    ClaWorkspaceKind kind;
    void *lu;
    void *work;
    void *residual;
    void *correction;
    double *scale;
    size_t *permutation;
} ClaWorkspace;

ClaOptions cla_default_options(void);

ClaStatus cla_workspace_init_real(Arena *arena, size_t n,
                                  ClaWorkspace *workspace);
ClaStatus cla_workspace_init_complex(Arena *arena, size_t n,
                                     ClaWorkspace *workspace);
ClaStatus cla_solve_real_with_workspace(size_t n, const double *a,
                                        const double *b, double *x,
                                        const ClaOptions *options,
                                        ClaStats *stats,
                                        ClaWorkspace *workspace);
ClaStatus cla_solve_complex_with_workspace(size_t n, const Cplx *a,
                                           const Cplx *b, Cplx *x,
                                           const ClaOptions *options,
                                           ClaStats *stats,
                                           ClaWorkspace *workspace);

/* A and b are row-major and are never modified. x receives the solution.
   The factorization uses scaled partial pivoting. Iterative refinement is
   performed against the original matrix, not the overwritten LU factors. */
ClaStatus cla_solve_real(size_t n, const double *a, const double *b,
                         double *x, const ClaOptions *options,
                         ClaStats *stats);
ClaStatus cla_solve_complex(size_t n, const Cplx *a, const Cplx *b,
                            Cplx *x, const ClaOptions *options,
                            ClaStats *stats);

const char *cla_status_name(ClaStatus status);

#ifdef __cplusplus
}
#endif
#endif

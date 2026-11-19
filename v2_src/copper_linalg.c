#include "copper_linalg.h"
#include "nmath.h"

#include <float.h>
#include <math.h>
#include <string.h>

static int mul_size(size_t a, size_t b, size_t *out) {
  if (!out || (b && a > SIZE_MAX / b))
    return 0;
  *out = a * b;
  return 1;
}

static double absmax(double a, double b) { return a > b ? a : b; }

static double kahan_dot_real(const double *a, const double *x, size_t n) {
  double sum = 0.0, correction = 0.0;
  for (size_t i = 0; i < n; ++i) {
    double product = a[i] * x[i];
    double y = product - correction;
    double t = sum + y;
    correction = (t - sum) - y;
    sum = t;
  }
  return sum;
}

static double real_residual(size_t n, const double *a, const double *b,
                            const double *x, double *r) {
  double numerator = 0.0, denominator = 0.0;
  for (size_t i = 0; i < n; ++i) {
    double ax = kahan_dot_real(a + i * n, x, n);
    r[i] = b[i] - ax;
    double scale = fabs(b[i]);
    for (size_t j = 0; j < n; ++j)
      scale += fabs(a[i * n + j]) * fabs(x[j]);
    double rel = fabs(r[i]) / absmax(scale, DBL_MIN);
    numerator = absmax(numerator, rel);
    denominator = absmax(denominator, fabs(r[i]));
  }
  (void)denominator;
  return numerator;
}

static double cabs1(Cplx z) { return fabs(z.re) + fabs(z.im); }

static Cplx kahan_dot_complex(const Cplx *a, const Cplx *x, size_t n) {
  double sr = 0.0, si = 0.0, cr = 0.0, ci = 0.0;
  for (size_t i = 0; i < n; ++i) {
    double pr = a[i].re * x[i].re - a[i].im * x[i].im;
    double pi = a[i].re * x[i].im + a[i].im * x[i].re;
    double yr = pr - cr, tr = sr + yr;
    cr = (tr - sr) - yr;
    sr = tr;
    double yi = pi - ci, ti = si + yi;
    ci = (ti - si) - yi;
    si = ti;
  }
  return cplx(sr, si);
}

static double complex_residual(size_t n, const Cplx *a, const Cplx *b,
                               const Cplx *x, Cplx *r) {
  double norm = 0.0;
  for (size_t i = 0; i < n; ++i) {
    Cplx ax = kahan_dot_complex(a + i * n, x, n);
    r[i] = cplx_sub(b[i], ax);
    double scale = cabs1(b[i]);
    for (size_t j = 0; j < n; ++j)
      scale += cabs1(a[i * n + j]) * cabs1(x[j]);
    double q = cabs1(r[i]) / absmax(scale, DBL_MIN);
    norm = absmax(norm, q);
  }
  return norm;
}

static ClaStatus factor_real(size_t n, double *lu, double *scale, size_t *perm,
                             double pivot_tol, double *growth) {
  double max_a = 0.0, max_u = 0.0;
  for (size_t r = 0; r < n; ++r) {
    double row = 0.0;
    perm[r] = r;
    for (size_t c = 0; c < n; ++c) {
      double v = fabs(lu[r * n + c]);
      if (!nm_isfinite(v))
        return CLA_ERR_NONFINITE;
      if (v > row)
        row = v;
      if (v > max_a)
        max_a = v;
    }
    if (row == 0.0)
      return CLA_ERR_SINGULAR;
    scale[r] = row;
  }
  for (size_t k = 0; k < n; ++k) {
    size_t pivot = k;
    double best = -1.0;
    for (size_t r = k; r < n; ++r) {
      double score = fabs(lu[r * n + k]) / scale[r];
      if (score > best) {
        best = score;
        pivot = r;
      }
    }
    if (!nm_isfinite(best) || best <= pivot_tol)
      return CLA_ERR_SINGULAR;
    if (pivot != k) {
      for (size_t c = 0; c < n; ++c) {
        double t = lu[k * n + c];
        lu[k * n + c] = lu[pivot * n + c];
        lu[pivot * n + c] = t;
      }
      double ts = scale[k];
      scale[k] = scale[pivot];
      scale[pivot] = ts;
      size_t tp = perm[k];
      perm[k] = perm[pivot];
      perm[pivot] = tp;
    }
    double d = lu[k * n + k];
    if (!nm_isfinite(d) || fabs(d) <= pivot_tol * scale[k])
      return CLA_ERR_SINGULAR;
    for (size_t r = k + 1; r < n; ++r) {
      double factor = lu[r * n + k] / d;
      if (!nm_isfinite(factor))
        return CLA_ERR_NONFINITE;
      lu[r * n + k] = factor;
      for (size_t c = k + 1; c < n; ++c)
        lu[r * n + c] -= factor * lu[k * n + c];
      for (size_t c = k + 1; c < n; ++c)
        if (fabs(lu[r * n + c]) > max_u)
          max_u = fabs(lu[r * n + c]);
    }
  }
  max_u = 0.0;
  for (size_t r = 0; r < n; ++r)
    for (size_t c = r; c < n; ++c)
      if (fabs(lu[r * n + c]) > max_u)
        max_u = fabs(lu[r * n + c]);
  if (growth)
    *growth = max_a > 0.0 ? max_u / max_a : 1.0;
  return CLA_OK;
}

static ClaStatus solve_factored_real(size_t n, const double *lu,
                                     const size_t *perm, const double *b,
                                     double *x, double *work) {
  for (size_t i = 0; i < n; ++i) {
    double sum = b[perm[i]], corr = 0.0;
    for (size_t j = 0; j < i; ++j) {
      double y = -(lu[i * n + j] * work[j]) - corr;
      double t = sum + y;
      corr = (t - sum) - y;
      sum = t;
    }
    work[i] = sum;
  }
  for (size_t ii = n; ii-- > 0;) {
    double sum = work[ii], corr = 0.0;
    for (size_t j = ii + 1; j < n; ++j) {
      double y = -(lu[ii * n + j] * x[j]) - corr;
      double t = sum + y;
      corr = (t - sum) - y;
      sum = t;
    }
    double d = lu[ii * n + ii];
    if (!nm_isfinite(d) || d == 0.0)
      return CLA_ERR_SINGULAR;
    x[ii] = sum / d;
    if (!nm_isfinite(x[ii]))
      return CLA_ERR_NONFINITE;
  }
  return CLA_OK;
}

static ClaStatus factor_complex(size_t n, Cplx *lu, double *scale, size_t *perm,
                                double pivot_tol, double *growth) {
  double max_a = 0.0, max_u = 0.0;
  for (size_t r = 0; r < n; ++r) {
    double row = 0.0;
    perm[r] = r;
    for (size_t c = 0; c < n; ++c) {
      if (!cplx_isfinite(lu[r * n + c]))
        return CLA_ERR_NONFINITE;
      double v = cplx_abs(lu[r * n + c]);
      if (v > row)
        row = v;
      if (v > max_a)
        max_a = v;
    }
    if (row == 0.0)
      return CLA_ERR_SINGULAR;
    scale[r] = row;
  }
  for (size_t k = 0; k < n; ++k) {
    size_t pivot = k;
    double best = -1.0;
    for (size_t r = k; r < n; ++r) {
      double score = cplx_abs(lu[r * n + k]) / scale[r];
      if (score > best) {
        best = score;
        pivot = r;
      }
    }
    if (!nm_isfinite(best) || best <= pivot_tol)
      return CLA_ERR_SINGULAR;
    if (pivot != k) {
      for (size_t c = 0; c < n; ++c) {
        Cplx t = lu[k * n + c];
        lu[k * n + c] = lu[pivot * n + c];
        lu[pivot * n + c] = t;
      }
      double ts = scale[k];
      scale[k] = scale[pivot];
      scale[pivot] = ts;
      size_t tp = perm[k];
      perm[k] = perm[pivot];
      perm[pivot] = tp;
    }
    Cplx d = lu[k * n + k];
    if (cplx_abs(d) <= pivot_tol * scale[k])
      return CLA_ERR_SINGULAR;
    for (size_t r = k + 1; r < n; ++r) {
      Cplx factor;
      if (!cplx_div(lu[r * n + k], d, &factor))
        return CLA_ERR_NONFINITE;
      lu[r * n + k] = factor;
      for (size_t c = k + 1; c < n; ++c) {
        lu[r * n + c] =
            cplx_sub(lu[r * n + c], cplx_mul(factor, lu[k * n + c]));
        double v = cplx_abs(lu[r * n + c]);
        if (v > max_u)
          max_u = v;
      }
    }
  }
  max_u = 0.0;
  for (size_t r = 0; r < n; ++r)
    for (size_t c = r; c < n; ++c) {
      double value = cplx_abs(lu[r * n + c]);
      if (value > max_u)
        max_u = value;
    }
  if (growth)
    *growth = max_a > 0.0 ? max_u / max_a : 1.0;
  return CLA_OK;
}

static ClaStatus solve_factored_complex(size_t n, const Cplx *lu,
                                        const size_t *perm, const Cplx *b,
                                        Cplx *x, Cplx *work) {
  for (size_t i = 0; i < n; ++i) {
    Cplx sum = b[perm[i]];
    for (size_t j = 0; j < i; ++j)
      sum = cplx_sub(sum, cplx_mul(lu[i * n + j], work[j]));
    work[i] = sum;
  }
  for (size_t ii = n; ii-- > 0;) {
    Cplx sum = work[ii];
    for (size_t j = ii + 1; j < n; ++j)
      sum = cplx_sub(sum, cplx_mul(lu[ii * n + j], x[j]));
    if (!cplx_div(sum, lu[ii * n + ii], &x[ii]))
      return CLA_ERR_SINGULAR;
  }
  return CLA_OK;
}

ClaOptions cla_default_options(void) {
  ClaOptions o;
  o.pivot_relative_tolerance = 64.0 * DBL_EPSILON;
  o.refinement_iterations = 4;
  o.refinement_tolerance = 8.0 * DBL_EPSILON;
  return o;
}

static ClaStatus workspace_init_common(Arena *arena, size_t n,
                                        ClaWorkspaceKind kind,
                                        ClaWorkspace *workspace)
{
  size_t matrix_count;
  size_t element_size = kind == CLA_WORKSPACE_REAL ? sizeof(double) : sizeof(Cplx);
  size_t vector_size = kind == CLA_WORKSPACE_REAL ? sizeof(double) : sizeof(Cplx);

  if (!arena || !workspace || n == 0)
    return CLA_ERR_ARGUMENT;
  if (!mul_size(n, n, &matrix_count) ||
      matrix_count > SIZE_MAX / element_size ||
      n > SIZE_MAX / vector_size || n > SIZE_MAX / sizeof(size_t))
    return CLA_ERR_RANGE;

  memset(workspace, 0, sizeof(*workspace));
  workspace->dimension = n;
  workspace->kind = kind;
  workspace->lu = arena_alloc_array(arena, element_size, matrix_count);
  workspace->work = arena_alloc_array(arena, vector_size, n);
  workspace->residual = arena_alloc_array(arena, vector_size, n);
  workspace->correction = arena_alloc_array(arena, vector_size, n);
  workspace->scale = (double *)arena_alloc_array(arena, sizeof(double), n);
  workspace->permutation = (size_t *)arena_alloc_array(arena, sizeof(size_t), n);
  if (!workspace->lu || !workspace->work || !workspace->residual ||
      !workspace->correction || !workspace->scale ||
      !workspace->permutation) {
    memset(workspace, 0, sizeof(*workspace));
    return CLA_ERR_MEMORY;
  }
  return CLA_OK;
}

ClaStatus cla_workspace_init_real(Arena *arena, size_t n,
                                  ClaWorkspace *workspace)
{
  return workspace_init_common(arena, n, CLA_WORKSPACE_REAL, workspace);
}

ClaStatus cla_workspace_init_complex(Arena *arena, size_t n,
                                     ClaWorkspace *workspace)
{
  return workspace_init_common(arena, n, CLA_WORKSPACE_COMPLEX, workspace);
}

ClaStatus cla_solve_real_with_workspace(size_t n, const double *a,
                                        const double *b, double *x,
                                        const ClaOptions *options,
                                        ClaStats *stats,
                                        ClaWorkspace *workspace)
{
  if (stats)
    memset(stats, 0, sizeof(*stats));
  if (!a || !b || !x || n == 0 || !workspace ||
      workspace->kind != CLA_WORKSPACE_REAL || workspace->dimension < n)
    return CLA_ERR_ARGUMENT;

  size_t nn;
  if (!mul_size(n, n, &nn) || nn > SIZE_MAX / sizeof(double))
    return CLA_ERR_RANGE;

  ClaOptions o = options ? *options : cla_default_options();
  if (!(o.pivot_relative_tolerance > 0.0))
    o.pivot_relative_tolerance = 64.0 * DBL_EPSILON;
  if (!(o.refinement_tolerance > 0.0))
    o.refinement_tolerance = 8.0 * DBL_EPSILON;

  double *lu = (double *)workspace->lu;
  double *work = (double *)workspace->work;
  double *r = (double *)workspace->residual;
  double *dx = (double *)workspace->correction;
  double *scale = workspace->scale;
  size_t *perm = workspace->permutation;

  memcpy(lu, a, nn * sizeof(double));
  ClaStatus st = factor_real(n, lu, scale, perm, o.pivot_relative_tolerance,
                             stats ? &stats->pivot_growth_estimate : NULL);
  if (st == CLA_OK)
    st = solve_factored_real(n, lu, perm, b, x, work);

  double prev = DBL_MAX;
  for (uint32_t it = 0; st == CLA_OK && it < o.refinement_iterations; ++it) {
    double rel = real_residual(n, a, b, x, r);
    if (stats)
      stats->relative_residual = rel;
    if (!nm_isfinite(rel)) {
      st = CLA_ERR_NONFINITE;
      break;
    }
    if (rel <= o.refinement_tolerance ||
        rel >= prev * (1.0 - 8.0 * DBL_EPSILON))
      break;
    prev = rel;
    st = solve_factored_real(n, lu, perm, r, dx, work);
    if (st != CLA_OK)
      break;
    for (size_t i = 0; i < n; ++i)
      x[i] += dx[i];
    if (stats)
      stats->refinement_steps = it + 1;
  }
  if (st == CLA_OK && stats)
    stats->relative_residual = real_residual(n, a, b, x, r);
  return st;
}

ClaStatus cla_solve_complex_with_workspace(size_t n, const Cplx *a,
                                           const Cplx *b, Cplx *x,
                                           const ClaOptions *options,
                                           ClaStats *stats,
                                           ClaWorkspace *workspace)
{
  if (stats)
    memset(stats, 0, sizeof(*stats));
  if (!a || !b || !x || n == 0 || !workspace ||
      workspace->kind != CLA_WORKSPACE_COMPLEX || workspace->dimension < n)
    return CLA_ERR_ARGUMENT;

  size_t nn;
  if (!mul_size(n, n, &nn) || nn > SIZE_MAX / sizeof(Cplx))
    return CLA_ERR_RANGE;

  ClaOptions o = options ? *options : cla_default_options();
  if (!(o.pivot_relative_tolerance > 0.0))
    o.pivot_relative_tolerance = 64.0 * DBL_EPSILON;
  if (!(o.refinement_tolerance > 0.0))
    o.refinement_tolerance = 8.0 * DBL_EPSILON;

  Cplx *lu = (Cplx *)workspace->lu;
  Cplx *work = (Cplx *)workspace->work;
  Cplx *r = (Cplx *)workspace->residual;
  Cplx *dx = (Cplx *)workspace->correction;
  double *scale = workspace->scale;
  size_t *perm = workspace->permutation;

  memcpy(lu, a, nn * sizeof(Cplx));
  ClaStatus st = factor_complex(n, lu, scale, perm,
                                o.pivot_relative_tolerance,
                                stats ? &stats->pivot_growth_estimate : NULL);
  if (st == CLA_OK)
    st = solve_factored_complex(n, lu, perm, b, x, work);

  double prev = DBL_MAX;
  for (uint32_t it = 0; st == CLA_OK && it < o.refinement_iterations; ++it) {
    double rel = complex_residual(n, a, b, x, r);
    if (stats)
      stats->relative_residual = rel;
    if (!nm_isfinite(rel)) {
      st = CLA_ERR_NONFINITE;
      break;
    }
    if (rel <= o.refinement_tolerance ||
        rel >= prev * (1.0 - 8.0 * DBL_EPSILON))
      break;
    prev = rel;
    st = solve_factored_complex(n, lu, perm, r, dx, work);
    if (st != CLA_OK)
      break;
    for (size_t i = 0; i < n; ++i)
      x[i] = cplx_add(x[i], dx[i]);
    if (stats)
      stats->refinement_steps = it + 1;
  }
  if (st == CLA_OK && stats)
    stats->relative_residual = complex_residual(n, a, b, x, r);
  return st;
}

ClaStatus cla_solve_real(size_t n, const double *a, const double *b,
                         double *x, const ClaOptions *options,
                         ClaStats *stats)
{
  Arena scratch;
  ClaWorkspace workspace;
  ClaStatus status;

  if (!a || !b || !x || n == 0)
    return CLA_ERR_ARGUMENT;
  arena_init(&scratch, 4096u);
  status = cla_workspace_init_real(&scratch, n, &workspace);
  if (status == CLA_OK)
    status = cla_solve_real_with_workspace(n, a, b, x, options, stats,
                                           &workspace);
  arena_destroy(&scratch);
  return status;
}

ClaStatus cla_solve_complex(size_t n, const Cplx *a, const Cplx *b, Cplx *x,
                            const ClaOptions *options, ClaStats *stats)
{
  Arena scratch;
  ClaWorkspace workspace;
  ClaStatus status;

  if (!a || !b || !x || n == 0)
    return CLA_ERR_ARGUMENT;
  arena_init(&scratch, 4096u);
  status = cla_workspace_init_complex(&scratch, n, &workspace);
  if (status == CLA_OK)
    status = cla_solve_complex_with_workspace(n, a, b, x, options, stats,
                                              &workspace);
  arena_destroy(&scratch);
  return status;
}

const char *cla_status_name(ClaStatus s) {
  static const char *const names[] = {"OK",    "ARGUMENT", "MEMORY",
                                      "RANGE", "SINGULAR", "NONFINITE"};
  return (unsigned)s < sizeof(names) / sizeof(names[0]) ? names[s] : "UNKNOWN";
}

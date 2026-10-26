#include "matrix.h"
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

bool sparse_matrix_create(SparseMatrix *m, size_t rows, size_t cols,
                          size_t nnz) {
  if (m == NULL) {
    return false;
  }
  m->rows = rows;
  m->cols = cols;
  m->nnz = nnz;

  m->cols_ptr = (size_t *)calloc(cols + 1, sizeof(size_t));
  m->row_idx = (size_t *)malloc(nnz * sizeof(size_t));
  m->values = (double *)calloc(nnz, sizeof(double));

  if (m->cols_ptr == NULL || m->row_idx == NULL || m->values == NULL) {

    sparse_matrix_free(m);

    return false;
  }

  return true;
}

void sparse_matrix_print(const SparseMatrix *m) {
  if (m == NULL) {
    printf("Matrix is NULL.\n");
    return;
  }

  printf("--- Sparse Matrix (%zu x %zu) | NNZ: %zu ---\n", m->rows, m->cols,
         m->nnz);

  // 1. Print the literal values
  printf("values:  [ ");
  for (size_t i = 0; i < m->nnz; i++) {
    printf("%.2f ", m->values[i]);
  }
  printf("]\n");

  // 2. Print the row indices
  printf("row_idx: [ ");
  for (size_t i = 0; i < m->nnz; i++) {
    printf("%zu ", m->row_idx[i]);
  }
  printf("]\n");

  // 3. Print the column bookmarks (Remember, size is cols + 1)
  printf("cols_ptr:[ ");
  for (size_t i = 0; i < (m->cols + 1); i++) {
    printf("%zu ", m->cols_ptr[i]);
  }
  printf("]\n\n");
}

double *sparse_matrix_get_pointer(const SparseMatrix *m, size_t row,
                                  size_t col) {
  if (m == NULL || col >= m->cols || row >= m->rows) {
    return NULL;
  }

  // Determine the array window for this specific column
  size_t low = m->cols_ptr[col];
  size_t high = m->cols_ptr[col + 1]; // non-inclusive boundary

  // Binary search inside this column's row_idx array
  while (low < high) {
    size_t mid = low + (high - low) / 2;

    if (m->row_idx[mid] == row) {
      return &m->values[mid]; // Found! Return exact memory address
    } else if (m->row_idx[mid] < row) {
      low = mid + 1;
    } else {
      high = mid;
    }
  }

  // Cell does not exist in the symbolic pattern
  return NULL;
}

/*
 * Adds `delta` to entry (row, col).
 * Returns false if the coordinate does not exist in the symbolic pattern.
 */
bool sparse_matrix_add(SparseMatrix *m, size_t row, size_t col, double delta) {
  double *slot = sparse_matrix_get_pointer(m, row, col);

  if (slot == NULL) {
    // Safe guard: Attempted to stamp a cell that wasn't in the symbolic pattern
    return false;
  }

  *slot += delta; // Accumulate conductance/derivative
  return true;
}

bool sparse_matrix_build_pattern(SparseMatrix *m, size_t rows, size_t cols,
                                 const uint8_t *checklist) {
  if (!m || !checklist)
    return false;

  size_t total_nnz = 0;
  for (size_t i = 0; i < (rows * cols); i++) {
    if (checklist[i] == 1) {
      total_nnz++;
    }
  }

  if (!sparse_matrix_create(m, rows, cols, total_nnz)) {
    return false;
  }

  size_t current_nnz_index = 0;

  for (size_t c = 0; c < cols; c++) {
    m->cols_ptr[c] = current_nnz_index;
    for (size_t r = 0; r < rows; r++) {
      size_t flat_index = (r * cols) + c;
      if (checklist[flat_index] == 1) {
        m->row_idx[current_nnz_index] = r;
        current_nnz_index++;
      }
    }
  }
  m->cols_ptr[cols] = current_nnz_index;
  return true;
}

void sparse_matrix_free(SparseMatrix *m) {
  if (m != NULL) {
    if (m->cols_ptr != NULL) {
      free(m->cols_ptr);
      m->cols_ptr = NULL;
    }
    if (m->row_idx != NULL) {
      free(m->row_idx);
      m->row_idx = NULL;
    }

    if (m->values != NULL) {
      free(m->values);
      m->values = NULL;
    }
    m->rows = 0;
    m->cols = 0;
    m->nnz = 0;
  }
}

bool matrix_create(Matrix *m, size_t rows, size_t cols) {
  if (!m)
    return false;
  m->rows = rows;
  m->cols = cols;
  // calloc guarantees un-stamped structural zeros remain 0.0
  m->data = (double *)calloc(rows * cols, sizeof(double));
  return m->data != NULL;
}

void sparse_to_dense(const SparseMatrix *sparse, Matrix *dense_out) {
  if (!sparse || !dense_out || !dense_out->data)
    return;

  // Walk through every column in the CSC structure
  for (size_t c = 0; c < sparse->cols; c++) {
    size_t start_idx = sparse->cols_ptr[c];
    size_t end_idx = sparse->cols_ptr[c + 1];

    // For every non-zero value in this column
    for (size_t i = start_idx; i < end_idx; i++) {
      size_t r = sparse->row_idx[i];
      double val = sparse->values[i];

      // Write to the flat 1D dense array: index = (row * total_cols) + col
      size_t flat_index = (r * dense_out->cols) + c;
      dense_out->data[flat_index] = val;
    }
  }
}

/*
 * Solves Ax = b using Gaussian Elimination with Partial Pivoting.
 * WARNING: The input matrix 'a' is mutated/destroyed during the solve.
 */
/* Dense LU with partial (row) pivoting, P A = L U, factored in place: U on and above the diagonal, the unit-lower
 * L multipliers below it, perm[i] the original row now at position i. Column k is singular when its pivot is
 * at most n * DBL_EPSILON times the largest entry the column had before elimination (a relative test: the
 * answer for a system scaled by any power of ten is the same), or when the column was entirely zero. */
static bool lu_factor(Matrix *a, size_t *perm, size_t *singular_column) {
  size_t n = a->rows;
  double *colmax = calloc(n ? n : 1, sizeof *colmax);
  if (!colmax)
    return false;
  for (size_t i = 0; i < n; i++)
    for (size_t j = 0; j < n; j++)
      if (fabs(a->data[i * n + j]) > colmax[j])
        colmax[j] = fabs(a->data[i * n + j]);
  for (size_t i = 0; i < n; i++)
    perm[i] = i;
  for (size_t k = 0; k < n; k++) {
    size_t pivot_row = k;
    double max_val = fabs(a->data[k * n + k]);
    for (size_t i = k + 1; i < n; i++)
      if (fabs(a->data[i * n + k]) > max_val) {
        max_val = fabs(a->data[i * n + k]);
        pivot_row = i;
      }
    if (colmax[k] == 0.0 || max_val <= (double)n * DBL_EPSILON * colmax[k]) {
      if (singular_column)
        *singular_column = k;
      free(colmax);
      return false;
    }
    if (pivot_row != k) {
      size_t t = perm[k];
      perm[k] = perm[pivot_row];
      perm[pivot_row] = t;
      for (size_t j = 0; j < n; j++) {
        double temp = a->data[k * n + j];
        a->data[k * n + j] = a->data[pivot_row * n + j];
        a->data[pivot_row * n + j] = temp;
      }
    }
    for (size_t i = k + 1; i < n; i++) {
      double factor = a->data[i * n + k] / a->data[k * n + k];
      a->data[i * n + k] = factor;
      for (size_t j = k + 1; j < n; j++)
        a->data[i * n + j] -= factor * a->data[k * n + j];
    }
  }
  free(colmax);
  return true;
}

/* Solves A y = b (transpose = 0) or A^T y = b (transpose = 1) with the factors; y may alias nothing. */
static void lu_solve(const Matrix *lu, const size_t *perm, const double *b, double *y, int transpose) {
  size_t n = lu->rows;
  const double *a = lu->data;
  if (!transpose) {
    for (size_t i = 0; i < n; i++) { /* L c = P b */
      double s = b[perm[i]];
      for (size_t j = 0; j < i; j++)
        s -= a[i * n + j] * y[j];
      y[i] = s;
    }
    for (size_t i = n; i-- > 0;) { /* U y = c */
      double s = y[i];
      for (size_t j = i + 1; j < n; j++)
        s -= a[i * n + j] * y[j];
      y[i] = s / a[i * n + i];
    }
    return;
  }
  /* A^T = U^T L^T P: U^T s = b, L^T t = s, then y[perm[i]] = t[i]. */
  double *t = malloc((n ? n : 1) * sizeof *t);
  if (!t)
    return;
  for (size_t i = 0; i < n; i++) {
    double s = b[i];
    for (size_t j = 0; j < i; j++)
      s -= a[j * n + i] * t[j];
    t[i] = s / a[i * n + i];
  }
  for (size_t i = n; i-- > 0;) {
    double s = t[i];
    for (size_t j = i + 1; j < n; j++)
      s -= a[j * n + i] * t[j];
    t[i] = s;
  }
  for (size_t i = 0; i < n; i++)
    y[perm[i]] = t[i];
  free(t);
}

/* Hager's estimate of ||A^-1||_1 with Higham's refinements (Higham, Accuracy and Stability of Numerical
 * Algorithms, 2nd ed., Algorithm 15.4 and the extra test vector of section 15.3). It can underestimate by a
 * modest factor, never overestimates by more than rounding. */
static double inverse_norm1_estimate(const Matrix *lu, const size_t *perm) {
  size_t n = lu->rows;
  double *x = malloc(n * sizeof *x), *y = malloc(n * sizeof *y), *z = malloc(n * sizeof *z);
  double est = 0.0;
  if (!x || !y || !z) {
    free(x), free(y), free(z);
    return NAN;
  }
  for (size_t i = 0; i < n; i++)
    x[i] = 1.0 / (double)n;
  for (int k = 0; k < 5; k++) {
    lu_solve(lu, perm, x, y, 0);
    double norm = 0.0;
    for (size_t i = 0; i < n; i++)
      norm += fabs(y[i]);
    if (k > 0 && norm <= est)
      break;
    est = norm;
    for (size_t i = 0; i < n; i++)
      y[i] = y[i] >= 0.0 ? 1.0 : -1.0;
    lu_solve(lu, perm, y, z, 1);
    size_t jmax = 0;
    double ztx = 0.0;
    for (size_t i = 0; i < n; i++) {
      ztx += z[i] * x[i];
      if (fabs(z[i]) > fabs(z[jmax]))
        jmax = i;
    }
    if (k > 0 && fabs(z[jmax]) <= ztx)
      break;
    for (size_t i = 0; i < n; i++)
      x[i] = i == jmax ? 1.0 : 0.0;
  }
  for (size_t i = 0; i < n; i++)
    x[i] = (i % 2 ? -1.0 : 1.0) * (1.0 + (n > 1 ? (double)i / (double)(n - 1) : 0.0));
  lu_solve(lu, perm, x, y, 0);
  double alt = 0.0;
  for (size_t i = 0; i < n; i++)
    alt += fabs(y[i]);
  alt = 2.0 * alt / (3.0 * (double)n);
  free(x), free(y), free(z);
  return alt > est ? alt : est;
}

bool matrix_solve_info(Matrix *a, const double *b, double *x, MatrixSolveInfo *info) {
  size_t n = a->rows;
  if (n != a->cols)
    return false;
  if (info) {
    info->singular_column = (size_t)-1;
    info->condition_estimate = NAN;
  }
  double norm1 = 0.0;
  for (size_t j = 0; info && j < n; j++) { /* ||A||_1, before the factors overwrite A */
    double s = 0.0;
    for (size_t i = 0; i < n; i++)
      s += fabs(a->data[i * n + j]);
    if (s > norm1)
      norm1 = s;
  }
  size_t *perm = malloc((n ? n : 1) * sizeof *perm);
  if (!perm)
    return false;
  if (!lu_factor(a, perm, info ? &info->singular_column : NULL)) {
    free(perm);
    return false;
  }
  lu_solve(a, perm, b, x, 0);
  if (info)
    info->condition_estimate = n ? norm1 * inverse_norm1_estimate(a, perm) : 1.0;
  free(perm);
  return true;
}

bool matrix_solve(Matrix *a, const double *b, double *x) { return matrix_solve_info(a, b, x, NULL); }

void matrix_free(Matrix *m) {
  if (m && m->data) {
    free(m->data);
    m->data = NULL;
    m->rows = 0;
    m->cols = 0;
  }
}

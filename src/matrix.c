#include "matrix.h"
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
bool matrix_solve(Matrix *a, const double *b, double *x) {
  size_t n = a->rows;
  if (n != a->cols)
    return false; // Must be a square matrix

  // 1. Initialize output vector x with the constants from b
  for (size_t i = 0; i < n; i++) {
    x[i] = b[i];
  }

  // 2. Elimination Phase (convert matrix to upper triangular)
  for (size_t k = 0; k < n; k++) {

    // --- Partial Pivoting ---
    size_t pivot_row = k;
    double max_val = fabs(a->data[(k * n) + k]);

    for (size_t i = k + 1; i < n; i++) {
      double current_val = fabs(a->data[(i * n) + k]);
      if (current_val > max_val) {
        max_val = current_val;
        pivot_row = i;
      }
    }

    // If the entire column is exactly 0, the matrix is singular (no unique
    // solution)
    if (max_val < 1e-12)
      return false;

    // Swap the current row with the pivot row in both A and x
    if (pivot_row != k) {
      // Swap in vector x
      double temp_x = x[k];
      x[k] = x[pivot_row];
      x[pivot_row] = temp_x;

      // Swap in matrix A
      for (size_t j = 0; j < n; j++) {
        double temp_a = a->data[(k * n) + j];
        a->data[(k * n) + j] = a->data[(pivot_row * n) + j];
        a->data[(pivot_row * n) + j] = temp_a;
      }
    }

    // --- Row Reduction ---
    // Subtract multiples of the pivot row from all rows below it
    for (size_t i = k + 1; i < n; i++) {
      double factor = a->data[(i * n) + k] / a->data[(k * n) + k];

      x[i] -= factor * x[k]; // Apply to vector x

      for (size_t j = k; j < n; j++) {
        a->data[(i * n) + j] -= factor * a->data[(k * n) + j];
      }
    }
  }

  // 3. Back Substitution Phase (solve for x from bottom to top)
  // n is size_t (unsigned), so "i >= 0" would never be false -- it wraps
  // around instead of stopping. `i-- > 0` post-decrements before the
  // comparison, which is the standard idiom for a reverse loop over an
  // unsigned index.
  for (size_t i = n; i-- > 0;) {
    double sum = x[i];
    for (size_t j = i + 1; j < n; j++) {
      sum -= a->data[(i * n) + j] * x[j];
    }
    x[i] = sum / a->data[(i * n) + i];
  }

  return true;
}

void matrix_free(Matrix *m) {
  if (m && m->data) {
    free(m->data);
    m->data = NULL;
    m->rows = 0;
    m->cols = 0;
  }
}

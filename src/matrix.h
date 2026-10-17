// matrix.h
#ifndef MATRIX_H
#define MATRIX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  size_t rows;
  size_t cols;
  size_t nnz;

  size_t *cols_ptr;
  size_t *row_idx;
  double *values;

} SparseMatrix;

typedef struct {
  double *pos_pos;
  double *pos_neg;
  double *neg_pos;
  double *neg_neg;
} JacobianStamp;

typedef struct {
  double *data;
  size_t rows, cols;
} Matrix;

bool sparse_matrix_create(SparseMatrix *m, size_t rows, size_t cols,
                          size_t nnz);

void sparse_matrix_free(SparseMatrix *m);

void sparse_matrix_print(const SparseMatrix *m);

double *sparse_matrix_get_pointer(const SparseMatrix *m, size_t row,
                                  size_t col);

bool sparse_matrix_add(SparseMatrix *m, size_t row, size_t col, double delta);

bool sparse_matrix_add(SparseMatrix *m, size_t row, size_t col, double delta);

bool sparse_matrix_build_pattern(SparseMatrix *m, size_t rows, size_t cols,
                                 const uint8_t *checklist);

// tests
bool matrix_create(Matrix *m, size_t rows, size_t cols);
void matrix_free(Matrix *m);

// The Bridge & Solver
void sparse_to_dense(const SparseMatrix *sparse, Matrix *dense_out);
bool matrix_solve(Matrix *a, const double *b, double *x);

#endif

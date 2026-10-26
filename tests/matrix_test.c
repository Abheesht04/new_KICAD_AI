#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "matrix.h"

/*
 * End-to-end pipeline test: hand-stamp the MNA matrix for the classic
 * three-resistor chain (R1 = R2 = R3 = 10k ohm, VIN = 5V):
 *
 *     VIN --[R1]-- N2 --[R2]-- N3 --[R3]-- GND
 *
 * Unknown vector: [V1, V2, V3, Is]  (Is = voltage source branch current)
 *
 * Row 0 (KCL @ V1, current INTO node1 from resistor = Is leaving via source):
 *     G1*V1 - G1*V2 - Is = 0
 * Row 1 (KCL @ V2):
 *     -G1*V1 + (G1+G2)*V2 - G2*V3 = 0
 * Row 2 (KCL @ V3, R3 returns to ground which has no row):
 *     -G2*V2 + (G2+G3)*V3 = 0
 * Row 3 (source constraint V1 - 0 = VIN):
 *     V1 = 5
 *
 * Hand-solved expected result:
 *     V1 = 5.0000, V2 = 3.3333, V3 = 1.6667, Is = 0.00016667 (~166.7uA)
 */

static int approx(double a, double b, double eps)
{
    return fabs(a - b) < eps;
}

static void test_sparse_stamp_and_solve(void)
{
    const double r = 10000.0;
    const double g = 1.0 / r; /* 1e-4 S for all three resistors */

    const size_t n = 4;

    uint8_t checklist[16];
    memset(checklist, 0, sizeof(checklist));

    /* row-major flat index = row * cols + col, matches matrix.c's convention */
    checklist[0 * n + 0] = 1; /* (0,0) */
    checklist[0 * n + 1] = 1; /* (0,1) */
    checklist[0 * n + 3] = 1; /* (0,3) */
    checklist[1 * n + 0] = 1; /* (1,0) */
    checklist[1 * n + 1] = 1; /* (1,1) */
    checklist[1 * n + 2] = 1; /* (1,2) */
    checklist[2 * n + 1] = 1; /* (2,1) */
    checklist[2 * n + 2] = 1; /* (2,2) */
    checklist[3 * n + 0] = 1; /* (3,0) */

    SparseMatrix sparse;
    assert(sparse_matrix_build_pattern(&sparse, n, n, checklist) == true);

    /* stamp KCL @ V1 */
    assert(sparse_matrix_add(&sparse, 0, 0, g) == true);
    assert(sparse_matrix_add(&sparse, 0, 1, -g) == true);
    assert(sparse_matrix_add(&sparse, 0, 3, -1.0) == true);

    /* stamp KCL @ V2 */
    assert(sparse_matrix_add(&sparse, 1, 0, -g) == true);
    assert(sparse_matrix_add(&sparse, 1, 1, g + g) == true);
    assert(sparse_matrix_add(&sparse, 1, 2, -g) == true);

    /* stamp KCL @ V3 */
    assert(sparse_matrix_add(&sparse, 2, 1, -g) == true);
    assert(sparse_matrix_add(&sparse, 2, 2, g + g) == true);

    /* stamp voltage source constraint row: V1 = VIN */
    assert(sparse_matrix_add(&sparse, 3, 0, 1.0) == true);

    /* stamping a cell that ISN'T in the symbolic pattern must fail */
    assert(sparse_matrix_add(&sparse, 3, 3, 1.0) == false);

    Matrix dense;
    assert(matrix_create(&dense, n, n) == true);
    sparse_to_dense(&sparse, &dense);

    double b[4] = {0.0, 0.0, 0.0, 5.0};
    double x[4];
    bool ok = matrix_solve(&dense, b, x);

    assert(ok == true);
    assert(approx(x[0], 5.0, 1e-6));
    assert(approx(x[1], 3.3333333333, 1e-6));
    assert(approx(x[2], 1.6666666667, 1e-6));
    assert(approx(x[3], 0.0001666667, 1e-9));

    printf("solved=%d  V1=%.4f V2=%.4f V3=%.4f Is=%.8f\n",
           ok, x[0], x[1], x[2], x[3]);
    printf("PASS: sparse stamp -> dense -> Gaussian solve matches hand-derived values\n");

    matrix_free(&dense);
    sparse_matrix_free(&sparse);
}

static void test_singular_matrix_reports_failure(void)
{
    const size_t n = 2;
    uint8_t checklist[4] = {1, 1, 1, 1};

    SparseMatrix sparse;
    assert(sparse_matrix_build_pattern(&sparse, n, n, checklist) == true);

    /* two identical rows -> singular */
    assert(sparse_matrix_add(&sparse, 0, 0, 1.0) == true);
    assert(sparse_matrix_add(&sparse, 0, 1, 1.0) == true);
    assert(sparse_matrix_add(&sparse, 1, 0, 1.0) == true);
    assert(sparse_matrix_add(&sparse, 1, 1, 1.0) == true);

    Matrix dense;
    assert(matrix_create(&dense, n, n) == true);
    sparse_to_dense(&sparse, &dense);

    double b[2] = {1.0, 2.0};
    double x[2];
    bool ok = matrix_solve(&dense, b, x);

    assert(ok == false);
    printf("PASS: singular matrix correctly reported as unsolvable\n");

    matrix_free(&dense);
    sparse_matrix_free(&sparse);
}

/* Step 7 (S1): a relative pivot test (the same answer for any scaling of the system), the singular column, and the
 * 1-norm condition estimate against closed forms. */
static void fill(Matrix *m, const double *v, double scale)
{
    for (size_t i = 0; i < m->rows * m->cols; ++i)
        m->data[i] = v[i] * scale;
}

static void test_relative_pivot_and_condition(void)
{
    /* [[1 2] [3 4]]: ||A||_1 = 6, A^-1 = [[-2 1] [1.5 -0.5]], ||A^-1||_1 = 3.5, kappa_1 = 21. */
    const double a2[] = {1, 2, 3, 4}, b2[] = {5, 6};
    Matrix m;
    MatrixSolveInfo info;
    double x[3];
    assert(matrix_create(&m, 2, 2));
    for (int e = -15; e <= 15; e += 5) {
        double s = pow(10.0, e), b[2] = {b2[0] * s, b2[1] * s};
        fill(&m, a2, s);
        assert(matrix_solve_info(&m, b, x, &info)); /* the old absolute 1e-12 test refused e <= -15 */
        assert(approx(x[0], -4.0, 1e-12) && approx(x[1], 4.5, 1e-12));
        assert(approx(info.condition_estimate, 21.0, 1e-9) && info.singular_column == (size_t)-1);
    }
    matrix_free(&m);

    /* Tridiagonal Toeplitz (2, -1): symmetric, so kappa_1 = ||A||_1 ||A^-1||_1 with ||A^-1||_1 the largest
     * column sum of the known inverse; n = 3: inverse = [[3 2 1] [2 4 2] [1 2 3]] / 4, ||A^-1||_1 = 2, ||A||_1 = 4. */
    const double t3[] = {2, -1, 0, -1, 2, -1, 0, -1, 2}, b3[] = {1, 0, 1};
    assert(matrix_create(&m, 3, 3));
    fill(&m, t3, 1.0);
    assert(matrix_solve_info(&m, b3, x, &info));
    assert(approx(x[0], 1.0, 1e-12) && approx(x[1], 1.0, 1e-12) && approx(x[2], 1.0, 1e-12));
    assert(approx(info.condition_estimate, 8.0, 1e-9));

    /* Singular: the third column is the sum of the first two; the factorisation stops at column 2. */
    const double s3[] = {1, 2, 3, 2, 1, 3, 1, 1, 2};
    fill(&m, s3, 1e-20);
    assert(!matrix_solve_info(&m, b3, x, &info));
    assert(info.singular_column == 2 && isnan(info.condition_estimate));
    matrix_free(&m);
}

int main(void)
{
    test_sparse_stamp_and_solve();
    test_singular_matrix_reports_failure();
    test_relative_pivot_and_condition();

    printf("\n================================\n");
    printf("MATRIX TEST SUITE: PASS\n");
    printf("================================\n");
    return 0;
}

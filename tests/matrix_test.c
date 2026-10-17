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

int main(void)
{
    test_sparse_stamp_and_solve();
    test_singular_matrix_reports_failure();

    printf("\n================================\n");
    printf("MATRIX TEST SUITE: PASS\n");
    printf("================================\n");
    return 0;
}

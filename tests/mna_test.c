/* mna_test_2.c */
#include "mna.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define EXPECT(expr)                                                           \
  do {                                                                         \
    if (!(expr)) {                                                             \
      fprintf(stderr, "  FAIL: %s (%s:%d)\n", #expr, __FILE__, __LINE__);      \
      failures++;                                                              \
    }                                                                          \
  } while (0)

static int nearly_equal(double a, double b, double abs_tol, double rel_tol) {
  double scale = fmax(fabs(a), fabs(b));
  double tolerance = abs_tol + rel_tol * fmax(1.0, scale);
  return fabs(a - b) <= tolerance;
}

/* -------------------------------------------------------------------------
 * External test devices & Trace
 * ------------------------------------------------------------------------- */

typedef struct {
  size_t node;
  double current_a;
} TestCurrentSource;

static size_t current_source_unknown_count(const void *data) {
  (void)data;
  return 0;
}

static void trace_print_vector(const char *name, const double *values,
                               size_t count) {
  printf("%s = [", name);

  for (size_t i = 0; i < count; ++i) {
    if (i != 0)
      printf(", ");

    printf("%.9e", values[i]);
  }

  printf("]\n");
}

static void trace_print_matrix(const char *name, const Matrix *matrix) {
  printf("%s:\n", name);

  for (size_t row = 0; row < matrix->rows; ++row) {
    printf("    [");

    for (size_t col = 0; col < matrix->cols; ++col) {
      if (col != 0)
        printf(", ");

      printf("%.9e", matrix->data[row * matrix->cols + col]);
    }

    printf("]\n");
  }
}

static size_t matrix_nnz(const Matrix *matrix) {
  size_t count = 0;

  if (matrix == NULL || matrix->data == NULL)
    return 0;

  for (size_t row = 0; row < matrix->rows; ++row) {
    for (size_t col = 0; col < matrix->cols; ++col) {
      if (matrix->data[row * matrix->cols + col] != 0.0)
        count++;
    }
  }

  return count;
}

static void mna_test_trace(void *context, const MnaTraceEvent *event) {
  (void)context;

  if (event == NULL)
    return;

  printf("\n--- ITERATION %zu ---\n", event->iteration);

  switch (event->kind) {
  case MNA_TRACE_LINEAR_SYSTEM: {
    size_t nnz;
    size_t total_elements;

    trace_print_vector("x", event->x, event->unknown_count);
    trace_print_vector("F(x)", event->residual, event->unknown_count);
    trace_print_matrix("J", event->jacobian);

    nnz = matrix_nnz(event->jacobian);
    total_elements = event->jacobian->rows * event->jacobian->cols;
    printf("Jacobian NNZ = %zu / %zu (%.1f%% sparse)\n", nnz, total_elements,
           100.0 * (1.0 - (double)nnz / (total_elements ? total_elements : 1)));

    trace_print_vector("rhs = -F", event->rhs, event->unknown_count);
    break;
  }

  case MNA_TRACE_STEP:
    trace_print_vector("raw delta", event->raw_delta, event->unknown_count);
    trace_print_vector("applied delta", event->applied_delta,
                       event->unknown_count);
    printf("maximum raw step     = %.9e\n", event->maximum_raw_step);
    printf("maximum applied step = %.9e\n", event->maximum_applied_step);
    printf("step scale           = %.9e\n", event->step_scale);
    break;

  case MNA_TRACE_RESIDUAL:
    trace_print_vector("x_new", event->x, event->unknown_count);
    trace_print_vector("F(x_new)", event->residual, event->unknown_count);
    printf("maximum residual = %.9e\n", event->maximum_residual);
    break;

  case MNA_TRACE_CONVERGED:
    printf("CONVERGED after %zu iteration(s)\n", event->iteration);
    printf("final residual = %.9e\n", event->maximum_residual);
    break;
  }
}

typedef struct {
  size_t linear_system_count;
  size_t step_count;
  size_t residual_count;
  size_t converged_count;
  int step_payload_valid;
  int linear_payload_valid;
} TraceStats;

static void trace_collect(void *context, const MnaTraceEvent *event) {
  TraceStats *stats = (TraceStats *)context;

  if (stats == NULL || event == NULL)
    return;

  switch (event->kind) {
  case MNA_TRACE_LINEAR_SYSTEM:
    stats->linear_system_count++;
    if (event->x != NULL && event->residual != NULL &&
        event->jacobian != NULL && event->rhs != NULL) {
      stats->linear_payload_valid = 1;
    }
    break;

  case MNA_TRACE_STEP:
    stats->step_count++;
    if (event->raw_delta != NULL && event->applied_delta != NULL &&
        event->maximum_raw_step >= 0.0 && event->maximum_applied_step >= 0.0 &&
        event->step_scale > 0.0 && event->step_scale <= 1.0) {
      stats->step_payload_valid = 1;
    }
    break;

  case MNA_TRACE_RESIDUAL:
    stats->residual_count++;
    break;

  case MNA_TRACE_CONVERGED:
    stats->converged_count++;
    break;
  }
}

static int current_source_load(void *data, const MnaDevice *device,
                               MnaAssembly *assembly) {
  const TestCurrentSource *src = (const TestCurrentSource *)data;
  (void)device;

  if (src == NULL || assembly == NULL || src->node == 0 ||
      src->node > assembly->node_count || !isfinite(src->current_a)) {
    return 0;
  }

  return mna_assembly_add_residual(assembly, src->node - 1, -src->current_a);
}

static void current_source_destroy(void *data) { free(data); }

static const MnaDeviceOps CURRENT_SOURCE_OPS = {
    .flags = MNA_DEVICE_LINEAR,
    .unknown_count = current_source_unknown_count,
    .load = current_source_load,
    .destroy = current_source_destroy,
    .branch_current = NULL};

static int add_test_current_source(MnaCircuit *circuit, size_t node,
                                   double current_a) {
  TestCurrentSource *src = malloc(sizeof(*src));

  if (src == NULL)
    return 0;

  src->node = node;
  src->current_a = current_a;

  if (!mna_add_device(circuit, &CURRENT_SOURCE_OPS, src)) {
    free(src);
    return 0;
  }

  return 1;
}

typedef struct {
  size_t node_a;
  size_t node_b;
  double capacitance_f;
} TestCapacitor;

static size_t capacitor_unknown_count(const void *data) {
  (void)data;
  return 0;
}

static int capacitor_load(void *data, const MnaDevice *device,
                          MnaAssembly *assembly) {
  const TestCapacitor *cap = (const TestCapacitor *)data;
  (void)device;
  (void)assembly;

  if (cap == NULL || !isfinite(cap->capacitance_f) ||
      cap->capacitance_f <= 0.0) {
    return 0;
  }

  return 1;
}

static void capacitor_destroy(void *data) { free(data); }

static const MnaDeviceOps CAPACITOR_OPS = {.flags = MNA_DEVICE_LINEAR,
                                           .unknown_count =
                                               capacitor_unknown_count,
                                           .load = capacitor_load,
                                           .destroy = capacitor_destroy,
                                           .branch_current = NULL};

static int add_test_capacitor(MnaCircuit *circuit, size_t node_a, size_t node_b,
                              double capacitance_f) {
  TestCapacitor *cap = malloc(sizeof(*cap));

  if (cap == NULL)
    return 0;

  cap->node_a = node_a;
  cap->node_b = node_b;
  cap->capacitance_f = capacitance_f;

  if (!mna_add_device(circuit, &CAPACITOR_OPS, cap)) {
    free(cap);
    return 0;
  }

  return 1;
}

typedef struct {
  size_t node_a;
  size_t node_b;
  double conductance_s;
} TestNonlinearConductance;

static size_t nonlinear_conductance_unknown_count(const void *data) {
  (void)data;
  return 0;
}

static int nonlinear_conductance_load(void *data, const MnaDevice *device,
                                      MnaAssembly *assembly) {
  const TestNonlinearConductance *g = (const TestNonlinearConductance *)data;
  double va;
  double vb;
  double current;

  (void)device;

  if (g == NULL || assembly == NULL || !isfinite(g->conductance_s) ||
      g->conductance_s <= 0.0) {
    return 0;
  }

  va = mna_assembly_node_voltage(assembly, g->node_a);
  vb = mna_assembly_node_voltage(assembly, g->node_b);
  current = g->conductance_s * (va - vb);

  if (g->node_a != 0 &&
      !mna_assembly_add_residual(assembly, g->node_a - 1, current)) {
    return 0;
  }

  if (g->node_b != 0 &&
      !mna_assembly_add_residual(assembly, g->node_b - 1, -current)) {
    return 0;
  }

  if (g->node_a != 0 &&
      !mna_assembly_add_jacobian(assembly, g->node_a - 1, g->node_a - 1,
                                 g->conductance_s)) {
    return 0;
  }

  if (g->node_b != 0 &&
      !mna_assembly_add_jacobian(assembly, g->node_b - 1, g->node_b - 1,
                                 g->conductance_s)) {
    return 0;
  }

  if (g->node_a != 0 && g->node_b != 0) {
    if (!mna_assembly_add_jacobian(assembly, g->node_a - 1, g->node_b - 1,
                                   -g->conductance_s)) {
      return 0;
    }

    if (!mna_assembly_add_jacobian(assembly, g->node_b - 1, g->node_a - 1,
                                   -g->conductance_s)) {
      return 0;
    }
  }

  return 1;
}

static void nonlinear_conductance_destroy(void *data) { free(data); }

static const MnaDeviceOps NONLINEAR_CONDUCTANCE_OPS = {
    .flags = MNA_DEVICE_NONLINEAR,
    .unknown_count = nonlinear_conductance_unknown_count,
    .load = nonlinear_conductance_load,
    .destroy = nonlinear_conductance_destroy,
    .branch_current = NULL};

static int add_test_nonlinear_conductance(MnaCircuit *circuit, size_t node_a,
                                          size_t node_b, double conductance_s) {
  TestNonlinearConductance *g = malloc(sizeof(*g));

  if (g == NULL)
    return 0;

  g->node_a = node_a;
  g->node_b = node_b;
  g->conductance_s = conductance_s;

  if (!mna_add_device(circuit, &NONLINEAR_CONDUCTANCE_OPS, g)) {
    free(g);
    return 0;
  }

  return 1;
}

typedef struct {
  int tag;
} TestTwoUnknownDevice;

static size_t two_unknown_count(const void *data) {
  (void)data;
  return 2;
}

static int two_unknown_load(void *data, const MnaDevice *device,
                            MnaAssembly *assembly) {
  size_t i0;
  size_t i1;
  (void)data;

  if (device == NULL || assembly == NULL || assembly->x == NULL) {
    return 0;
  }

  if (!mna_device_unknown_index(device, 0, &i0) ||
      !mna_device_unknown_index(device, 1, &i1)) {
    return 0;
  }

  if (!mna_assembly_add_residual(assembly, i0, assembly->x[i0]) ||
      !mna_assembly_add_residual(assembly, i1, assembly->x[i1])) {
    return 0;
  }

  if (!mna_assembly_add_jacobian(assembly, i0, i0, 1.0) ||
      !mna_assembly_add_jacobian(assembly, i1, i1, 1.0)) {
    return 0;
  }

  return 1;
}

static void two_unknown_destroy(void *data) { free(data); }

static const MnaDeviceOps TWO_UNKNOWN_OPS = {.flags = MNA_DEVICE_LINEAR,
                                             .unknown_count = two_unknown_count,
                                             .load = two_unknown_load,
                                             .destroy = two_unknown_destroy,
                                             .branch_current = NULL};

static int add_test_two_unknown_device(MnaCircuit *circuit) {
  TestTwoUnknownDevice *data = malloc(sizeof(*data));

  if (data == NULL)
    return 0;

  data->tag = 0;

  if (!mna_add_device(circuit, &TWO_UNKNOWN_OPS, data)) {
    free(data);
    return 0;
  }

  return 1;
}

static size_t failing_unknown_count(const void *data) {
  (void)data;
  return 0;
}

static int failing_load(void *data, const MnaDevice *device,
                        MnaAssembly *assembly) {
  (void)data;
  (void)device;
  (void)assembly;
  return 0;
}

static const MnaDeviceOps FAILING_OPS = {.flags = MNA_DEVICE_LINEAR,
                                         .unknown_count = failing_unknown_count,
                                         .load = failing_load,
                                         .destroy = NULL,
                                         .branch_current = NULL};

typedef enum { NONFINITE_RESIDUAL = 0, NONFINITE_JACOBIAN } NonFiniteKind;

typedef struct {
  NonFiniteKind kind;
} TestNonFiniteDevice;

static size_t nonfinite_unknown_count(const void *data) {
  (void)data;
  return 0;
}

static int nonfinite_load(void *data, const MnaDevice *device,
                          MnaAssembly *assembly) {
  const TestNonFiniteDevice *test = (const TestNonFiniteDevice *)data;

  (void)device;

  if (test == NULL || assembly == NULL || assembly->unknown_count == 0) {
    return 0;
  }

  if (test->kind == NONFINITE_RESIDUAL) {
    return mna_assembly_add_residual(assembly, 0, NAN);
  }

  return mna_assembly_add_jacobian(assembly, 0, 0, INFINITY);
}

static void nonfinite_destroy(void *data) { free(data); }

static const MnaDeviceOps NONFINITE_OPS = {.flags = MNA_DEVICE_LINEAR,
                                           .unknown_count =
                                               nonfinite_unknown_count,
                                           .load = nonfinite_load,
                                           .destroy = nonfinite_destroy,
                                           .branch_current = NULL};

static int add_test_nonfinite_device(MnaCircuit *circuit, NonFiniteKind kind) {
  TestNonFiniteDevice *data = malloc(sizeof(*data));

  if (data == NULL)
    return 0;

  data->kind = kind;

  if (!mna_add_device(circuit, &NONFINITE_OPS, data)) {
    free(data);
    return 0;
  }

  return 1;
}

/* -------------------------------------------------------------------------
 * Tests
 * ------------------------------------------------------------------------- */

static void test_empty_circuit(void) {
  MnaCircuit circuit;
  MnaResult result = {0};
  MnaOptions options;

  printf("[1] empty circuit\n");
  mna_circuit_init(&circuit, 0);
  mna_options_default(&options);
  mna_solve(&circuit, &options, &result);

  EXPECT(result.status == MNA_STATUS_SUCCESS);
  EXPECT(result.unknown_count == 0);
  EXPECT(result.x == NULL);

  mna_result_free(&result);
  mna_circuit_free(&circuit);
}

static void test_assembly_helpers(void) {
  Matrix matrix = {0};
  MnaAssembly assembly = {0};
  double x[3] = {1.5, -2.0, 7.25};
  double residual[3] = {0.0, 0.0, 0.0};
  size_t index = SIZE_MAX;

  printf("[2] assembly helpers\n");
  EXPECT(matrix_create(&matrix, 3, 3));

  assembly.x = x;
  assembly.jacobian = &matrix;
  assembly.residual = residual;
  assembly.node_count = 3;
  assembly.unknown_count = 3;

  EXPECT(!mna_assembly_node_index(0, &index));
  EXPECT(mna_assembly_node_index(1, &index) && index == 0);
  EXPECT(mna_assembly_node_index(3, &index) && index == 2);

  EXPECT(
      nearly_equal(mna_assembly_node_voltage(&assembly, 1), 1.5, 1e-15, 0.0));
  EXPECT(
      nearly_equal(mna_assembly_node_voltage(&assembly, 3), 7.25, 1e-15, 0.0));
  EXPECT(nearly_equal(mna_assembly_node_voltage(&assembly, 0), 0.0, 0.0, 0.0));
  EXPECT(nearly_equal(mna_assembly_node_voltage(&assembly, 4), 0.0, 0.0, 0.0));

  EXPECT(mna_assembly_add_residual(&assembly, 0, 2.0));
  EXPECT(mna_assembly_add_residual(&assembly, 0, -0.5));
  EXPECT(nearly_equal(residual[0], 1.5, 1e-15, 0.0));
  EXPECT(!mna_assembly_add_residual(&assembly, 3, 1.0));

  EXPECT(mna_assembly_add_jacobian(&assembly, 0, 1, 4.0));
  EXPECT(mna_assembly_add_jacobian(&assembly, 0, 1, -1.0));
  EXPECT(nearly_equal(matrix.data[1], 3.0, 1e-15, 0.0));
  EXPECT(!mna_assembly_add_jacobian(&assembly, 3, 0, 1.0));
  EXPECT(!mna_assembly_add_jacobian(&assembly, 0, 3, 1.0));

  matrix_free(&matrix);
}

static void test_three_resistor_chain(void) {
  MnaCircuit circuit;
  MnaResult result = {0};
  MnaValidation validation = {0};
  MnaOptions options;
  double source_current;

  printf("[3] three-resistor linear chain\n");
  mna_circuit_init(&circuit, 3);
  mna_options_default(&options);

  EXPECT(mna_add_voltage_source(&circuit, 1, 0, 5.0));
  EXPECT(mna_add_resistor(&circuit, 1, 2, 10000.0));
  EXPECT(mna_add_resistor(&circuit, 2, 3, 10000.0));
  EXPECT(mna_add_resistor(&circuit, 3, 0, 10000.0));

  mna_solve(&circuit, &options, &result);

  EXPECT(result.status == MNA_STATUS_SUCCESS);
  EXPECT(result.iterations == 1);
  EXPECT(nearly_equal(mna_node_voltage(&result, 1), 5.0, 1e-12, 1e-12));
  EXPECT(nearly_equal(mna_node_voltage(&result, 2), 10.0 / 3.0, 1e-12, 1e-12));
  EXPECT(nearly_equal(mna_node_voltage(&result, 3), 5.0 / 3.0, 1e-12, 1e-12));

  source_current = mna_voltage_source_current(&circuit, &result, 0);
  EXPECT(nearly_equal(source_current, -1.6666666666666667e-4, 1e-12, 1e-9));
  EXPECT(source_current < 0.0);

  EXPECT(mna_validate(&circuit, &result, &options, &validation));
  EXPECT(validation.valid);
  EXPECT(validation.maximum_residual < 1e-12);

  EXPECT(nearly_equal(mna_node_voltage(&result, 4), 0.0, 0.0, 0.0));

  mna_result_free(&result);
  mna_circuit_free(&circuit);
}

static void test_resistor_divider(void) {
  MnaCircuit circuit;
  MnaResult result = {0};
  MnaOptions options;

  printf("[4] resistor divider\n");
  mna_circuit_init(&circuit, 2);
  mna_options_default(&options);

  EXPECT(mna_add_voltage_source(&circuit, 1, 0, 5.0));
  EXPECT(mna_add_resistor(&circuit, 1, 2, 10000.0));
  EXPECT(mna_add_resistor(&circuit, 2, 0, 10000.0));

  mna_solve(&circuit, &options, &result);

  EXPECT(result.status == MNA_STATUS_SUCCESS);
  EXPECT(result.iterations == 1);
  EXPECT(nearly_equal(mna_node_voltage(&result, 2), 2.5, 1e-12, 1e-12));

  mna_result_free(&result);
  mna_circuit_free(&circuit);
}

static void test_diode(void) {
  MnaCircuit circuit;
  MnaResult result = {0};
  MnaValidation validation = {0};
  MnaOptions options;
  double vout;
  double current;

  printf("[5] nonlinear diode\n");
  mna_circuit_init(&circuit, 2);
  mna_options_default(&options);

  options.max_iterations = 100;
  options.absolute_residual_tolerance = 1e-10;
  options.maximum_voltage_step_v = 0.25;

  EXPECT(mna_add_voltage_source(&circuit, 1, 0, 5.0));
  EXPECT(mna_add_resistor(&circuit, 1, 2, 1000.0));
  EXPECT(mna_add_diode(&circuit, 2, 0, 1e-12, 1.0, 300.0));

  mna_solve(&circuit, &options, &result);

  EXPECT(result.status == MNA_STATUS_SUCCESS);
  EXPECT(result.iterations > 1);

  vout = mna_node_voltage(&result, 2);
  current = (5.0 - vout) / 1000.0;

  printf("    Vd=%.12f V, I=%.12e A, iterations=%zu\n", vout, current,
         result.iterations);

  EXPECT(nearly_equal(vout, 0.574191498, 1e-7, 1e-7));
  EXPECT(current > 0.0);
  printf("abs tol = %.17e\n", options.absolute_residual_tolerance);
  printf("rel tol = %.17e\n", options.relative_residual_tolerance);
  EXPECT(mna_validate(&circuit, &result, &options, &validation));
  EXPECT(validation.valid);
  EXPECT(validation.maximum_residual < 1e-9);

  mna_result_free(&result);
  mna_circuit_free(&circuit);
}

static void test_two_voltage_sources(void) {
  MnaCircuit circuit;
  MnaResult result = {0};
  MnaOptions options;
  double i1;
  double i2;

  printf("[6] two voltage sources\n");
  mna_circuit_init(&circuit, 2);
  mna_options_default(&options);

  EXPECT(mna_add_voltage_source(&circuit, 1, 0, 5.0));
  EXPECT(mna_add_resistor(&circuit, 1, 0, 2500.0));
  EXPECT(mna_add_voltage_source(&circuit, 2, 0, -5.0));
  EXPECT(mna_add_resistor(&circuit, 2, 0, 2500.0));

  EXPECT(circuit.unknown_count == 4);

  mna_solve(&circuit, &options, &result);

  EXPECT(result.status == MNA_STATUS_SUCCESS);
  EXPECT(result.iterations == 1);
  EXPECT(nearly_equal(mna_node_voltage(&result, 1), 5.0, 1e-12, 1e-12));
  EXPECT(nearly_equal(mna_node_voltage(&result, 2), -5.0, 1e-12, 1e-12));

  i1 = mna_voltage_source_current(&circuit, &result, 0);
  i2 = mna_voltage_source_current(&circuit, &result, 1);

  EXPECT(nearly_equal(i1, -0.002, 1e-12, 1e-9));
  EXPECT(nearly_equal(i2, +0.002, 1e-12, 1e-9));

  mna_result_free(&result);
  mna_circuit_free(&circuit);
}

static void test_extra_unknown_device(void) {
  MnaCircuit circuit;
  MnaResult result = {0};
  MnaOptions options;
  const MnaDevice *dummy;
  const MnaDevice *source;
  size_t idx;

  printf("[7] generic extra-unknown device\n");
  mna_circuit_init(&circuit, 1);
  mna_options_default(&options);

  EXPECT(add_test_two_unknown_device(&circuit));
  EXPECT(mna_add_voltage_source(&circuit, 1, 0, 5.0));
  EXPECT(mna_add_resistor(&circuit, 1, 0, 1000.0));

  EXPECT(circuit.unknown_count == 4);

  dummy = mna_device_at(&circuit, 0);
  source = mna_device_at(&circuit, 1);

  EXPECT(dummy != NULL);
  EXPECT(source != NULL);

  if (dummy != NULL) {
    EXPECT(dummy->unknown_offset == 1);
    EXPECT(dummy->unknown_count == 2);
    EXPECT(mna_device_unknown_index(dummy, 0, &idx) && idx == 1);
    EXPECT(mna_device_unknown_index(dummy, 1, &idx) && idx == 2);
    EXPECT(!mna_device_unknown_index(dummy, 2, &idx));
  }

  if (source != NULL) {
    EXPECT(source->unknown_offset == 3);
    EXPECT(source->unknown_count == 1);
  }

  mna_solve(&circuit, &options, &result);

  EXPECT(result.status == MNA_STATUS_SUCCESS);
  EXPECT(result.iterations == 1);
  EXPECT(nearly_equal(mna_node_voltage(&result, 1), 5.0, 1e-12, 1e-12));
  EXPECT(nearly_equal(mna_voltage_source_current(&circuit, &result, 0), -0.005,
                      1e-12, 1e-9));
  EXPECT(nearly_equal(result.x[1], 0.0, 0.0, 0.0));
  EXPECT(nearly_equal(result.x[2], 0.0, 0.0, 0.0));

  mna_result_free(&result);
  mna_circuit_free(&circuit);
}

static void test_external_current_source(void) {
  MnaCircuit circuit;
  MnaResult result = {0};
  MnaValidation validation = {0};
  MnaOptions options;

  printf("[8] external current source\n");
  mna_circuit_init(&circuit, 1);
  mna_options_default(&options);

  EXPECT(mna_add_resistor(&circuit, 1, 0, 1000.0));
  EXPECT(add_test_current_source(&circuit, 1, 0.001));

  mna_solve(&circuit, &options, &result);

  EXPECT(result.status == MNA_STATUS_SUCCESS);
  EXPECT(result.iterations == 1);
  EXPECT(nearly_equal(mna_node_voltage(&result, 1), 1.0, 1e-12, 1e-12));
  EXPECT(mna_validate(&circuit, &result, &options, &validation));
  EXPECT(validation.valid);

  mna_result_free(&result);
  mna_circuit_free(&circuit);
}

static void test_external_capacitor(void) {
  MnaCircuit circuit;
  MnaResult result = {0};
  MnaOptions options;

  printf("[9] external capacitor placeholder\n");
  mna_circuit_init(&circuit, 2);
  mna_options_default(&options);

  EXPECT(mna_add_voltage_source(&circuit, 1, 0, 5.0));
  EXPECT(mna_add_resistor(&circuit, 1, 2, 10000.0));
  EXPECT(mna_add_resistor(&circuit, 2, 0, 10000.0));
  EXPECT(add_test_capacitor(&circuit, 2, 0, 1e-6));

  EXPECT(circuit.device_count == 4);
  EXPECT(circuit.unknown_count == 3);

  mna_solve(&circuit, &options, &result);

  EXPECT(result.status == MNA_STATUS_SUCCESS);
  EXPECT(result.iterations == 1);
  EXPECT(nearly_equal(mna_node_voltage(&result, 2), 2.5, 1e-12, 1e-12));

  mna_result_free(&result);
  mna_circuit_free(&circuit);
}

static void test_nonlinear_flag(void) {
  MnaCircuit circuit;
  MnaResult result = {0};
  MnaOptions options;

  printf("[10] nonlinear capability flag drives damping\n");
  mna_circuit_init(&circuit, 1);
  mna_options_default(&options);
  options.maximum_voltage_step_v = 0.25;

  EXPECT(mna_add_voltage_source(&circuit, 1, 0, 5.0));
  EXPECT(add_test_nonlinear_conductance(&circuit, 1, 0, 0.001));

  mna_solve(&circuit, &options, &result);

  EXPECT(result.status == MNA_STATUS_SUCCESS);
  EXPECT(nearly_equal(mna_node_voltage(&result, 1), 5.0, 1e-12, 1e-12));
  EXPECT(result.iterations == 20);

  mna_result_free(&result);
  mna_circuit_free(&circuit);
}

static void test_mixed_linear_nonlinear(void) {
  MnaCircuit circuit;
  MnaResult result = {0};
  MnaOptions options;

  printf("[11] mixed linear + nonlinear device set\n");
  mna_circuit_init(&circuit, 2);
  mna_options_default(&options);
  options.maximum_voltage_step_v = 0.25;

  EXPECT(mna_add_voltage_source(&circuit, 1, 0, 5.0));
  EXPECT(mna_add_resistor(&circuit, 1, 2, 1000.0));
  EXPECT(add_test_nonlinear_conductance(&circuit, 2, 0, 0.001));

  mna_solve(&circuit, &options, &result);

  EXPECT(result.status == MNA_STATUS_SUCCESS);
  EXPECT(nearly_equal(mna_node_voltage(&result, 1), 5.0, 1e-12, 1e-12));
  EXPECT(nearly_equal(mna_node_voltage(&result, 2), 2.5, 1e-12, 1e-12));
  EXPECT(result.iterations == 20);

  mna_result_free(&result);
  mna_circuit_free(&circuit);
}

static void test_floating_node(void) {
  MnaCircuit circuit;
  MnaResult result = {0};
  MnaOptions options;

  printf("[12] floating node / singular matrix\n");
  mna_circuit_init(&circuit, 1);
  mna_options_default(&options);

  mna_solve(&circuit, &options, &result);

  EXPECT(result.status == MNA_STATUS_SINGULAR_MATRIX);
  EXPECT(result.unknown_count == 1);

  mna_result_free(&result);
  mna_circuit_free(&circuit);
}

static void test_node_accessor_bounds(void) {
  MnaCircuit circuit;
  MnaResult result = {0};
  MnaOptions options;

  printf("[13] node accessor bounds\n");
  mna_circuit_init(&circuit, 2);
  mna_options_default(&options);

  EXPECT(mna_add_voltage_source(&circuit, 1, 0, 5.0));
  EXPECT(mna_add_resistor(&circuit, 1, 2, 1000.0));
  EXPECT(mna_add_resistor(&circuit, 2, 0, 1000.0));

  mna_solve(&circuit, &options, &result);

  EXPECT(result.status == MNA_STATUS_SUCCESS);
  EXPECT(nearly_equal(mna_node_voltage(&result, 1), 5.0, 1e-12, 1e-12));
  EXPECT(nearly_equal(mna_node_voltage(&result, 2), 2.5, 1e-12, 1e-12));
  EXPECT(nearly_equal(mna_node_voltage(&result, 0), 0.0, 0.0, 0.0));
  EXPECT(nearly_equal(mna_node_voltage(&result, 3), 0.0, 0.0, 0.0));

  mna_result_free(&result);
  mna_circuit_free(&circuit);
}

static void test_result_reuse(void) {
  MnaCircuit a;
  MnaCircuit b;
  MnaResult result = {0};
  MnaOptions options;

  printf("[14] result reuse\n");
  mna_options_default(&options);

  mna_circuit_init(&a, 1);
  EXPECT(mna_add_voltage_source(&a, 1, 0, 3.0));
  EXPECT(mna_add_resistor(&a, 1, 0, 1000.0));
  mna_solve(&a, &options, &result);
  EXPECT(result.status == MNA_STATUS_SUCCESS);
  EXPECT(nearly_equal(mna_node_voltage(&result, 1), 3.0, 1e-12, 1e-12));
  mna_circuit_free(&a);

  mna_circuit_init(&b, 1);
  EXPECT(mna_add_voltage_source(&b, 1, 0, 7.0));
  EXPECT(mna_add_resistor(&b, 1, 0, 1000.0));
  mna_solve(&b, &options, &result);
  EXPECT(result.status == MNA_STATUS_SUCCESS);
  EXPECT(nearly_equal(mna_node_voltage(&result, 1), 7.0, 1e-12, 1e-12));
  mna_circuit_free(&b);

  mna_result_free(&result);
}

static void test_input_validation(void) {
  MnaCircuit circuit;
  MnaResult result = {0};
  MnaOptions options;

  printf("[15] input validation\n");
  mna_circuit_init(&circuit, 2);
  mna_options_default(&options);

  EXPECT(!mna_add_resistor(&circuit, 1, 1, 1000.0));
  EXPECT(!mna_add_resistor(&circuit, 3, 0, 1000.0));
  EXPECT(!mna_add_resistor(&circuit, 1, 0, 0.0));
  EXPECT(!mna_add_resistor(&circuit, 1, 0, -1.0));
  EXPECT(!mna_add_resistor(&circuit, 1, 0, NAN));

  EXPECT(!mna_add_voltage_source(&circuit, 1, 1, 5.0));
  EXPECT(!mna_add_voltage_source(&circuit, 3, 0, 5.0));
  EXPECT(!mna_add_voltage_source(&circuit, 1, 0, INFINITY));

  EXPECT(!mna_add_diode(&circuit, 1, 1, 1e-12, 1.0, 300.0));
  EXPECT(!mna_add_diode(&circuit, 3, 0, 1e-12, 1.0, 300.0));
  EXPECT(!mna_add_diode(&circuit, 1, 0, 0.0, 1.0, 300.0));
  EXPECT(!mna_add_diode(&circuit, 1, 0, 1e-12, 0.0, 300.0));
  EXPECT(!mna_add_diode(&circuit, 1, 0, 1e-12, 1.0, 0.0));

  mna_solve(NULL, &options, &result);
  EXPECT(result.status == MNA_STATUS_INVALID_ARGUMENT);
  mna_result_free(&result);

  mna_circuit_free(&circuit);
}

static void test_device_failure(void) {
  MnaCircuit circuit;
  MnaResult result = {0};
  MnaOptions options;

  printf("[16] device-load failure propagation\n");
  mna_circuit_init(&circuit, 1);
  mna_options_default(&options);

  EXPECT(mna_add_device(&circuit, &FAILING_OPS, NULL));

  mna_solve(&circuit, &options, &result);
  EXPECT(result.status == MNA_STATUS_DEVICE_FAILURE);

  mna_result_free(&result);
  mna_circuit_free(&circuit);
}

static void test_interleaved_devices(void) {
  MnaCircuit circuit;
  MnaResult result = {0};
  MnaOptions options;
  double source_current;

  printf("[17] interleaved generic devices\n");
  mna_circuit_init(&circuit, 1);
  mna_options_default(&options);

  EXPECT(add_test_capacitor(&circuit, 1, 0, 1e-6));
  EXPECT(mna_add_voltage_source(&circuit, 1, 0, 5.0));
  EXPECT(add_test_current_source(&circuit, 1, 0.001));
  EXPECT(mna_add_resistor(&circuit, 1, 0, 5000.0));

  mna_solve(&circuit, &options, &result);

  EXPECT(result.status == MNA_STATUS_SUCCESS);
  EXPECT(result.iterations == 1);
  EXPECT(nearly_equal(mna_node_voltage(&result, 1), 5.0, 1e-12, 1e-12));

  source_current = mna_voltage_source_current(&circuit, &result, 0);
  EXPECT(nearly_equal(source_current, 0.0, 1e-12, 1e-9));

  mna_result_free(&result);
  mna_circuit_free(&circuit);
}

static void test_trace_semantics(void) {
  MnaCircuit circuit;
  MnaResult result = {0};
  MnaOptions options;
  TraceStats stats = {0};

  printf("[18] trace semantics\n");
  mna_circuit_init(&circuit, 1);
  mna_options_default(&options);
  options.trace = trace_collect;
  options.trace_context = &stats;

  EXPECT(mna_add_voltage_source(&circuit, 1, 0, 5.0));
  EXPECT(mna_add_resistor(&circuit, 1, 0, 1000.0));

  mna_solve(&circuit, &options, &result);

  EXPECT(result.status == MNA_STATUS_SUCCESS);
  EXPECT(result.iterations == 1);
  EXPECT(stats.linear_system_count == 1);
  EXPECT(stats.step_count == 1);
  EXPECT(stats.residual_count == 1);
  EXPECT(stats.converged_count == 1);
  EXPECT(stats.linear_payload_valid);
  EXPECT(stats.step_payload_valid);

  mna_result_free(&result);
  mna_circuit_free(&circuit);
}

static void test_options_validation(void) {
  MnaCircuit circuit;
  MnaOptions options;
  MnaResult result = {0};

  printf("[19] solver options validation\n");
  mna_circuit_init(&circuit, 1);
  mna_options_default(&options);

  EXPECT(mna_add_voltage_source(&circuit, 1, 0, 5.0));
  EXPECT(mna_add_resistor(&circuit, 1, 0, 1000.0));

  options.max_iterations = 0;
  mna_solve(&circuit, &options, &result);
  EXPECT(result.status == MNA_STATUS_INVALID_ARGUMENT);
  mna_result_free(&result);

  mna_options_default(&options);
  options.absolute_residual_tolerance = -1.0;
  mna_solve(&circuit, &options, &result);
  EXPECT(result.status == MNA_STATUS_INVALID_ARGUMENT);
  mna_result_free(&result);

  mna_options_default(&options);
  options.relative_residual_tolerance = -1.0;
  mna_solve(&circuit, &options, &result);
  EXPECT(result.status == MNA_STATUS_INVALID_ARGUMENT);
  mna_result_free(&result);

  mna_options_default(&options);
  options.maximum_voltage_step_v = -1.0;
  mna_solve(&circuit, &options, &result);
  EXPECT(result.status == MNA_STATUS_INVALID_ARGUMENT);
  mna_result_free(&result);

  mna_options_default(&options);
  options.absolute_residual_tolerance = NAN;
  mna_solve(&circuit, &options, &result);
  EXPECT(result.status == MNA_STATUS_INVALID_ARGUMENT);
  mna_result_free(&result);

  mna_options_default(&options);
  options.relative_residual_tolerance = INFINITY;
  mna_solve(&circuit, &options, &result);
  EXPECT(result.status == MNA_STATUS_INVALID_ARGUMENT);
  mna_result_free(&result);

  mna_options_default(&options);
  options.maximum_voltage_step_v = NAN;
  mna_solve(&circuit, &options, &result);
  EXPECT(result.status == MNA_STATUS_INVALID_ARGUMENT);
  mna_result_free(&result);

  mna_options_default(&options);
  options.maximum_voltage_step_v = INFINITY;
  mna_solve(&circuit, &options, &result);
  EXPECT(result.status == MNA_STATUS_INVALID_ARGUMENT);
  mna_result_free(&result);

  mna_options_default(&options);
  mna_solve(&circuit, &options, &result);
  EXPECT(result.status == MNA_STATUS_SUCCESS);
  EXPECT(nearly_equal(mna_node_voltage(&result, 1), 5.0, 1e-12, 1e-12));

  mna_result_free(&result);
  mna_circuit_free(&circuit);
}

static void test_nonfinite_residual(void) {
  MnaCircuit circuit;
  MnaResult result = {0};
  MnaOptions options;

  printf("[20] non-finite residual rejected\n");
  mna_circuit_init(&circuit, 1);
  mna_options_default(&options);

  EXPECT(add_test_nonfinite_device(&circuit, NONFINITE_RESIDUAL));

  mna_solve(&circuit, &options, &result);
  EXPECT(result.status == MNA_STATUS_DEVICE_FAILURE);

  mna_result_free(&result);
  mna_circuit_free(&circuit);
}

static void test_nonfinite_jacobian(void) {
  MnaCircuit circuit;
  MnaResult result = {0};
  MnaOptions options;

  printf("[21] non-finite Jacobian rejected\n");
  mna_circuit_init(&circuit, 1);
  mna_options_default(&options);

  EXPECT(add_test_nonfinite_device(&circuit, NONFINITE_JACOBIAN));

  mna_solve(&circuit, &options, &result);
  EXPECT(result.status == MNA_STATUS_DEVICE_FAILURE);

  mna_result_free(&result);
  mna_circuit_free(&circuit);
}

/* -------------------------------------------------------------------------
 * Complex Circuit Test
 * ------------------------------------------------------------------------- */

static void print_wheatstone_ascii(void) {
  printf("\nCircuit Topology: Wheatstone Diode Bridge\n");
  printf("         (Node 1)               \n");
  printf("        /        \\             \n");
  printf("    R1(1k)      R2(2k)          \n");
  printf("      /            \\           \n");
  printf("  (Node 2) --D1-- (Node 3)      \n");
  printf("      \\            /           \n");
  printf("    R3(2k)      R4(1k)          \n");
  printf("        \\        /             \n");
  printf("         (Node 0)               \n");
  printf("  [ V1(5V) connected 1 -> 0 ]   \n\n");
}

static void test_wheatstone_diode_bridge(void) {
  MnaCircuit circuit;
  MnaResult result = {0};
  MnaValidation validation = {0};
  MnaOptions options;

  printf("[22] Wheatstone Bridge with Nonlinear Center Diode\n");
  print_wheatstone_ascii();

  mna_circuit_init(&circuit, 3);
  mna_options_default(&options);

  options.trace = mna_test_trace;
  options.max_iterations = 50;
  options.absolute_residual_tolerance = 1e-10;
  options.maximum_voltage_step_v = 0.5;

  EXPECT(mna_add_voltage_source(&circuit, 1, 0, 5.0));
  EXPECT(mna_add_resistor(&circuit, 1, 2, 1000.0));
  EXPECT(mna_add_resistor(&circuit, 1, 3, 2000.0));
  EXPECT(mna_add_resistor(&circuit, 2, 0, 2000.0));
  EXPECT(mna_add_resistor(&circuit, 3, 0, 1000.0));
  EXPECT(mna_add_diode(&circuit, 2, 3, 1e-12, 1.0, 300.0));

  mna_solve(&circuit, &options, &result);

  EXPECT(result.status == MNA_STATUS_SUCCESS);

  {
    double v1 = mna_node_voltage(&result, 1);
    double v2 = mna_node_voltage(&result, 2);
    double v3 = mna_node_voltage(&result, 3);

    printf("\nSolved Voltages:\n");
    printf("  V1 = %.6f V\n", v1);
    printf("  V2 = %.6f V\n", v2);
    printf("  V3 = %.6f V\n", v3);

    EXPECT(v2 > v3);
  }

  EXPECT(mna_validate(&circuit, &result, &options, &validation));
  printf("validation residual = %.12e\n", validation.maximum_residual);
  printf("validation KCL      = %.12e\n", validation.maximum_kcl_residual_a);
  EXPECT(validation.valid);
  EXPECT(validation.maximum_residual < 1e-9);

  mna_result_free(&result);
  mna_circuit_free(&circuit);
}

int main(void) {
  printf("============================================\n");
  printf("MNA GENERIC TEST SUITE\n");
  printf("============================================\n");

  test_empty_circuit();
  test_assembly_helpers();
  test_three_resistor_chain();
  test_resistor_divider();
  test_diode();
  test_two_voltage_sources();
  test_extra_unknown_device();
  test_external_current_source();
  test_external_capacitor();
  test_nonlinear_flag();
  test_mixed_linear_nonlinear();
  test_floating_node();
  test_node_accessor_bounds();
  test_result_reuse();
  test_input_validation();
  test_device_failure();
  test_interleaved_devices();
  test_trace_semantics();
  test_options_validation();
  test_nonfinite_residual();
  test_nonfinite_jacobian();
  test_wheatstone_diode_bridge();

  printf("\n============================================\n");
  if (failures == 0) {
    printf("MNA GENERIC TEST SUITE: PASS\n");
  } else {
    printf("MNA GENERIC TEST SUITE: %d FAILURE(S)\n", failures);
  }
  printf("============================================\n");

  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

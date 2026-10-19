/* mna.c */
#include "mna.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const double MNA_BOLTZMANN = 1.380649e-23;
static const double MNA_ELEMENTARY_CHARGE = 1.602176634e-19;

static int vector_is_finite(const double *values, size_t count) {
  size_t i;

  if (values == NULL) {
    return count == 0;
  }

  for (i = 0; i < count; ++i) {
    if (!isfinite(values[i])) {
      return 0;
    }
  }

  return 1;
}

static int matrix_is_finite(const Matrix *matrix) {
  size_t row;
  size_t col;

  if (matrix == NULL) {
    return 0;
  }

  if (matrix->data == NULL) {
    return matrix->rows == 0 || matrix->cols == 0;
  }

  for (row = 0; row < matrix->rows; ++row) {
    for (col = 0; col < matrix->cols; ++col) {
      if (!isfinite(matrix->data[row * matrix->cols + col])) {
        return 0;
      }
    }
  }

  return 1;
}

static int valid_node(const MnaCircuit *circuit, size_t node) {
  return circuit != NULL && node <= circuit->node_count;
}

static int grow_array(void **data, size_t *capacity, size_t required_count,
                      size_t element_size) {
  size_t new_capacity;
  size_t bytes;
  void *new_data;

  if (data == NULL || capacity == NULL || element_size == 0) {
    return 0;
  }

  if (required_count <= *capacity) {
    return 1;
  }

  new_capacity = (*capacity == 0) ? 4 : *capacity;

  while (new_capacity < required_count) {
    if (new_capacity > SIZE_MAX / 2) {
      new_capacity = required_count;
      break;
    }
    new_capacity *= 2;
  }

  if (new_capacity > SIZE_MAX / element_size) {
    return 0;
  }

  bytes = new_capacity * element_size;
  new_data = realloc(*data, bytes);

  if (new_data == NULL) {
    return 0;
  }

  *data = new_data;
  *capacity = new_capacity;
  return 1;
}

/* --------------------------------------------------------------------------
 * Generic assembly helpers
 * --------------------------------------------------------------------------
 */

int mna_assembly_add_jacobian(MnaAssembly *assembly, size_t row, size_t column,
                              double value) {
  if (assembly == NULL || assembly->jacobian == NULL ||
      assembly->jacobian->data == NULL || row >= assembly->unknown_count ||
      column >= assembly->unknown_count) {
    return 0;
  }

  assembly->jacobian->data[row * assembly->jacobian->cols + column] += value;

  return 1;
}

int mna_assembly_add_residual(MnaAssembly *assembly, size_t row, double value) {
  if (assembly == NULL || assembly->residual == NULL ||
      row >= assembly->unknown_count) {
    return 0;
  }

  assembly->residual[row] += value;
  return 1;
}

int mna_assembly_node_index(size_t node, size_t *index) {
  if (index == NULL || node == 0) {
    return 0;
  }

  *index = node - 1;
  return 1;
}

double mna_assembly_node_voltage(const MnaAssembly *assembly, size_t node) {
  size_t index;

  if (assembly == NULL || assembly->x == NULL ||
      !mna_assembly_node_index(node, &index) || node > assembly->node_count) {
    return 0.0;
  }

  return assembly->x[index];
}

int mna_device_unknown_index(const MnaDevice *device, size_t local_unknown,
                             size_t *global_index) {
  if (device == NULL || global_index == NULL ||
      local_unknown >= device->unknown_count ||
      local_unknown > SIZE_MAX - device->unknown_offset) {
    return 0;
  }

  *global_index = device->unknown_offset + local_unknown;
  return 1;
}

static void mna_trace(const MnaOptions *options, MnaTraceEventKind kind,
                      size_t iteration, size_t unknown_count, const double *x,
                      const double *residual, const Matrix *jacobian,
                      const double *rhs, const double *raw_delta,
                      const double *applied_delta, double maximum_residual,
                      double maximum_raw_step, double maximum_applied_step,
                      double step_scale) {
  MnaTraceEvent event;

  if (options == NULL || options->trace == NULL) {
    return;
  }

  event.kind = kind;
  event.iteration = iteration;
  event.unknown_count = unknown_count;
  event.x = x;
  event.residual = residual;
  event.jacobian = jacobian;
  event.rhs = rhs;
  event.raw_delta = raw_delta;
  event.applied_delta = applied_delta;
  event.maximum_residual = maximum_residual;
  event.maximum_raw_step = maximum_raw_step;
  event.maximum_applied_step = maximum_applied_step;
  event.step_scale = step_scale;

  options->trace(options->trace_context, &event);
}

/* --------------------------------------------------------------------------
 * Device lifetime and registration
 * --------------------------------------------------------------------------
 */

void mna_circuit_init(MnaCircuit *circuit, size_t node_count) {
  if (circuit == NULL) {
    return;
  }

  memset(circuit, 0, sizeof(*circuit));
  circuit->node_count = node_count;
  circuit->unknown_count = node_count;
}

void mna_circuit_free(MnaCircuit *circuit) {
  size_t i;

  if (circuit == NULL) {
    return;
  }

  for (i = 0; i < circuit->device_count; ++i) {
    MnaDevice *device = &circuit->devices[i];

    if (device->ops != NULL && device->ops->destroy != NULL) {
      device->ops->destroy(device->data);
    }
  }

  free(circuit->devices);
  memset(circuit, 0, sizeof(*circuit));
}

int mna_add_device(MnaCircuit *circuit, const MnaDeviceOps *ops, void *data) {
  MnaDevice *device;
  size_t local_unknown_count;
  size_t required_unknown_count;

  if (circuit == NULL || ops == NULL || ops->load == NULL) {
    return 0;
  }

  local_unknown_count = 0;

  if (ops->unknown_count != NULL) {
    local_unknown_count = ops->unknown_count(data);
  }

  if (local_unknown_count > SIZE_MAX - circuit->unknown_count) {
    return 0;
  }

  if (circuit->device_count == SIZE_MAX) {
    return 0;
  }

  required_unknown_count = circuit->device_count + 1;

  if (!grow_array((void **)&circuit->devices, &circuit->device_capacity,
                  required_unknown_count, sizeof(MnaDevice))) {
    return 0;
  }

  device = &circuit->devices[circuit->device_count++];
  device->ops = ops;
  device->data = data;
  device->unknown_offset = circuit->unknown_count;
  device->unknown_count = local_unknown_count;

  circuit->unknown_count += local_unknown_count;
  return 1;
}

const MnaDevice *mna_device_at(const MnaCircuit *circuit, size_t index) {
  if (circuit == NULL || index >= circuit->device_count) {
    return NULL;
  }

  return &circuit->devices[index];
}

/* --------------------------------------------------------------------------
 * Built-in models
 * --------------------------------------------------------------------------
 */

static size_t no_extra_unknowns(const void *data) {
  (void)data;
  return 0;
}

static size_t voltage_source_unknowns(const void *data) {
  (void)data;
  return 1;
}

static double node_voltage_from_x(const MnaAssembly *assembly, size_t node) {
  return mna_assembly_node_voltage(assembly, node);
}

static void add_two_terminal_residual(MnaAssembly *assembly, size_t node_a,
                                      size_t node_b, double current) {
  if (node_a != 0) {
    (void)mna_assembly_add_residual(assembly, node_a - 1, current);
  }

  if (node_b != 0) {
    (void)mna_assembly_add_residual(assembly, node_b - 1, -current);
  }
}

static void stamp_two_terminal_conductance(MnaAssembly *assembly, size_t node_a,
                                           size_t node_b, double conductance) {
  if (node_a != 0) {
    (void)mna_assembly_add_jacobian(assembly, node_a - 1, node_a - 1,
                                    conductance);
  }

  if (node_b != 0) {
    (void)mna_assembly_add_jacobian(assembly, node_b - 1, node_b - 1,
                                    conductance);
  }

  if (node_a != 0 && node_b != 0) {
    (void)mna_assembly_add_jacobian(assembly, node_a - 1, node_b - 1,
                                    -conductance);
    (void)mna_assembly_add_jacobian(assembly, node_b - 1, node_a - 1,
                                    -conductance);
  }
}

static int resistor_load(void *data, const MnaDevice *device,
                         MnaAssembly *assembly) {
  MnaResistor *resistor = (MnaResistor *)data;
  double va;
  double vb;
  double voltage;
  double conductance;
  double current;

  (void)device;

  if (resistor == NULL || assembly == NULL) {
    return 0;
  }

  va = node_voltage_from_x(assembly, resistor->node_a);
  vb = node_voltage_from_x(assembly, resistor->node_b);
  voltage = va - vb;
  conductance = 1.0 / resistor->resistance_ohm;
  current = conductance * voltage;

  add_two_terminal_residual(assembly, resistor->node_a, resistor->node_b,
                            current);

  stamp_two_terminal_conductance(assembly, resistor->node_a, resistor->node_b,
                                 conductance);

  return 1;
}

static int voltage_source_branch_current(const void *data,
                                         size_t *local_unknown_index) {
  (void)data;

  if (local_unknown_index == NULL) {
    return 0;
  }

  *local_unknown_index = 0;
  return 1;
}

static int voltage_source_load(void *data, const MnaDevice *device,
                               MnaAssembly *assembly) {
  MnaVoltageSource *source = (MnaVoltageSource *)data;
  size_t branch_index;
  size_t branch_unknown;
  double va;
  double vb;
  double branch_current;

  if (source == NULL || device == NULL || assembly == NULL) {
    return 0;
  }

  branch_index = device->unknown_offset;
  branch_unknown = branch_index;

  va = node_voltage_from_x(assembly, source->node_a);
  vb = node_voltage_from_x(assembly, source->node_b);

  if (assembly->x == NULL || branch_unknown >= assembly->unknown_count) {
    return 0;
  }

  branch_current = assembly->x[branch_unknown];

  add_two_terminal_residual(assembly, source->node_a, source->node_b,
                            branch_current);

  (void)mna_assembly_add_residual(assembly, branch_unknown,
                                  va - vb - source->voltage_v);

  if (source->node_a != 0) {
    (void)mna_assembly_add_jacobian(assembly, source->node_a - 1,
                                    branch_unknown, 1.0);
    (void)mna_assembly_add_jacobian(assembly, branch_unknown,
                                    source->node_a - 1, 1.0);
  }

  if (source->node_b != 0) {
    (void)mna_assembly_add_jacobian(assembly, source->node_b - 1,
                                    branch_unknown, -1.0);
    (void)mna_assembly_add_jacobian(assembly, branch_unknown,
                                    source->node_b - 1, -1.0);
  }

  return 1;
}

static double diode_thermal_voltage(const MnaDiode *diode) {
  return (MNA_BOLTZMANN * diode->temperature_k) / MNA_ELEMENTARY_CHARGE;
}

void mna_diode_evaluate(const MnaDiode *diode, double voltage_v,
                           double *current_a, double *conductance_s) {
  double vt;
  double exponent;
  double exp_value;

  vt = diode_thermal_voltage(diode);
  exponent = voltage_v / (diode->ideality_factor * vt);

  if (exponent > 40.0) {
    exponent = 40.0;
  }

  if (exponent < -40.0) {
    exponent = -40.0;
  }

  exp_value = exp(exponent);

  *current_a = diode->saturation_current_a * (exp_value - 1.0);
  *conductance_s =
      diode->saturation_current_a * exp_value / (diode->ideality_factor * vt);
}

static int diode_load(void *data, const MnaDevice *device,
                      MnaAssembly *assembly) {
  MnaDiode *diode = (MnaDiode *)data;
  double va;
  double vc;
  double vd;
  double current;
  double conductance;

  (void)device;

  if (diode == NULL || assembly == NULL) {
    return 0;
  }

  va = node_voltage_from_x(assembly, diode->node_anode);
  vc = node_voltage_from_x(assembly, diode->node_cathode);
  vd = va - vc;

  mna_diode_evaluate(diode, vd, &current, &conductance);

  add_two_terminal_residual(assembly, diode->node_anode, diode->node_cathode,
                            current);

  stamp_two_terminal_conductance(assembly, diode->node_anode,
                                 diode->node_cathode, conductance);

  return 1;
}

static void destroy_payload(void *data) { free(data); }

static const MnaDeviceOps RESISTOR_OPS = {.flags = MNA_DEVICE_LINEAR,
                                          .unknown_count = no_extra_unknowns,
                                          .load = resistor_load,
                                          .destroy = destroy_payload,
                                          .branch_current = NULL};

static const MnaDeviceOps VOLTAGE_SOURCE_OPS = {
    .flags = MNA_DEVICE_LINEAR,
    .unknown_count = voltage_source_unknowns,
    .load = voltage_source_load,
    .destroy = destroy_payload,
    .branch_current = voltage_source_branch_current};

static const MnaDeviceOps DIODE_OPS = {.flags = MNA_DEVICE_NONLINEAR,
                                       .unknown_count = no_extra_unknowns,
                                       .load = diode_load,
                                       .destroy = destroy_payload,
                                       .branch_current = NULL};

int mna_add_resistor(MnaCircuit *circuit, size_t node_a, size_t node_b,
                     double resistance_ohm) {
  MnaResistor *resistor;

  if (!valid_node(circuit, node_a) || !valid_node(circuit, node_b) ||
      node_a == node_b || !isfinite(resistance_ohm) || resistance_ohm <= 0.0) {
    return 0;
  }

  resistor = (MnaResistor *)malloc(sizeof(*resistor));

  if (resistor == NULL) {
    return 0;
  }

  resistor->node_a = node_a;
  resistor->node_b = node_b;
  resistor->resistance_ohm = resistance_ohm;

  if (!mna_add_device(circuit, &RESISTOR_OPS, resistor)) {
    free(resistor);
    return 0;
  }

  return 1;
}

int mna_add_voltage_source(MnaCircuit *circuit, size_t node_a, size_t node_b,
                           double voltage_v) {
  MnaVoltageSource *source;

  if (!valid_node(circuit, node_a) || !valid_node(circuit, node_b) ||
      node_a == node_b || !isfinite(voltage_v)) {
    return 0;
  }

  source = (MnaVoltageSource *)malloc(sizeof(*source));

  if (source == NULL) {
    return 0;
  }

  source->node_a = node_a;
  source->node_b = node_b;
  source->voltage_v = voltage_v;

  if (!mna_add_device(circuit, &VOLTAGE_SOURCE_OPS, source)) {
    free(source);
    return 0;
  }

  return 1;
}

int mna_add_diode(MnaCircuit *circuit, size_t node_anode, size_t node_cathode,
                  double saturation_current_a, double ideality_factor,
                  double temperature_k) {
  MnaDiode *diode;

  if (!valid_node(circuit, node_anode) || !valid_node(circuit, node_cathode) ||
      node_anode == node_cathode || !isfinite(saturation_current_a) ||
      saturation_current_a <= 0.0 || !isfinite(ideality_factor) ||
      ideality_factor <= 0.0 || !isfinite(temperature_k) ||
      temperature_k <= 0.0) {
    return 0;
  }

  diode = (MnaDiode *)malloc(sizeof(*diode));

  if (diode == NULL) {
    return 0;
  }

  diode->node_anode = node_anode;
  diode->node_cathode = node_cathode;
  diode->saturation_current_a = saturation_current_a;
  diode->ideality_factor = ideality_factor;
  diode->temperature_k = temperature_k;

  if (!mna_add_device(circuit, &DIODE_OPS, diode)) {
    free(diode);
    return 0;
  }

  return 1;
}

/* --------------------------------------------------------------------------
 * Solver
 * --------------------------------------------------------------------------
 */

static int mna_options_valid(const MnaOptions *options) {
  if (options == NULL) {
    return 0;
  }

  if (options->max_iterations == 0) {
    return 0;
  }

  if (!isfinite(options->absolute_residual_tolerance) ||
      options->absolute_residual_tolerance < 0.0) {
    return 0;
  }

  if (!isfinite(options->relative_residual_tolerance) ||
      options->relative_residual_tolerance < 0.0) {
    return 0;
  }

  if (!isfinite(options->maximum_voltage_step_v) ||
      options->maximum_voltage_step_v < 0.0) {
    return 0;
  }

  return 1;
}

void mna_options_default(MnaOptions *options) {
  if (options == NULL) {
    return;
  }

  options->max_iterations = 100;
  options->absolute_residual_tolerance = 1.0e-10;
  options->relative_residual_tolerance = 1.0e-10;
  options->maximum_voltage_step_v = 0.25;
  options->trace = NULL;
  options->trace_context = NULL;
}

void mna_result_free(MnaResult *result) {
  if (result == NULL) {
    return;
  }

  free(result->x);
  memset(result, 0, sizeof(*result));
}

static double vector_max_abs(const double *values, size_t count) {
  double maximum = 0.0;
  size_t i;

  if (values == NULL) {
    return 0.0;
  }

  for (i = 0; i < count; ++i) {
    double value = fabs(values[i]);

    if (value > maximum) {
      maximum = value;
    }
  }

  return maximum;
}

static void matrix_zero(Matrix *matrix) {
  if (matrix == NULL || matrix->data == NULL) {
    return;
  }

  memset(matrix->data, 0, matrix->rows * matrix->cols * sizeof(double));
}

static void residual_zero(double *residual, size_t count) {
  if (residual == NULL) {
    return;
  }

  memset(residual, 0, count * sizeof(double));
}

void mna_solve(const MnaCircuit *circuit, const MnaOptions *options,
               MnaResult *result) {
  MnaOptions local_options;
  Matrix jacobian = {0};
  double *residual = NULL;
  double *rhs = NULL;
  double *raw_delta = NULL;
  double *applied_delta = NULL;
  size_t unknown_count;
  size_t iteration = 0;
  int device_failure = 0;
  double initial_residual_scale = 1.0;

  if (result == NULL) {
    return;
  }

  mna_result_free(result);

  if (circuit == NULL) {
    result->status = MNA_STATUS_INVALID_ARGUMENT;
    return;
  }

  if (options == NULL) {
    mna_options_default(&local_options);
    options = &local_options;
  }

  if (!mna_options_valid(options)) {
    result->status = MNA_STATUS_INVALID_ARGUMENT;
    return;
  }

  unknown_count = circuit->unknown_count;
  result->node_count = circuit->node_count;
  result->unknown_count = unknown_count;
  int has_nonlinear_device = 0;

  for (size_t i = 0; i < circuit->device_count; ++i) {
    const MnaDevice *device = &circuit->devices[i];

    if (device->ops != NULL &&
        (device->ops->flags & MNA_DEVICE_NONLINEAR) != 0u) {
      has_nonlinear_device = 1;
      break;
    }
  }

  if (unknown_count == 0) {
    result->status = MNA_STATUS_SUCCESS;
    return;
  }

  result->x = (double *)calloc(unknown_count, sizeof(double));
  residual = (double *)calloc(unknown_count, sizeof(double));
  rhs = (double *)calloc(unknown_count, sizeof(double));
  raw_delta = (double *)calloc(unknown_count, sizeof(double));
  applied_delta = (double *)calloc(unknown_count, sizeof(double));

  if (result->x == NULL || residual == NULL || rhs == NULL ||
      raw_delta == NULL || applied_delta == NULL) {
    result->status = MNA_STATUS_ALLOCATION_FAILURE;
    goto cleanup;
  }

  if (!matrix_create(&jacobian, unknown_count, unknown_count)) {
    result->status = MNA_STATUS_ALLOCATION_FAILURE;
    goto cleanup;
  }

  for (iteration = 0; iteration < options->max_iterations; ++iteration) {

    MnaAssembly assembly;
    size_t i;
    double max_raw_step = 0.0;
    double step_scale = 1.0;
    double max_residual;
    double max_kcl_residual = 0.0;

    matrix_zero(&jacobian);
    residual_zero(residual, unknown_count);
    residual_zero(rhs, unknown_count);
    residual_zero(raw_delta, unknown_count);
    residual_zero(applied_delta, unknown_count);

    assembly.x = result->x;
    assembly.jacobian = &jacobian;
    assembly.residual = residual;
    assembly.node_count = circuit->node_count;
    assembly.unknown_count = unknown_count;

    for (i = 0; i < circuit->device_count; ++i) {
      const MnaDevice *device = &circuit->devices[i];

      if (device->ops == NULL || device->ops->load == NULL ||
          !device->ops->load(device->data, device, &assembly)) {
        device_failure = 1;
        break;
      }
    }

    if (device_failure) {
      result->status = MNA_STATUS_DEVICE_FAILURE;
      break;
    }

    if (!vector_is_finite(residual, unknown_count) ||
        !matrix_is_finite(&jacobian)) {
      result->status = MNA_STATUS_DEVICE_FAILURE;
      break;
    }

    if (iteration == 0) {
      initial_residual_scale = vector_max_abs(residual, unknown_count);

      if (initial_residual_scale < 1.0) {
        initial_residual_scale = 1.0;
      }
    }

    result->initial_residual_scale = initial_residual_scale;

    for (i = 0; i < unknown_count; ++i) {
      rhs[i] = -residual[i];
    }

    mna_trace(options, MNA_TRACE_LINEAR_SYSTEM, iteration + 1, unknown_count,
              result->x, residual, &jacobian, rhs, NULL, NULL,
              vector_max_abs(residual, unknown_count), 0.0, 0.0, 1.0);

    if (!matrix_solve(&jacobian, rhs, raw_delta)) {
      result->status = MNA_STATUS_SINGULAR_MATRIX;
      break;
    }

    if (!vector_is_finite(raw_delta, unknown_count)) {
      result->status = MNA_STATUS_SINGULAR_MATRIX;
      break;
    }

    for (i = 0; i < circuit->node_count; ++i) {
      double absolute_step = fabs(raw_delta[i]);

      if (absolute_step > max_raw_step) {
        max_raw_step = absolute_step;
      }
    }

    memcpy(applied_delta, raw_delta, unknown_count * sizeof(double));

    if (has_nonlinear_device && options->maximum_voltage_step_v > 0.0 &&
        max_raw_step > options->maximum_voltage_step_v) {

      step_scale = options->maximum_voltage_step_v / max_raw_step;
      for (i = 0; i < unknown_count; ++i) {
        applied_delta[i] *= step_scale;
      }
    }

    double max_applied_step = 0.0;

    for (i = 0; i < circuit->node_count; ++i) {
      double absolute_step = fabs(applied_delta[i]);

      if (absolute_step > max_applied_step) {
        max_applied_step = absolute_step;
      }
    }

    /* Trace raw & applied step BEFORE updating state x */
    mna_trace(options, MNA_TRACE_STEP, iteration + 1, unknown_count, result->x,
              residual, NULL, NULL, raw_delta, applied_delta, 0.0, max_raw_step,
              max_applied_step, step_scale);

    for (i = 0; i < unknown_count; ++i) {
      result->x[i] += applied_delta[i];
    }

    /* Evaluate exact residual at the new state x_{k+1} */
    matrix_zero(&jacobian);
    residual_zero(residual, unknown_count);

    assembly.x = result->x;

    for (i = 0; i < circuit->device_count; ++i) {
      const MnaDevice *device = &circuit->devices[i];

      if (device->ops == NULL || device->ops->load == NULL ||
          !device->ops->load(device->data, device, &assembly)) {
        device_failure = 1;
        break;
      }
    }

    if (device_failure) {
      result->status = MNA_STATUS_DEVICE_FAILURE;
      break;
    }

    if (!vector_is_finite(residual, unknown_count) ||
        !matrix_is_finite(&jacobian)) {
      result->status = MNA_STATUS_DEVICE_FAILURE;
      break;
    }

    max_residual = vector_max_abs(residual, unknown_count);

    for (i = 0; i < circuit->node_count; ++i) {
      double value = fabs(residual[i]);
      if (value > max_kcl_residual) {
        max_kcl_residual = value;
      }
    }

    result->iterations = iteration + 1;
    result->maximum_voltage_step_v = max_applied_step;
    result->maximum_residual = max_residual;
    result->maximum_kcl_residual_a = max_kcl_residual;

    /* Trace new residual state */
    mna_trace(options, MNA_TRACE_RESIDUAL, iteration + 1, unknown_count,
              result->x, residual, NULL, NULL, raw_delta, applied_delta,
              max_residual, max_raw_step, max_applied_step, step_scale);

    if (max_residual <=
        options->absolute_residual_tolerance +
            options->relative_residual_tolerance * initial_residual_scale) {
      mna_trace(options, MNA_TRACE_CONVERGED, iteration + 1, unknown_count,
                result->x, residual, NULL, NULL, raw_delta, applied_delta,
                max_residual, max_raw_step, max_applied_step, step_scale);
      result->status = MNA_STATUS_SUCCESS;
      break;
    }
  }

  if (iteration >= options->max_iterations && result->status == 0) {
    result->status = MNA_STATUS_NOT_CONVERGED;
  }

cleanup:

  free(residual);
  free(rhs);
  free(raw_delta);
  free(applied_delta);
  matrix_free(&jacobian);

  if (result->status != MNA_STATUS_SUCCESS &&
      result->status != MNA_STATUS_NOT_CONVERGED) {
    if (result->x == NULL) {
      result->unknown_count = 0;
    }
  }
}

/* --------------------------------------------------------------------------
 * Result accessors
 * --------------------------------------------------------------------------
 */

double mna_node_voltage(const MnaResult *result, size_t node) {
  if (result == NULL || result->x == NULL || node == 0 ||
      node > result->node_count) {
    return 0.0;
  }

  return result->x[node - 1];
}

double mna_voltage_source_current(const MnaCircuit *circuit,
                                  const MnaResult *result,
                                  size_t source_index) {
  size_t seen = 0;
  size_t i;

  if (circuit == NULL || result == NULL || result->x == NULL) {
    return 0.0;
  }

  for (i = 0; i < circuit->device_count; ++i) {
    const MnaDevice *device = &circuit->devices[i];
    size_t local_unknown;
    size_t global_unknown;

    if (device->ops == NULL || device->ops->branch_current == NULL ||
        !device->ops->branch_current(device->data, &local_unknown)) {
      continue;
    }

    if (seen == source_index &&
        mna_device_unknown_index(device, local_unknown, &global_unknown) &&
        global_unknown < result->unknown_count) {
      return result->x[global_unknown];
    }

    ++seen;
  }

  return 0.0;
}

/* --------------------------------------------------------------------------
 * Independent validation
 * --------------------------------------------------------------------------
 */

int mna_validate(const MnaCircuit *circuit, const MnaResult *result,
                 const MnaOptions *options, MnaValidation *validation) {
  MnaOptions local_options;
  MnaAssembly assembly;
  Matrix jacobian = {0};
  double *residual = NULL;
  size_t i;
  int ok = 1;

  if (validation != NULL) {
    memset(validation, 0, sizeof(*validation));
  }

  if (circuit == NULL || result == NULL || result->x == NULL ||
      validation == NULL) {
    return 0;
  }

  if (options == NULL) {
    mna_options_default(&local_options);
    options = &local_options;
  }

  residual = (double *)calloc(result->unknown_count, sizeof(double));

  if (result->unknown_count != 0 && residual == NULL) {
    return 0;
  }

  if (!matrix_create(&jacobian, result->unknown_count, result->unknown_count)) {
    free(residual);
    return 0;
  }

  assembly.x = result->x;
  assembly.jacobian = &jacobian;
  assembly.residual = residual;
  assembly.node_count = circuit->node_count;
  assembly.unknown_count = result->unknown_count;

  for (i = 0; i < circuit->device_count; ++i) {
    const MnaDevice *device = &circuit->devices[i];

    if (device->ops == NULL || device->ops->load == NULL ||
        !device->ops->load(device->data, device, &assembly)) {
      ok = 0;
      break;
    }
  }

  if (ok) {
    validation->maximum_residual =
        vector_max_abs(residual, result->unknown_count);

    validation->maximum_kcl_residual_a =
        vector_max_abs(residual, circuit->node_count);

    validation->valid = validation->maximum_residual <=
                        options->absolute_residual_tolerance +
                            options->relative_residual_tolerance *
                                result->initial_residual_scale;
  }

  if (!isfinite(result->initial_residual_scale) ||
      result->initial_residual_scale < 1.0) {
    return 0;
  }

  matrix_free(&jacobian);
  free(residual);



  return ok && validation->valid;
}

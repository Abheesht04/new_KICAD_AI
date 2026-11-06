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

/*
 * exp(x), continued linearly above x = 40 so that value and slope stay
 * consistent (C1): a Newton step taken beyond the limit sees the true slope of
 * the function it is solving. Below the limit it is exact (exp underflows to 0
 * for large negative x, which is harmless).
 */
double mna_limexp(double x, double *slope) {
  static const double LIMIT = 40.0;

  if (x > LIMIT) {
    double e = exp(LIMIT);
    *slope = e;
    return e * (1.0 + (x - LIMIT));
  }

  *slope = exp(x);
  return *slope;
}

void mna_diode_evaluate(const MnaDiode *diode, double voltage_v,
                        double *current_a, double *conductance_s) {
  double nvt = diode->ideality_factor * diode_thermal_voltage(diode);
  double slope;
  double e = mna_limexp(voltage_v / nvt, &slope);

  *current_a = diode->saturation_current_a * (e - 1.0);
  *conductance_s = diode->saturation_current_a * slope / nvt;
}

/* SPICE junction-voltage limiting (pnjlim; Nagel, SPICE2, UCB/ERL M520, 1975): above the critical voltage a large
 * increase is compressed logarithmically, so the exponential stays in range while the rest of the circuit takes
 * a full Newton step. vold is the previous linearisation's (limited) value. */
static double pnjlim(double vnew, double vold, double nvt, double is) {
  double vcrit = nvt * log(nvt / (sqrt(2.0) * is));
  if (vnew > vcrit && fabs(vnew - vold) > 2.0 * nvt) {
    if (vold > 0.0) {
      double arg = 1.0 + (vnew - vold) / nvt;
      vnew = arg > 0.0 ? vold + nvt * log(arg) : vcrit;
    } else {
      vnew = nvt * log(vnew / nvt);
    }
  }
  return vnew;
}

/* The junction voltage to evaluate at: exact (limit 0), the first linearisation of a solve (1, starts the history),
 * or limited against the previous linearisation (2). Updates the history for nonzero limit. */
static double junction(const MnaAssembly *assembly, double v, double *last, double nvt, double is) {
  if (assembly->limit == 0) {
    return v;
  }
  if (assembly->limit == 2) {
    v = pnjlim(v, *last, nvt, is);
  }
  *last = v;
  return v;
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
  /* The companion model at the evaluation voltage: I(v_eval) + G (vd - v_eval); exact when they are equal. */
  double v_eval = junction(assembly, vd, &diode->v_last, diode->ideality_factor * diode_thermal_voltage(diode),
                           diode->saturation_current_a);

  mna_diode_evaluate(diode, v_eval, &current, &conductance);
  current += conductance * (vd - v_eval);

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
  diode->v_last = 0.0;

  if (!mna_add_device(circuit, &DIODE_OPS, diode)) {
    free(diode);
    return 0;
  }

  return 1;
}

/* --------------------------------------------------------------------------
 * Bipolar transistor: first-order Ebers-Moll, transport form
 * --------------------------------------------------------------------------
 *
 * NPN, with Vbe = Vb - Ve and Vbc = Vb - Vc:
 *     If = Is (exp(Vbe/(Nf Vt)) - 1)        Ir = Is (exp(Vbc/(Nr Vt)) - 1)
 *     Ic = If - Ir (1 + 1/Br)               Ib = If/Bf + Ir/Br
 *     Ie = -(Ic + Ib)                        (all currents INTO the terminal)
 * PNP: the same equations on negated terminal voltages, with negated currents.
 * The Jacobian then has the same form, evaluated at the transformed voltages.
 */
static double thermal_voltage(double temperature_k) {
  return MNA_BOLTZMANN * temperature_k / MNA_ELEMENTARY_CHARGE;
}

void mna_bjt_evaluate(const MnaBjt *bjt, double vc, double vb, double ve,
                      MnaBjtEval *out) {
  double p = bjt->polarity < 0 ? -1.0 : 1.0;
  double vt = thermal_voltage(bjt->temperature_k);
  double sf, sr;
  double ef = mna_limexp(p * (vb - ve) / (bjt->nf * vt), &sf);
  double er = mna_limexp(p * (vb - vc) / (bjt->nr * vt), &sr);
  double i_f = bjt->saturation_current_a * (ef - 1.0);
  double i_r = bjt->saturation_current_a * (er - 1.0);
  double gf = bjt->saturation_current_a * sf / (bjt->nf * vt); /* dIf/dVbe */
  double gr = bjt->saturation_current_a * sr / (bjt->nr * vt); /* dIr/dVbc */
  double kr = 1.0 + 1.0 / bjt->beta_r;

  out->ic = p * (i_f - i_r * kr);
  out->ib = p * (i_f / bjt->beta_f + i_r / bjt->beta_r);
  out->ie = -(out->ic + out->ib);

  /* Rows: Ic, Ib, Ie. Columns: Vc, Vb, Ve. */
  out->d[0][0] = gr * kr;
  out->d[0][1] = gf - gr * kr;
  out->d[0][2] = -gf;
  out->d[1][0] = -gr / bjt->beta_r;
  out->d[1][1] = gf / bjt->beta_f + gr / bjt->beta_r;
  out->d[1][2] = -gf / bjt->beta_f;
  for (int j = 0; j < 3; ++j) {
    out->d[2][j] = -(out->d[0][j] + out->d[1][j]);
  }
}

/* Adds three-terminal currents (into the device) and their Jacobian. */
static void stamp_three_terminal(MnaAssembly *assembly, const size_t *nodes,
                                 const double *current, double d[3][3]) {
  for (int i = 0; i < 3; ++i) {
    if (nodes[i] == 0) {
      continue;
    }
    (void)mna_assembly_add_residual(assembly, nodes[i] - 1, current[i]);
    for (int j = 0; j < 3; ++j) {
      if (nodes[j] != 0) {
        (void)mna_assembly_add_jacobian(assembly, nodes[i] - 1, nodes[j] - 1,
                                        d[i][j]);
      }
    }
  }
}

static int bjt_load(void *data, const MnaDevice *device, MnaAssembly *assembly) {
  MnaBjt *bjt = (MnaBjt *)data;
  MnaBjtEval e;
  size_t nodes[3];
  double current[3];

  (void)device;
  if (bjt == NULL || assembly == NULL) {
    return 0;
  }

  nodes[0] = bjt->node_collector;
  nodes[1] = bjt->node_base;
  nodes[2] = bjt->node_emitter;
  double v[3], ev[3], p = bjt->polarity < 0 ? -1.0 : 1.0, vt = thermal_voltage(bjt->temperature_k);
  for (int i = 0; i < 3; ++i) {
    v[i] = node_voltage_from_x(assembly, nodes[i]);
  }
  /* Both junctions limited (polarity applied); evaluated at node voltages that give the limited junctions with the
   * base unchanged, then extended linearly to the true voltages: I(v_eval) + dI/dV (v - v_eval). */
  double vbe = junction(assembly, p * (v[1] - v[2]), &bjt->vbe_last, bjt->nf * vt, bjt->saturation_current_a);
  double vbc = junction(assembly, p * (v[1] - v[0]), &bjt->vbc_last, bjt->nr * vt, bjt->saturation_current_a);
  ev[1] = v[1];
  ev[2] = v[1] - p * vbe;
  ev[0] = v[1] - p * vbc;
  mna_bjt_evaluate(bjt, ev[0], ev[1], ev[2], &e);
  current[0] = e.ic;
  current[1] = e.ib;
  current[2] = e.ie;
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      current[i] += e.d[i][j] * (v[j] - ev[j]);
    }
  }
  stamp_three_terminal(assembly, nodes, current, e.d);
  return 1;
}

static const MnaDeviceOps BJT_OPS = {.flags = MNA_DEVICE_NONLINEAR,
                                     .unknown_count = no_extra_unknowns,
                                     .load = bjt_load,
                                     .destroy = destroy_payload,
                                     .branch_current = NULL};

static int positive_finite(double v) { return isfinite(v) && v > 0.0; }

int mna_add_bjt(MnaCircuit *circuit, const MnaBjt *params) {
  MnaBjt *bjt;

  if (circuit == NULL || params == NULL ||
      !valid_node(circuit, params->node_collector) ||
      !valid_node(circuit, params->node_base) ||
      !valid_node(circuit, params->node_emitter) ||
      (params->polarity != 1 && params->polarity != -1) ||
      !positive_finite(params->saturation_current_a) ||
      !positive_finite(params->beta_f) || !positive_finite(params->beta_r) ||
      !positive_finite(params->nf) || !positive_finite(params->nr) ||
      !positive_finite(params->temperature_k)) {
    return 0;
  }

  bjt = (MnaBjt *)malloc(sizeof(*bjt));
  if (bjt == NULL) {
    return 0;
  }
  *bjt = *params;
  if (!mna_add_device(circuit, &BJT_OPS, bjt)) {
    free(bjt);
    return 0;
  }
  return 1;
}

/* --------------------------------------------------------------------------
 * MOSFET: Level-1 Shichman-Hodges, body tied to source
 * --------------------------------------------------------------------------
 *
 * NMOS with beta = Kp W / L, Vov = Vgs - Vto, for Vds >= 0:
 *     cutoff      Vov <= 0          Id = 0
 *     triode      0 <= Vds < Vov    Id = beta (Vov Vds - Vds^2/2) (1 + lambda Vds)
 *     saturation  Vds >= Vov > 0    Id = beta/2 Vov^2 (1 + lambda Vds)
 * Id flows into the drain and out of the source. For Vds < 0 the channel is
 * symmetric: drain and source swap roles and Id changes sign. Value, gm and
 * gds are continuous at both region boundaries.
 * PMOS: the same equations on negated voltages and Vto, with negated current.
 */
static void level1_forward(double beta, double vto, double lambda, double vgs,
                           double vds, double *id, double *gm, double *gds,
                           int *region) {
  double vov = vgs - vto;
  double clm = 1.0 + lambda * vds;

  if (vov <= 0.0) {
    *id = *gm = *gds = 0.0;
    *region = MNA_MOS_CUTOFF;
  } else if (vds < vov) {
    double core = vov * vds - 0.5 * vds * vds;
    *id = beta * core * clm;
    *gm = beta * vds * clm;
    *gds = beta * (vov - vds) * clm + beta * core * lambda;
    *region = MNA_MOS_TRIODE;
  } else {
    *id = 0.5 * beta * vov * vov * clm;
    *gm = beta * vov * clm;
    *gds = 0.5 * beta * vov * vov * lambda;
    *region = MNA_MOS_SATURATION;
  }
}

void mna_mosfet_evaluate(const MnaMosfet *mos, double vd, double vg, double vs,
                         MnaMosfetEval *out) {
  double p = mos->polarity < 0 ? -1.0 : 1.0;
  double beta = mos->kp * mos->w / mos->l;
  double nd = p * vd, ng = p * vg, ns = p * vs, vto = p * mos->vto;
  double id, gm, gds;

  if (nd - ns >= 0.0) {
    level1_forward(beta, vto, mos->lambda, ng - ns, nd - ns, &id, &gm, &gds,
                   &out->region);
    out->reversed = 0;
    out->id = p * id;
    out->d_vd = gds;
    out->d_vg = gm;
    out->d_vs = -(gm + gds);
  } else {
    level1_forward(beta, vto, mos->lambda, ng - nd, ns - nd, &id, &gm, &gds,
                   &out->region);
    out->reversed = 1;
    out->id = -p * id;
    out->d_vd = gm + gds;
    out->d_vg = -gm;
    out->d_vs = -gds;
  }
}

static int mosfet_load(void *data, const MnaDevice *device,
                       MnaAssembly *assembly) {
  const MnaMosfet *mos = (const MnaMosfet *)data;
  MnaMosfetEval e;
  size_t nodes[3];
  double current[3];
  double d[3][3];

  (void)device;
  if (mos == NULL || assembly == NULL) {
    return 0;
  }

  nodes[0] = mos->node_drain;
  nodes[1] = mos->node_gate;
  nodes[2] = mos->node_source;
  mna_mosfet_evaluate(mos, node_voltage_from_x(assembly, nodes[0]),
                      node_voltage_from_x(assembly, nodes[1]),
                      node_voltage_from_x(assembly, nodes[2]), &e);
  current[0] = e.id;
  current[1] = 0.0;
  current[2] = -e.id;
  for (int j = 0; j < 3; ++j) {
    double dj = j == 0 ? e.d_vd : j == 1 ? e.d_vg : e.d_vs;
    d[0][j] = dj;
    d[1][j] = 0.0;
    d[2][j] = -dj;
  }
  stamp_three_terminal(assembly, nodes, current, d);
  return 1;
}

static const MnaDeviceOps MOSFET_OPS = {.flags = MNA_DEVICE_NONLINEAR,
                                        .unknown_count = no_extra_unknowns,
                                        .load = mosfet_load,
                                        .destroy = destroy_payload,
                                        .branch_current = NULL};

int mna_add_mosfet(MnaCircuit *circuit, const MnaMosfet *params) {
  MnaMosfet *mos;

  if (circuit == NULL || params == NULL ||
      !valid_node(circuit, params->node_drain) ||
      !valid_node(circuit, params->node_gate) ||
      !valid_node(circuit, params->node_source) ||
      (params->polarity != 1 && params->polarity != -1) ||
      !isfinite(params->vto) || !positive_finite(params->kp) ||
      !positive_finite(params->w) || !positive_finite(params->l) ||
      !isfinite(params->lambda) || params->lambda < 0.0) {
    return 0;
  }

  mos = (MnaMosfet *)malloc(sizeof(*mos));
  if (mos == NULL) {
    return 0;
  }
  *mos = *params;
  if (!mna_add_device(circuit, &MOSFET_OPS, mos)) {
    free(mos);
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

  if (!isfinite(options->absolute_update_tolerance_v) ||
      options->absolute_update_tolerance_v < 0.0 ||
      !isfinite(options->relative_update_tolerance) ||
      options->relative_update_tolerance < 0.0) {
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
  options->maximum_voltage_step_v = 0.0;
  options->junction_limiting = 1;
  options->absolute_update_tolerance_v = 1.0e-9;
  options->relative_update_tolerance = 1.0e-9;
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
  int converged = 0;
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
  result->condition_estimate = NAN;
  result->singular_unknown = (size_t)-1;
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
    assembly.limit = options->junction_limiting ? (iteration == 0 ? 1 : 2) : 0;

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

    MatrixSolveInfo solve_info;
    if (!matrix_solve_info(&jacobian, rhs, raw_delta, &solve_info)) {
      result->singular_unknown = solve_info.singular_column;
      result->status = MNA_STATUS_SINGULAR_MATRIX;
      break;
    }
    result->condition_estimate = solve_info.condition_estimate;

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
    assembly.limit = 0;

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

    int residual_ok =
        max_residual <=
        options->absolute_residual_tolerance +
            options->relative_residual_tolerance * initial_residual_scale;
    int update_ok =
        !has_nonlinear_device ||
        max_applied_step <=
            options->absolute_update_tolerance_v +
                options->relative_update_tolerance *
                    vector_max_abs(result->x, circuit->node_count);

    if (residual_ok && update_ok) {
      mna_trace(options, MNA_TRACE_CONVERGED, iteration + 1, unknown_count,
                result->x, residual, NULL, NULL, raw_delta, applied_delta,
                max_residual, max_raw_step, max_applied_step, step_scale);
      result->status = MNA_STATUS_SUCCESS;
      converged = 1;
      break;
    }
  }

  /* Ran out of iterations without an error status and without converging. */
  if (!converged && result->status == MNA_STATUS_SUCCESS) {
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
 * Structural diagnosis of a singular system
 * --------------------------------------------------------------------------
 * The Jacobian's nonzero pattern (devices loaded exactly at x, or at zero) is a bipartite graph of equations and
 * unknowns. A maximum matching (augmenting paths) leaves an unknown unmatched when no assignment of equations can
 * determine it; every unknown reachable from such an unknown by alternating paths (unknown -> an equation using it
 * -> the unknown matched to that equation) belongs to the same underdetermined set (Dulmage-Mendelsohn; Duff,
 * Erisman & Reid, Direct Methods for Sparse Matrices). A floating node is a set of one with no equation; parallel
 * or looped voltage sources share one set of branch currents. Structural rank cannot see numerical cancellation:
 * then the set is empty and the relative pivot test's column is the only clue.
 */
static int augment(size_t col, const unsigned char *pattern, size_t n, size_t *row_of, size_t *col_of, unsigned char *seen) {
  for (size_t r = 0; r < n; ++r) {
    if (!pattern[r * n + col] || seen[r]) {
      continue;
    }
    seen[r] = 1;
    if (row_of[r] == (size_t)-1 || augment(row_of[r], pattern, n, row_of, col_of, seen)) {
      row_of[r] = col;
      col_of[col] = r;
      return 1;
    }
  }
  return 0;
}

size_t mna_structural_deficiency(const MnaCircuit *circuit, const double *x, size_t *unknowns, size_t cap) {
  size_t n = circuit ? circuit->unknown_count : 0, found = 0;
  if (n == 0) {
    return 0;
  }
  Matrix j = {0};
  double *residual = calloc(n, sizeof *residual), *zero = calloc(n, sizeof *zero);
  unsigned char *pattern = calloc(n * n, 1), *seen = calloc(n, 1), *member = calloc(n, 1);
  size_t *row_of = malloc(n * sizeof *row_of), *col_of = malloc(n * sizeof *col_of), *queue = malloc(n * sizeof *queue);
  if (!residual || !zero || !pattern || !seen || !member || !row_of || !col_of || !queue || !matrix_create(&j, n, n)) {
    goto done;
  }
  MnaAssembly a = {x ? x : zero, &j, residual, circuit->node_count, n, 0};
  for (size_t i = 0; i < circuit->device_count; ++i) {
    const MnaDevice *d = &circuit->devices[i];
    if (d->ops && d->ops->load) {
      (void)d->ops->load(d->data, d, &a);
    }
  }
  for (size_t i = 0; i < n * n; ++i) {
    pattern[i] = j.data[i] != 0.0;
  }
  for (size_t i = 0; i < n; ++i) {
    row_of[i] = col_of[i] = (size_t)-1;
  }
  for (size_t col = 0; col < n; ++col) {
    memset(seen, 0, n);
    (void)augment(col, pattern, n, row_of, col_of, seen);
  }
  size_t head = 0, tail = 0;
  for (size_t col = 0; col < n; ++col) {
    if (col_of[col] == (size_t)-1) {
      member[col] = 1;
      queue[tail++] = col;
    }
  }
  while (head < tail) {
    size_t col = queue[head++];
    for (size_t r = 0; r < n; ++r) {
      size_t next = row_of[r];
      if (pattern[r * n + col] && next != (size_t)-1 && !member[next]) {
        member[next] = 1;
        queue[tail++] = next;
      }
    }
  }
  for (size_t col = 0; col < n; ++col) {
    if (member[col]) {
      if (found < cap && unknowns) {
        unknowns[found] = col;
      }
      ++found;
    }
  }
done:
  matrix_free(&j);
  free(residual), free(zero), free(pattern), free(seen), free(member), free(row_of), free(col_of), free(queue);
  return found;
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

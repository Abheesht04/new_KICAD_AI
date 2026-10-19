
#ifndef MNA_H
#define MNA_H

#include "matrix.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * --------------------------------------------------------------------------
 * MNA model objects
 * --------------------------------------------------------------------------
 *
 * The circuit does not know concrete device types.  A device is represented
 * by behavior (ops) plus opaque model data (data).
 */

typedef struct MnaCircuit MnaCircuit;
typedef struct MnaDevice MnaDevice;
typedef struct MnaAssembly MnaAssembly;

/* Device properties.  These are properties, not device kinds. */
enum { MNA_DEVICE_LINEAR = 0u, MNA_DEVICE_NONLINEAR = 1u << 0 };

/*
 * Called by a device to report how many additional unknowns it introduces.
 * A resistor/diode normally returns 0.  A voltage source returns 1 for its
 * branch current, and more advanced devices may introduce multiple local
 * unknowns.
 */
typedef size_t (*MnaUnknownCountFn)(const void *data);

/*
 * Load both the exact residual F(x) and its Jacobian J(x).
 *
 * The nonlinear solver then solves:
 *
 *      J(x) * delta = -F(x)
 *      x_next = x + delta
 *
 * A linear device simply produces values independent of x.
 */
typedef int (*MnaDeviceLoadFn)(void *data, const MnaDevice *device,
                               MnaAssembly *assembly);

/* Device-owned payload destruction. */
typedef void (*MnaDeviceDestroyFn)(void *data);

/*
 * Optional capability used by the legacy voltage-source-current accessor.
 * The callback returns the local unknown slot representing a branch current.
 * It is a capability, not a device-type discriminator.
 */
typedef int (*MnaBranchCurrentFn)(const void *data,
                                  size_t *local_unknown_index);

typedef struct {
  unsigned flags;
  MnaUnknownCountFn unknown_count;
  MnaDeviceLoadFn load;
  MnaDeviceDestroyFn destroy;
  MnaBranchCurrentFn branch_current;
} MnaDeviceOps;

struct MnaDevice {
  const MnaDeviceOps *ops;
  void *data;

  /* Offset of this device's private unknowns after the node voltages. */
  size_t unknown_offset;
  size_t unknown_count;
};

/*
 * Generic assembly context exposed to device models.
 *
 * Equation rows 0 .. node_count-1 are the node equations.
 * Additional device equations/unknowns start at node_count.
 */
struct MnaAssembly {
  const double *x;
  Matrix *jacobian;
  double *residual;

  size_t node_count;
  size_t unknown_count;
};

/* --------------------------------------------------------------------------
 * Generic assembly helpers
 * --------------------------------------------------------------------------
 */

int mna_assembly_add_jacobian(MnaAssembly *assembly, size_t row, size_t column,
                              double value);

int mna_assembly_add_residual(MnaAssembly *assembly, size_t row, double value);

int mna_assembly_node_index(size_t node, size_t *index);

double mna_assembly_node_voltage(const MnaAssembly *assembly, size_t node);

int mna_device_unknown_index(const MnaDevice *device, size_t local_unknown,
                             size_t *global_index);

/* --------------------------------------------------------------------------
 * Built-in model payloads
 * --------------------------------------------------------------------------
 */

typedef struct {
  size_t node_a;
  size_t node_b;
  double resistance_ohm;
} MnaResistor;

typedef struct {
  size_t node_a;
  size_t node_b;
  double voltage_v;
} MnaVoltageSource;

typedef struct {
  size_t node_anode;
  size_t node_cathode;
  double saturation_current_a;
  double ideality_factor;
  double temperature_k;
} MnaDiode;

/* --------------------------------------------------------------------------
 * Circuit container
 * --------------------------------------------------------------------------
 */

struct MnaCircuit {
  size_t node_count;

  MnaDevice *devices;
  size_t device_count;
  size_t device_capacity;

  size_t unknown_count;
};

void mna_circuit_init(MnaCircuit *circuit, size_t node_count);
void mna_circuit_free(MnaCircuit *circuit);

/* The one generic insertion point for all devices. Returns 1 on success, 0 on failure. */
int mna_add_device(MnaCircuit *circuit, const MnaDeviceOps *ops, void *data);

const MnaDevice *mna_device_at(const MnaCircuit *circuit, size_t index);

/* Existing ergonomic built-in constructors. Each returns 1 on success, 0 on failure. */
int mna_add_resistor(MnaCircuit *circuit, size_t node_a, size_t node_b,
                     double resistance_ohm);

int mna_add_voltage_source(MnaCircuit *circuit, size_t node_a, size_t node_b,
                           double voltage_v);

int mna_add_diode(MnaCircuit *circuit, size_t node_anode, size_t node_cathode,
                  double saturation_current_a, double ideality_factor,
                  double temperature_k);

/* The diode's Shockley equation at anode-cathode voltage voltage_v: current and
 * dI/dV (the small-signal conductance used by AC). The exponent is clamped at
 * +/-40, exactly as in DC. */
void mna_diode_evaluate(const MnaDiode *diode, double voltage_v,
                        double *current_a, double *conductance_s);

/* --------------------------------------------------------------------------
 * Solver configuration
 * --------------------------------------------------------------------------
 */

typedef enum {
  MNA_TRACE_LINEAR_SYSTEM = 0,
  MNA_TRACE_STEP,
  MNA_TRACE_RESIDUAL,
  MNA_TRACE_CONVERGED
} MnaTraceEventKind;

typedef struct {
  MnaTraceEventKind kind;

  size_t iteration;
  size_t unknown_count;

  const double *x;
  const double *residual;
  const Matrix *jacobian;
  const double *rhs;
  const double *raw_delta;
  const double *applied_delta;
  double maximum_residual;
  double maximum_raw_step;
  double maximum_applied_step;
  double step_scale;
} MnaTraceEvent;

typedef void (*MnaTraceFn)(void *context, const MnaTraceEvent *event);

typedef struct {
  size_t max_iterations;

  double absolute_residual_tolerance;
  double relative_residual_tolerance;
  double pivot_tolerance;
  double maximum_voltage_step_v;

  MnaTraceFn trace;
  void *trace_context;

} MnaOptions;

void mna_options_default(MnaOptions *options);

/* --------------------------------------------------------------------------
 * Solver result
 * --------------------------------------------------------------------------
 */

typedef enum {
  MNA_STATUS_SUCCESS = 0,
  MNA_STATUS_INVALID_ARGUMENT,
  MNA_STATUS_ALLOCATION_FAILURE,
  MNA_STATUS_SINGULAR_MATRIX,
  MNA_STATUS_NOT_CONVERGED,
  MNA_STATUS_DEVICE_FAILURE
} MnaStatus;

typedef struct {
  MnaStatus status;
  size_t unknown_count;
  double *x;
  size_t node_count;
  size_t iterations;
  double maximum_voltage_step_v;
  double maximum_residual;
  double maximum_kcl_residual_a;
  double initial_residual_scale;
} MnaResult;

void mna_result_free(MnaResult *result);

void mna_solve(const MnaCircuit *circuit, const MnaOptions *options,
               MnaResult *result);

/* Convenience accessors. */
double mna_node_voltage(const MnaResult *result, size_t node);

/*
 * Returns the current through the requested voltage source.
 *
 * Current is defined positive from node_a to node_b, matching the
 * voltage-source convention:
 *
 *     V(node_a) - V(node_b) = voltage_v
 *
 * Therefore a negative returned current means current is flowing
 * from node_b toward node_a under this convention.
 */
double mna_voltage_source_current(const MnaCircuit *circuit,
                                  const MnaResult *result, size_t source_index);

/* --------------------------------------------------------------------------
 * Independent validation
 * --------------------------------------------------------------------------
 */

typedef struct {
  int valid;
  double maximum_residual;
  double maximum_kcl_residual_a;
} MnaValidation;

int mna_validate(const MnaCircuit *circuit, const MnaResult *result,
                 const MnaOptions *options, MnaValidation *validation);

#ifdef __cplusplus
}
#endif

#endif /* MNA_H */

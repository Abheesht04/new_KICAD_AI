#ifndef COPPER_SPICE_MNA_H
#define COPPER_SPICE_MNA_H

#include "arena.h"
#include "circuit.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MNA_OK = 0,
    MNA_ERR_ARGUMENT,
    MNA_ERR_CIRCUIT,
    MNA_ERR_MEMORY,
    MNA_ERR_SINGULAR,
    MNA_ERR_NOT_CONVERGED,
    MNA_ERR_RANGE,
    MNA_ERR_UNSUPPORTED,
    MNA_ERR_NUMERIC
} MnaStatus;

typedef struct {
    double voltage_absolute_tolerance_v;
    double current_absolute_tolerance_a;
    double relative_tolerance;
    uint32_t max_iterations;
    double max_junction_step_v; /* Newton voltage limiting; <= 0 disables */
    double device_exponent_limit; /* abs() cap for semiconductor exponent arguments */
    double residual_tolerance;     /* scaled KCL/branch residual target */
    double gmin_start_s;           /* continuation shunt conductance */
    double gmin_end_s;             /* final shunt conductance */
    uint32_t gmin_steps;           /* logarithmic continuation stages */
    uint32_t source_stepping_steps;/* ramp independent sources from 0 to 1 */
    uint32_t line_search_steps;    /* damped Newton backtracking limit */
    const double *initial_node_voltages_v; /* optional node_count+1 seed */
    const uint8_t *initial_node_mask;      /* optional; NULL means all supplied */
    size_t initial_node_voltage_count;
} MnaOptions;

typedef struct {
    size_t node_voltage_count; /* node 0 is included at index 0 */
    double *node_voltages_v;
    size_t element_current_count;
    double *element_currents_a; /* branch current in element terminal order */
    uint32_t iterations;
    double max_delta_v;
    bool converged;
    double scaled_residual;
    double gmin_used_s;
} MnaOperatingPoint;

typedef struct {
    size_t node_voltage_count;
    Cplx *node_voltages_v;
    size_t element_current_count;
    Cplx *element_currents_a;
    double frequency_hz;
    double relative_residual;
    double pivot_growth_estimate;
} MnaAcResult;

typedef enum {
    MNA_INTEGRATION_BACKWARD_EULER = 0,
    MNA_INTEGRATION_TRAPEZOIDAL,
    MNA_INTEGRATION_GEAR2
} MnaIntegrationMethod;

typedef struct {
    double time_step_s;
    double stop_time_s;
    uint32_t max_steps;
    MnaOptions nonlinear;
    MnaIntegrationMethod integration_method;
    bool use_initial_operating_point; /* SPICE default: initialize C/L from the DC OP */
    const double *initial_node_voltages_v; /* optional node_count+1 .IC overrides */
    const uint8_t *initial_node_mask;
    size_t initial_node_voltage_count;
} MnaTransientOptions;

typedef struct {
    size_t steps;
    size_t node_count;       /* excludes ground */
    size_t element_count;
    double *time_s;          /* steps entries */
    double *node_voltages_v; /* row-major: steps x (node_count + 1) */
    double *element_currents_a; /* row-major: steps x element_count */
    double maximum_scaled_residual;
    uint32_t maximum_newton_iterations;
} MnaTransientResult;

MnaOptions mna_default_options(void);
MnaTransientOptions mna_default_transient_options(void);

MnaStatus mna_solve_dc(Arena *result_arena, const Circuit *circuit,
                       const MnaOptions *options, MnaOperatingPoint *out);
MnaStatus mna_solve_ac(Arena *result_arena, const Circuit *circuit,
                       const MnaOperatingPoint *operating_point,
                       double frequency_hz, MnaAcResult *out);
MnaStatus mna_solve_transient(Arena *result_arena, const Circuit *circuit,
                              const MnaTransientOptions *options,
                              MnaTransientResult *out);

const char *mna_status_name(MnaStatus status);

#ifdef COPPER_TESTING
/* Test-only access to the analytic device current/Jacobian evaluation. */
size_t mna_test_device_eval(const CircuitElement *element,
                            const double *node_solution,
                            double terminal_currents[4],
                            double terminal_jacobian[4][4],
                            double exponent_limit);
double mna_test_pnjlim(double proposed, double previous, double thermal_voltage,
                       double saturation_current);
double mna_test_fetlim(double proposed, double previous, double threshold);
#endif

#ifdef __cplusplus
}
#endif
#endif

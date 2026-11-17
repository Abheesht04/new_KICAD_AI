#ifndef COPPER_SPICE_H
#define COPPER_SPICE_H

#include "mna.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CSP_MAX_MODEL_PARAMS 32u

typedef enum {
    CSP_OK = 0,
    CSP_ERR_ARGUMENT,
    CSP_ERR_SYNTAX,
    CSP_ERR_UNSUPPORTED,
    CSP_ERR_RANGE,
    CSP_ERR_MEMORY,
    CSP_ERR_DUPLICATE,
    CSP_ERR_UNRESOLVED,
    CSP_ERR_SOLVER,
    CSP_ERR_IO
} CspStatus;

typedef enum {
    CSP_R, CSP_C, CSP_L, CSP_V, CSP_I,
    CSP_D, CSP_Q, CSP_M, CSP_J,
    CSP_E, CSP_G, CSP_F, CSP_H
} CspElementKind;

typedef enum {
    CSP_MODEL_DIODE,
    CSP_MODEL_NPN,
    CSP_MODEL_PNP,
    CSP_MODEL_NMOS,
    CSP_MODEL_PMOS,
    CSP_MODEL_NJF,
    CSP_MODEL_PJF
} CspModelKind;

typedef enum {
    CSP_ANALYSIS_NONE = 0,
    CSP_ANALYSIS_OP,
    CSP_ANALYSIS_AC,
    CSP_ANALYSIS_TRAN,
    CSP_ANALYSIS_DC
} CspAnalysisKind;

typedef enum { CSP_SWEEP_DEC, CSP_SWEEP_OCT, CSP_SWEEP_LIN } CspSweepKind;

typedef struct {
    const char *name;
    double value;
} CspModelParam;

typedef struct {
    const char *name;
    CspModelKind kind;
    CspModelParam *params;
    size_t param_count;
} CspModel;

typedef struct {
    const char *name;
    CspElementKind kind;
    const char *nodes[4];
    const char *model_name;
    const char *control_name;
    double value_si;
    double instance_factor; /* MOSFET multiplier * W/L; 1 when omitted */
    double instance_width_m;
    double instance_length_m;
    double instance_multiplier;
    bool has_dc_value;
    Cplx ac_value;
    CircuitWaveform waveform;
} CspElement;

typedef struct {
    CspAnalysisKind kind;
    CspSweepKind sweep;
    uint32_t points;
    double start;
    double stop;
    double step;
    double start_time;
    double max_step;
    MnaIntegrationMethod integration_method;
    const char *source_name;
} CspAnalysis;

typedef struct {
    size_t points;
    double *sweep_values;
    size_t node_voltage_count;
    double *node_voltages_v;       /* row-major: points x node_voltage_count */
    size_t element_current_count;
    double *element_currents_a;    /* row-major: points x element_current_count */
    double maximum_scaled_residual;
    uint32_t maximum_newton_iterations;
} CspDcSweepResult;

typedef struct {
    size_t points;
    double *frequency_hz;
    size_t node_voltage_count;
    Cplx *node_voltages_v;         /* row-major: points x node_voltage_count */
    size_t element_current_count;
    Cplx *element_currents_a;      /* row-major: points x element_current_count */
    double maximum_relative_residual;
    double maximum_pivot_growth_estimate;
} CspAcSweepResult;

typedef struct {
    CspAnalysisKind kind;
    union {
        MnaOperatingPoint op;
        CspAcSweepResult ac;
        MnaTransientResult tran;
        CspDcSweepResult dc;
    } data;
} CspAnalysisResult;

typedef struct { const char *node_name; double voltage_v; } CspNodeValue;

typedef struct {
    const char *title;
    CspElement *elements;
    size_t element_count;
    CspModel *models;
    size_t model_count;
    CspNodeValue *initial_conditions;
    size_t initial_condition_count;
    CspNodeValue *nodesets;
    size_t nodeset_count;
    size_t source_line_capacity;
    double temperature_c;
    double nominal_temperature_c;
    const char **node_names; /* non-ground nodes; node ID is index + 1 */
    size_t node_count;
    CspAnalysis analysis;
    MnaOptions solver_options;
    size_t error_line;
} CspNetlist;

/* Supports SPICE3-style R/C/L/V/I, D/Q/M/J devices and E/G/F/H dependent
   sources; .MODEL for D/NPN/PNP/NMOS/PMOS/NJF/PJF; .OP/.AC/.TRAN/.DC and
   common .OPTIONS tolerances. Unsupported syntax fails with a source line. */
CspStatus csp_parse(Arena *arena, const char *text, CspNetlist *out,
                    size_t *error_line);
/* Preferred entry point for decks containing .INCLUDE and .SUBCKT. The
 * include callback returns data/path buffers in the expansion scratch Arena;
 * see CspIncludeReadFn in copper_spice_expand.h. */
struct CspExpandOptions;
CspStatus csp_parse_with_includes(Arena *arena, const char *source_name,
                                  const char *text, size_t text_size,
                                  const struct CspExpandOptions *options,
                                  CspNetlist *out, size_t *error_line);
CspStatus csp_build_circuit(Arena *arena, const CspNetlist *netlist,
                            Circuit *out);
/* Executes the selected analysis. With no directive, an operating point is run. */
CspStatus csp_run_analysis(Arena *result_arena, const CspNetlist *netlist,
                           CspAnalysisResult *out);
const char *csp_status_name(CspStatus status);

#ifdef __cplusplus
}
#endif
#endif

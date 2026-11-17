#ifndef COPPER_CIRCUIT_H
#define COPPER_CIRCUIT_H

#include "arena.h"
#include "cplx.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Optional structured tracing shared by the circuit, solver, and SPICE layers.
   Compile with -DCOPPER_TRACE=1 to enable dispatch. The default build compiles
   trace sites to no-ops and emits no output. Callback registration is intended
   to be configured before running simulations, not changed concurrently. */
#ifndef COPPER_TRACE
#define COPPER_TRACE 0
#endif

typedef enum {
    COPPER_TRACE_PARSE_LINE = 1,
    COPPER_TRACE_LINEAR_SOLVE,
    COPPER_TRACE_NEWTON_ITERATION,
    COPPER_TRACE_TRANSIENT_STEP
} CopperTraceKind;

typedef struct {
    CopperTraceKind kind;
    const char *module;
    /* detail and token strings are borrowed; copy them in the callback if
       they must outlive the callback invocation. */
    const char *detail;
    const char *const *tokens;
    size_t token_count;
    size_t line_number;
    uint32_t iteration;
    double residual;
    double alpha;
    double max_delta_v;
    double gmin_s;
    double source_scale;
    double pivot_growth;
    double relative_residual;
    uint32_t refinement_steps;
    double time_s;
    double dt_s;
    int method;
    int status;
} CopperTraceEvent;

typedef void (*CopperTraceCallback)(void *context,
                                    const CopperTraceEvent *event);

void copper_trace_set_callback(CopperTraceCallback callback, void *context);
void copper_trace_emit(const CopperTraceEvent *event);

#if COPPER_TRACE
#define COPPER_TRACE_EVENT(event_value) copper_trace_emit(&(event_value))
#else
#define COPPER_TRACE_EVENT(event_value) ((void)0)
#endif

typedef enum {
    CIRCUIT_OK = 0,
    CIRCUIT_ERR_ARGUMENT,
    CIRCUIT_ERR_MEMORY,
    CIRCUIT_ERR_RANGE,
    CIRCUIT_ERR_UNSUPPORTED
} CircuitStatus;

typedef enum {
    CIRCUIT_ELEMENT_INVALID = 0,
    CIRCUIT_RESISTOR,
    CIRCUIT_CAPACITOR,
    CIRCUIT_INDUCTOR,
    CIRCUIT_VOLTAGE_SOURCE,
    CIRCUIT_CURRENT_SOURCE,
    CIRCUIT_DIODE,
    CIRCUIT_BJT,
    CIRCUIT_MOSFET,
    CIRCUIT_JFET,
    CIRCUIT_VCVS, /* E: voltage-controlled voltage source */
    CIRCUIT_VCCS, /* G: voltage-controlled current source */
    CIRCUIT_CCCS, /* F: current-controlled current source */
    CIRCUIT_CCVS  /* H: current-controlled voltage source */
} CircuitElementKind;

typedef enum { CIRCUIT_BJT_NPN = 0, CIRCUIT_BJT_PNP } CircuitBjtPolarity;
typedef enum { CIRCUIT_MOS_N = 0, CIRCUIT_MOS_P } CircuitMosPolarity;
typedef enum { CIRCUIT_JFET_N = 0, CIRCUIT_JFET_P } CircuitJfetPolarity;

typedef enum {
    CIRCUIT_WAVEFORM_NONE = 0,
    CIRCUIT_WAVEFORM_PULSE,
    CIRCUIT_WAVEFORM_SIN,
    CIRCUIT_WAVEFORM_EXP
} CircuitWaveformKind;

typedef struct {
    CircuitWaveformKind kind;
    /* PULSE: v1,v2,td,tr,tf,pw,per
       SIN: offset,amplitude,frequency,delay,damping,phase_rad
       EXP: v1,v2,td1,tau1,td2,tau2 */
    double p[7];
} CircuitWaveform;

typedef struct {
    double saturation_current_a;
    double emission_coefficient;
    double thermal_voltage_v;
    double series_resistance_ohm;
    double breakdown_voltage_v;
    double breakdown_current_a;
    double junction_capacitance_f;
    double junction_potential_v;
    double grading_coefficient;
    double transit_time_s;
    double temperature_exponent;
    double bandgap_ev;
    double nominal_temperature_k;
    double operating_temperature_k;
} CircuitDiodeModel;

typedef struct {
    double saturation_current_a;
    double beta_forward;
    double early_voltage_v;
    double thermal_voltage_v;
    CircuitBjtPolarity polarity;
    double beta_reverse; /* <= 0 selects the forward beta as a fallback */
    double cje_f;
    double vje_v;
    double mje;
    double cjc_f;
    double vjc_v;
    double mjc;
    double tf_s;
    double temperature_exponent;
    double bandgap_ev;
    double nominal_temperature_k;
    double operating_temperature_k;
} CircuitBjtModel;

typedef struct {
    double kp_a_per_v2;
    double threshold_voltage_v;
    double channel_modulation_per_v;
    CircuitMosPolarity polarity;
    double body_effect_gamma;       /* Level-1 body-effect coefficient */
    double surface_potential_v;     /* Level-1 PHI, normally around 0.6 V */
    double gate_oxide_capacitance_f; /* total Cox*W*L, per instance */
    double gate_overlap_gs_f;
    double gate_overlap_gd_f;
    double gate_overlap_gb_f;
} CircuitMosModel;

typedef struct {
    double idss_a;
    double pinch_off_voltage_v; /* normally negative for N-channel JFET */
    double channel_modulation_per_v;
    CircuitJfetPolarity polarity;
} CircuitJfetModel;

typedef struct {
    CircuitElementKind kind;
    const char *reference;
    uint32_t n1; /* output +; diode anode; BJT collector; FET drain */
    uint32_t n2; /* output -; diode cathode; BJT base; FET gate */
    uint32_t n3; /* BJT/FET source or dependent-source control + */
    uint32_t n4; /* dependent-source control -; MOS body is reserved */
    double value_si; /* SI passive value, independent DC value, or gain */
    bool has_dc_value; /* distinguishes explicit source DC value from waveform initial value */
    Cplx ac_value;   /* independent source small-signal phasor */
    uint32_t control_element_index; /* F/H control branch index in Circuit.elements */
    CircuitWaveform waveform;
    union {
        CircuitDiodeModel diode;
        CircuitBjtModel bjt;
        CircuitMosModel mos;
        CircuitJfetModel jfet;
    } model;
} CircuitElement;

typedef struct Circuit {
    uint32_t node_count; /* node 0 is ground; valid non-ground nodes are 1..N */
    CircuitElement *elements;
    size_t element_count;
    size_t element_capacity;
} Circuit;

CircuitStatus circuit_init(Arena *arena, Circuit *out,
                           uint32_t node_count, size_t element_capacity);
CircuitStatus circuit_add(Circuit *circuit, const CircuitElement *element);
CircuitStatus circuit_validate(const Circuit *circuit);
const char *circuit_status_name(CircuitStatus status);
const char *circuit_element_kind_name(CircuitElementKind kind);

double circuit_source_value(const CircuitElement *element, double time_s);
double circuit_source_dc_value(const CircuitElement *element);

#ifdef __cplusplus
}
#endif
#endif

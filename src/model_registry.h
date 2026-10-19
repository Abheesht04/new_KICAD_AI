#ifndef MODEL_REGISTRY_H
#define MODEL_REGISTRY_H

#include <stddef.h>

#include "component_model.h"
#include "constraint.h"
#include "diagnostic.h"
#include "mna.h"
#include "mna_ac.h"

struct Component;

/* --------------------------------------------------------------------------
 * Legacy Design validation hooks (keyed by coarse ComponentKind).
 * Used by the V1 Design path only; unchanged.
 * --------------------------------------------------------------------------
 */

typedef int (*ModelValidateFn)(const struct Component *component,
                               DiagnosticList *out);

typedef int (*ModelDeriveConstraintsFn)(const struct Component *component,
                                        ConstraintList *out);

typedef struct {
  const char *name;
  ComponentKind kind;
  ModelValidateFn validate;
  ModelDeriveConstraintsFn derive_constraints;
} ModelOps;

/* --------------------------------------------------------------------------
 * Device model contract, version 2 (docs/engineering/DEVICE_MODEL_CONTRACT.md)
 * v2 adds the AC small-signal hook (stamp_ac) and its declarations.
 *
 * A DeviceModelSpec says what equations a model implements, on which ordered
 * terminals, with which validated parameters, and for which analyses. It is
 * separate from catalogue parts (manufacturer/MPN/ratings), KiCad symbols and
 * footprints; a .copper part binds to a model by id and maps each terminal to
 * a symbol pin explicitly. Equations are stamped through the existing
 * MnaDeviceOps engine; there is no second solver.
 *
 * Specs are static, caller-owned and must outlive the registry. The registry
 * stores pointers only.
 * --------------------------------------------------------------------------
 */

#define DEVICE_MODEL_CONTRACT_VERSION 2u
#define DEVICE_MODEL_MAX_TERMINALS 8
#define DEVICE_MODEL_MAX_PARAMS 8
#define DEVICE_MODEL_MAX_STATE 2

typedef enum {
  DEVICE_UNIT_INVALID = 0,
  DEVICE_UNIT_DIMENSIONLESS,
  DEVICE_UNIT_OHM,
  DEVICE_UNIT_FARAD,
  DEVICE_UNIT_HENRY,
  DEVICE_UNIT_VOLT,
  DEVICE_UNIT_AMPERE,
  DEVICE_UNIT_KELVIN,
  DEVICE_UNIT_METRE,
  DEVICE_UNIT_AMPERE_PER_VOLT2, /* MOSFET transconductance parameter */
  DEVICE_UNIT_PER_VOLT          /* channel-length modulation */
} DeviceUnit;

typedef enum {
  DEVICE_ANALYSIS_DC = 0,
  DEVICE_ANALYSIS_TRANSIENT,
  DEVICE_ANALYSIS_AC
} DeviceAnalysis;

/* Capability bits. TEMPERATURE is reserved: the contract defines no hook for
 * it, so registration rejects a spec that claims it. */
enum {
  DEVICE_CAP_DC = 1u << 0,
  DEVICE_CAP_TRANSIENT = 1u << 1,
  DEVICE_CAP_AC = 1u << 2,
  DEVICE_CAP_TEMPERATURE = 1u << 3
};

typedef struct {
  const char *name;        /* "anode"; unique within the model */
  const char *role;        /* free-text electrical role */
  const char *pin_names;   /* '|'-separated KiCad pin names that mean this
                              terminal ("K|C|CATHODE"), or NULL: any name */
} DeviceTerminal;

typedef struct {
  const char *name;        /* "resistance" */
  DeviceUnit unit;
  double minimum, maximum; /* inclusive, except minimum when min_exclusive */
  int min_exclusive;
  int has_default;         /* a parameter without a default is required */
  double default_value;
  const char *meaning;
} DeviceParam;

typedef struct {
  DeviceAnalysis analysis;
  double dt_s;          /* transient time step; 0 for DC */
  const double *state;  /* previous accepted state (state_count values) */
} DeviceStampContext;

/* nodes[] holds one MNA node per terminal, in spec terminal order (0 = ground).
 * params[] holds validated values in spec parameter order. Return 1 on success. */
typedef int (*DeviceStampFn)(MnaCircuit *circuit, const size_t *nodes,
                             const double *params,
                             const DeviceStampContext *context);

/* After an accepted transient step, compute the next state from the solved
 * result. Must not modify anything but next_state. Return 1 on success. */
typedef int (*DeviceCommitFn)(const MnaResult *result, const size_t *nodes,
                              const double *params,
                              const DeviceStampContext *context,
                              double *next_state);

/* Small-signal AC stamping at angular frequency omega. Linearise around the DC
 * operating point (always provided when the spec sets needs_operating_point),
 * stamp the complex admittances/branch equations, and for an independent
 * source add its AC excitation phasor (zero unless the component declares one).
 * Instance branch unknowns are branch .. branch + ac_branch_count - 1. */
typedef struct {
  double omega;                     /* rad/s, >= 0 */
  const MnaResult *operating_point; /* DC solution, or NULL when not needed */
  Cplx excitation;                  /* AC phasor of an independent source */
  size_t branch;                    /* first branch unknown of this instance */
} DeviceAcContext;

typedef int (*DeviceAcStampFn)(MnaAcSystem *system, const size_t *nodes,
                               const double *params,
                               const DeviceAcContext *context);

typedef struct {
  const char *id;          /* stable, e.g. "ideal.resistor" */
  unsigned version;        /* model version, >= 1 */
  const char *family;      /* "resistor"; must match the part kind */
  const char *summary;
  const char *assumptions; /* what the equations leave out */
  const DeviceTerminal *terminals;
  size_t terminal_count;
  const DeviceParam *params;
  size_t param_count;
  int primary_param;       /* index bound to a component's (value ...), or -1 */
  unsigned capabilities;
  size_t state_count;      /* transient state values per instance */
  DeviceStampFn stamp_dc;
  DeviceStampFn stamp_transient;
  DeviceCommitFn commit_transient;
  /* AC small-signal (capability DEVICE_CAP_AC). */
  DeviceAcStampFn stamp_ac;
  size_t ac_branch_count;    /* branch unknowns needed in the AC system */
  int ac_excitation;         /* 1: an independent source that accepts (ac MAG PHASE) */
  int needs_operating_point; /* 1: nonlinear; AC linearises around the DC solution */
} DeviceModelSpec;

typedef struct {
  const ModelOps **items;               /* legacy Design validation hooks */
  const DeviceModelSpec **device_models;
} ModelRegistry;

void model_registry_init(ModelRegistry *registry);
void model_registry_free(ModelRegistry *registry);

int model_registry_register(ModelRegistry *registry, const ModelOps *ops);

const ModelOps *model_registry_find(const ModelRegistry *registry,
                                    ComponentKind kind);

int model_registry_register_builtins(ModelRegistry *registry);

/* ---- Device model API ---- */

/* 1 if the spec satisfies contract v1, else 0 with a reason in err. */
int device_model_validate_spec(const DeviceModelSpec *spec, char *err,
                               size_t cap);

/* Validates and registers. Rejects invalid specs and duplicate ids.
 * Returns 1 on success, 0 on failure (reason in err). */
int model_registry_register_device(ModelRegistry *registry,
                                   const DeviceModelSpec *spec, char *err,
                                   size_t cap);

const DeviceModelSpec *model_registry_find_device(const ModelRegistry *registry,
                                                  const char *id);
size_t model_registry_device_count(const ModelRegistry *registry);
const DeviceModelSpec *model_registry_device_at(const ModelRegistry *registry,
                                                size_t index);

/* Registers the built-in V2 device models (src/device_models.c). */
int model_registry_register_builtin_devices(ModelRegistry *registry,
                                            char *err, size_t cap);

/* Resolves parameters: present[i] selects values[i]; absent values take the
 * declared default or fail when required. Rejects non-finite and out-of-range
 * values. Writes param_count values to resolved. 1 on success. */
int device_model_resolve_params(const DeviceModelSpec *spec,
                                const double *values, const int *present,
                                double *resolved, char *err, size_t cap);

int device_model_supports(const DeviceModelSpec *spec, DeviceAnalysis analysis);
int device_model_terminal_index(const DeviceModelSpec *spec, const char *name);
/* 1 when name is one of the terminal's KiCad pin names, or when the terminal
 * accepts any name; comparison is ASCII case-insensitive. */
int device_terminal_accepts_pin_name(const DeviceTerminal *terminal,
                                     const char *name);

const char *device_unit_symbol(DeviceUnit unit);
DeviceUnit device_unit_parse(const char *text);
const char *device_analysis_name(DeviceAnalysis analysis);

#endif

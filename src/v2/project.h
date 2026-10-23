#ifndef COPPER_PROJECT_H
#define COPPER_PROJECT_H
#include "IR.h"
#include "mna.h"
#include "model_registry.h"
#include "pcb.h"
#include "dfm_pcb.h"
#define CP_MAX_COMPONENTS 256
#define CP_MAX_NETS 256
#define CP_MAX_PINS 64

/* A rating or dimension. known == 0 means UNKNOWN: value is meaningless and
 * every check that needs it reports rating_unknown/height_unknown. Zero is
 * never used to mean unknown. source names where a known value came from:
 * "part:<part id>" or "override:<component ref>". */
typedef struct {
    double value;
    int known;
    const char *source;
} CpQuantity;

/* MCU behavioral boundary (docs/engineering/PART_REGISTRY.md): typed electrical constraints of a
 * microcontroller part. It is not a silicon or firmware model. In DC every pin is high impedance
 * except GPIOs the project drives; supply current is never modelled and is always reported. */
#define CP_MCU_MAX_PINS 64
#define CP_MCU_MAX_RAILS 8
enum { CP_GPIO_IN = 1, CP_GPIO_OUT = 2, CP_GPIO_STRAP = 4 };
typedef struct {
    const char *pin, *name, *strap_note;
    unsigned flags;
} CpMcuPin;
typedef struct {
    const char *behavior, *provenance;
    const char *supply[CP_MCU_MAX_RAILS], *ground[CP_MCU_MAX_RAILS];
    size_t supply_count, ground_count;
    double vmin, vmax;                       /* operating supply range, required */
    CpQuantity vih_ratio, vil_ratio;         /* input thresholds as a fraction of the supply */
    CpQuantity max_source_a, max_sink_a;     /* per-pin output current limits */
    CpQuantity output_ohm, pull_ohm;         /* only when sourced; otherwise outputs are ideal */
    CpMcuPin gpio[CP_MCU_MAX_PINS];
    size_t gpio_count;
    const char *reset;                       /* "" when the part has no reset pin */
    int reset_active_low;
    const char *program[CP_MCU_MAX_RAILS];
    size_t program_count;
    /* Reference circuit from the module's source documentation: what a design must add around it. */
    struct { const char *kind, *pin; double value; } reference[CP_MCU_MAX_PINS];
    size_t reference_count;
    const char *reference_provenance;
} CpMcu;
/* Project-side state of an MCU GPIO pin, from the component's (drive ...). */
enum { CP_DRIVE_NONE, CP_DRIVE_HIGH, CP_DRIVE_LOW, CP_DRIVE_INPUT, CP_DRIVE_PULL_UP, CP_DRIVE_PULL_DOWN };
/* An axis-aligned keepout taken from a footprint's own rule area. */
#define CP_MAX_KEEPOUTS 64
enum { CP_KEEPOUT_TRACKS = 1, CP_KEEPOUT_VIAS = 2, CP_KEEPOUT_PADS = 4, CP_KEEPOUT_FOOTPRINTS = 8 };
typedef struct {
    size_t component;
    double x1, y1, x2, y2;
    unsigned layers;   /* bit 0: F.Cu, bit 1: B.Cu */
    unsigned flags;
} CpKeepout;

/* Immutable parsed IR owns all strings. No model/LLM ever owns geometry. */
typedef struct {
    const char *ref, *part, *symbol, *footprint;
    const char *family, *role;       /* logical family and role from the part, or "" */
    const CpMcu *mcu;                /* MCU behavioral boundary, or NULL */
    unsigned char drive[CP_MAX_PINS];/* CP_DRIVE_* per pin, MCU only */
    const char *nc[CP_MAX_PINS];     /* pins deliberately left unconnected */
    size_t nc_count;
    const char *manufacturer, *mpn, *datasheet;
    const char *provenance;          /* part-level data provenance, or "" */
    const char *catalogue;           /* catalogue the part definition came from, or "" */
    const char *override_provenance; /* required when the component overrides data, else "" */
    char kind;
    /* Bound device model; NULL for J (ideal boundary) and U (unmodelled, blocks analysis). */
    const DeviceModelSpec *model;
    const char *model_source;        /* "explicit", "kind_default" or "none" */
    size_t terminal_pin[DEVICE_MODEL_MAX_TERMINALS]; /* model terminal -> index into pins[] */
    double params[DEVICE_MODEL_MAX_PARAMS];          /* validated, in spec order */
    double value;                    /* the (value ...) atom; equals params[primary] when bound */
    double tolerance;
    CpQuantity power_w, voltage_v, height_mm;
    Cplx ac;    /* AC excitation phasor of an independent source; 0 unless (ac MAG PHASE) */
    int has_ac;
    double x, y, rotation;
    size_t nodes[CP_MAX_PINS];
    const char *pins[CP_MAX_PINS];
    size_t pin_count;
} CpComponent;
typedef struct {
    Arena arena;
    const IRNode *root;
    const char *name;
    const char *nets[CP_MAX_NETS];
    size_t net_count, component_count;
    CpComponent components[CP_MAX_COMPONENTS];
    PcbBoard board;
    FabProfile fab;
    ModelRegistry models;
    double max_aspect, mask_expansion, min_mask_web, courtyard_gap;
    double max_drop, max_current_density, power_derating, voltage_derating;
    int has_geometry;
    CpKeepout keepouts[CP_MAX_KEEPOUTS];
    size_t keepout_count;
} CpProject;
void cp_init(CpProject *p);
void cp_free(CpProject *p);
int cp_load(CpProject *p, const char *path, char *error, size_t cap);
int cp_analyze(const CpProject *p, FILE *json);
/* method: DEVICE_INTEGRATION_BACKWARD_EULER or DEVICE_INTEGRATION_TRAPEZOIDAL (src/transient.h). */
int cp_transient(const CpProject *p, double dt, size_t steps, int method, FILE *csv);
/* Small-signal AC sweep over the project's circuit (docs/engineering/AC_ANALYSIS.md).
 * Writes CSV (frequency, per-frequency status, node voltage and branch current
 * phasors with magnitude and phase) and returns 0; returns 2 with
 * "AC <rule>: <reason>" on stderr when the sweep is invalid, a device lacks an
 * AC model, no source is excited, the operating point fails, or any frequency
 * is singular (rows for singular frequencies carry the status and no values). */
int cp_ac(const CpProject *p, double start_hz, double stop_hz, size_t points, int logarithmic, FILE *csv);
/* Writes the registered device model specs as JSON (contract version, terminals,
 * parameters, capabilities). Returns 0 on success. */
int cp_models_json(FILE *json);
/* Validates a (catalogue (part ...) ...) file with the same rules as a project's (parts ...):
 * kinds, model binding, pin maps, ratings, parameters, unique ids. On success writes a JSON
 * summary and returns 1; otherwise returns 0 with the reason in error. */
int cp_check_catalogue(const char *path, FILE *json, char *error, size_t cap);
#endif

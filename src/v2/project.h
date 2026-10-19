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

/* Immutable parsed IR owns all strings. No model/LLM ever owns geometry. */
typedef struct {
    const char *ref, *part, *symbol, *footprint;
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
} CpProject;
void cp_init(CpProject *p);
void cp_free(CpProject *p);
int cp_load(CpProject *p, const char *path, char *error, size_t cap);
int cp_analyze(const CpProject *p, FILE *json);
int cp_transient(const CpProject *p, double dt, size_t steps, FILE *csv);
/* Writes the registered device model specs as JSON (contract version, terminals,
 * parameters, capabilities). Returns 0 on success. */
int cp_models_json(FILE *json);
/* Validates a (catalogue (part ...) ...) file with the same rules as a project's (parts ...):
 * kinds, model binding, pin maps, ratings, parameters, unique ids. On success writes a JSON
 * summary and returns 1; otherwise returns 0 with the reason in error. */
int cp_check_catalogue(const char *path, FILE *json, char *error, size_t cap);
#endif

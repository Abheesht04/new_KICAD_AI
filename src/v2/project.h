#ifndef COPPER_PROJECT_H
#define COPPER_PROJECT_H
#include "IR.h"
#include "mna.h"
#include "pcb.h"
#include "dfm_pcb.h"
#define CP_MAX_COMPONENTS 256
#define CP_MAX_NETS 256
#define CP_MAX_PINS 64
/* Immutable parsed IR owns all strings. No model/LLM ever owns geometry. */
typedef struct {
    const char *ref, *part, *symbol, *footprint;
    const char *manufacturer, *mpn, *datasheet;
    char kind;
    double value, power_w, voltage_v, tolerance, is_a, ideality, height_mm;
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
    double max_aspect, mask_expansion, min_mask_web, courtyard_gap;
    double max_drop, max_current_density, power_derating, voltage_derating;
    int has_geometry;
} CpProject;
void cp_init(CpProject *p);
void cp_free(CpProject *p);
int cp_load(CpProject *p, const char *path, char *error, size_t cap);
int cp_analyze(const CpProject *p, FILE *json);
int cp_transient(const CpProject *p, double dt, size_t steps, FILE *csv);
#endif

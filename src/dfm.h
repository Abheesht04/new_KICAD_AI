#ifndef DFM_H
#define DFM_H

#include <stddef.h>
#include <stdint.h>

#include "diagnostic.h"

struct Design;

/*
 * Manufacturing capabilities / assumptions supplied by the user or vendor.
 * This is deliberately data-driven rather than hard-coded into a rule.
 */
typedef struct {
    uint8_t layer_count;
    double copper_weight_oz;
    double min_trace_width_mm;
    double min_clearance_mm;
    double min_via_diameter_mm;
    double min_drill_mm;
    double min_annular_ring_mm;
    double board_edge_clearance_mm;
    double max_component_height_mm;
} DfmProfile;

typedef int (*DfmCheckFn)(
    const struct Design *design,
    const DfmProfile *profile,
    DiagnosticList *out
);

typedef struct {
    const char *name;
    DfmCheckFn check;
} DfmRule;

typedef struct {
    DfmRule *rules;
} DfmRegistry;

int dfm_registry_init(DfmRegistry *registry);
void dfm_registry_free(DfmRegistry *registry);

int dfm_register_rule(
    DfmRegistry *registry,
    const DfmRule *rule
);

int dfm_run_all(
    const DfmRegistry *registry,
    const struct Design *design,
    const DfmProfile *profile,
    DiagnosticList *out
);

int dfm_register_builtin_rules(DfmRegistry *registry);

#endif

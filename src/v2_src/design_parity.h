#ifndef COPPER_DESIGN_PARITY_H
#define COPPER_DESIGN_PARITY_H

#include "design_board.h"
#include "netlist_kicad.h"
#include "report.h"
#include "schematic.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DesignParitySummary {
    size_t matched_components;
    size_t missing_footprints;
    size_t symbols_without_footprint_assignment;
    size_t extra_footprints;
    size_t reference_mismatches;
    size_t footprint_mismatches;
    size_t checked_pad_nets;
    size_t pad_net_mismatches;
    bool netlist_available;
} DesignParitySummary;

bool design_parity_check(const SchematicIR *schematic, const Board *board,
                         const KicadNetlist *netlist,
                         CopperDiagnosticReport *report,
                         DesignParitySummary *summary);

#ifdef __cplusplus
}
#endif
#endif

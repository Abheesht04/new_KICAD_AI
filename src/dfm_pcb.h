#ifndef DFM_PCB_H
#define DFM_PCB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pcb.h"

#define PCB_DFM_MESSAGE_MAX 256
#define FAB_PROFILE_NAME_MAX 64

typedef enum {
    PCB_DFM_INFO,
    PCB_DFM_WARNING,
    PCB_DFM_ERROR
} PcbDfmSeverity;

typedef enum {
    PCB_DFM_BOARD_SIZE,
    PCB_DFM_LAYER_COUNT,
    PCB_DFM_TRACE_WIDTH,
    PCB_DFM_TRACE_CLEARANCE,
    PCB_DFM_VIA_DIAMETER,
    PCB_DFM_VIA_DRILL,
    PCB_DFM_VIA_ANNULAR_RING,
    PCB_DFM_PAD_ANNULAR_RING,
    PCB_DFM_HOLE_CLEARANCE,
    PCB_DFM_EDGE_CLEARANCE,
    PCB_DFM_COMPONENT_HEIGHT,
    PCB_DFM_COPPER_CLEARANCE,
    PCB_DFM_OPEN_CONNECTION,
    PCB_DFM_SHORT_CIRCUIT,
    PCB_DFM_INVALID_GEOMETRY,
    PCB_DFM_ENGINE_INCONSISTENCY
} PcbDfmRule;

typedef enum {
    PCB_DFM_OBJECT_UNKNOWN = 0,
    PCB_DFM_OBJECT_TRACE,
    PCB_DFM_OBJECT_PAD,
    PCB_DFM_OBJECT_VIA,
    PCB_DFM_OBJECT_COMPONENT,
    PCB_DFM_OBJECT_BOARD
} PcbDfmObjectKind;

typedef struct {
    PcbDfmSeverity severity;
    PcbDfmRule rule;
    uint32_t object_id;
    double measured;
    double limit;
    char message[PCB_DFM_MESSAGE_MAX];

    /* Optional structured second-object reference for pairwise DRC/DFM rules. */
    PcbDfmObjectKind object_kind;
    bool has_related_object;
    PcbDfmObjectKind related_object_kind;
    uint32_t related_object_id;
} PcbDfmIssue;

typedef struct {
    PcbDfmIssue *items;
} PcbDfmReport;

typedef struct {
    char name[FAB_PROFILE_NAME_MAX];

    uint8_t max_layer_count;

    double min_board_width_mm;
    double max_board_width_mm;
    double min_board_height_mm;
    double max_board_height_mm;

    double min_trace_width_mm;
    double min_trace_clearance_mm;

    double min_via_diameter_mm;
    double min_via_drill_mm;
    double min_via_annular_ring_mm;

    double min_pad_annular_ring_mm;
    double min_hole_clearance_mm;
    double min_copper_to_edge_mm;

    double max_component_height_mm;
} FabProfile;

void pcb_dfm_report_init(PcbDfmReport *report);
void pcb_dfm_report_free(PcbDfmReport *report);

size_t pcb_dfm_issue_count(const PcbDfmReport *report);
const PcbDfmIssue *pcb_dfm_issue_get(
    const PcbDfmReport *report,
    size_t index
);

FabProfile fab_profile_conservative_2layer(void);

int pcb_dfm_validate(
    const PcbBoard *board,
    const FabProfile *profile,
    PcbDfmReport *report
);

const char *pcb_dfm_severity_name(PcbDfmSeverity severity);
const char *pcb_dfm_rule_name(PcbDfmRule rule);
const char *pcb_dfm_object_kind_name(PcbDfmObjectKind kind);

#endif

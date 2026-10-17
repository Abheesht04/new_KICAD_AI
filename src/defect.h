#ifndef DEFECT_H
#define DEFECT_H

/*
 * Defect analysis for the deterministic PCB kernel.
 *
 * This module is deliberately not a second DFM report implementation:
 * findings are emitted through a visitor and adapted into PcbDfmReport by
 * dfm_pcb.c. It has two responsibilities:
 *   1. detect opens/shorts visible in the current physical PCB IR;
 *   2. audit existing DFM findings for self-inconsistency and expose coverage
 *      gaps so unsupported checks cannot be mistaken for PASS.
 *
 * Current geometric limitations: straight tracks and circular pads/vias only.
 * Connectivity checks are conservative and require non-zero, assigned net IDs
 * for open-net analysis. The PCB IR still needs an explicit no-net sentinel.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "copper.h"
#include "dfm_pcb.h"

#define PCB_DEFECT_MESSAGE_MAX PCB_DFM_MESSAGE_MAX

typedef enum {
    PCB_DEFECT_OPEN_CONNECTION = 0,
    PCB_DEFECT_SHORT_CIRCUIT,
    PCB_DEFECT_INVALID_COPPER_GEOMETRY,
    PCB_DEFECT_DFM_REPORT_INCONSISTENCY
} PcbDefectCode;

typedef enum {
    DEFECT_OBJECT_UNKNOWN = 0,
    DEFECT_OBJECT_TRACE,
    DEFECT_OBJECT_PAD,
    DEFECT_OBJECT_VIA,
    DEFECT_OBJECT_COMPONENT,
    DEFECT_OBJECT_BOARD,
    DEFECT_OBJECT_DFM_ISSUE
} DefectObjectKind;

typedef struct {
    DefectObjectKind kind;
    size_t index;
} DefectObjectRef;

typedef struct {
    PcbDefectCode code;
    PcbDfmSeverity severity;
    DefectObjectRef object;
    bool has_related_object;
    DefectObjectRef related_object;
    bool has_measurement;
    double measured;
    double limit;
    char message[PCB_DEFECT_MESSAGE_MAX];
} PcbDefectFinding;

typedef struct {
    size_t objects_seen;
    size_t active_copper_objects;
    size_t candidate_pairs;
    size_t connected_pairs;
    size_t opens;
    size_t shorts;
    size_t invalid_geometry;
} PcbDefectStats;

typedef int (*PcbDefectVisitor)(
    const PcbDefectFinding *finding,
    void *user_data
);

/*
 * Inspect the physical copper represented by PcbBoard.
 * - Same-net shapes physically touching on a common layer are connected.
 * - Different-net shapes touching/overlapping on a common layer are shorts.
 * - Pads assigned to the same non-zero net but in distinct copper components
 *   are reported as opens.
 *
 * Returns 0 when analysis completes (findings may have been emitted), or a
 * negative value for invalid top-level input, allocation failure, or visitor
 * failure. Invalid individual copper geometry is emitted as a finding and
 * excluded from connectivity conclusions.
 */
int pcb_defect_check_board(
    const PcbBoard *board,
    PcbDefectVisitor visitor,
    void *user_data,
    PcbDefectStats *out_stats
);

/* Coverage-status registry: callers/CLI can show where this engine has gaps. */
typedef enum {
    PCB_DEFECT_COVERAGE_SUPPORTED,
    PCB_DEFECT_COVERAGE_PARTIAL,
    PCB_DEFECT_COVERAGE_UNSUPPORTED
} PcbDefectCoverageStatus;

typedef enum {
    PCB_DEFECT_FEATURE_COPPER_CLEARANCE = 0,
    PCB_DEFECT_FEATURE_OPEN_CONNECTIVITY,
    PCB_DEFECT_FEATURE_INTER_NET_SHORTS,
    PCB_DEFECT_FEATURE_DRILL_RULES,
    PCB_DEFECT_FEATURE_BOARD_OUTLINE,
    PCB_DEFECT_FEATURE_SILK_OVER_COPPER,
    PCB_DEFECT_FEATURE_SILK_OVERLAP,
    PCB_DEFECT_FEATURE_ARCS_AND_ZONES,
    PCB_DEFECT_FEATURE_CAM_PACKAGE,
    PCB_DEFECT_FEATURE_VLSI_RULES
} PcbDefectFeature;

typedef struct {
    PcbDefectFeature feature;
    PcbDefectCoverageStatus status;
    const char *name;
    const char *detail;
} PcbDefectCoverage;

const PcbDefectCoverage *pcb_defect_coverage(size_t *out_count);
const char *pcb_defect_code_name(PcbDefectCode code);
const char *pcb_defect_coverage_status_name(PcbDefectCoverageStatus status);

/*
 * Audit a range of an existing DFM report. `first_issue` lets the caller audit
 * only findings emitted by the current validation pass. Findings whose
 * measurements contradict their declared rule, or whose structured geometry
 * references are invalid, are reported as PCB_DEFECT_DFM_REPORT_INCONSISTENCY.
 *
 * This is a consistency audit, not an independent proof that all checks ran;
 * use pcb_defect_coverage() to expose unimplemented/partial rule families.
 */
int pcb_defect_audit_dfm_report(
    const PcbBoard *board,
    const FabProfile *profile,
    const PcbDfmReport *report,
    size_t first_issue,
    PcbDefectVisitor visitor,
    void *user_data,
    size_t *out_inconsistencies
);

#endif

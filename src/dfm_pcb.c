#include "dfm_pcb.h"
#include "copper.h"
#include "nmath.h"
#include "pcb_copper.h"
#include "defect.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int report_add(
    PcbDfmReport *report,
    PcbDfmSeverity severity,
    PcbDfmRule rule,
    uint32_t object_id,
    double measured,
    double limit,
    const char *format,
    ...
)
{
    if (!report || !format) {
        return -1;
    }

    PcbDfmIssue issue;
    memset(&issue, 0, sizeof(issue));

    issue.severity = severity;
    issue.rule = rule;
    issue.object_id = object_id;
    issue.measured = measured;
    issue.limit = limit;

    va_list args;
    va_start(args, format);
    int written = vsnprintf(
        issue.message,
        sizeof(issue.message),
        format,
        args
    );
    va_end(args);

    if (written < 0) {
        return -1;
    }

    return vec_push(report->items, issue);
}

static double sq(double value)
{
    return value * value;
}

static double point_distance(
    double ax,
    double ay,
    double bx,
    double by
)
{
    return nm_sqrt(sq(ax - bx) + sq(ay - by));
}

static void report_set_object(PcbDfmReport *report, PcbDfmObjectKind kind, uint32_t object_id)
{
    if (!report || vec_len(report->items) == 0) return;
    PcbDfmIssue *issue = &report->items[vec_len(report->items) - 1u];
    issue->object_kind = kind;
    issue->object_id = object_id;
}

static void report_set_related_object(PcbDfmReport *report, PcbDfmObjectKind kind, uint32_t object_id)
{
    if (!report || vec_len(report->items) == 0) return;
    PcbDfmIssue *issue = &report->items[vec_len(report->items) - 1u];
    issue->has_related_object = true;
    issue->related_object_kind = kind;
    issue->related_object_id = object_id;
}


void pcb_dfm_report_init(PcbDfmReport *report)
{
    if (report) {
        memset(report, 0, sizeof(*report));
    }
}

void pcb_dfm_report_free(PcbDfmReport *report)
{
    if (!report) {
        return;
    }

    vec_free(report->items);
}

size_t pcb_dfm_issue_count(const PcbDfmReport *report)
{
    return report ? vec_len(report->items) : 0;
}

const PcbDfmIssue *pcb_dfm_issue_get(
    const PcbDfmReport *report,
    size_t index
)
{
    if (!report || index >= vec_len(report->items)) {
        return NULL;
    }

    return &report->items[index];
}

FabProfile fab_profile_conservative_2layer(void)
{
    FabProfile profile = {
        .name = "conservative_2layer",
        .max_layer_count = 2,

        .min_board_width_mm = 10.0,
        .max_board_width_mm = 100.0,
        .min_board_height_mm = 10.0,
        .max_board_height_mm = 100.0,

        .min_trace_width_mm = 0.15,
        .min_trace_clearance_mm = 0.15,

        .min_via_diameter_mm = 0.60,
        .min_via_drill_mm = 0.30,
        .min_via_annular_ring_mm = 0.15,

        .min_pad_annular_ring_mm = 0.15,
        .min_hole_clearance_mm = 0.20,
        .min_copper_to_edge_mm = 0.25,

        .max_component_height_mm = 5.0
    };

    return profile;
}

static int check_board_size(
    const PcbBoard *board,
    const FabProfile *profile,
    PcbDfmReport *report
)
{
    int errors = 0;

    if (board->spec.layer_count > profile->max_layer_count) {
        if (report_add(
                report, PCB_DFM_ERROR, PCB_DFM_LAYER_COUNT, 0,
                board->spec.layer_count, profile->max_layer_count,
                "board uses %u copper layers; fab profile permits at most %u",
                (unsigned)board->spec.layer_count,
                (unsigned)profile->max_layer_count) != 0) {
            return -1;
        }
        errors++;
    }

    if (board->spec.width_mm < profile->min_board_width_mm ||
        board->spec.width_mm > profile->max_board_width_mm) {
        if (report_add(
                report, PCB_DFM_ERROR, PCB_DFM_BOARD_SIZE, 0,
                board->spec.width_mm, profile->max_board_width_mm,
                "board width %.3f mm is outside fab range [%.3f, %.3f] mm",
                board->spec.width_mm,
                profile->min_board_width_mm,
                profile->max_board_width_mm) != 0) {
            return -1;
        }
        errors++;
    }

    if (board->spec.height_mm < profile->min_board_height_mm ||
        board->spec.height_mm > profile->max_board_height_mm) {
        if (report_add(
                report, PCB_DFM_ERROR, PCB_DFM_BOARD_SIZE, 1,
                board->spec.height_mm, profile->max_board_height_mm,
                "board height %.3f mm is outside fab range [%.3f, %.3f] mm",
                board->spec.height_mm,
                profile->min_board_height_mm,
                profile->max_board_height_mm) != 0) {
            return -1;
        }
        errors++;
    }

    return errors;
}

static int check_traces(
    const PcbBoard *board,
    const FabProfile *profile,
    PcbDfmReport *report
)
{
    int errors = 0;

    for (size_t i = 0; i < vec_len(board->traces); ++i) {
        const PcbTrace *trace = &board->traces[i];

        if (trace->width_mm < profile->min_trace_width_mm) {
            if (report_add(
                    report, PCB_DFM_ERROR, PCB_DFM_TRACE_WIDTH,
                    (uint32_t)i,
                    trace->width_mm, profile->min_trace_width_mm,
                    "trace %zu width %.3f mm is below minimum %.3f mm",
                    i, trace->width_mm, profile->min_trace_width_mm) != 0) {
                return -1;
            }
            report_set_object(report, PCB_DFM_OBJECT_TRACE, (uint32_t)i);
            errors++;
        }

        /* This legacy check assumes a rectangular board with origin (0, 0).
         * Arbitrary Edge.Cuts geometry will be handled by the outline model. */
        double half_width = trace->width_mm * 0.5;
        double edge_clearance = nm_min_f64(
            nm_min_f64(nm_min_f64(trace->x1_mm, trace->x2_mm),
                 nm_min_f64(board->spec.width_mm - trace->x1_mm,
                      board->spec.width_mm - trace->x2_mm)),
            nm_min_f64(nm_min_f64(trace->y1_mm, trace->y2_mm),
                 nm_min_f64(board->spec.height_mm - trace->y1_mm,
                      board->spec.height_mm - trace->y2_mm))) - half_width;

        if (edge_clearance < profile->min_copper_to_edge_mm) {
            if (report_add(
                    report, PCB_DFM_ERROR, PCB_DFM_EDGE_CLEARANCE,
                    (uint32_t)i,
                    edge_clearance, profile->min_copper_to_edge_mm,
                    "trace %zu has %.3f mm copper-to-edge clearance; minimum is %.3f mm",
                    i, edge_clearance, profile->min_copper_to_edge_mm) != 0) {
                return -1;
            }
            report_set_object(report, PCB_DFM_OBJECT_TRACE, (uint32_t)i);
            errors++;
        }
    }

    return errors;
}

static int check_vias(
    const PcbBoard *board,
    const FabProfile *profile,
    PcbDfmReport *report
)
{
    int errors = 0;

    for (size_t i = 0; i < vec_len(board->vias); ++i) {
        const PcbVia *via = &board->vias[i];

        double annular = (via->diameter_mm - via->drill_mm) * 0.5;

        if (via->diameter_mm < profile->min_via_diameter_mm) {
            if (report_add(
                    report, PCB_DFM_ERROR, PCB_DFM_VIA_DIAMETER,
                    (uint32_t)i,
                    via->diameter_mm, profile->min_via_diameter_mm,
                    "via %zu diameter %.3f mm is below minimum %.3f mm",
                    i, via->diameter_mm, profile->min_via_diameter_mm) != 0) {
                return -1;
            }
            report_set_object(report, PCB_DFM_OBJECT_VIA, (uint32_t)i);
            errors++;
        }

        if (via->drill_mm < profile->min_via_drill_mm) {
            if (report_add(
                    report, PCB_DFM_ERROR, PCB_DFM_VIA_DRILL,
                    (uint32_t)i,
                    via->drill_mm, profile->min_via_drill_mm,
                    "via %zu drill %.3f mm is below minimum %.3f mm",
                    i, via->drill_mm, profile->min_via_drill_mm) != 0) {
                return -1;
            }
            report_set_object(report, PCB_DFM_OBJECT_VIA, (uint32_t)i);
            errors++;
        }

        if (annular < profile->min_via_annular_ring_mm) {
            if (report_add(
                    report, PCB_DFM_ERROR, PCB_DFM_VIA_ANNULAR_RING,
                    (uint32_t)i,
                    annular, profile->min_via_annular_ring_mm,
                    "via %zu annular ring %.3f mm is below minimum %.3f mm",
                    i, annular, profile->min_via_annular_ring_mm) != 0) {
                return -1;
            }
            report_set_object(report, PCB_DFM_OBJECT_VIA, (uint32_t)i);
            errors++;
        }

        double edge_x = nm_min_f64(
            via->x_mm,
            board->spec.width_mm - via->x_mm
        );
        double edge_y = nm_min_f64(
            via->y_mm,
            board->spec.height_mm - via->y_mm
        );
        double edge_clearance = nm_min_f64(edge_x, edge_y) - via->diameter_mm * 0.5;

        if (edge_clearance < profile->min_copper_to_edge_mm) {
            if (report_add(
                    report, PCB_DFM_ERROR, PCB_DFM_EDGE_CLEARANCE,
                    (uint32_t)i,
                    edge_clearance, profile->min_copper_to_edge_mm,
                    "via %zu has %.3f mm copper-to-edge clearance; minimum is %.3f mm",
                    i, edge_clearance, profile->min_copper_to_edge_mm) != 0) {
                return -1;
            }
            report_set_object(report, PCB_DFM_OBJECT_VIA, (uint32_t)i);
            errors++;
        }
    }

    return errors;
}

static int check_pads(
    const PcbBoard *board,
    const FabProfile *profile,
    PcbDfmReport *report
)
{
    int errors = 0;

    for (size_t i = 0; i < vec_len(board->pads); ++i) {
        const PcbPad *pad = &board->pads[i];

        if (pad->plated && pad->drill_mm > 0.0) {
            double annular = (pad->pad_diameter_mm - pad->drill_mm) * 0.5;
            if (annular < profile->min_pad_annular_ring_mm) {
                if (report_add(
                        report, PCB_DFM_ERROR, PCB_DFM_PAD_ANNULAR_RING,
                        (uint32_t)i,
                        annular, profile->min_pad_annular_ring_mm,
                        "pad %zu annular ring %.3f mm is below minimum %.3f mm",
                        i, annular, profile->min_pad_annular_ring_mm) != 0) {
                    return -1;
                }
                report_set_object(report, PCB_DFM_OBJECT_PAD, (uint32_t)i);
                errors++;
            }
        }

        double edge_x = nm_min_f64(
            pad->x_mm,
            board->spec.width_mm - pad->x_mm
        );
        double edge_y = nm_min_f64(
            pad->y_mm,
            board->spec.height_mm - pad->y_mm
        );
        double edge_clearance = pad->rectangular
            ? nm_min_f64(edge_x-pad->width_mm*.5,edge_y-pad->height_mm*.5)
            : nm_min_f64(edge_x, edge_y) - pad->pad_diameter_mm * 0.5;

        if (edge_clearance < profile->min_copper_to_edge_mm) {
            if (report_add(
                    report, PCB_DFM_ERROR, PCB_DFM_EDGE_CLEARANCE,
                    (uint32_t)i,
                    edge_clearance, profile->min_copper_to_edge_mm,
                    "pad %zu has %.3f mm copper-to-edge clearance; minimum is %.3f mm",
                    i, edge_clearance, profile->min_copper_to_edge_mm) != 0) {
                return -1;
            }
            report_set_object(report, PCB_DFM_OBJECT_PAD, (uint32_t)i);
            errors++;
        }
    }

    /* Hole-to-hole clearance is mechanical: it does not depend on net ID or
     * copper-layer membership. Only features with actual drill diameters take
     * part in this check. */
    for (size_t i = 0; i < vec_len(board->pads); ++i) {
        const PcbPad *a = &board->pads[i];
        if (a->drill_mm <= 0.0) continue;

        for (size_t j = i + 1; j < vec_len(board->pads); ++j) {
            const PcbPad *b = &board->pads[j];
            if (b->drill_mm <= 0.0) continue;

            double center_distance = point_distance(
                a->x_mm, a->y_mm, b->x_mm, b->y_mm);
            double hole_clearance = center_distance -
                a->drill_mm * 0.5 - b->drill_mm * 0.5;

            if (hole_clearance < profile->min_hole_clearance_mm) {
                if (report_add(
                        report, PCB_DFM_ERROR, PCB_DFM_HOLE_CLEARANCE,
                        (uint32_t)i,
                        hole_clearance, profile->min_hole_clearance_mm,
                        "holes %zu and %zu have %.3f mm clearance; minimum is %.3f mm",
                        i, j, hole_clearance, profile->min_hole_clearance_mm) != 0) {
                    return -1;
                }
                report_set_object(report, PCB_DFM_OBJECT_PAD, (uint32_t)i);
                report_set_related_object(report, PCB_DFM_OBJECT_PAD, (uint32_t)j);
                errors++;
            }
        }
    }

    return errors;
}

static int check_components(
    const PcbBoard *board,
    const FabProfile *profile,
    PcbDfmReport *report
)
{
    int errors = 0;

    for (size_t i = 0; i < vec_len(board->placements); ++i) {
        const PcbPlacement *placement = &board->placements[i];

        if (placement->height_z_mm > profile->max_component_height_mm) {
            if (report_add(
                    report, PCB_DFM_ERROR, PCB_DFM_COMPONENT_HEIGHT,
                    placement->component_id,
                    placement->height_z_mm,
                    profile->max_component_height_mm,
                    "component %u height %.3f mm exceeds %.3f mm",
                    placement->component_id,
                    placement->height_z_mm,
                    profile->max_component_height_mm) != 0) {
                return -1;
            }
            errors++;
        }

        double half_w = placement->width_mm * 0.5;
        double half_h = placement->height_mm * 0.5;

        double component_edge_clearance = nm_min_f64(
            nm_min_f64(placement->x_mm - half_w,
                 board->spec.width_mm - (placement->x_mm + half_w)),
            nm_min_f64(placement->y_mm - half_h,
                 board->spec.height_mm - (placement->y_mm + half_h)));

        if (component_edge_clearance < profile->min_copper_to_edge_mm) {
            if (report_add(
                    report, PCB_DFM_ERROR, PCB_DFM_EDGE_CLEARANCE,
                    placement->component_id,
                    component_edge_clearance, profile->min_copper_to_edge_mm,
                    "component %u courtyard has %.3f mm board-edge clearance; minimum is %.3f mm",
                    placement->component_id, component_edge_clearance,
                    profile->min_copper_to_edge_mm) != 0) {
                return -1;
            }
            report_set_object(report, PCB_DFM_OBJECT_COMPONENT, placement->component_id);
            errors++;
        }
    }

    return errors;
}

typedef struct {
    PcbDfmReport *report;
} CopperDfmContext;

static PcbDfmObjectKind dfm_object_kind_from_copper(CopperItemKind kind)
{
    switch (kind) {
        case COPPER_ITEM_TRACE: return PCB_DFM_OBJECT_TRACE;
        case COPPER_ITEM_PAD: return PCB_DFM_OBJECT_PAD;
        case COPPER_ITEM_VIA: return PCB_DFM_OBJECT_VIA;
        default: return PCB_DFM_OBJECT_UNKNOWN;
    }
}

static int copper_clearance_to_issue(
    const PcbCopperFinding *finding,
    void *user_data
)
{
    if (!finding || !user_data) return -1;
    CopperDfmContext *context = (CopperDfmContext *)user_data;

    const PcbDfmRule rule =
        finding->a.kind == COPPER_ITEM_TRACE &&
        finding->b.kind == COPPER_ITEM_TRACE
            ? PCB_DFM_TRACE_CLEARANCE
            : PCB_DFM_COPPER_CLEARANCE;

    if (report_add(
            context->report,
            PCB_DFM_ERROR,
            rule,
            (uint32_t)finding->a.source_index,
            finding->gap_mm,
            nm_to_mm(finding->required_nm),
            "%s %u and %s %u have %.6f mm copper clearance; minimum is %.6f mm",
            copper_item_kind_name(finding->a.kind), (unsigned)finding->a.source_index,
            copper_item_kind_name(finding->b.kind), (unsigned)finding->b.source_index,
            finding->gap_mm,
            nm_to_mm(finding->required_nm)) != 0) {
        return -1;
    }

    /* report_add appends one issue; preserve both participants as structured
     * data so CLI/JSON clients need not parse English text. */
    PcbDfmIssue *issue = &context->report->items[
        vec_len(context->report->items) - 1u];
    issue->object_kind = dfm_object_kind_from_copper(finding->a.kind);
    issue->has_related_object = true;
    issue->related_object_kind = dfm_object_kind_from_copper(finding->b.kind);
    issue->related_object_id = (uint32_t)finding->b.source_index;
    return 0;
}

static PcbDfmObjectKind dfm_object_kind_from_defect(DefectObjectKind kind)
{
    switch (kind) {
        case DEFECT_OBJECT_TRACE: return PCB_DFM_OBJECT_TRACE;
        case DEFECT_OBJECT_PAD: return PCB_DFM_OBJECT_PAD;
        case DEFECT_OBJECT_VIA: return PCB_DFM_OBJECT_VIA;
        case DEFECT_OBJECT_COMPONENT: return PCB_DFM_OBJECT_COMPONENT;
        case DEFECT_OBJECT_BOARD: return PCB_DFM_OBJECT_BOARD;
        default: return PCB_DFM_OBJECT_UNKNOWN;
    }
}

static PcbDfmRule dfm_rule_from_defect(PcbDefectCode code)
{
    switch (code) {
        case PCB_DEFECT_OPEN_CONNECTION: return PCB_DFM_OPEN_CONNECTION;
        case PCB_DEFECT_SHORT_CIRCUIT: return PCB_DFM_SHORT_CIRCUIT;
        case PCB_DEFECT_INVALID_COPPER_GEOMETRY: return PCB_DFM_INVALID_GEOMETRY;
        case PCB_DEFECT_DFM_REPORT_INCONSISTENCY: return PCB_DFM_ENGINE_INCONSISTENCY;
        default: return PCB_DFM_ENGINE_INCONSISTENCY;
    }
}

static int defect_to_dfm_issue(const PcbDefectFinding *finding, void *user_data)
{
    if (!finding || !user_data) return -1;
    PcbDfmReport *report = (PcbDfmReport *)user_data;
    uint32_t id = finding->object.index <= UINT32_MAX
        ? (uint32_t)finding->object.index : UINT32_MAX;
    double measured = finding->has_measurement ? finding->measured : 0.0;
    double limit = finding->has_measurement ? finding->limit : 0.0;

    if (report_add(report, finding->severity, dfm_rule_from_defect(finding->code),
            id, measured, limit, "%s", finding->message) != 0) {
        return -1;
    }

    PcbDfmIssue *issue = &report->items[vec_len(report->items) - 1u];
    issue->object_kind = dfm_object_kind_from_defect(finding->object.kind);
    issue->has_related_object = finding->has_related_object;
    issue->related_object_kind = dfm_object_kind_from_defect(finding->related_object.kind);
    issue->related_object_id = finding->related_object.index <= UINT32_MAX
        ? (uint32_t)finding->related_object.index : UINT32_MAX;
    return 0;
}

int pcb_dfm_validate(
    const PcbBoard *board,
    const FabProfile *profile,
    PcbDfmReport *report
)
{
    if (!board || !profile || !report) {
        return -1;
    }

    const size_t report_start = pcb_dfm_issue_count(report);
    int errors = 0;
    int result;

    result = check_board_size(board, profile, report);
    if (result < 0) {
        return -1;
    }
    errors += result;

    result = check_traces(board, profile, report);
    if (result < 0) {
        return -1;
    }
    errors += result;

    result = check_vias(board, profile, report);
    if (result < 0) {
        return -1;
    }
    errors += result;

    result = check_pads(board, profile, report);
    if (result < 0) {
        return -1;
    }
    errors += result;

    Nm required_clearance_nm;
    if (pcb_copper_mm_to_nm_ceil(
            profile->min_trace_clearance_mm,
            &required_clearance_nm) != PCB_COPPER_OK) {
        return -1;
    }
    CopperDfmContext copper_context = { .report = report };
    PcbCopperStats copper_stats;
    result = pcb_copper_check_clearance(
        board,
        required_clearance_nm,
        copper_clearance_to_issue,
        &copper_context,
        &copper_stats
    );
    if (result < 0) {
        return -1;
    }
    errors += (int)copper_stats.violations;

    result = check_components(board, profile, report);
    if (result < 0) {
        return -1;
    }
    errors += result;

    /* Detect physical opens/shorts represented by the current PCB IR. */
    PcbDefectStats defect_stats;
    result = pcb_defect_check_board(
        board, defect_to_dfm_issue, report, &defect_stats);
    if (result < 0) {
        return -1;
    }
    errors += (int)(defect_stats.opens + defect_stats.shorts +
                    defect_stats.invalid_geometry);

    /* Audit the diagnostics emitted in this pass for internal contradictions. */
    size_t inconsistencies = 0;
    result = pcb_defect_audit_dfm_report(
        board, profile, report, report_start, defect_to_dfm_issue, report,
        &inconsistencies);
    if (result < 0) {
        return -1;
    }
    errors += (int)inconsistencies;

    return errors;
}

const char *pcb_dfm_severity_name(PcbDfmSeverity severity)
{
    switch (severity) {
        case PCB_DFM_INFO: return "INFO";
        case PCB_DFM_WARNING: return "WARNING";
        case PCB_DFM_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

const char *pcb_dfm_rule_name(PcbDfmRule rule)
{
    switch (rule) {
        case PCB_DFM_BOARD_SIZE: return "board_size";
        case PCB_DFM_LAYER_COUNT: return "layer_count";
        case PCB_DFM_TRACE_WIDTH: return "trace_width";
        case PCB_DFM_TRACE_CLEARANCE: return "trace_clearance";
        case PCB_DFM_VIA_DIAMETER: return "via_diameter";
        case PCB_DFM_VIA_DRILL: return "via_drill";
        case PCB_DFM_VIA_ANNULAR_RING: return "via_annular_ring";
        case PCB_DFM_PAD_ANNULAR_RING: return "pad_annular_ring";
        case PCB_DFM_HOLE_CLEARANCE: return "hole_clearance";
        case PCB_DFM_EDGE_CLEARANCE: return "edge_clearance";
        case PCB_DFM_COMPONENT_HEIGHT: return "component_height";
        case PCB_DFM_COPPER_CLEARANCE: return "copper_clearance";
        case PCB_DFM_OPEN_CONNECTION: return "open_connection";
        case PCB_DFM_SHORT_CIRCUIT: return "short_circuit";
        case PCB_DFM_INVALID_GEOMETRY: return "invalid_geometry";
        case PCB_DFM_ENGINE_INCONSISTENCY: return "engine_inconsistency";
        default: return "unknown";
    }
}

const char *pcb_dfm_object_kind_name(PcbDfmObjectKind kind)
{
    switch (kind) {
        case PCB_DFM_OBJECT_TRACE: return "trace";
        case PCB_DFM_OBJECT_PAD: return "pad";
        case PCB_DFM_OBJECT_VIA: return "via";
        case PCB_DFM_OBJECT_COMPONENT: return "component";
        case PCB_DFM_OBJECT_BOARD: return "board";
        default: return "unknown";
    }
}

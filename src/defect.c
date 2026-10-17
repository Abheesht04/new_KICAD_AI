#include "defect.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nmath.h"
#include "pcb_copper.h"
#include "vec.h"

static size_t board_copper_count(const PcbBoard *board)
{
    return vec_len(board->traces) + vec_len(board->pads) + vec_len(board->vias);
}

static DefectObjectRef object_ref(CopperItemKind kind, size_t index)
{
    DefectObjectRef ref = { .kind = DEFECT_OBJECT_UNKNOWN, .index = index };
    switch (kind) {
        case COPPER_ITEM_TRACE: ref.kind = DEFECT_OBJECT_TRACE; break;
        case COPPER_ITEM_PAD: ref.kind = DEFECT_OBJECT_PAD; break;
        case COPPER_ITEM_VIA: ref.kind = DEFECT_OBJECT_VIA; break;
        default: break;
    }
    return ref;
}

static PcbCopperStatus item_at(
    const PcbBoard *board,
    size_t flat_index,
    CopperItem *out_item
)
{
    const size_t trace_count = vec_len(board->traces);
    const size_t pad_count = vec_len(board->pads);

    if (flat_index < trace_count) {
        return pcb_copper_item(board, COPPER_ITEM_TRACE, flat_index, out_item);
    }
    flat_index -= trace_count;
    if (flat_index < pad_count) {
        return pcb_copper_item(board, COPPER_ITEM_PAD, flat_index, out_item);
    }
    flat_index -= pad_count;
    return pcb_copper_item(board, COPPER_ITEM_VIA, flat_index, out_item);
}

static size_t dsu_find(size_t *parent, size_t x)
{
    size_t root = x;
    while (parent[root] != root) root = parent[root];
    while (parent[x] != x) {
        size_t next = parent[x];
        parent[x] = root;
        x = next;
    }
    return root;
}

static void dsu_union(size_t *parent, uint8_t *rank, size_t a, size_t b)
{
    a = dsu_find(parent, a);
    b = dsu_find(parent, b);
    if (a == b) return;

    if (rank[a] < rank[b]) {
        size_t tmp = a; a = b; b = tmp;
    }
    parent[b] = a;
    if (rank[a] == rank[b]) rank[a]++;
}

static int emit_finding(
    PcbDefectVisitor visitor,
    void *user_data,
    PcbDefectCode code,
    PcbDfmSeverity severity,
    DefectObjectRef object,
    bool has_related,
    DefectObjectRef related,
    bool has_measurement,
    double measured,
    double limit,
    const char *message
)
{
    if (!visitor) return 0;

    PcbDefectFinding finding;
    memset(&finding, 0, sizeof(finding));
    finding.code = code;
    finding.severity = severity;
    finding.object = object;
    finding.has_related_object = has_related;
    finding.related_object = related;
    finding.has_measurement = has_measurement;
    finding.measured = measured;
    finding.limit = limit;
    if (message) {
        (void)snprintf(finding.message, sizeof(finding.message), "%s", message);
    }
    return visitor(&finding, user_data) == 0 ? 0 : -1;
}

int pcb_defect_check_board(
    const PcbBoard *board,
    PcbDefectVisitor visitor,
    void *user_data,
    PcbDefectStats *out_stats
)
{
    if (out_stats) memset(out_stats, 0, sizeof(*out_stats));
    if (!board || board->spec.layer_count == 0 || board->spec.layer_count > 32) {
        return -1;
    }

    const size_t count = board_copper_count(board);
    if (out_stats) out_stats->objects_seen = count;
    if (count == 0) return 0;
    if (count > SIZE_MAX / sizeof(CopperItem) ||
        count > SIZE_MAX / sizeof(size_t)) {
        return -1;
    }

    CopperItem *items = (CopperItem *)calloc(count, sizeof(*items));
    bool *active = (bool *)calloc(count, sizeof(*active));
    size_t *parent = (size_t *)malloc(count * sizeof(*parent));
    uint8_t *rank = (uint8_t *)calloc(count, sizeof(*rank));
    if (!items || !active || !parent || !rank) {
        free(items); free(active); free(parent); free(rank);
        return -1;
    }

    int result = 0;
    for (size_t i = 0; i < count; ++i) {
        parent[i] = i;
        PcbCopperStatus status = item_at(board, i, &items[i]);
        if (status == PCB_COPPER_SKIP) continue;
        if (status == PCB_COPPER_OK) {
            active[i] = true;
            if (out_stats) out_stats->active_copper_objects++;
            continue;
        }

        /* An invalid object is a finding, not a silent exclusion. */
        CopperItemKind kind;
        size_t index;
        if (i < vec_len(board->traces)) {
            kind = COPPER_ITEM_TRACE; index = i;
        } else if (i < vec_len(board->traces) + vec_len(board->pads)) {
            kind = COPPER_ITEM_PAD; index = i - vec_len(board->traces);
        } else {
            kind = COPPER_ITEM_VIA;
            index = i - vec_len(board->traces) - vec_len(board->pads);
        }
        char message[PCB_DEFECT_MESSAGE_MAX];
        (void)snprintf(message, sizeof(message),
            "%s %zu has invalid or unsupported copper geometry (status %d)",
            copper_item_kind_name(kind), index, (int)status);
        if (out_stats) out_stats->invalid_geometry++;
        if (emit_finding(visitor, user_data,
                PCB_DEFECT_INVALID_COPPER_GEOMETRY, PCB_DFM_ERROR,
                object_ref(kind, index), false,
                (DefectObjectRef){0}, false, 0.0, 0.0, message) != 0) {
            result = -1; goto cleanup;
        }
    }

    /* Construct connected copper components and detect direct inter-net shorts.
     * Touching is decided exactly (copper_pair_touch); the net rule is applied
     * here: only a real, shared, non-zero net may connect copper. */
    for (size_t i = 0; i < count; ++i) {
        if (!active[i]) continue;
        for (size_t j = i + 1; j < count; ++j) {
            if (!active[j] ||
                !copper_layer_masks_overlap(items[i].layers, items[j].layers)) {
                continue;
            }
            if (out_stats) out_stats->candidate_pairs++;

            bool touching = false;
            CopperResult measured = copper_pair_touch(&items[i], &items[j], &touching);
            if (measured == COPPER_RESULT_NOT_APPLICABLE) continue;
            if (measured != COPPER_RESULT_PASS) {
                result = -1; goto cleanup;
            }

            const bool same_net = items[i].net == items[j].net &&
                                  items[i].net != COPPER_NET_NONE;
            if (same_net) {
                if (touching) {
                    dsu_union(parent, rank, i, j);
                    if (out_stats) out_stats->connected_pairs++;
                }
                continue;
            }

            if (touching) {
                double gap_nm = 0.0;
                double gap_mm = 0.0;
                if (copper_pair_gap_nm(&items[i], &items[j], &gap_nm)) {
                    gap_mm = gap_nm / (double)NM_PER_MM;
                }
                char message[PCB_DEFECT_MESSAGE_MAX];
                (void)snprintf(message, sizeof(message),
                    "%s %zu (net %u) touches/overlaps %s %zu (net %u); copper short",
                    copper_item_kind_name(items[i].kind),
                    (size_t)items[i].source_index, (unsigned)items[i].net,
                    copper_item_kind_name(items[j].kind),
                    (size_t)items[j].source_index, (unsigned)items[j].net);
                if (out_stats) out_stats->shorts++;
                if (emit_finding(visitor, user_data,
                        PCB_DEFECT_SHORT_CIRCUIT, PCB_DFM_ERROR,
                        object_ref(items[i].kind, items[i].source_index),
                        true,
                        object_ref(items[j].kind, items[j].source_index),
                        true, gap_mm, 0.0, message) != 0) {
                    result = -1; goto cleanup;
                }
            }
        }
    }

    /* Same-net pads in distinct physical copper components are open connections.
     * Net ID 0 is skipped here because the current IR has no no-net sentinel. */
    const size_t trace_count = vec_len(board->traces);
    const size_t pad_count = vec_len(board->pads);
    for (size_t i = 0; i < pad_count; ++i) {
        const PcbPad *a = &board->pads[i];
        const size_t ai = trace_count + i;
        if (!active[ai] || a->net_id == 0) continue;
        for (size_t j = i + 1; j < pad_count; ++j) {
            const PcbPad *b = &board->pads[j];
            const size_t bi = trace_count + j;
            if (!active[bi] || b->net_id != a->net_id) continue;
            if (dsu_find(parent, ai) == dsu_find(parent, bi)) continue;

            char message[PCB_DEFECT_MESSAGE_MAX];
            (void)snprintf(message, sizeof(message),
                "pad %zu (component %u pin %u) and pad %zu (component %u pin %u) share net %u but are not physically connected",
                i, a->component_id, (unsigned)a->pin_number,
                j, b->component_id, (unsigned)b->pin_number, a->net_id);
            if (out_stats) out_stats->opens++;
            if (emit_finding(visitor, user_data,
                    PCB_DEFECT_OPEN_CONNECTION, PCB_DFM_ERROR,
                    object_ref(COPPER_ITEM_PAD, i), true,
                    object_ref(COPPER_ITEM_PAD, j), false,
                    0.0, 0.0, message) != 0) {
                result = -1; goto cleanup;
            }
        }
    }

cleanup:
    free(items); free(active); free(parent); free(rank);
    return result;
}

static const PcbDefectCoverage g_coverage[] = {
    { PCB_DEFECT_FEATURE_COPPER_CLEARANCE, PCB_DEFECT_COVERAGE_PARTIAL,
      "Copper clearance", "Straight traces and circular pads/vias; no arc, polygon or filled-zone geometry yet." },
    { PCB_DEFECT_FEATURE_OPEN_CONNECTIVITY, PCB_DEFECT_COVERAGE_PARTIAL,
      "Open connectivity", "Checks assigned non-zero nets with multiple pads in the current physical IR; import/parity against a source board is not complete." },
    { PCB_DEFECT_FEATURE_INTER_NET_SHORTS, PCB_DEFECT_COVERAGE_PARTIAL,
      "Inter-net shorts", "Detects touching/overlapping supported copper shapes on shared layers; excludes unsupported geometries." },
    { PCB_DEFECT_FEATURE_DRILL_RULES, PCB_DEFECT_COVERAGE_PARTIAL,
      "Drill and hole rules", "Basic via and pad drill/annular checks exist; slots, non-plated-hole-to-all-copper rules and process-specific drill classes are not complete." },
    { PCB_DEFECT_FEATURE_BOARD_OUTLINE, PCB_DEFECT_COVERAGE_UNSUPPORTED,
      "Board outline", "PcbBoard has no first-class imported Edge.Cuts geometry; exporter currently synthesizes a rectangle from board dimensions." },
    { PCB_DEFECT_FEATURE_SILK_OVER_COPPER, PCB_DEFECT_COVERAGE_UNSUPPORTED,
      "Silkscreen to soldermask/copper", "Silkscreen strokes/text and soldermask openings are not represented in the physical IR." },
    { PCB_DEFECT_FEATURE_SILK_OVERLAP, PCB_DEFECT_COVERAGE_UNSUPPORTED,
      "Silkscreen overlap", "Text and graphic geometry needed to test overlaps is not represented." },
    { PCB_DEFECT_FEATURE_ARCS_AND_ZONES, PCB_DEFECT_COVERAGE_UNSUPPORTED,
      "Arcs and zones", "No first-class arc tracks or resolved copper-zone geometry in PcbBoard." },
    { PCB_DEFECT_FEATURE_CAM_PACKAGE, PCB_DEFECT_COVERAGE_UNSUPPORTED,
      "CAM package integrity", "Exported Gerber/drill/BOM/CPL packages are not yet independently validated." },
    { PCB_DEFECT_FEATURE_VLSI_RULES, PCB_DEFECT_COVERAGE_UNSUPPORTED,
      "VLSI rule deck", "The shared geometry foundation can be reused later, but foundry-rule parsing and VLSI-specific DRC are not implemented." }
};

const PcbDefectCoverage *pcb_defect_coverage(size_t *out_count)
{
    if (out_count) *out_count = sizeof(g_coverage) / sizeof(g_coverage[0]);
    return g_coverage;
}

const char *pcb_defect_code_name(PcbDefectCode code)
{
    switch (code) {
        case PCB_DEFECT_OPEN_CONNECTION: return "open_connection";
        case PCB_DEFECT_SHORT_CIRCUIT: return "short_circuit";
        case PCB_DEFECT_INVALID_COPPER_GEOMETRY: return "invalid_copper_geometry";
        case PCB_DEFECT_DFM_REPORT_INCONSISTENCY: return "dfm_report_inconsistency";
        default: return "unknown_defect";
    }
}

const char *pcb_defect_coverage_status_name(PcbDefectCoverageStatus status)
{
    switch (status) {
        case PCB_DEFECT_COVERAGE_SUPPORTED: return "SUPPORTED";
        case PCB_DEFECT_COVERAGE_PARTIAL: return "PARTIAL";
        case PCB_DEFECT_COVERAGE_UNSUPPORTED: return "UNSUPPORTED";
        default: return "UNKNOWN";
    }
}

/* A deliberate NaN without <math.h>; fails every finiteness check. */
static double not_a_number(void)
{
    volatile double zero = 0.0;
    return zero / zero;
}

static bool near_value(double a, double b, double tolerance)
{
    return nm_isfinite(a) && nm_isfinite(b) && nm_abs_f64(a - b) <= tolerance;
}

static bool min_rule_is_violated(double measured, double limit)
{
    return nm_isfinite(measured) && nm_isfinite(limit) && measured < limit;
}

static bool ref_is_valid(const PcbBoard *board, DefectObjectRef ref)
{
    if (!board) return false;
    switch (ref.kind) {
        case DEFECT_OBJECT_TRACE: return ref.index < vec_len(board->traces);
        case DEFECT_OBJECT_PAD: return ref.index < vec_len(board->pads);
        case DEFECT_OBJECT_VIA: return ref.index < vec_len(board->vias);
        case DEFECT_OBJECT_COMPONENT:
            return ref.index <= UINT32_MAX && pcb_find_placement(board, (uint32_t)ref.index) != NULL;
        case DEFECT_OBJECT_BOARD: return ref.index == 0;
        case DEFECT_OBJECT_DFM_ISSUE: return true;
        case DEFECT_OBJECT_UNKNOWN: return true;
        default: return false;
    }
}

static DefectObjectRef dfm_ref(PcbDfmObjectKind kind, uint32_t index)
{
    DefectObjectRef result = { .kind = DEFECT_OBJECT_UNKNOWN, .index = index };
    switch (kind) {
        case PCB_DFM_OBJECT_TRACE: result.kind = DEFECT_OBJECT_TRACE; break;
        case PCB_DFM_OBJECT_PAD: result.kind = DEFECT_OBJECT_PAD; break;
        case PCB_DFM_OBJECT_VIA: result.kind = DEFECT_OBJECT_VIA; break;
        case PCB_DFM_OBJECT_COMPONENT: result.kind = DEFECT_OBJECT_COMPONENT; break;
        case PCB_DFM_OBJECT_BOARD: result.kind = DEFECT_OBJECT_BOARD; break;
        default: break;
    }
    return result;
}

static bool audit_issue_consistent(
    const PcbBoard *board,
    const FabProfile *profile,
    const PcbDfmIssue *issue,
    char *reason,
    size_t reason_size
)
{
    if (!nm_isfinite(issue->measured) || !nm_isfinite(issue->limit)) {
        (void)snprintf(reason, reason_size, "measurement or rule limit is non-finite");
        return false;
    }
    if (issue->message[0] == '\0') {
        (void)snprintf(reason, reason_size, "finding has no diagnostic message");
        return false;
    }

    if (issue->object_kind != PCB_DFM_OBJECT_UNKNOWN &&
        !ref_is_valid(board, dfm_ref(issue->object_kind, issue->object_id))) {
        (void)snprintf(reason, reason_size, "primary object reference is outside the board model");
        return false;
    }
    if (issue->has_related_object &&
        !ref_is_valid(board, dfm_ref(issue->related_object_kind, issue->related_object_id))) {
        (void)snprintf(reason, reason_size, "related object reference is outside the board model");
        return false;
    }

    switch (issue->rule) {
        case PCB_DFM_BOARD_SIZE: {
            /* Only object ids 0 (width) and 1 (height) exist for this rule; any
             * other id yields a non-finite value that fails the check below. */
            const double actual = issue->object_id == 0
                ? board->spec.width_mm
                : issue->object_id == 1 ? board->spec.height_mm
                                        : not_a_number();
            const double minimum = issue->object_id == 0
                ? profile->min_board_width_mm : profile->min_board_height_mm;
            const double maximum = issue->object_id == 0
                ? profile->max_board_width_mm : profile->max_board_height_mm;
            if (!nm_isfinite(actual) ||
                !near_value(issue->measured, actual, 1e-8) ||
                !(actual < minimum || actual > maximum)) {
                (void)snprintf(reason, reason_size, "board-size finding does not match the board/profile dimensions");
                return false;
            }
            break;
        }
        case PCB_DFM_LAYER_COUNT:
            if (!near_value(issue->measured, board->spec.layer_count, 1e-8) ||
                !(board->spec.layer_count > profile->max_layer_count)) {
                (void)snprintf(reason, reason_size, "layer-count finding does not match the profile limit");
                return false;
            }
            break;
        case PCB_DFM_TRACE_WIDTH: {
            if (issue->object_id >= vec_len(board->traces)) {
                (void)snprintf(reason, reason_size, "trace-width finding references a missing trace");
                return false;
            }
            double actual = board->traces[issue->object_id].width_mm;
            if (!near_value(issue->measured, actual, 1e-8) ||
                !min_rule_is_violated(actual, profile->min_trace_width_mm) ||
                !near_value(issue->limit, profile->min_trace_width_mm, 1e-8)) {
                (void)snprintf(reason, reason_size, "trace-width measurement/limit is inconsistent");
                return false;
            }
            break;
        }
        case PCB_DFM_VIA_DIAMETER:
        case PCB_DFM_VIA_DRILL:
        case PCB_DFM_VIA_ANNULAR_RING: {
            if (issue->object_id >= vec_len(board->vias)) {
                (void)snprintf(reason, reason_size, "via finding references a missing via");
                return false;
            }
            const PcbVia *via = &board->vias[issue->object_id];
            double actual = issue->rule == PCB_DFM_VIA_DIAMETER ? via->diameter_mm :
                issue->rule == PCB_DFM_VIA_DRILL ? via->drill_mm :
                (via->diameter_mm - via->drill_mm) * 0.5;
            double minimum = issue->rule == PCB_DFM_VIA_DIAMETER ? profile->min_via_diameter_mm :
                issue->rule == PCB_DFM_VIA_DRILL ? profile->min_via_drill_mm :
                profile->min_via_annular_ring_mm;
            if (!near_value(issue->measured, actual, 1e-8) ||
                !min_rule_is_violated(actual, minimum) ||
                !near_value(issue->limit, minimum, 1e-8)) {
                (void)snprintf(reason, reason_size, "via measurement/limit does not match the represented via");
                return false;
            }
            break;
        }
        case PCB_DFM_PAD_ANNULAR_RING: {
            if (issue->object_id >= vec_len(board->pads)) {
                (void)snprintf(reason, reason_size, "pad annular-ring finding references a missing pad");
                return false;
            }
            const PcbPad *pad = &board->pads[issue->object_id];
            double actual = (pad->pad_diameter_mm - pad->drill_mm) * 0.5;
            if (!pad->plated || pad->drill_mm <= 0.0 ||
                !near_value(issue->measured, actual, 1e-8) ||
                !min_rule_is_violated(actual, profile->min_pad_annular_ring_mm) ||
                !near_value(issue->limit, profile->min_pad_annular_ring_mm, 1e-8)) {
                (void)snprintf(reason, reason_size, "pad annular-ring finding does not match pad geometry/profile");
                return false;
            }
            break;
        }
        case PCB_DFM_COMPONENT_HEIGHT: {
            if (issue->object_id > UINT32_MAX) {
                (void)snprintf(reason, reason_size, "component identifier is out of range");
                return false;
            }
            const PcbPlacement *placement = pcb_find_placement(board, issue->object_id);
            if (!placement || !near_value(issue->measured, placement->height_z_mm, 1e-8) ||
                !(placement->height_z_mm > profile->max_component_height_mm) ||
                !near_value(issue->limit, profile->max_component_height_mm, 1e-8)) {
                (void)snprintf(reason, reason_size, "component-height finding does not match placement/profile");
                return false;
            }
            break;
        }
        case PCB_DFM_TRACE_CLEARANCE:
        case PCB_DFM_COPPER_CLEARANCE: {
            if (!issue->has_related_object ||
                issue->object_kind == PCB_DFM_OBJECT_UNKNOWN ||
                issue->related_object_kind == PCB_DFM_OBJECT_UNKNOWN) {
                (void)snprintf(reason, reason_size, "clearance finding is missing structured object references");
                return false;
            }
            CopperItemKind ak, bk;
            if (issue->object_kind == PCB_DFM_OBJECT_TRACE) ak = COPPER_ITEM_TRACE;
            else if (issue->object_kind == PCB_DFM_OBJECT_PAD) ak = COPPER_ITEM_PAD;
            else if (issue->object_kind == PCB_DFM_OBJECT_VIA) ak = COPPER_ITEM_VIA;
            else { (void)snprintf(reason, reason_size, "unsupported primary copper object kind"); return false; }
            if (issue->related_object_kind == PCB_DFM_OBJECT_TRACE) bk = COPPER_ITEM_TRACE;
            else if (issue->related_object_kind == PCB_DFM_OBJECT_PAD) bk = COPPER_ITEM_PAD;
            else if (issue->related_object_kind == PCB_DFM_OBJECT_VIA) bk = COPPER_ITEM_VIA;
            else { (void)snprintf(reason, reason_size, "unsupported related copper object kind"); return false; }
            CopperItem a, b;
            if (pcb_copper_item(board, ak, issue->object_id, &a) != PCB_COPPER_OK ||
                pcb_copper_item(board, bk, issue->related_object_id, &b) != PCB_COPPER_OK) {
                (void)snprintf(reason, reason_size, "clearance finding references invalid copper geometry");
                return false;
            }
            double actual_gap_nm;
            if (!copper_pair_gap_nm(&a, &b, &actual_gap_nm)) {
                (void)snprintf(reason, reason_size, "clearance finding references copper that cannot be measured");
                return false;
            }
            const double actual_gap = actual_gap_nm / (double)NM_PER_MM;
            if (!near_value(issue->measured, actual_gap, 0.000002) ||
                !(actual_gap < profile->min_trace_clearance_mm) ||
                !near_value(issue->limit, profile->min_trace_clearance_mm, 1e-8)) {
                (void)snprintf(reason, reason_size, "reported clearance contradicts geometry or configured minimum");
                return false;
            }
            if (issue->rule == PCB_DFM_TRACE_CLEARANCE &&
                (ak != COPPER_ITEM_TRACE || bk != COPPER_ITEM_TRACE)) {
                (void)snprintf(reason, reason_size, "trace-clearance rule references a non-trace object");
                return false;
            }
            break;
        }
        case PCB_DFM_HOLE_CLEARANCE: {
            if (!issue->has_related_object ||
                issue->object_kind != PCB_DFM_OBJECT_PAD ||
                issue->related_object_kind != PCB_DFM_OBJECT_PAD ||
                issue->object_id >= vec_len(board->pads) ||
                issue->related_object_id >= vec_len(board->pads)) {
                (void)snprintf(reason, reason_size, "hole-clearance finding lacks valid structured pad references");
                return false;
            }
            const PcbPad *a = &board->pads[issue->object_id];
            const PcbPad *b = &board->pads[issue->related_object_id];
            if (a->drill_mm <= 0.0 || b->drill_mm <= 0.0) {
                (void)snprintf(reason, reason_size, "hole-clearance finding references a pad without a drill");
                return false;
            }
            double actual = nm_sqrt((a->x_mm - b->x_mm) * (a->x_mm - b->x_mm) +
                                 (a->y_mm - b->y_mm) * (a->y_mm - b->y_mm)) -
                            a->drill_mm * 0.5 - b->drill_mm * 0.5;
            if (!near_value(issue->measured, actual, 1e-8) ||
                !min_rule_is_violated(actual, profile->min_hole_clearance_mm) ||
                !near_value(issue->limit, profile->min_hole_clearance_mm, 1e-8)) {
                (void)snprintf(reason, reason_size, "hole-clearance finding contradicts drill geometry or configured minimum");
                return false;
            }
            break;
        }
        case PCB_DFM_EDGE_CLEARANCE: {
            double actual = not_a_number();
            switch (issue->object_kind) {
                case PCB_DFM_OBJECT_TRACE: {
                    if (issue->object_id >= vec_len(board->traces)) break;
                    const PcbTrace *t = &board->traces[issue->object_id];
                    actual = nm_min_f64(
                        nm_min_f64(nm_min_f64(t->x1_mm, t->x2_mm),
                             nm_min_f64(board->spec.width_mm - t->x1_mm,
                                  board->spec.width_mm - t->x2_mm)),
                        nm_min_f64(nm_min_f64(t->y1_mm, t->y2_mm),
                             nm_min_f64(board->spec.height_mm - t->y1_mm,
                                  board->spec.height_mm - t->y2_mm))) - t->width_mm * 0.5;
                    break;
                }
                case PCB_DFM_OBJECT_PAD: {
                    if (issue->object_id >= vec_len(board->pads)) break;
                    const PcbPad *p = &board->pads[issue->object_id];
                    actual = nm_min_f64(nm_min_f64(p->x_mm, board->spec.width_mm - p->x_mm),
                                  nm_min_f64(p->y_mm, board->spec.height_mm - p->y_mm)) - p->pad_diameter_mm * 0.5;
                    break;
                }
                case PCB_DFM_OBJECT_VIA: {
                    if (issue->object_id >= vec_len(board->vias)) break;
                    const PcbVia *v = &board->vias[issue->object_id];
                    actual = nm_min_f64(nm_min_f64(v->x_mm, board->spec.width_mm - v->x_mm),
                                  nm_min_f64(v->y_mm, board->spec.height_mm - v->y_mm)) - v->diameter_mm * 0.5;
                    break;
                }
                case PCB_DFM_OBJECT_COMPONENT: {
                    if (issue->object_id > UINT32_MAX) break;
                    const PcbPlacement *p = pcb_find_placement(board, issue->object_id);
                    if (!p) break;
                    actual = nm_min_f64(
                        nm_min_f64(p->x_mm - p->width_mm * 0.5,
                             board->spec.width_mm - (p->x_mm + p->width_mm * 0.5)),
                        nm_min_f64(p->y_mm - p->height_mm * 0.5,
                             board->spec.height_mm - (p->y_mm + p->height_mm * 0.5)));
                    break;
                }
                default: break;
            }
            if (!nm_isfinite(actual) || !near_value(issue->measured, actual, 1e-8) ||
                !min_rule_is_violated(actual, profile->min_copper_to_edge_mm) ||
                !near_value(issue->limit, profile->min_copper_to_edge_mm, 1e-8)) {
                (void)snprintf(reason, reason_size, "edge-clearance measurement/reference contradicts board geometry or configured minimum");
                return false;
            }
            break;
        }
        case PCB_DFM_OPEN_CONNECTION:
            if (!issue->has_related_object ||
                issue->object_kind != PCB_DFM_OBJECT_PAD ||
                issue->related_object_kind != PCB_DFM_OBJECT_PAD ||
                issue->object_id >= vec_len(board->pads) ||
                issue->related_object_id >= vec_len(board->pads) ||
                board->pads[issue->object_id].net_id == 0 ||
                board->pads[issue->object_id].net_id != board->pads[issue->related_object_id].net_id) {
                (void)snprintf(reason, reason_size, "open-connection finding has invalid or mismatched pad/net references");
                return false;
            }
            break;
        case PCB_DFM_SHORT_CIRCUIT:
            if (!issue->has_related_object || issue->measured > 0.0) {
                (void)snprintf(reason, reason_size, "short-circuit finding is not a touching/overlapping copper pair");
                return false;
            }
            break;
        case PCB_DFM_INVALID_GEOMETRY:
        case PCB_DFM_ENGINE_INCONSISTENCY:
            /* These are explicit diagnoses, not threshold-based measurements. */
            break;
        default:
            (void)snprintf(reason, reason_size, "unknown DFM rule identifier");
            return false;
    }
    return true;
}

int pcb_defect_audit_dfm_report(
    const PcbBoard *board,
    const FabProfile *profile,
    const PcbDfmReport *report,
    size_t first_issue,
    PcbDefectVisitor visitor,
    void *user_data,
    size_t *out_inconsistencies
)
{
    if (out_inconsistencies) *out_inconsistencies = 0;
    if (!board || !profile || !report || first_issue > pcb_dfm_issue_count(report)) return -1;

    const size_t end = pcb_dfm_issue_count(report); /* snapshot avoids recursion */
    for (size_t i = first_issue; i < end; ++i) {
        const PcbDfmIssue *issue = pcb_dfm_issue_get(report, i);
        if (!issue || issue->rule == PCB_DFM_ENGINE_INCONSISTENCY) continue;

        char reason[PCB_DEFECT_MESSAGE_MAX] = {0};
        if (audit_issue_consistent(board, profile, issue, reason, sizeof(reason))) continue;

        char message[PCB_DEFECT_MESSAGE_MAX];
        (void)snprintf(message, sizeof(message),
            "DFM report issue #%zu (%.60s) is internally inconsistent: %.110s",
            i, pcb_dfm_rule_name(issue->rule), reason);
        if (out_inconsistencies) (*out_inconsistencies)++;
        if (emit_finding(visitor, user_data,
                PCB_DEFECT_DFM_REPORT_INCONSISTENCY, PCB_DFM_ERROR,
                (DefectObjectRef){ .kind = DEFECT_OBJECT_DFM_ISSUE, .index = i },
                false, (DefectObjectRef){0}, false, 0.0, 0.0, message) != 0) {
            return -1;
        }
    }
    return 0;
}

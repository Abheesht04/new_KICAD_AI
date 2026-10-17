#include "defect.h"
#include "dfm_pcb.h"
#include "pcb.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    size_t opens;
    size_t shorts;
    size_t bad_geometry;
    size_t report_inconsistencies;
} Counts;

static int count_finding(const PcbDefectFinding *finding, void *user_data)
{
    assert(finding != NULL);
    Counts *counts = (Counts *)user_data;
    assert(counts != NULL);
    switch (finding->code) {
        case PCB_DEFECT_OPEN_CONNECTION: counts->opens++; break;
        case PCB_DEFECT_SHORT_CIRCUIT: counts->shorts++; break;
        case PCB_DEFECT_INVALID_COPPER_GEOMETRY: counts->bad_geometry++; break;
        case PCB_DEFECT_DFM_REPORT_INCONSISTENCY: counts->report_inconsistencies++; break;
        default: assert(!"unexpected defect code");
    }
    return 0;
}

static PcbBoard make_board(void)
{
    PcbBoard board;
    pcb_board_init(&board, (PcbBoardSpec){
        .width_mm = 50.0,
        .height_mm = 40.0,
        .board_thickness_mm = 1.6,
        .copper_thickness_mm = 0.035,
        .layer_count = 2
    });
    return board;
}

static void test_open_connectivity(void)
{
    PcbBoard board = make_board();
    assert(pcb_add_pad(&board, (PcbPad){
        .component_id = 1, .pin_number = 1, .net_id = 7,
        .x_mm = 5.0, .y_mm = 10.0, .pad_diameter_mm = 0.8,
        .drill_mm = 0.0, .plated = false, .layer = PCB_LAYER_F_CU
    }) == 0);
    assert(pcb_add_pad(&board, (PcbPad){
        .component_id = 2, .pin_number = 1, .net_id = 7,
        .x_mm = 15.0, .y_mm = 10.0, .pad_diameter_mm = 0.8,
        .drill_mm = 0.0, .plated = false, .layer = PCB_LAYER_F_CU
    }) == 0);

    Counts counts = {0};
    PcbDefectStats stats;
    assert(pcb_defect_check_board(&board, count_finding, &counts, &stats) == 0);
    assert(counts.opens == 1);
    assert(stats.opens == 1);

    PcbDfmReport report;
    pcb_dfm_report_init(&report);
    FabProfile profile = fab_profile_conservative_2layer();
    assert(pcb_dfm_validate(&board, &profile, &report) > 0);
    bool found_open = false;
    for (size_t i = 0; i < pcb_dfm_issue_count(&report); ++i) {
        const PcbDfmIssue *issue = pcb_dfm_issue_get(&report, i);
        if (issue && issue->rule == PCB_DFM_OPEN_CONNECTION) {
            found_open = true;
            assert(issue->object_kind == PCB_DFM_OBJECT_PAD);
            assert(issue->has_related_object);
            assert(issue->related_object_kind == PCB_DFM_OBJECT_PAD);
        }
    }
    assert(found_open);
    pcb_dfm_report_free(&report);
    pcb_board_free(&board);
    puts("PASS: detects unconnected same-net pads and reports structured references");
}

static void test_connected_pads(void)
{
    PcbBoard board = make_board();
    assert(pcb_add_pad(&board, (PcbPad){
        .component_id = 1, .pin_number = 1, .net_id = 7,
        .x_mm = 5.0, .y_mm = 10.0, .pad_diameter_mm = 0.8,
        .drill_mm = 0.0, .plated = false, .layer = PCB_LAYER_F_CU
    }) == 0);
    assert(pcb_add_pad(&board, (PcbPad){
        .component_id = 2, .pin_number = 1, .net_id = 7,
        .x_mm = 15.0, .y_mm = 10.0, .pad_diameter_mm = 0.8,
        .drill_mm = 0.0, .plated = false, .layer = PCB_LAYER_F_CU
    }) == 0);
    assert(pcb_add_trace(&board, (PcbTrace){
        .net_id = 7, .layer = PCB_LAYER_F_CU,
        .x1_mm = 5.0, .y1_mm = 10.0, .x2_mm = 15.0, .y2_mm = 10.0,
        .width_mm = 0.3
    }) == 0);

    Counts counts = {0};
    PcbDefectStats stats;
    assert(pcb_defect_check_board(&board, count_finding, &counts, &stats) == 0);
    assert(counts.opens == 0);
    assert(counts.shorts == 0);
    assert(stats.connected_pairs >= 2);
    pcb_board_free(&board);
    puts("PASS: same-net trace joins pads into one copper component");
}

static void test_short_detection(void)
{
    PcbBoard board = make_board();
    assert(pcb_add_trace(&board, (PcbTrace){
        .net_id = 1, .layer = PCB_LAYER_F_CU,
        .x1_mm = 5.0, .y1_mm = 10.0, .x2_mm = 20.0, .y2_mm = 10.0,
        .width_mm = 0.2
    }) == 0);
    assert(pcb_add_trace(&board, (PcbTrace){
        .net_id = 2, .layer = PCB_LAYER_F_CU,
        .x1_mm = 5.0, .y1_mm = 10.1, .x2_mm = 20.0, .y2_mm = 10.1,
        .width_mm = 0.2
    }) == 0);

    Counts counts = {0};
    PcbDefectStats stats;
    assert(pcb_defect_check_board(&board, count_finding, &counts, &stats) == 0);
    assert(counts.shorts == 1);
    assert(stats.shorts == 1);
    pcb_board_free(&board);
    puts("PASS: detects touching/overlapping copper on different nets");
}

static void test_invalid_geometry_is_reported(void)
{
    PcbBoard board = make_board();
    /* NaN bypasses the old <= 0.0 constructor guard; the defect layer must catch it. */
    assert(pcb_add_trace(&board, (PcbTrace){
        .net_id = 3, .layer = PCB_LAYER_F_CU,
        .x1_mm = 5.0, .y1_mm = 10.0, .x2_mm = 20.0, .y2_mm = 10.0,
        .width_mm = NAN
    }) == 0);

    Counts counts = {0};
    PcbDefectStats stats;
    assert(pcb_defect_check_board(&board, count_finding, &counts, &stats) == 0);
    assert(counts.bad_geometry == 1);
    assert(stats.invalid_geometry == 1);

    PcbDfmReport report;
    pcb_dfm_report_init(&report);
    FabProfile profile = fab_profile_conservative_2layer();
    assert(pcb_dfm_validate(&board, &profile, &report) > 0);
    bool found_invalid = false;
    for (size_t i = 0; i < pcb_dfm_issue_count(&report); ++i) {
        const PcbDfmIssue *issue = pcb_dfm_issue_get(&report, i);
        if (issue && issue->rule == PCB_DFM_INVALID_GEOMETRY) found_invalid = true;
    }
    assert(found_invalid);
    pcb_dfm_report_free(&report);
    pcb_board_free(&board);
    puts("PASS: invalid copper geometry is explicit rather than silently accepted");
}

static void test_report_self_audit(void)
{
    PcbBoard board = make_board();
    assert(pcb_add_trace(&board, (PcbTrace){
        .net_id = 1, .layer = PCB_LAYER_F_CU,
        .x1_mm = 5.0, .y1_mm = 10.0, .x2_mm = 20.0, .y2_mm = 10.0,
        .width_mm = 0.10
    }) == 0);

    FabProfile profile = fab_profile_conservative_2layer();
    PcbDfmReport report;
    pcb_dfm_report_init(&report);
    assert(pcb_dfm_validate(&board, &profile, &report) > 0);

    size_t target = (size_t)-1;
    for (size_t i = 0; i < pcb_dfm_issue_count(&report); ++i) {
        PcbDfmIssue *issue = &report.items[i];
        if (issue->rule == PCB_DFM_TRACE_WIDTH) {
            target = i;
            issue->measured = 0.20; /* Intentionally corrupt report evidence. */
            break;
        }
    }
    assert(target != (size_t)-1);

    Counts counts = {0};
    size_t inconsistencies = 0;
    assert(pcb_defect_audit_dfm_report(
        &board, &profile, &report, target,
        count_finding, &counts, &inconsistencies) == 0);
    assert(inconsistencies == 1);
    assert(counts.report_inconsistencies == 1);

    pcb_dfm_report_free(&report);
    pcb_board_free(&board);
    puts("PASS: catches a DFM finding whose measurement contradicts the board");
}

static void test_coverage_registry(void)
{
    size_t count = 0;
    const PcbDefectCoverage *coverage = pcb_defect_coverage(&count);
    assert(coverage != NULL);
    assert(count >= 8);
    bool saw_outline = false;
    bool saw_vlsi = false;
    for (size_t i = 0; i < count; ++i) {
        if (coverage[i].feature == PCB_DEFECT_FEATURE_BOARD_OUTLINE) {
            saw_outline = true;
            assert(coverage[i].status == PCB_DEFECT_COVERAGE_UNSUPPORTED);
        }
        if (coverage[i].feature == PCB_DEFECT_FEATURE_VLSI_RULES) {
            saw_vlsi = true;
            assert(coverage[i].status == PCB_DEFECT_COVERAGE_UNSUPPORTED);
        }
        assert(coverage[i].name != NULL && coverage[i].detail != NULL);
    }
    assert(saw_outline && saw_vlsi);
    puts("PASS: DFM coverage registry exposes unsupported rule families");
}

int main(void)
{
    test_open_connectivity();
    test_connected_pads();
    test_short_detection();
    test_invalid_geometry_is_reported();
    test_report_self_audit();
    test_coverage_registry();
    puts("DEFECT TEST SUITE: PASS");
    return 0;
}

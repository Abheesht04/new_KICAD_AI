#include "dfm_pcb.h"
#include "pcb.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_clean_board_passes(void)
{
    FabProfile profile = fab_profile_conservative_2layer();

    PcbBoard board;
    pcb_board_init(
        &board,
        (PcbBoardSpec){
            .width_mm = 50.0,
            .height_mm = 40.0,
            .board_thickness_mm = 1.6,
            .copper_thickness_mm = 0.035,
            .layer_count = 2
        }
    );

    assert(pcb_add_trace(
        &board,
        (PcbTrace){
            .net_id = 1,
            .layer = PCB_LAYER_F_CU,
            .x1_mm = 5.0,
            .y1_mm = 10.0,
            .x2_mm = 25.0,
            .y2_mm = 10.0,
            .width_mm = 0.30
        }
    ) == 0);

    assert(pcb_add_trace(
        &board,
        (PcbTrace){
            .net_id = 2,
            .layer = PCB_LAYER_F_CU,
            .x1_mm = 5.0,
            .y1_mm = 11.0,
            .x2_mm = 25.0,
            .y2_mm = 11.0,
            .width_mm = 0.30
        }
    ) == 0);

    PcbDfmReport report;
    pcb_dfm_report_init(&report);

    assert(pcb_dfm_validate(&board, &profile, &report) == 0);
    assert(pcb_dfm_issue_count(&report) == 0);

    pcb_dfm_report_free(&report);
    pcb_board_free(&board);

    printf("PASS: clean board\n");
}

static void test_bad_geometry_is_caught(void)
{
    FabProfile profile = fab_profile_conservative_2layer();

    PcbBoard board;
    pcb_board_init(
        &board,
        (PcbBoardSpec){
            .width_mm = 50.0,
            .height_mm = 40.0,
            .board_thickness_mm = 1.6,
            .copper_thickness_mm = 0.035,
            .layer_count = 2
        }
    );

    assert(pcb_add_trace(
        &board,
        (PcbTrace){
            .net_id = 1,
            .layer = PCB_LAYER_F_CU,
            .x1_mm = 0.30,
            .y1_mm = 10.0,
            .x2_mm = 20.0,
            .y2_mm = 10.0,
            .width_mm = 0.10
        }
    ) == 0);

    assert(pcb_add_trace(
        &board,
        (PcbTrace){
            .net_id = 2,
            .layer = PCB_LAYER_F_CU,
            .x1_mm = 5.0,
            .y1_mm = 10.20,
            .x2_mm = 20.0,
            .y2_mm = 10.20,
            .width_mm = 0.10
        }
    ) == 0);

    assert(pcb_add_via(
        &board,
        (PcbVia){
            .net_id = 3,
            .x_mm = 2.0,
            .y_mm = 20.0,
            .diameter_mm = 0.40,
            .drill_mm = 0.20,
            .layer_start = PCB_LAYER_F_CU,
            .layer_end = PCB_LAYER_B_CU,
            .plated = true
        }
    ) == 0);

    PcbDfmReport report;
    pcb_dfm_report_init(&report);

    assert(pcb_dfm_validate(&board, &profile, &report) > 0);
    assert(pcb_dfm_issue_count(&report) >= 3);

    size_t saw_width = 0;
    size_t saw_clearance = 0;
    size_t saw_via = 0;

    for (size_t i = 0; i < pcb_dfm_issue_count(&report); ++i) {
        const PcbDfmIssue *issue = pcb_dfm_issue_get(&report, i);

        if (issue && issue->rule == PCB_DFM_TRACE_WIDTH) {
            saw_width++;
        }

        if (issue && issue->rule == PCB_DFM_TRACE_CLEARANCE) {
            saw_clearance++;
            assert(issue->object_kind == PCB_DFM_OBJECT_TRACE);
            assert(issue->has_related_object);
            assert(issue->related_object_kind == PCB_DFM_OBJECT_TRACE);
        }

        if (issue &&
            (issue->rule == PCB_DFM_VIA_DIAMETER ||
             issue->rule == PCB_DFM_VIA_ANNULAR_RING)) {
            saw_via++;
        }
    }

    assert(saw_width > 0);
    assert(saw_clearance > 0);
    assert(saw_via > 0);

    pcb_dfm_report_free(&report);
    pcb_board_free(&board);

    printf("PASS: invalid geometry is rejected\n");
}

int main(void)
{
    test_clean_board_passes();
    test_bad_geometry_is_caught();

    printf("PCB DFM TEST SUITE: PASS\n");
    return 0;
}

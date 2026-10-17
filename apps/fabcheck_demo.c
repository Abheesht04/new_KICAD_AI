#include "dfm_pcb.h"
#include "defect.h"
#include "design.h"
#include "fab_profile_io.h"
#include "kicad_pcb.h"
#include "pcb.h"

#include <stdio.h>
#include <string.h>

static int add_demo_data(PcbBoard *board)
{
    if (pcb_add_trace(
            board,
            (PcbTrace){
                .net_id = 1,
                .layer = PCB_LAYER_F_CU,
                .x1_mm = 5.0,
                .y1_mm = 8.0,
                .x2_mm = 25.0,
                .y2_mm = 8.0,
                .width_mm = 0.30
            }) != 0) {
        return -1;
    }

    if (pcb_add_trace(
            board,
            (PcbTrace){
                .net_id = 2,
                .layer = PCB_LAYER_F_CU,
                .x1_mm = 5.0,
                .y1_mm = 9.0,
                .x2_mm = 25.0,
                .y2_mm = 9.0,
                .width_mm = 0.30
            }) != 0) {
        return -1;
    }

    if (pcb_add_via(
            board,
            (PcbVia){
                .net_id = 3,
                .x_mm = 10.0,
                .y_mm = 16.0,
                .diameter_mm = 0.60,
                .drill_mm = 0.30,
                .layer_start = PCB_LAYER_F_CU,
                .layer_end = PCB_LAYER_B_CU,
                .plated = true
            }) != 0) {
        return -1;
    }

    return 0;
}

static int print_coverage(void)
{
    size_t count = 0;
    const PcbDefectCoverage *items = pcb_defect_coverage(&count);
    if (!items) return 1;
    puts("DFM CHECK COVERAGE");
    puts("Status         Check                          Scope / limitation");
    puts("-------------- ------------------------------ ----------------------------------------");
    for (size_t i = 0; i < count; ++i) {
        printf("%-14s %-30s %s\n",
            pcb_defect_coverage_status_name(items[i].status),
            items[i].name,
            items[i].detail);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        if (strcmp(argv[1], "--coverage") == 0) return print_coverage();
        if (strcmp(argv[1], "--help") == 0) {
            puts("Usage: fabcheck_demo [--help|--coverage]");
            puts("  default     Run the deterministic DFM demonstration");
            puts("  --coverage  List implemented, partial, and unsupported checks");
            return 0;
        }
        fprintf(stderr, "unknown option: %s\n", argv[1]);
        fprintf(stderr, "use --help for usage\n");
        return 64;
    }
    Design design;
    if (design_init(&design) != 0) {
        return 1;
    }

    Component resistor;
    component_init(&resistor);
    strcpy(resistor.reference, "R1");
    resistor.model.kind = COMPONENT_RESISTOR;
    resistor.model.data.resistor.resistance_ohm =
        range_exact(10000.0);

    uint32_t component_id;

    if (design_add_component(&design, &resistor, &component_id) != 0) {
        component_free(&resistor);
        design_free(&design);
        return 1;
    }

    component_free(&resistor);

    PcbBoard board;

    pcb_board_init(
        &board,
        (PcbBoardSpec){
            .width_mm = 30.0,
            .height_mm = 20.0,
            .board_thickness_mm = 1.6,
            .copper_thickness_mm = 0.035,
            .layer_count = 2
        }
    );

    if (pcb_add_placement(
            &board,
            (PcbPlacement){
                .component_id = component_id,
                .x_mm = 15.0,
                .y_mm = 12.0,
                .rotation_deg = 0.0,
                .width_mm = 4.0,
                .height_mm = 2.0,
                .height_z_mm = 1.0
            }) != 0 ||
        add_demo_data(&board) != 0) {
        pcb_board_free(&board);
        design_free(&design);
        return 1;
    }

    FabProfile profile = fab_profile_conservative_2layer();

    PcbDfmReport report;
    pcb_dfm_report_init(&report);

    int errors = pcb_dfm_validate(&board, &profile, &report);

    printf("FAB PROFILE: %s\n", profile.name);
    printf("BOARD: %.2f x %.2f mm, %u layers\n",
           board.spec.width_mm,
           board.spec.height_mm,
           (unsigned)board.spec.layer_count);
    printf("DFM CORE: %s (%d errors, %zu issues)\n",
           errors == 0 ? "PASS" : "FAIL",
           errors,
           pcb_dfm_issue_count(&report));
    puts("DFM COVERAGE: PARTIAL (run fabcheck_demo --coverage for known gaps)");

    for (size_t i = 0; i < pcb_dfm_issue_count(&report); ++i) {
        const PcbDfmIssue *issue = pcb_dfm_issue_get(&report, i);
        printf("  [%s] %s",
               pcb_dfm_severity_name(issue->severity),
               pcb_dfm_rule_name(issue->rule));
        if (issue->object_kind != PCB_DFM_OBJECT_UNKNOWN) {
            printf(" %s#%u",
                   pcb_dfm_object_kind_name(issue->object_kind),
                   issue->object_id);
            if (issue->has_related_object) {
                printf(" <-> %s#%u",
                       pcb_dfm_object_kind_name(issue->related_object_kind),
                       issue->related_object_id);
            }
        }
        printf(": %s\n", issue->message);
    }

    if (kicad_pcb_write_file("fabcheck_demo.kicad_pcb", &board, &design) != 0) {
        fprintf(stderr, "failed to write KiCad PCB\n");
        pcb_dfm_report_free(&report);
        pcb_board_free(&board);
        design_free(&design);
        return 1;
    }

    FILE *profile_file = fopen("fabcheck_demo.fab", "wb");
    if (profile_file) {
        fab_profile_write(profile_file, &profile);
        fclose(profile_file);
    }

    pcb_dfm_report_free(&report);
    pcb_board_free(&board);
    design_free(&design);

    return errors == 0 ? 0 : 2;
}

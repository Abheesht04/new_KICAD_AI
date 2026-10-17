#include "copper.h"
#include "pcb_copper.h"
#include "pcb.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    size_t count;
    size_t shorts;
    bool saw_trace_pad;
    bool saw_trace_via;
} FindingCounts;

static int record_finding(const PcbCopperFinding *f, void *user_data)
{
    FindingCounts *counts = (FindingCounts *)user_data;
    counts->count++;
    if (f->result == COPPER_RESULT_SHORT) counts->shorts++;
    CopperItemKind a = f->a.kind, b = f->b.kind;
    if ((a == COPPER_ITEM_TRACE && b == COPPER_ITEM_PAD) ||
        (a == COPPER_ITEM_PAD && b == COPPER_ITEM_TRACE)) counts->saw_trace_pad = true;
    if ((a == COPPER_ITEM_TRACE && b == COPPER_ITEM_VIA) ||
        (a == COPPER_ITEM_VIA && b == COPPER_ITEM_TRACE)) counts->saw_trace_via = true;
    return 0;
}

static PcbBoardSpec spec2(void)
{
    return (PcbBoardSpec){ .width_mm = 50.0, .height_mm = 40.0,
        .board_thickness_mm = 1.6, .copper_thickness_mm = 0.035, .layer_count = 2 };
}

static void test_layer_masks(void)
{
    CopperLayerMask top = 0, in1 = 0, in2 = 0, bottom = 0;
    assert(pcb_copper_mask_from_kicad_layer(PCB_LAYER_F_CU, 4, &top) == PCB_COPPER_OK);
    assert(pcb_copper_mask_from_kicad_layer(PCB_LAYER_IN1_CU, 4, &in1) == PCB_COPPER_OK);
    assert(pcb_copper_mask_from_kicad_layer(PCB_LAYER_IN2_CU, 4, &in2) == PCB_COPPER_OK);
    assert(pcb_copper_mask_from_kicad_layer(PCB_LAYER_B_CU, 4, &bottom) == PCB_COPPER_OK);
    assert(top == 1 && in1 == 2 && in2 == 4 && bottom == 8);
    assert(!copper_layer_masks_overlap(top, bottom));
    assert(copper_layer_masks_overlap(in1, in1));

    CopperLayerMask through = 0;
    assert(copper_layer_mask_span(0, 3, &through) && through == 0x0f);
    assert(pcb_copper_mask_from_kicad_layer(PCB_LAYER_B_CU, 1, &bottom) < 0);
    assert(pcb_copper_mask_from_kicad_layer(PCB_LAYER_IN1_CU, 2, &in1) < 0);
    puts("PASS: layer masks and KiCad layer mapping");
}

static void test_trace_clearance_and_layer_filter(void)
{
    PcbBoard board;
    pcb_board_init(&board, spec2());
    /* 0.20 mm copper; centre spacing 0.349 mm -> 0.149 mm gap. */
    assert(pcb_add_trace(&board, (PcbTrace){ .net_id = 1, .layer = PCB_LAYER_F_CU,
        .x1_mm = 5, .y1_mm = 10, .x2_mm = 25, .y2_mm = 10, .width_mm = 0.20 }) == 0);
    assert(pcb_add_trace(&board, (PcbTrace){ .net_id = 2, .layer = PCB_LAYER_F_CU,
        .x1_mm = 5, .y1_mm = 10.349, .x2_mm = 25, .y2_mm = 10.349, .width_mm = 0.20 }) == 0);
    /* Same XY on bottom copper: no planar check. */
    assert(pcb_add_trace(&board, (PcbTrace){ .net_id = 3, .layer = PCB_LAYER_B_CU,
        .x1_mm = 5, .y1_mm = 10.349, .x2_mm = 25, .y2_mm = 10.349, .width_mm = 0.20 }) == 0);
    /* Same net as trace 1: ordinary inter-net clearance does not apply. */
    assert(pcb_add_trace(&board, (PcbTrace){ .net_id = 1, .layer = PCB_LAYER_F_CU,
        .x1_mm = 5, .y1_mm = 20.05, .x2_mm = 25, .y2_mm = 20.05, .width_mm = 0.20 }) == 0);

    FindingCounts found = {0};
    PcbCopperStats stats;
    assert(pcb_copper_check_clearance(&board, 150000, record_finding, &found, &stats) == 0);
    assert(found.count == 1);
    assert(stats.violations == 1);
    assert(stats.filtered_by_layer > 0);
    assert(stats.filtered_same_net > 0);
    pcb_board_free(&board);
    puts("PASS: trace clearance, layers, and same-net semantics");
}

static void test_trace_to_pad_and_via(void)
{
    PcbBoard board;
    pcb_board_init(&board, spec2());
    assert(pcb_add_trace(&board, (PcbTrace){ .net_id = 1, .layer = PCB_LAYER_F_CU,
        .x1_mm = 5, .y1_mm = 10, .x2_mm = 20, .y2_mm = 10, .width_mm = 0.20 }) == 0);
    assert(pcb_add_pad(&board, (PcbPad){ .component_id = 1, .pin_number = 1, .net_id = 2,
        .x_mm = 10, .y_mm = 10.30, .pad_diameter_mm = 0.20, .drill_mm = 0.0,
        .plated = false, .layer = PCB_LAYER_F_CU }) == 0);
    assert(pcb_add_via(&board, (PcbVia){ .net_id = 3, .x_mm = 15, .y_mm = 10.50,
        .diameter_mm = 0.60, .drill_mm = 0.30, .layer_start = PCB_LAYER_F_CU,
        .layer_end = PCB_LAYER_B_CU, .plated = true }) == 0);

    FindingCounts found = {0};
    PcbCopperStats stats;
    assert(pcb_copper_check_clearance(&board, 150000, record_finding, &found, &stats) == 0);
    assert(found.saw_trace_pad && found.saw_trace_via && found.count >= 2);
    pcb_board_free(&board);
    puts("PASS: trace-to-pad and trace-to-via clearance");
}

static void test_no_net_copper_is_never_silent(void)
{
    PcbBoard board;
    pcb_board_init(&board, spec2());
    /* Two net-0 (no net) traces overlapping: must be a SHORT, not ignored. */
    assert(pcb_add_trace(&board, (PcbTrace){ .net_id = 0, .layer = PCB_LAYER_F_CU,
        .x1_mm = 10, .y1_mm = 20, .x2_mm = 30, .y2_mm = 20, .width_mm = 0.25 }) == 0);
    assert(pcb_add_trace(&board, (PcbTrace){ .net_id = 0, .layer = PCB_LAYER_F_CU,
        .x1_mm = 10, .y1_mm = 20.05, .x2_mm = 30, .y2_mm = 20.05, .width_mm = 0.25 }) == 0);
    FindingCounts found = {0};
    assert(pcb_copper_check_clearance(&board, 150000, record_finding, &found, NULL) == 0);
    assert(found.count == 1 && found.shorts == 1);
    pcb_board_free(&board);
    puts("PASS: net-0 copper that touches is reported as a short");
}

static CopperItem item(CopperShape s, CopperNet net)
{
    CopperItem it;
    memset(&it, 0, sizeof(it));
    it.shape = s;
    it.layers = 1;
    it.net = net;
    return it;
}

static void test_pair_check_shapes(void)
{
    const Nm mm = NM_PER_MM;
    /* rect pad 1x1 mm centred at origin, radius 0 */
    CopperItem rect = item(copper_shape_rect((Pt){-mm / 2, -mm / 2}, (Pt){mm / 2, mm / 2}, 0), 1);
    /* trace 0.2 mm wide, radius 0.1 mm, running parallel 0.35 mm above the rect top edge */
    CopperItem trace = item(copper_shape_segment(
        (Pt){-3 * mm, mm / 2 + 450000}, (Pt){3 * mm, mm / 2 + 450000}, mm / 10), 2);
    /* core distance 0.45 mm, minus radius 0.1 -> gap 0.35 mm */
    assert(copper_pair_check(&rect, &trace, 349999) == COPPER_RESULT_PASS);
    assert(copper_pair_check(&rect, &trace, 350000) == COPPER_RESULT_PASS);   /* equal passes */
    assert(copper_pair_check(&rect, &trace, 350001) == COPPER_RESULT_TOO_CLOSE);

    /* same net: not applicable; different layer: not applicable */
    trace.net = 1;
    assert(copper_pair_check(&rect, &trace, 350001) == COPPER_RESULT_NOT_APPLICABLE);
    trace.net = 2;
    trace.layers = 2;
    assert(copper_pair_check(&rect, &trace, 350001) == COPPER_RESULT_NOT_APPLICABLE);
    trace.layers = 1;

    /* touching with required 0 is a SHORT, never a pass */
    CopperItem touch = item(copper_shape_segment(
        (Pt){-3 * mm, mm / 2 + 100000}, (Pt){3 * mm, mm / 2 + 100000}, mm / 10), 2);
    assert(copper_pair_check(&rect, &touch, 0) == COPPER_RESULT_SHORT);

    /* crossing segments are a short (the old checker missed this) */
    CopperItem x1 = item(copper_shape_segment((Pt){0, 0}, (Pt){10 * mm, 10 * mm}, 100000), 1);
    CopperItem x2 = item(copper_shape_segment((Pt){0, 10 * mm}, (Pt){10 * mm, 0}, 100000), 2);
    assert(copper_pair_check(&x1, &x2, 100000) == COPPER_RESULT_SHORT);

    /* unsupported kinds must be UNVERIFIED, never PASS */
    CopperItem arc = item((CopperShape){ .kind = COPPER_SHAPE_ARC, .radius = 1000 }, 3);
    assert(copper_pair_check(&rect, &arc, 100000) == COPPER_RESULT_UNVERIFIED);

    /* bad input */
    CopperItem bad = item(copper_shape_point((Pt){0, 0}, -1), 1);
    assert(copper_pair_check(&rect, &bad, 0) == COPPER_RESULT_INVALID_INPUT);
    assert(copper_pair_check(&rect, &trace, -1) == COPPER_RESULT_INVALID_INPUT);
    /* rounded rect whose corner radius exceeds half the short side is rejected */
    CopperShape inv = copper_shape_rect((Pt){0, 0}, (Pt){mm, 2 * mm}, 600000);
    assert(copper_shape_validate(&inv) == COPPER_RESULT_INVALID_INPUT);
    puts("PASS: pair check on rect/segment/point, shorts, unsupported, invalid");
}

static void test_conversion_rejects_bad_inputs(void)
{
    Nm value;
    assert(pcb_copper_mm_to_nm_coordinate(1.2345674, &value) == PCB_COPPER_OK);
    assert(value == 1234567);
    assert(pcb_copper_mm_to_nm_ceil(0.1500001, &value) == PCB_COPPER_OK);
    assert(value == 150001);
    assert(pcb_copper_mm_to_nm_ceil(0.15, &value) == PCB_COPPER_OK);
    assert(value == 150000); /* representation noise must not add a nanometre */
    assert(pcb_copper_mm_to_nm_ceil(0.2, &value) == PCB_COPPER_OK && value == 200000);
    assert(pcb_copper_mm_to_nm_coordinate(INFINITY, &value) < 0);
    assert(pcb_copper_mm_to_nm_coordinate(NAN, &value) < 0);
    assert(pcb_copper_mm_to_nm_coordinate(501.0, &value) < 0);
    assert(pcb_copper_mm_to_nm_ceil(-0.1, &value) < 0);
    puts("PASS: unit conversion and invalid input handling");
}

int main(void)
{
    test_layer_masks();
    test_trace_clearance_and_layer_filter();
    test_trace_to_pad_and_via();
    test_no_net_copper_is_never_silent();
    test_pair_check_shapes();
    test_conversion_rejects_bad_inputs();
    puts("COPPER TEST SUITE: PASS");
    return 0;
}

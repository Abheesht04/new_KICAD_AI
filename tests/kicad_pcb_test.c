#include "design.h"
#include "kicad_pcb.h"
#include "pcb.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    Design design;
    assert(design_init(&design) == 0);

    Component resistor;
    component_init(&resistor);
    strcpy(resistor.reference, "R1");
    strcpy(resistor.manufacturer_part_number, "TEST-R");
    resistor.model.kind = COMPONENT_RESISTOR;
    resistor.model.data.resistor.resistance_ohm = range_exact(10000.0);
    component_set_value(&resistor, 10000.0, 1.0);
    assert(design_add_component(&design, &resistor, NULL) == 0);
    component_free(&resistor);

    uint32_t net_id;
    assert(design_add_net(&design, "VCC", &net_id) == 0);
    assert(net_id == 0);

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

    assert(pcb_add_placement(
        &board,
        (PcbPlacement){
            .component_id = 0,
            .x_mm = 15.0,
            .y_mm = 10.0,
            .rotation_deg = 0.0,
            .width_mm = 4.0,
            .height_mm = 2.0,
            .height_z_mm = 1.0
        }
    ) == 0);

    uint32_t trace_net;
    assert(design_add_net(&design, "SIGNAL", &trace_net) == 0);

    assert(pcb_add_trace(
        &board,
        (PcbTrace){
            .net_id = trace_net,
            .layer = PCB_LAYER_F_CU,
            .x1_mm = 5.0,
            .y1_mm = 10.0,
            .x2_mm = 10.0,
            .y2_mm = 10.0,
            .width_mm = 0.25
        }
    ) == 0);

    FILE *out = tmpfile();
    assert(out != NULL);
    assert(kicad_pcb_write(&board, &design, out) == 0);

    rewind(out);

    char data[8192];
    size_t count = fread(data, 1, sizeof(data) - 1, out);
    data[count] = '\0';

    assert(strstr(data, "(kicad_pcb (version 20241229)") != NULL);
    assert(strstr(data, "\"F.Cu\"") != NULL);
    assert(strstr(data, "(2 \"B.Cu\" signal)") != NULL);
    assert(strstr(data, "(segment") != NULL);
    assert(strstr(data, "(net 1 \"SIGNAL\")") != NULL);
    assert(strstr(data, "\"R1\"") != NULL);
    assert(strstr(data, "\"Edge.Cuts\"") != NULL);

    fclose(out);

    /* The internal stackup order maps to KiCad file layer IDs: In1=4,
     * In2=6, bottom copper=2. These IDs are not stackup indices. */
    board.spec.layer_count = 4;
    out = tmpfile();
    assert(out != NULL);
    assert(kicad_pcb_write(&board, &design, out) == 0);
    rewind(out);
    count = fread(data, 1, sizeof(data) - 1, out);
    data[count] = '\0';
    assert(strstr(data, "(2 \"B.Cu\" signal)") != NULL);
    assert(strstr(data, "(4 \"In1.Cu\" signal)") != NULL);
    assert(strstr(data, "(6 \"In2.Cu\" signal)") != NULL);

    fclose(out);
    pcb_board_free(&board);
    design_free(&design);

    printf("KiCad PCB writer test PASSED\n");
    return 0;
}

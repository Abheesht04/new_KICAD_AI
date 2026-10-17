#include "kicad_schematic.h"
#include "IR.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "arena.h"
#include "component.h"
#include "design.h"
#include "range.h"

int main(void) {
    Design design;
    assert(design_init(&design) == 0);
    Component resistor;
    component_init(&resistor);
    strcpy(resistor.reference, "R1");
    resistor.symbol = "Device:R";
    resistor.footprint = "Resistor_SMD:R_0603_1608Metric";
    resistor.package = "0603";
    resistor.model.kind = COMPONENT_RESISTOR;
    resistor.model.data.resistor.resistance_ohm = range_exact(10000.0);
    component_set_value(&resistor, 10000.0, 0.0);
    assert(component_add_pin(&resistor, 1, "1") == 0);
    assert(component_add_pin(&resistor, 2, "2") == 0);
    assert(design_add_component(&design, &resistor, NULL) == 0);
    component_free(&resistor);

    HwcIntent intent;
    memset(&intent, 0, sizeof(intent));
    intent.kind = HWC_INTENT_MAKE_RESISTOR;
    intent.resistor_ohm = 10000.0;
    snprintf(intent.package, sizeof(intent.package), "0603");
    char error[512] = {0};
    assert(hwc_kicad_schematic_write("out/schematic_export_test.kicad_sch", &design,
                                     &intent, error, sizeof(error)));

    Arena arena;
    arena_init(&arena, 65536);
    IRError parse_error;
    IRNode *root = ir_parse_file(&arena, "out/schematic_export_test.kicad_sch", &parse_error);
    assert(root != NULL);
    assert(ir_list_head(root) && strcmp(ir_list_head(root)->text, "kicad_sch") == 0);
    const IRNode *lib_symbols = ir_list_find(root, "lib_symbols");
    assert(lib_symbols != NULL);
    assert(ir_list_find_named(lib_symbols, "symbol", "Device:R") != NULL);
    assert(ir_list_find(root, "sheet_instances") != NULL);
    arena_destroy(&arena);
    design_free(&design);
    puts("PASS: C Design -> IR -> .kicad_sch exporter");
    return 0;
}

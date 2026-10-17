#include "mna_from_design.h"

#include "component.h"
#include "design.h"
#include "diagnostic.h"
#include "node_map.h"
#include "range.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static uint32_t add_two_pin_component(Design *design, const char *ref,
                                      ComponentKind kind, double value) {
    Component component;
    component_init(&component);
    (void)snprintf(component.reference, sizeof(component.reference), "%s", ref);
    component.model.kind = kind;
    if (kind == COMPONENT_SOURCE) {
        component.model.data.source.voltage_dc_v = value;
    } else if (kind == COMPONENT_RESISTOR) {
        component.model.data.resistor.resistance_ohm = range_exact(value);
        component_set_value(&component, value, 0.0);
    }
    assert(component_add_pin(&component, 1, "1") == 0);
    assert(component_add_pin(&component, 2, "2") == 0);
    uint32_t id = UINT32_MAX;
    assert(design_add_component(design, &component, &id) == 0);
    component_free(&component);
    return id;
}

int main(void) {
    Design design;
    assert(design_init(&design) == 0);

    uint32_t source = add_two_pin_component(&design, "VSIM", COMPONENT_SOURCE, 5.0);
    uint32_t r1 = add_two_pin_component(&design, "R1", COMPONENT_RESISTOR, 10000.0);
    uint32_t r2 = add_two_pin_component(&design, "R2", COMPONENT_RESISTOR, 20000.0);

    uint32_t vin = UINT32_MAX, vout = UINT32_MAX, gnd = UINT32_MAX;
    assert(design_add_net(&design, "VIN", &vin) == 0);
    assert(design_add_net(&design, "VOUT", &vout) == 0);
    assert(design_add_net(&design, "GND", &gnd) == 0);

    assert(design_connect_pin(&design, source, 1, vin) == 0);
    assert(design_connect_pin(&design, source, 2, gnd) == 0);
    assert(design_connect_pin(&design, r1, 1, vin) == 0);
    assert(design_connect_pin(&design, r1, 2, vout) == 0);
    assert(design_connect_pin(&design, r2, 1, vout) == 0);
    assert(design_connect_pin(&design, r2, 2, gnd) == 0);

    DiagnosticList diagnostics;
    diagnostic_list_init(&diagnostics);
    MnaCircuit circuit;
    memset(&circuit, 0, sizeof(circuit));
    assert(design_to_mna(&design, gnd, &circuit, &diagnostics));
    assert(circuit.device_count == 3);

    MnaOptions options;
    mna_options_default(&options);
    MnaResult result;
    memset(&result, 0, sizeof(result));
    mna_solve(&circuit, &options, &result);
    assert(result.status == MNA_STATUS_SUCCESS);

    NodeMap map;
    assert(node_map_build(&design, gnd, &map));
    size_t vout_row = node_map_row(&map, vout);
    assert(vout_row != SIZE_MAX);
    double voltage = mna_node_voltage(&result, vout_row + 1);
    assert(fabs(voltage - (5.0 * 20000.0 / 30000.0)) < 1e-8);

    MnaValidation validation;
    memset(&validation, 0, sizeof(validation));
    assert(mna_validate(&circuit, &result, &options, &validation));
    assert(validation.valid);

    node_map_free(&map);
    mna_result_free(&result);
    mna_circuit_free(&circuit);
    diagnostic_list_free(&diagnostics);
    design_free(&design);
    puts("PASS: Design -> NodeMap -> MNA divider solves to 3.333333 V");
    return 0;
}

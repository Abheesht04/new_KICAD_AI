#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "design.h"

int main(void)
{
    Design design;
    assert(design_init(&design) == 0);

    Component r1;
    component_init(&r1);

    strcpy(r1.reference, "R1");
    strcpy(r1.manufacturer_part_number, "RC0603FR-0710KL");

    r1.manufacturer = "Yageo";
    r1.package = "0603";
    r1.footprint = "Resistor_SMD:R_0603_1608Metric";
    r1.symbol = "Device:R";

    r1.model.kind = COMPONENT_RESISTOR;
    component_set_value(&r1, 10000.0, 1.0);
    r1.model.data.resistor.resistance_ohm = r1.value_range;

    assert(component_add_pin(&r1, 1, "1") == 0);
    assert(component_add_pin(&r1, 2, "2") == 0);

    uint32_t r1_id;
    assert(design_add_component(&design, &r1, &r1_id) == 0);

    component_free(&r1);

    uint32_t vin_id;
    uint32_t gnd_id;

    assert(design_add_net(&design, "VIN", &vin_id) == 0);
    assert(design_add_net(&design, "GND", &gnd_id) == 0);

    assert(design_connect_pin(&design, r1_id, 1, vin_id) == 0);
    assert(design_connect_pin(&design, r1_id, 2, gnd_id) == 0);

    Component *stored = design_get_component(&design, r1_id);
    assert(stored != NULL);
    assert(strcmp(stored->reference, "R1") == 0);
    assert(stored->model.kind == COMPONENT_RESISTOR);
    assert(stored->value_range.minimum == 9900.0);
    assert(stored->value_range.maximum == 10100.0);

    Pin *pin1 = component_find_pin(stored, 1);
    Pin *pin2 = component_find_pin(stored, 2);

    assert(pin1 != NULL);
    assert(pin2 != NULL);
    assert(pin1->net_id == (int32_t)vin_id);
    assert(pin2->net_id == (int32_t)gnd_id);

    Net *vin = design_get_net(&design, vin_id);
    Net *gnd = design_get_net(&design, gnd_id);

    assert(vin != NULL);
    assert(gnd != NULL);
    assert(vec_len(vin->connections) == 1);
    assert(vec_len(gnd->connections) == 1);

    const char *vin1 = vin->name;
    const char *vin2 = intern_string(&design.interner, "VIN");
    assert(vin1 == vin2);

    Constraint constraint = {
        0,
        CONSTRAINT_POWER,
        r1_id,
        "R1_POWER",
        range_exact(0.25)
    };

    assert(design_add_constraint(&design, constraint) == 0);
    assert(constraint_count(&design.constraints) == 1);
    assert(design.constraints.items[0].target_id == r1_id);

    assert(model_registry_find(
        &design.models,
        COMPONENT_RESISTOR
    ) != NULL);

    design_free(&design);

    printf("Design test PASSED\n");
    return 0;
}

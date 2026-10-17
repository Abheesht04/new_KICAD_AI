#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "component.h"
#include "component_model.h"

int main(void)
{
    Component r;
    component_init(&r);

    r.model.kind = COMPONENT_RESISTOR;
    component_set_value(&r, 10000.0, 1.0);
    r.model.data.resistor.resistance_ohm = r.value_range;

    assert(r.model.kind == COMPONENT_RESISTOR);
    assert(r.value == 10000.0);
    assert(r.value_range.minimum == 9900.0);
    assert(r.value_range.maximum == 10100.0);
    assert(r.model.data.resistor.resistance_ohm.minimum == 9900.0);

    assert(strcmp(component_kind_name(COMPONENT_MOSFET), "mosfet") == 0);

    component_free(&r);

    printf("Component model test PASSED\n");
    return 0;
}

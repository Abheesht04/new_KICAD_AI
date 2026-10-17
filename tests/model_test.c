#include <assert.h>
#include <stdio.h>

#include "component.h"
#include "model_registry.h"

int main(void)
{
    ModelRegistry registry;
    model_registry_init(&registry);

    assert(model_registry_register_builtins(&registry) == 0);

    const ModelOps *resistor =
        model_registry_find(&registry, COMPONENT_RESISTOR);
    const ModelOps *capacitor =
        model_registry_find(&registry, COMPONENT_CAPACITOR);
    const ModelOps *mosfet =
        model_registry_find(&registry, COMPONENT_MOSFET);

    assert(resistor != NULL);
    assert(capacitor != NULL);
    assert(mosfet != NULL);

    Component r;
    component_init(&r);
    r.model.kind = COMPONENT_RESISTOR;
    component_set_value(&r, 1000.0, 5.0);
    r.model.data.resistor.resistance_ohm = r.value_range;

    DiagnosticList diagnostics;
    diagnostic_list_init(&diagnostics);

    assert(resistor->validate != NULL);
    assert(resistor->validate(&r, &diagnostics) == 0);
    assert(diagnostic_count(&diagnostics) == 0);

    component_free(&r);
    diagnostic_list_free(&diagnostics);
    model_registry_free(&registry);

    printf("Model registry test PASSED\n");
    return 0;
}

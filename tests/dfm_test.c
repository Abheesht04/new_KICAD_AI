#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "dfm.h"
#include "design.h"

static void add_basic_resistor(
    Design *design,
    const char *reference,
    const char *footprint,
    int connect_pins,
    uint32_t *component_id
)
{
    Component r;
    component_init(&r);

    strcpy(r.reference, reference);
    r.footprint = footprint;
    r.symbol = "Device:R";
    r.model.kind = COMPONENT_RESISTOR;
    component_set_value(&r, 1000.0, 1.0);
    r.model.data.resistor.resistance_ohm = r.value_range;

    assert(component_add_pin(&r, 1, "1") == 0);
    assert(component_add_pin(&r, 2, "2") == 0);

    assert(design_add_component(design, &r, component_id) == 0);

    component_free(&r);

    if (connect_pins) {
        uint32_t net_a;
        uint32_t net_b;

        assert(design_add_net(design, "A", &net_a) == 0);
        assert(design_add_net(design, "B", &net_b) == 0);

        assert(design_connect_pin(design, *component_id, 1, net_a) == 0);
        assert(design_connect_pin(design, *component_id, 2, net_b) == 0);
    }
}

int main(void)
{
    Design design;
    assert(design_init(&design) == 0);

    uint32_t good_id;
    add_basic_resistor(
        &design,
        "R1",
        "Resistor_SMD:R_0603_1608Metric",
        1,
        &good_id
    );

    DfmRegistry registry;
    assert(dfm_registry_init(&registry) == 0);
    assert(dfm_register_builtin_rules(&registry) == 0);

    DfmProfile profile = {
        4,
        1.0,
        0.15,
        0.15,
        0.60,
        0.30,
        0.15,
        0.25,
        2.0
    };

    DiagnosticList diagnostics;
    diagnostic_list_init(&diagnostics);

    int errors = dfm_run_all(
        &registry,
        &design,
        &profile,
        &diagnostics
    );

    assert(errors == 0);
    assert(diagnostic_count(&diagnostics) == 0);

    /* Add a deliberately broken component. */
    uint32_t bad_id;
    add_basic_resistor(
        &design,
        "R2",
        NULL,
        0,
        &bad_id
    );

    errors = dfm_run_all(
        &registry,
        &design,
        &profile,
        &diagnostics
    );

    assert(errors >= 3);
    assert(diagnostic_count(&diagnostics) >= 3);

    assert(bad_id < vec_len(design.components));

    diagnostic_list_free(&diagnostics);
    dfm_registry_free(&registry);
    design_free(&design);

    printf("DFM test PASSED\n");
    return 0;
}

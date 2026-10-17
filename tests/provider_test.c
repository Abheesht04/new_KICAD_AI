#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "part_provider.h"

static int mock_resolve(
    void *context,
    const PartRequest *request,
    PartResult *result
)
{
    (void)context;

    if (strcmp(request->role, "resistor") != 0) {
        return -1;
    }

    result->manufacturer = "Yageo";
    result->manufacturer_part_number = "RC0603FR-0710KL";
    result->package = "0603";
    result->footprint = "Resistor_SMD:R_0603_1608Metric";
    result->symbol = "Device:R";
    result->model.kind = COMPONENT_RESISTOR;
    result->model.data.resistor.resistance_ohm = request->value;
    result->electrical.max_voltage = 75.0;
    result->electrical.max_current = 0.1;
    result->electrical.max_power = 0.1;
    result->dimensions.width_mm = 1.6;
    result->dimensions.length_mm = 0.8;
    result->dimensions.height_mm = 0.6;

    return 0;
}

int main(void)
{
    PartProvider provider = {
        "mock",
        NULL,
        mock_resolve
    };

    PartRequest request = {
        "resistor",
        range_percent(10000.0, 1.0),
        "0603",
        24.0,
        0.01,
        0.05
    };

    PartResult result;

    assert(
        part_provider_resolve(
            &provider,
            &request,
            &result
        ) == 0
    );

    assert(strcmp(result.manufacturer, "Yageo") == 0);
    assert(strcmp(result.manufacturer_part_number, "RC0603FR-0710KL") == 0);
    assert(result.model.kind == COMPONENT_RESISTOR);
    assert(result.model.data.resistor.resistance_ohm.nominal == 10000.0);

    printf("Part provider test PASSED\n");
    return 0;
}

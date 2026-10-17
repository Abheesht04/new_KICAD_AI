#include "nlp_rules.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    HwcIntent intent;
    char error[512];
    assert(hwc_intent_parse("make resistor", &intent, error, sizeof(error)));
    assert(intent.kind == HWC_INTENT_MAKE_RESISTOR);
    assert(intent.used_default_value);
    assert(fabs(intent.resistor_ohm - 10000.0) < 1e-9);

    assert(hwc_intent_parse("make resistor 4.7k ohm", &intent, error, sizeof(error)));
    assert(intent.kind == HWC_INTENT_MAKE_RESISTOR);
    assert(fabs(intent.resistor_ohm - 4700.0) < 1e-9);

    assert(hwc_intent_parse("make resistor divider 5 V to 3.3 V using 10k ohm and 20k ohm",
                            &intent, error, sizeof(error)));
    assert(intent.kind == HWC_INTENT_RESISTOR_DIVIDER);
    assert(fabs(intent.input_voltage_v - 5.0) < 1e-9);
    assert(fabs(intent.target_voltage_v - 3.3) < 1e-9);
    assert(fabs(intent.r_top_ohm - 10000.0) < 1e-9);
    assert(fabs(intent.r_bottom_ohm - 20000.0) < 1e-9);

    assert(!hwc_intent_parse("make capacitor 100nF", &intent, error, sizeof(error)));
    assert(strstr(error, "resistor") != NULL);
    puts("PASS: deterministic prompt intent parser");
    return 0;
}

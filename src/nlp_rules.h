#ifndef HWC_NLP_RULES_H
#define HWC_NLP_RULES_H

#include <stdbool.h>
#include <stddef.h>

typedef enum HwcIntentKind {
    HWC_INTENT_INVALID = 0,
    HWC_INTENT_MAKE_RESISTOR,
    HWC_INTENT_RESISTOR_DIVIDER
} HwcIntentKind;

typedef struct HwcIntent {
    HwcIntentKind kind;
    double resistor_ohm;
    double input_voltage_v;
    double target_voltage_v;
    double r_top_ohm;
    double r_bottom_ohm;
    char package[8];
    bool used_default_value;
} HwcIntent;

/* Deterministic, dependency-free intent parser. It never silently guesses a
 * topology outside the explicitly supported grammar. */
int hwc_intent_parse(const char *text, HwcIntent *out,
                     char *error, size_t error_capacity);
const char *hwc_intent_kind_name(HwcIntentKind kind);

#endif

#include "nlp_rules.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_error(char *dst, size_t cap, const char *message) {
    if (dst && cap) {
        (void)snprintf(dst, cap, "%s", message ? message : "invalid prompt");
    }
}

static int contains_casefold(const char *text, const char *needle) {
    if (!text || !needle || !*needle) return 0;
    size_t n = strlen(needle);
    for (const char *p = text; *p; ++p) {
        size_t i = 0;
        while (i < n && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            ++i;
        }
        if (i == n) return 1;
    }
    return 0;
}

static const char *skip_space(const char *p) {
    while (*p && isspace((unsigned char)*p)) ++p;
    return p;
}

static int starts_word_ci(const char *p, const char *word) {
    size_t n = strlen(word);
    for (size_t i = 0; i < n; ++i) {
        if (!p[i] || tolower((unsigned char)p[i]) != tolower((unsigned char)word[i])) return 0;
    }
    return !(isalnum((unsigned char)p[n]) || p[n] == '_');
}

/* Returns 1 for a recognised voltage token, 2 for resistance, 0 otherwise.
 * Scales are explicit: k=1e3, M=1e6, m=1e-3. */
static int quantity_after(const char *after, double raw, double *value,
                          int *kind) {
    const char *p = skip_space(after);
    double scale = 1.0;
    int is_resistance = 0;

    if (*p == 'V' || *p == 'v') {
        if (starts_word_ci(p, "volt")) p += 4;
        else ++p;
        if (starts_word_ci(p, "olts")) p += 4;
        *value = raw;
        *kind = 1;
        return 1;
    }

    if (*p == 'k' || *p == 'K') { scale = 1e3; ++p; is_resistance = 1; }
    else if (*p == 'M') { scale = 1e6; ++p; is_resistance = 1; }
    else if (*p == 'm') { scale = 1e-3; ++p; is_resistance = 1; }

    if (is_resistance) {
        p = skip_space(p);
        if (starts_word_ci(p, "ohms")) p += 4;
        else if (starts_word_ci(p, "ohm")) p += 3;
        else if ((unsigned char)p[0] == 0xCE && (unsigned char)p[1] == 0xA9) p += 2;
        /* A scaled value such as 10k is a complete resistance quantity. */
        *value = raw * scale;
        *kind = 2;
        return isfinite(*value) ? 1 : 0;
    }

    if (starts_word_ci(p, "ohms")) p += 4, is_resistance = 1;
    else if (starts_word_ci(p, "ohm")) p += 3, is_resistance = 1;
    else if ((unsigned char)p[0] == 0xCE && (unsigned char)p[1] == 0xA9) p += 2, is_resistance = 1;

    if (is_resistance) {
        *value = raw;
        *kind = 2;
        return isfinite(*value) ? 1 : 0;
    }
    return 0;
}

static size_t extract_quantities(const char *text, double *volts, size_t vcap,
                                 double *ohms, size_t rcap) {
    size_t nv = 0, nr = 0;
    const char *p = text;
    while (*p) {
        if (!(isdigit((unsigned char)*p) || *p == '.' ||
              ((*p == '+' || *p == '-') && (isdigit((unsigned char)p[1]) || p[1] == '.')))) {
            ++p;
            continue;
        }
        char *end = NULL;
        errno = 0;
        double raw = strtod(p, &end);
        if (end == p) { ++p; continue; }
        if (errno == ERANGE || !isfinite(raw)) { p = end; continue; }
        double value = 0.0;
        int kind = 0;
        if (quantity_after(end, raw, &value, &kind)) {
            if (kind == 1 && nv < vcap) volts[nv++] = value;
            if (kind == 2 && nr < rcap) ohms[nr++] = value;
        }
        p = end;
    }
    return (nv << 16) | nr;
}

int hwc_intent_parse(const char *text, HwcIntent *out,
                     char *error, size_t error_capacity) {
    if (!text || !out) {
        set_error(error, error_capacity, "prompt and output must be non-null");
        return 0;
    }
    memset(out, 0, sizeof(*out));
    (void)snprintf(out->package, sizeof(out->package), "0603");

    if (!contains_casefold(text, "resistor")) {
        set_error(error, error_capacity,
                  "supported V1 intents: 'make resistor [value]' and 'make resistor divider <Vin> to <Vout> using <Rtop> and <Rbottom>'");
        return 0;
    }

    double volts[4] = {0};
    double resistors[8] = {0};
    size_t counts = extract_quantities(text, volts, 4, resistors, 8);
    size_t voltage_count = counts >> 16;
    size_t resistor_count = counts & 0xFFFFu;

    if (contains_casefold(text, "divider")) {
        if (voltage_count < 2 || resistor_count < 2) {
            set_error(error, error_capacity,
                      "divider needs two voltages and two resistor values, e.g. 'make resistor divider 5 V to 3.3 V using 10k ohm and 20k ohm'");
            return 0;
        }
        if (!(volts[0] > 0.0 && volts[1] > 0.0 && volts[1] < volts[0] &&
              resistors[0] > 0.0 && resistors[1] > 0.0)) {
            set_error(error, error_capacity,
                      "divider values must be positive and target voltage must be below input voltage");
            return 0;
        }
        out->kind = HWC_INTENT_RESISTOR_DIVIDER;
        out->input_voltage_v = volts[0];
        out->target_voltage_v = volts[1];
        out->r_top_ohm = resistors[0];
        out->r_bottom_ohm = resistors[1];
        return 1;
    }

    if (resistor_count > 0) {
        if (!(resistors[0] > 0.0) || !isfinite(resistors[0])) {
            set_error(error, error_capacity, "resistor value must be finite and positive");
            return 0;
        }
        out->resistor_ohm = resistors[0];
    } else {
        out->resistor_ohm = 10000.0;
        out->used_default_value = true;
    }
    out->kind = HWC_INTENT_MAKE_RESISTOR;
    return 1;
}

const char *hwc_intent_kind_name(HwcIntentKind kind) {
    switch (kind) {
        case HWC_INTENT_MAKE_RESISTOR: return "make_resistor";
        case HWC_INTENT_RESISTOR_DIVIDER: return "resistor_divider";
        default: return "invalid";
    }
}

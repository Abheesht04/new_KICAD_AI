#include "model_registry.h"

#include "component.h"
#include "vec.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int resistor_validate(
    const Component *component,
    DiagnosticList *out
)
{
    const Range range = component->model.data.resistor.resistance_ohm;

    if (range.minimum <= 0.0 || range.maximum <= 0.0) {
        return diagnostic_add(
            out,
            DIAGNOSTIC_ERROR,
            DIAG_TARGET_COMPONENT,
            DIAGNOSTIC_INVALID_ID,
            "resistor_model",
            "%s has a non-positive resistance range",
            component->reference
        ) == 0 ? 1 : -1;
    }

    return 0;
}

static int capacitor_validate(
    const Component *component,
    DiagnosticList *out
)
{
    const Range capacitance = component->model.data.capacitor.capacitance_f;
    const double voltage = component->model.data.capacitor.voltage_rating_v;

    int errors = 0;

    if (capacitance.minimum <= 0.0 || capacitance.maximum <= 0.0) {
        if (diagnostic_add(
                out,
                DIAGNOSTIC_ERROR,
                DIAG_TARGET_COMPONENT,
                DIAGNOSTIC_INVALID_ID,
                "capacitor_model",
                "%s has a non-positive capacitance range",
                component->reference) != 0) {
            return -1;
        }
        errors++;
    }

    if (voltage <= 0.0) {
        if (diagnostic_add(
                out,
                DIAGNOSTIC_ERROR,
                DIAG_TARGET_COMPONENT,
                DIAGNOSTIC_INVALID_ID,
                "capacitor_model",
                "%s has no valid voltage rating",
                component->reference) != 0) {
            return -1;
        }
        errors++;
    }

    return errors;
}

static int mosfet_validate(
    const Component *component,
    DiagnosticList *out
)
{
    const MosfetModel *model =
        &component->model.data.mosfet;

    int errors = 0;

    if (model->max_vds_v <= 0.0) {
        if (diagnostic_add(
                out,
                DIAGNOSTIC_ERROR,
                DIAG_TARGET_COMPONENT,
                DIAGNOSTIC_INVALID_ID,
                "mosfet_model",
                "%s has invalid maximum VDS",
                component->reference) != 0) {
            return -1;
        }
        errors++;
    }

    if (model->max_id_a <= 0.0) {
        if (diagnostic_add(
                out,
                DIAGNOSTIC_ERROR,
                DIAG_TARGET_COMPONENT,
                DIAGNOSTIC_INVALID_ID,
                "mosfet_model",
                "%s has invalid maximum ID",
                component->reference) != 0) {
            return -1;
        }
        errors++;
    }

    return errors;
}

static const ModelOps RESISTOR_OPS = {
    "resistor",
    COMPONENT_RESISTOR,
    resistor_validate,
    NULL
};

static const ModelOps CAPACITOR_OPS = {
    "capacitor",
    COMPONENT_CAPACITOR,
    capacitor_validate,
    NULL
};

static const ModelOps MOSFET_OPS = {
    "mosfet",
    COMPONENT_MOSFET,
    mosfet_validate,
    NULL
};

void model_registry_init(ModelRegistry *registry)
{
    if (!registry) {
        return;
    }

    memset(registry, 0, sizeof(*registry));
}

void model_registry_free(ModelRegistry *registry)
{
    if (!registry) {
        return;
    }

    vec_free(registry->items);
    vec_free(registry->device_models);
}

int model_registry_register(
    ModelRegistry *registry,
    const ModelOps *ops
)
{
    if (!registry || !ops || !ops->name) {
        return -1;
    }

    if (model_registry_find(registry, ops->kind)) {
        return -1;
    }

    return vec_push(registry->items, ops);
}

const ModelOps *model_registry_find(
    const ModelRegistry *registry,
    ComponentKind kind
)
{
    if (!registry) {
        return NULL;
    }

    for (size_t i = 0; i < vec_len(registry->items); ++i) {
        const ModelOps *ops = registry->items[i];

        if (ops && ops->kind == kind) {
            return ops;
        }
    }

    return NULL;
}

int model_registry_register_builtins(
    ModelRegistry *registry
)
{
    if (!registry) {
        return -1;
    }

    if (model_registry_register(registry, &RESISTOR_OPS) != 0) {
        return -1;
    }

    if (model_registry_register(registry, &CAPACITOR_OPS) != 0) {
        return -1;
    }

    if (model_registry_register(registry, &MOSFET_OPS) != 0) {
        return -1;
    }

    return 0;
}

/* --------------------------------------------------------------------------
 * Device model contract v1
 * --------------------------------------------------------------------------
 */

static int fail(char *err, size_t cap, const char *fmt, ...)
{
    va_list ap;
    if (err && cap) {
        va_start(ap, fmt);
        vsnprintf(err, cap, fmt, ap);
        va_end(ap);
    }
    return 0;
}

static int identifier_ok(const char *s)
{
    if (!s || !*s || strlen(s) > 63) {
        return 0;
    }
    for (; *s; ++s) {
        if (!isalnum((unsigned char)*s) && *s != '_' && *s != '.' && *s != '-') {
            return 0;
        }
    }
    return 1;
}

const char *device_unit_symbol(DeviceUnit unit)
{
    switch (unit) {
        case DEVICE_UNIT_DIMENSIONLESS: return "1";
        case DEVICE_UNIT_OHM:           return "ohm";
        case DEVICE_UNIT_FARAD:         return "F";
        case DEVICE_UNIT_HENRY:         return "H";
        case DEVICE_UNIT_VOLT:          return "V";
        case DEVICE_UNIT_AMPERE:        return "A";
        case DEVICE_UNIT_KELVIN:        return "K";
        default:                        return NULL;
    }
}

DeviceUnit device_unit_parse(const char *text)
{
    for (int u = DEVICE_UNIT_DIMENSIONLESS; u <= DEVICE_UNIT_KELVIN; ++u) {
        const char *symbol = device_unit_symbol((DeviceUnit)u);
        if (text && symbol && strcmp(text, symbol) == 0) {
            return (DeviceUnit)u;
        }
    }
    return DEVICE_UNIT_INVALID;
}

const char *device_analysis_name(DeviceAnalysis analysis)
{
    switch (analysis) {
        case DEVICE_ANALYSIS_DC:        return "dc";
        case DEVICE_ANALYSIS_TRANSIENT: return "transient";
        case DEVICE_ANALYSIS_AC:        return "ac";
        default:                        return "unknown";
    }
}

int device_model_validate_spec(const DeviceModelSpec *spec, char *err, size_t cap)
{
    if (!spec) {
        return fail(err, cap, "null model spec");
    }
    if (!identifier_ok(spec->id)) {
        return fail(err, cap, "model id must be 1-63 characters of [A-Za-z0-9_.-]");
    }
    if (spec->version == 0) {
        return fail(err, cap, "%s: model version must be >= 1", spec->id);
    }
    if (!identifier_ok(spec->family) || !spec->summary || !spec->assumptions) {
        return fail(err, cap, "%s: family, summary and assumptions are required", spec->id);
    }
    if (spec->terminal_count == 0 || spec->terminal_count > DEVICE_MODEL_MAX_TERMINALS || !spec->terminals) {
        return fail(err, cap, "%s: 1..%d terminals required", spec->id, DEVICE_MODEL_MAX_TERMINALS);
    }
    for (size_t i = 0; i < spec->terminal_count; ++i) {
        if (!identifier_ok(spec->terminals[i].name) || !spec->terminals[i].role) {
            return fail(err, cap, "%s: terminal %zu needs a name and role", spec->id, i);
        }
        for (size_t j = 0; j < i; ++j) {
            if (strcmp(spec->terminals[i].name, spec->terminals[j].name) == 0) {
                return fail(err, cap, "%s: duplicate terminal '%s'", spec->id, spec->terminals[i].name);
            }
        }
    }
    if (spec->param_count > DEVICE_MODEL_MAX_PARAMS || (spec->param_count && !spec->params)) {
        return fail(err, cap, "%s: at most %d parameters", spec->id, DEVICE_MODEL_MAX_PARAMS);
    }
    for (size_t i = 0; i < spec->param_count; ++i) {
        const DeviceParam *p = &spec->params[i];
        if (!identifier_ok(p->name) || !p->meaning) {
            return fail(err, cap, "%s: parameter %zu needs a name and meaning", spec->id, i);
        }
        if (!device_unit_symbol(p->unit)) {
            return fail(err, cap, "%s.%s: unit is not a known SI unit", spec->id, p->name);
        }
        if (!isfinite(p->minimum) || !isfinite(p->maximum) || p->minimum > p->maximum) {
            return fail(err, cap, "%s.%s: range must be finite with minimum <= maximum", spec->id, p->name);
        }
        if (p->has_default) {
            double d = p->default_value;
            if (!isfinite(d) || d > p->maximum || d < p->minimum || (p->min_exclusive && d == p->minimum)) {
                return fail(err, cap, "%s.%s: default lies outside its range", spec->id, p->name);
            }
        }
        for (size_t j = 0; j < i; ++j) {
            if (strcmp(p->name, spec->params[j].name) == 0) {
                return fail(err, cap, "%s: duplicate parameter '%s'", spec->id, p->name);
            }
        }
    }
    if (spec->primary_param < -1 || spec->primary_param >= (int)spec->param_count) {
        return fail(err, cap, "%s: primary_param out of range", spec->id);
    }
    if (spec->capabilities & (DEVICE_CAP_AC | DEVICE_CAP_TEMPERATURE)) {
        return fail(err, cap, "%s: AC and temperature capabilities have no hook in contract v1", spec->id);
    }
    if (spec->capabilities & ~(unsigned)(DEVICE_CAP_DC | DEVICE_CAP_TRANSIENT)) {
        return fail(err, cap, "%s: unknown capability bits", spec->id);
    }
    if (!(spec->capabilities & DEVICE_CAP_DC) != !spec->stamp_dc) {
        return fail(err, cap, "%s: DC capability and stamp_dc must be declared together", spec->id);
    }
    if (!(spec->capabilities & DEVICE_CAP_TRANSIENT) != !spec->stamp_transient) {
        return fail(err, cap, "%s: transient capability and stamp_transient must be declared together", spec->id);
    }
    if ((spec->capabilities & DEVICE_CAP_TRANSIENT) && !(spec->capabilities & DEVICE_CAP_DC)) {
        return fail(err, cap, "%s: transient requires DC (the initial operating point)", spec->id);
    }
    if (spec->state_count > DEVICE_MODEL_MAX_STATE ||
        (spec->state_count > 0) != (spec->commit_transient != NULL) ||
        (spec->state_count > 0 && !(spec->capabilities & DEVICE_CAP_TRANSIENT))) {
        return fail(err, cap, "%s: transient state needs commit_transient and the transient capability", spec->id);
    }
    return 1;
}

int model_registry_register_device(ModelRegistry *registry, const DeviceModelSpec *spec,
                                   char *err, size_t cap)
{
    if (!registry) {
        return fail(err, cap, "null registry");
    }
    if (!device_model_validate_spec(spec, err, cap)) {
        return 0;
    }
    if (model_registry_find_device(registry, spec->id)) {
        return fail(err, cap, "duplicate model id '%s'", spec->id);
    }
    if (vec_push(registry->device_models, spec) != 0) {
        return fail(err, cap, "out of memory");
    }
    return 1;
}

const DeviceModelSpec *model_registry_find_device(const ModelRegistry *registry, const char *id)
{
    if (!registry || !id) {
        return NULL;
    }
    for (size_t i = 0; i < vec_len(registry->device_models); ++i) {
        if (strcmp(registry->device_models[i]->id, id) == 0) {
            return registry->device_models[i];
        }
    }
    return NULL;
}

size_t model_registry_device_count(const ModelRegistry *registry)
{
    return registry ? vec_len(registry->device_models) : 0;
}

const DeviceModelSpec *model_registry_device_at(const ModelRegistry *registry, size_t index)
{
    return index < model_registry_device_count(registry) ? registry->device_models[index] : NULL;
}

int device_model_resolve_params(const DeviceModelSpec *spec, const double *values,
                                const int *present, double *resolved, char *err, size_t cap)
{
    for (size_t i = 0; i < spec->param_count; ++i) {
        const DeviceParam *p = &spec->params[i];
        double v;
        if (present && present[i]) {
            v = values[i];
        } else if (p->has_default) {
            v = p->default_value;
        } else {
            return fail(err, cap, "%s requires parameter '%s' (%s)", spec->id, p->name,
                        device_unit_symbol(p->unit));
        }
        if (!isfinite(v)) {
            return fail(err, cap, "%s.%s must be finite", spec->id, p->name);
        }
        if (v > p->maximum || v < p->minimum || (p->min_exclusive && v == p->minimum)) {
            return fail(err, cap, "%s.%s=%g %s is outside %s%g..%g", spec->id, p->name, v,
                        device_unit_symbol(p->unit), p->min_exclusive ? "(" : "[", p->minimum, p->maximum);
        }
        resolved[i] = v;
    }
    return 1;
}

int device_model_supports(const DeviceModelSpec *spec, DeviceAnalysis analysis)
{
    if (!spec) {
        return 0;
    }
    switch (analysis) {
        case DEVICE_ANALYSIS_DC:        return (spec->capabilities & DEVICE_CAP_DC) != 0;
        case DEVICE_ANALYSIS_TRANSIENT: return (spec->capabilities & DEVICE_CAP_TRANSIENT) != 0;
        case DEVICE_ANALYSIS_AC:        return (spec->capabilities & DEVICE_CAP_AC) != 0;
        default:                        return 0;
    }
}

int device_model_terminal_index(const DeviceModelSpec *spec, const char *name)
{
    for (size_t i = 0; spec && name && i < spec->terminal_count; ++i) {
        if (strcmp(spec->terminals[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

int device_terminal_accepts_pin_name(const DeviceTerminal *terminal, const char *name)
{
    if (!terminal->pin_names) {
        return 1;
    }
    for (const char *p = terminal->pin_names; *p;) {
        size_t n = strcspn(p, "|");
        if (strlen(name) == n) {
            size_t k = 0;
            while (k < n && toupper((unsigned char)p[k]) == toupper((unsigned char)name[k])) {
                ++k;
            }
            if (k == n) {
                return 1;
            }
        }
        p += n;
        if (*p == '|') {
            ++p;
        }
    }
    return 0;
}

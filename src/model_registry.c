#include "model_registry.h"

#include "component.h"
#include "vec.h"

#include <math.h>
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

#include "design.h"

#include <string.h>

static void copy_fixed_string(
    char *destination,
    size_t destination_size,
    const char *source
)
{
    if (!destination || destination_size == 0) {
        return;
    }

    if (!source) {
        destination[0] = '\0';
        return;
    }

    size_t length = strlen(source);
    if (length >= destination_size) {
        length = destination_size - 1;
    }
    memcpy(destination, source, length);
    destination[length] = '\0';
}

static const char *design_intern(
    Design *design,
    const char *text
)
{
    if (!design || !text) {
        return NULL;
    }

    return intern_string(&design->interner, text);
}

int design_init(Design *design)
{
    if (!design) {
        return -1;
    }

    memset(design, 0, sizeof(*design));

    if (interner_init(&design->interner, 16) != 0) {
        return -1;
    }

    model_registry_init(&design->models);

    if (model_registry_register_builtins(&design->models) != 0) {
        model_registry_free(&design->models);
        interner_destroy(&design->interner);
        return -1;
    }

    constraint_list_init(&design->constraints);

    return 0;
}

void design_free(Design *design)
{
    if (!design) {
        return;
    }

    for (size_t i = 0; i < vec_len(design->components); ++i) {
        component_free(&design->components[i]);
    }

    for (size_t i = 0; i < vec_len(design->nets); ++i) {
        net_free(&design->nets[i]);
    }

    vec_free(design->components);
    vec_free(design->nets);
    constraint_list_free(&design->constraints);

    model_registry_free(&design->models);
    interner_destroy(&design->interner);

    memset(design, 0, sizeof(*design));
}

int design_add_component(
    Design *design,
    const Component *source,
    uint32_t *component_id
)
{
    if (!design || !source) {
        return -1;
    }

    Component stored;
    component_init(&stored);

    copy_fixed_string(
        stored.reference,
        sizeof(stored.reference),
        source->reference
    );

    copy_fixed_string(
        stored.manufacturer_part_number,
        sizeof(stored.manufacturer_part_number),
        source->manufacturer_part_number
    );

    stored.manufacturer = design_intern(design, source->manufacturer);
    stored.package = design_intern(design, source->package);
    stored.footprint = design_intern(design, source->footprint);
    stored.symbol = design_intern(design, source->symbol);

    stored.value = source->value;
    stored.value_range = source->value_range;
    stored.model = source->model;
    stored.electrical = source->electrical;
    stored.thermal = source->thermal;
    stored.dimensions = source->dimensions;

    for (size_t i = 0; i < vec_len(source->pins); ++i) {
        const Pin *source_pin = &source->pins[i];

        const char *interned_name =
            design_intern(design, source_pin->name);

        if (component_add_pin(
                &stored,
                source_pin->number,
                interned_name) != 0) {
            component_free(&stored);
            return -1;
        }
    }

    uint32_t id = (uint32_t)vec_len(design->components);

    if (vec_push(design->components, stored) != 0) {
        component_free(&stored);
        return -1;
    }

    if (component_id) {
        *component_id = id;
    }

    return 0;
}

Component *design_get_component(
    Design *design,
    uint32_t component_id
)
{
    if (!design ||
        component_id >= vec_len(design->components)) {
        return NULL;
    }

    return &design->components[component_id];
}

const Component *design_get_component_const(
    const Design *design,
    uint32_t component_id
)
{
    if (!design ||
        component_id >= vec_len(design->components)) {
        return NULL;
    }

    return &design->components[component_id];
}

int design_add_net(
    Design *design,
    const char *name,
    uint32_t *net_id
)
{
    if (!design) {
        return -1;
    }

    Net stored;
    net_init(&stored);

    stored.id = (uint32_t)vec_len(design->nets);
    stored.name = design_intern(design, name);

    if (name && !stored.name) {
        net_free(&stored);
        return -1;
    }

    if (vec_push(design->nets, stored) != 0) {
        net_free(&stored);
        return -1;
    }

    if (net_id) {
        *net_id = stored.id;
    }

    return 0;
}

Net *design_get_net(
    Design *design,
    uint32_t net_id
)
{
    if (!design ||
        net_id >= vec_len(design->nets)) {
        return NULL;
    }

    return &design->nets[net_id];
}

const Net *design_get_net_const(
    const Design *design,
    uint32_t net_id
)
{
    if (!design ||
        net_id >= vec_len(design->nets)) {
        return NULL;
    }

    return &design->nets[net_id];
}

int design_connect_pin(
    Design *design,
    uint32_t component_id,
    uint16_t pin_number,
    uint32_t net_id
)
{
    if (!design) {
        return -1;
    }

    Component *component =
        design_get_component(design, component_id);

    Net *net =
        design_get_net(design, net_id);

    if (!component || !net) {
        return -1;
    }

    Pin *pin =
        component_find_pin(component, pin_number);

    if (!pin || pin->net_id != -1) {
        return -1;
    }

    if (net_add_connection(
            net,
            component_id,
            pin_number) != 0) {
        return -1;
    }

    pin->net_id = (int32_t)net_id;

    return 0;
}

int design_add_constraint(
    Design *design,
    Constraint constraint
)
{
    if (!design) {
        return -1;
    }

    if (constraint.name) {
        constraint.name =
            design_intern(design, constraint.name);

        if (!constraint.name) {
            return -1;
        }
    }

    return constraint_add(
        &design->constraints,
        constraint
    );
}

int design_find_component(
    const Design *design,
    const char *reference,
    uint32_t *component_id
)
{
    if (!design || !reference) {
        return -1;
    }

    for (size_t i = 0; i < vec_len(design->components); ++i) {
        const Component *component =
            &design->components[i];

        if (strcmp(
                component->reference,
                reference) == 0) {

            if (component_id) {
                *component_id = (uint32_t)i;
            }

            return 0;
        }
    }

    return -1;
}

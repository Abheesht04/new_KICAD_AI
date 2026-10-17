#include "component.h"

#include <string.h>

void component_init(Component *component)
{
    if (!component) {
        return;
    }

    memset(component, 0, sizeof(*component));

    component->pins = NULL;
    component->model.kind = COMPONENT_GENERIC;
    component->value_range = range_exact(0.0);
}

void component_free(Component *component)
{
    if (!component) {
        return;
    }

    vec_free(component->pins);
}

int component_add_pin(Component *component, uint16_t number, const char *name)
{
    if (!component) {
        return -1;
    }

    if (component_find_pin(component, number)) {
        return -1;
    }

    Pin pin;

    pin.number = number;
    pin.name = name;
    pin.net_id = -1;

    return vec_push(component->pins, pin);
}

Pin *component_find_pin(Component *component, uint16_t number)
{
    if (!component) {
        return NULL;
    }

    for (size_t i = 0; i < vec_len(component->pins); ++i) {
        Pin *pin = &component->pins[i];

        if (pin->number == number) {
            return pin;
        }
    }

    return NULL;
}

const Pin *component_find_pin_const(const Component *component, uint16_t number)
{
    if (!component) {
        return NULL;
    }

    for (size_t i = 0; i < vec_len(component->pins); ++i) {
        const Pin *pin = &component->pins[i];

        if (pin->number == number) {
            return pin;
        }
    }

    return NULL;
}

void component_set_value(
    Component *component,
    double nominal,
    double tolerance_percent
)
{
    if (!component) {
        return;
    }

    component->value = nominal;
    component->value_range = range_percent(
        nominal,
        tolerance_percent
    );
}

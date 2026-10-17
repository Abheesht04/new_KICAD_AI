#ifndef COMPONENT_H
#define COMPONENT_H

#include <stddef.h>
#include <stdint.h>

#include "vec.h"
#include "range.h"
#include "component_model.h"

typedef struct {
    double max_voltage;
    double max_current;
    double max_power;
} ElectricalLimits;

typedef struct {
    double max_junction_temp;
    double thermal_resistance;
} ThermalLimits;

typedef struct {
    double width_mm;
    double length_mm;
    double height_mm;
} PhysicalDimensions;

typedef struct {
    uint16_t number;
    const char *name;    /* interned for Design-owned components */
    int32_t net_id;      /* -1 = unconnected */
} Pin;

typedef struct Component {
    char reference[16];
    char manufacturer_part_number[64];

    const char *manufacturer;
    const char *package;
    const char *footprint;
    const char *symbol;

    /* Nominal scalar value, retained for compatibility and display. */
    double value;

    /* Optional engineering range for tolerance-aware use. */
    Range value_range;

    ComponentModel model;

    Pin *pins;

    ElectricalLimits electrical;
    ThermalLimits thermal;
    PhysicalDimensions dimensions;
} Component;

void component_init(Component *component);
void component_free(Component *component);

int component_add_pin(Component *component, uint16_t number, const char *name);

Pin *component_find_pin(Component *component, uint16_t number);
const Pin *component_find_pin_const(const Component *component, uint16_t number);

void component_set_value(
    Component *component,
    double nominal,
    double tolerance_percent
);

#endif

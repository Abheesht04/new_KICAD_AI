#ifndef DESIGN_H
#define DESIGN_H

#include <stddef.h>
#include <stdint.h>

#include "component.h"
#include "constraint.h"
#include "intern.h"
#include "net.h"
#include "vec.h"
#include "model_registry.h"

typedef struct Design {
    Component *components;
    ConstraintList constraints;
    Net *nets;

    Interner interner;
    ModelRegistry models;
} Design;

int design_init(Design *design);
void design_free(Design *design);

int design_add_component(
    Design *design,
    const Component *source,
    uint32_t *component_id
);

Component *design_get_component(Design *design, uint32_t component_id);
const Component *design_get_component_const(
    const Design *design,
    uint32_t component_id
);

int design_add_net(
    Design *design,
    const char *name,
    uint32_t *net_id
);

Net *design_get_net(Design *design, uint32_t net_id);
const Net *design_get_net_const(
    const Design *design,
    uint32_t net_id
);

int design_connect_pin(
    Design *design,
    uint32_t component_id,
    uint16_t pin_number,
    uint32_t net_id
);

int design_add_constraint(
    Design *design,
    Constraint constraint
);

int design_find_component(
    const Design *design,
    const char *reference,
    uint32_t *component_id
);

#endif

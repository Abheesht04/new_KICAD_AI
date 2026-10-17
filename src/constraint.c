#include "constraint.h"

#include <string.h>

void constraint_list_init(ConstraintList *list)
{
    if (!list) {
        return;
    }

    memset(list, 0, sizeof(*list));
}

void constraint_list_free(ConstraintList *list)
{
    if (!list) {
        return;
    }

    vec_free(list->items);
}

int constraint_add(
    ConstraintList *list,
    Constraint constraint
)
{
    if (!list) {
        return -1;
    }

    constraint.id = (uint32_t)vec_len(list->items);

    return vec_push(list->items, constraint);
}

const Constraint *constraint_get(
    const ConstraintList *list,
    size_t index
)
{
    if (!list || index >= vec_len(list->items)) {
        return NULL;
    }

    return &list->items[index];
}

size_t constraint_count(const ConstraintList *list)
{
    if (!list) {
        return 0;
    }

    return vec_len(list->items);
}

const char *constraint_type_name(ConstraintType type)
{
    switch (type) {
        case CONSTRAINT_VOLTAGE:            return "voltage";
        case CONSTRAINT_CURRENT:            return "current";
        case CONSTRAINT_POWER:              return "power";
        case CONSTRAINT_TEMPERATURE:        return "temperature";
        case CONSTRAINT_TRACE_WIDTH:        return "trace_width";
        case CONSTRAINT_CLEARANCE:          return "clearance";
        case CONSTRAINT_COMPONENT_HEIGHT:   return "component_height";
        case CONSTRAINT_COMPONENT_WIDTH:    return "component_width";
        case CONSTRAINT_COMPONENT_LENGTH:   return "component_length";
        case CONSTRAINT_KEEP_OUT:           return "keep_out";
        case CONSTRAINT_VALUE_RANGE:        return "value_range";
        default:                            return "unknown";
    }
}

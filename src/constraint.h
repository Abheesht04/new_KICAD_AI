#ifndef CONSTRAINT_H
#define CONSTRAINT_H

#include <stddef.h>
#include <stdint.h>

#include "range.h"
#include "vec.h"

#define CONSTRAINT_GLOBAL_ID UINT32_MAX

typedef enum {
    CONSTRAINT_VOLTAGE,
    CONSTRAINT_CURRENT,
    CONSTRAINT_POWER,
    CONSTRAINT_TEMPERATURE,
    CONSTRAINT_TRACE_WIDTH,
    CONSTRAINT_CLEARANCE,
    CONSTRAINT_COMPONENT_HEIGHT,
    CONSTRAINT_COMPONENT_WIDTH,
    CONSTRAINT_COMPONENT_LENGTH,
    CONSTRAINT_KEEP_OUT,
    CONSTRAINT_VALUE_RANGE
} ConstraintType;

typedef struct {
    uint32_t id;
    ConstraintType type;
    uint32_t target_id;
    const char *name;
    Range range;
} Constraint;

typedef struct {
    Constraint *items;
} ConstraintList;

void constraint_list_init(ConstraintList *list);
void constraint_list_free(ConstraintList *list);

int constraint_add(
    ConstraintList *list,
    Constraint constraint
);

const Constraint *constraint_get(
    const ConstraintList *list,
    size_t index
);

size_t constraint_count(const ConstraintList *list);

const char *constraint_type_name(ConstraintType type);

#endif

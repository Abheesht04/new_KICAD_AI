#ifndef MODEL_REGISTRY_H
#define MODEL_REGISTRY_H

#include <stddef.h>

#include "component_model.h"
#include "constraint.h"
#include "diagnostic.h"

struct Component;

typedef int (*ModelValidateFn)(const struct Component *component,
                               DiagnosticList *out);

typedef int (*ModelDeriveConstraintsFn)(const struct Component *component,
                                        ConstraintList *out);

typedef struct {
  const char *name;
  ComponentKind kind;
  ModelValidateFn validate;
  ModelDeriveConstraintsFn derive_constraints;
} ModelOps;

typedef struct {
  const ModelOps **items;
} ModelRegistry;

void model_registry_init(ModelRegistry *registry);
void model_registry_free(ModelRegistry *registry);

int model_registry_register(ModelRegistry *registry, const ModelOps *ops);

const ModelOps *model_registry_find(const ModelRegistry *registry,
                                    ComponentKind kind);

int model_registry_register_builtins(ModelRegistry *registry);

#endif

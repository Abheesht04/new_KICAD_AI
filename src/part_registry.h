#ifndef PART_REGISTRY_H
#define PART_REGISTRY_H

#include <stdbool.h>

#include "part_provider.h"

typedef struct {
  PartProvider *providers;
} PartRegistry;

int part_registry_init(PartRegistry *registry);

void part_registry_free(PartRegistry *registry);

int part_registry_register(PartRegistry *registry, PartProvider provider);

bool part_registry_find(const PartRegistry *registry,
                        const PartRequest *request, PartResult *out);

#endif

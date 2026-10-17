#ifndef PART_PROVIDER_H
#define PART_PROVIDER_H

#include <stddef.h>

#include "PartIdentity.h"
#include "component.h"
#include "component_model.h"
#include "range.h"

/*
 * A request describes engineering requirements.
 * It does not name a database and does not require a specific vendor.
 */

typedef struct {
  const char *role;
  Range value;
  const char *required_package;

  double min_voltage_v;
  double min_current_a;
  double min_power_w;

  const PartIdentity *exact;
} PartRequest;

typedef struct {
  const char *manufacturer;
  const char *manufacturer_part_number;
  const char *package;
  const char *footprint;
  const char *symbol;

  ComponentModel model;
  ElectricalLimits electrical;
  ThermalLimits thermal;
  PhysicalDimensions dimensions;
} PartResult;

typedef int (*PartResolveFn)(void *context, const PartRequest *request,
                             PartResult *result);

typedef struct {
  const char *name;
  void *context;
  PartResolveFn resolve;
} PartProvider;

int part_provider_resolve(const PartProvider *provider,
                          const PartRequest *request, PartResult *result);

#endif

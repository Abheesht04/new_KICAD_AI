// kicad_generic_provider.c

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "component_model.h"
/* Ensure the header defining PartRequest, PartResult, and PartProvider is
 * included */
#include "part_provider.h"

typedef struct {
  ComponentKind kind;
  const char *package;
  const char *symbol;
  const char *footprint;
} GenericEntry;

/*
 * Static part lookup table for generic passive component packages.
 * Values (e.g. 10k vs 4.7k) are handled via component value attributes,
 * keeping table size minimal.
 */
static const GenericEntry GENERIC_TABLE[] = {
    {COMPONENT_RESISTOR, "0402", "Device:R", "Resistor_SMD:R_0402_1005Metric"},
    {COMPONENT_RESISTOR, "0603", "Device:R", "Resistor_SMD:R_0603_1608Metric"},
    {COMPONENT_RESISTOR, "0805", "Device:R", "Resistor_SMD:R_0805_2012Metric"},
    {COMPONENT_CAPACITOR, "0402", "Device:C",
     "Capacitor_SMD:C_0402_1005Metric"},
    {COMPONENT_CAPACITOR, "0603", "Device:C",
     "Capacitor_SMD:C_0603_1608Metric"},
    {COMPONENT_CAPACITOR, "0805", "Device:C",
     "Capacitor_SMD:C_0805_2012Metric"},
    {COMPONENT_INDUCTOR, "0603", "Device:L", "Inductor_SMD:L_0603_1608Metric"},
    {COMPONENT_INDUCTOR, "0805", "Device:L", "Inductor_SMD:L_0805_2012Metric"},
    {COMPONENT_LED, "0603", "Device:LED", "LED_SMD:LED_0603_1608Metric"},
    {COMPONENT_DIODE, "SOD-123", "Device:D", "Diode_SMD:D_SOD-123"},
};

static int kicad_generic_resolve(void *context, const PartRequest *req,
                                 PartResult *out) {
  (void)context;

  if (!req || !out) {
    return false;
  }

  if (req->exact != NULL) {
    return false;
  }

  size_t n = sizeof(GENERIC_TABLE) / sizeof(GENERIC_TABLE[0]);
  for (size_t i = 0; i < n; i++) {
    const GenericEntry *e = &GENERIC_TABLE[i];

    // 1. Convert the table's enum (e.g., COMPONENT_RESISTOR) to a string (e.g.,
    // "resistor")
    const char *entry_role_str = component_kind_name(e->kind);

    // 2. Safely compare BOTH the role string and the package string
    if (req->role != NULL && strcmp(req->role, entry_role_str) == 0 &&
        req->required_package != NULL &&
        strcmp(req->required_package, e->package) == 0) {

      out->manufacturer = NULL;
      out->manufacturer_part_number = NULL;
      out->symbol = e->symbol;
      out->footprint = e->footprint;
      return true;
    }
  }

  return false;
}

PartProvider kicad_generic_provider(void) {
  return (PartProvider){.name = "kicad_generic",
                        .context = NULL,
                        .resolve = kicad_generic_resolve};
}

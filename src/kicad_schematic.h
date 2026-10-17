#ifndef HWC_KICAD_SCHEMATIC_H
#define HWC_KICAD_SCHEMATIC_H

#include <stddef.h>

#include "design.h"
#include "nlp_rules.h"

/* Emit a minimal KiCad 9-compatible schematic from the existing deterministic
 * Design. V1 export intentionally supports one or two Device:R components.
 * Unconnected pins stay unconnected; export never invents circuit topology. */
int hwc_kicad_schematic_write(const char *path, const Design *design,
                              const HwcIntent *intent,
                              char *error, size_t error_capacity);

#endif

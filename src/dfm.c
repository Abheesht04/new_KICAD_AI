#include "dfm.h"

#include "component.h"
#include "design.h"
#include "vec.h"

#include <string.h>

// in dfm.c

DfmProfile profile_standard_default(void) {
  // Rigid 2-4 layer board, 1oz copper -- typical hobbyist/prosumer default
  return (DfmProfile){
      .layer_count = 2,
      .copper_weight_oz = 1.0,
      .min_trace_width_mm = 0.127, // 5 mil, JLCPCB 1-2 layer minimum
      .min_clearance_mm = 0.127,   // pad-to-pad, no hole, different nets
      .min_via_diameter_mm = 0.5,  // single/double layer minimum
      .min_drill_mm = 0.3,
      .min_annular_ring_mm = 0.13,     // 1oz copper
      .board_edge_clearance_mm = 0.25, // outer layer copper-to-edge
      .max_component_height_mm = 1.6};
}

DfmProfile profile_wearable_default(void) {
  // Flex/tight-space board -- smartwatch/wearable class, real flex-PCB limits
  return (DfmProfile){
      .layer_count = 2,
      .copper_weight_oz = 0.5,
      .min_trace_width_mm = 0.09, // ~3.5 mil, tighter flex process
      .min_clearance_mm = 0.09,
      .min_via_diameter_mm = 0.35,
      .min_drill_mm = 0.15,
      .min_annular_ring_mm = 0.1, // flex minimum, 0.125 recommended
      .board_edge_clearance_mm = 0.15,
      .max_component_height_mm = 0.8 // tight enclosure constraint
  };
}

static int check_no_floating_pins(const Design *design,
                                  const DfmProfile *profile,
                                  DiagnosticList *out) {
  (void)profile;

  if (!design || !out) {
    return -1;
  }

  int errors = 0;

  for (size_t i = 0; i < vec_len(design->components); ++i) {
    const Component *component = &design->components[i];

    for (size_t p = 0; p < vec_len(component->pins); ++p) {
      const Pin *pin = &component->pins[p];

      if (pin->net_id == -1) {
        if (diagnostic_add(out, DIAGNOSTIC_ERROR, DIAG_TARGET_COMPONENT,
                           (uint32_t)i, "floating_pin",
                           "%s pin %u (%s) is unconnected",
                           component->reference, pin->number,
                           pin->name ? pin->name : "unnamed") != 0) {
          return -1;
        }
        errors++;
      }
    }
  }

  return errors;
}

static int check_missing_footprints(const Design *design,
                                    const DfmProfile *profile,
                                    DiagnosticList *out) {
  (void)profile;

  if (!design || !out) {
    return -1;
  }

  int errors = 0;

  for (size_t i = 0; i < vec_len(design->components); ++i) {
    const Component *component = &design->components[i];

    /* COMPONENT_SOURCE is an ideal analysis excitation in Design, not a
       physical BOM component. It is deliberately excluded from footprint DFM. */
    if (component->model.kind == COMPONENT_SOURCE) {
      continue;
    }

    if (!component->footprint || component->footprint[0] == '\0') {

      if (diagnostic_add(out, DIAGNOSTIC_ERROR, DIAG_TARGET_COMPONENT,
                         (uint32_t)i, "missing_footprint",
                         "%s has no PCB footprint",
                         component->reference) != 0) {
        return -1;
      }

      errors++;
    }
  }

  return errors;
}

static int check_invalid_dimensions(const Design *design,
                                    const DfmProfile *profile,
                                    DiagnosticList *out) {
  if (!design || !profile || !out) {
    return -1;
  }

  if (profile->max_component_height_mm <= 0.0) {
    return 0;
  }

  int errors = 0;

  for (size_t i = 0; i < vec_len(design->components); ++i) {
    const Component *component = &design->components[i];

    if (component->dimensions.height_mm > profile->max_component_height_mm) {

      if (diagnostic_add(
              out, DIAGNOSTIC_ERROR, DIAG_TARGET_COMPONENT, (uint32_t)i,
              "component_height",
              "%s height %.3f mm exceeds manufacturing limit %.3f mm",
              component->reference, component->dimensions.height_mm,
              profile->max_component_height_mm) != 0) {
        return -1;
      }

      errors++;
    }
  }

  return errors;
}

static const DfmRule BUILTIN_RULES[] = {
    {"floating_pin", check_no_floating_pins},
    {"missing_footprint", check_missing_footprints},
    {"component_height", check_invalid_dimensions}};

int dfm_registry_init(DfmRegistry *registry) {
  if (!registry) {
    return -1;
  }

  memset(registry, 0, sizeof(*registry));
  return 0;
}

void dfm_registry_free(DfmRegistry *registry) {
  if (!registry) {
    return;
  }

  vec_free(registry->rules);
}

int dfm_register_rule(DfmRegistry *registry, const DfmRule *rule) {
  if (!registry || !rule || !rule->name || !rule->check) {
    return -1;
  }

  return vec_push(registry->rules, *rule);
}

int dfm_run_all(const DfmRegistry *registry, const Design *design,
                const DfmProfile *profile, DiagnosticList *out) {
  if (!registry || !design || !profile || !out) {
    return -1;
  }

  int total_errors = 0;

  for (size_t i = 0; i < vec_len(registry->rules); ++i) {
    const DfmRule *rule = &registry->rules[i];

    int result = rule->check(design, profile, out);

    if (result < 0) {
      return -1;
    }

    total_errors += result;
  }

  return total_errors;
}

int dfm_register_builtin_rules(DfmRegistry *registry) {
  if (!registry) {
    return -1;
  }

  for (size_t i = 0; i < sizeof(BUILTIN_RULES) / sizeof(BUILTIN_RULES[0]);
       ++i) {
    if (dfm_register_rule(registry, &BUILTIN_RULES[i]) != 0) {
      return -1;
    }
  }

  return 0;
}

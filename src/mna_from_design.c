#include "mna_from_design.h"

#include "node_map.h"

#include <stddef.h>
#include <stdint.h>

static void add_diagnostic(DiagnosticList *diag, DiagnosticSeverity severity,
                           DiagnosticTargetKind target_kind, uint32_t target_id,
                           const char *rule_name, const char *format,
                           const char *argument) {
  if (!diag) {
    return;
  }

  diagnostic_add(diag, severity, target_kind, target_id, rule_name, format,
                 argument);
}

static bool pin_to_mna_node(const NodeMap *map, const Pin *pin, size_t *node) {
  if (!map || !pin || !node) {
    return false;
  }

  /*
   * -1 means the pin has not been connected to a Design net.
   *
   * Important: an unconnected pin is NOT automatically ground.
   */
  if (pin->net_id < 0) {
    return false;
  }

  size_t row = node_map_row(map, (uint32_t)pin->net_id);

  /*
   * node_map_row() returns SIZE_MAX for ground.
   */
  if (row == SIZE_MAX) {
    *node = 0;
  } else {
    /*
     * NodeMap rows are 0-based.
     * MNA nodes are:
     *
     *     0 = ground
     *     1 ... N = actual circuit nodes
     */
    *node = row + 1;
  }

  return true;
}

static bool get_two_terminal_nodes(const NodeMap *map,
                                   const Component *component, size_t *node_a,
                                   size_t *node_b) {
  if (!map || !component || !node_a || !node_b) {
    return false;
  }

  const Pin *pin1 = component_find_pin_const(component, 1);
  const Pin *pin2 = component_find_pin_const(component, 2);

  if (!pin1 || !pin2) {
    return false;
  }

  if (!pin_to_mna_node(map, pin1, node_a)) {
    return false;
  }

  if (!pin_to_mna_node(map, pin2, node_b)) {
    return false;
  }

  return true;
}

static bool add_component_to_mna(const Component *component,
                                 uint32_t component_id, const NodeMap *map,
                                 MnaCircuit *circuit, DiagnosticList *diag) {
  if (!component || !map || !circuit) {
    return false;
  }

  size_t node_a = 0;
  size_t node_b = 0;

  switch (component->model.kind) {

  case COMPONENT_RESISTOR: {
    if (!get_two_terminal_nodes(map, component, &node_a, &node_b)) {

      add_diagnostic(diag, DIAGNOSTIC_ERROR, DIAG_TARGET_COMPONENT,
                     component_id, "mna_connectivity",
                     "%s: resistor requires connected pins 1 and 2",
                     component->reference);

      return false;
    }

    double resistance = component->model.data.resistor.resistance_ohm.nominal;

    if (resistance <= 0.0) {
      add_diagnostic(diag, DIAGNOSTIC_ERROR, DIAG_TARGET_COMPONENT,
                     component_id, "mna_resistance",
                     "%s: resistor resistance must be positive",
                     component->reference);

      return false;
    }

    if (mna_add_resistor(circuit, node_a, node_b, resistance) == 0) {

      add_diagnostic(diag, DIAGNOSTIC_ERROR, DIAG_TARGET_COMPONENT,
                     component_id, "mna_stamp",
                     "%s: failed to add resistor to MNA circuit",
                     component->reference);

      return false;
    }

    return true;
  }

  case COMPONENT_SOURCE: {
    if (!get_two_terminal_nodes(map, component, &node_a, &node_b)) {

      add_diagnostic(diag, DIAGNOSTIC_ERROR, DIAG_TARGET_COMPONENT,
                     component_id, "mna_connectivity",
                     "%s: voltage source requires connected pins 1 and 2",
                     component->reference);

      return false;
    }

    double voltage = component->model.data.source.voltage_dc_v;

    if (mna_add_voltage_source(circuit, node_a, node_b, voltage) == 0) {

      add_diagnostic(diag, DIAGNOSTIC_ERROR, DIAG_TARGET_COMPONENT,
                     component_id, "mna_stamp",
                     "%s: failed to add voltage source to MNA circuit",
                     component->reference);

      return false;
    }

    return true;
  }

  case COMPONENT_DIODE: {
    const Pin *pin1 = component_find_pin_const(component, 1);

    const Pin *pin2 = component_find_pin_const(component, 2);

    if (!pin1 || !pin2) {
      add_diagnostic(diag, DIAGNOSTIC_ERROR, DIAG_TARGET_COMPONENT,
                     component_id, "mna_connectivity",
                     "%s: diode requires connected pins 1 and 2",
                     component->reference);

      return false;
    }

    /*
     * For the KiCad Device:D convention we established:
     *
     *     pin 1 = cathode
     *     pin 2 = anode
     *
     * MNA wants:
     *
     *     node_anode
     *     node_cathode
     */
    size_t cathode = 0;
    size_t anode = 0;

    if (!pin_to_mna_node(map, pin1, &cathode) ||
        !pin_to_mna_node(map, pin2, &anode)) {

      add_diagnostic(diag, DIAGNOSTIC_ERROR, DIAG_TARGET_COMPONENT,
                     component_id, "mna_connectivity",
                     "%s: diode pins 1 and 2 must both be connected",
                     component->reference);

      return false;
    }

    const DiodeModel *model = &component->model.data.diode;

    double saturation_current = model->saturation_current_a;

    double ideality_factor = model->ideality_factor;

    /*
     * These defaults let a diode model participate in MNA
     * even when the higher-level component data has not yet
     * supplied explicit semiconductor parameters.
     */
    if (saturation_current <= 0.0) {
      saturation_current = 1e-14;
    }

    if (ideality_factor <= 0.0) {
      ideality_factor = 1.0;
    }

    if (mna_add_diode(circuit, anode, cathode, saturation_current,
                      ideality_factor, 300.0) == 0) {

      add_diagnostic(diag, DIAGNOSTIC_ERROR, DIAG_TARGET_COMPONENT,
                     component_id, "mna_stamp",
                     "%s: failed to add diode to MNA circuit",
                     component->reference);

      return false;
    }

    return true;
  }

  /*
   * These models are valid Design components, but this first
   * Design -> MNA bridge does not yet know their electrical
   * equations.
   *
   * Fail closed: an omitted active device invalidates the circuit solution.
   * The model system will grow independently of this adapter.
   */
  case COMPONENT_GENERIC:
  case COMPONENT_CAPACITOR:
  case COMPONENT_INDUCTOR:
  case COMPONENT_LED:
  case COMPONENT_BJT:
  case COMPONENT_MOSFET:
  case COMPONENT_CRYSTAL:
  case COMPONENT_IC:
  case COMPONENT_CONNECTOR:
  case COMPONENT_CUSTOM:

    add_diagnostic(
        diag, DIAGNOSTIC_ERROR, DIAG_TARGET_COMPONENT, component_id,
        "mna_unsupported_model",
        "%s: component model is not yet supported by the DC MNA bridge",
        component->reference);

    return false;
  }

  add_diagnostic(diag, DIAGNOSTIC_ERROR, DIAG_TARGET_COMPONENT, component_id,
                 "mna_unknown_model", "%s: unknown component model kind",
                 component->reference);

  return false;
}

bool design_to_mna(const Design *design, uint32_t ground_net_id,
                   MnaCircuit *out, DiagnosticList *diag) {
  if (!design || !out) {
    if (diag) {
      diagnostic_add(diag, DIAGNOSTIC_ERROR, DIAG_TARGET_DESIGN,
                     DIAGNOSTIC_INVALID_ID, "mna_arguments",
                     "design_to_mna received a null argument");
    }

    return false;
  }

  /*
   * Validate that the selected ground net actually exists.
   */
  if (!design_get_net_const(design, ground_net_id)) {
    if (diag) {
      diagnostic_add(diag, DIAGNOSTIC_ERROR, DIAG_TARGET_NET, ground_net_id,
                     "mna_ground", "ground net %u does not exist",
                     ground_net_id);
    }

    return false;
  }

  NodeMap map;

  if (!node_map_build(design, ground_net_id, &map)) {
    if (diag) {
      diagnostic_add(diag, DIAGNOSTIC_ERROR, DIAG_TARGET_DESIGN,
                     DIAGNOSTIC_INVALID_ID, "mna_node_map",
                     "failed to build Design-to-MNA node map");
    }

    return false;
  }

  /*
   * MNA's node_count excludes ground.
   *
   * NodeMap unknown_count is therefore exactly the number of
   * electrical node-voltage unknowns before voltage-source
   * branch unknowns are added.
   */
  mna_circuit_init(out, map.unknown_count);

  /*
   * Current Design IDs are dense and component ID == storage index.
   * We intentionally use the public accessor here rather than
   * reaching into the legacy Vec representation.
   *
   * When the Design storage changes later, this adapter does not
   * need to know about it.
   */
  for (uint32_t component_id = 0;; ++component_id) {

    const Component *component =
        design_get_component_const(design, component_id);

    if (!component) {
      break;
    }

    if (!add_component_to_mna(component, component_id, &map, out, diag)) {

      mna_circuit_free(out);
      node_map_free(&map);
      return false;
    }

    /*
     * UINT32_MAX cannot be a valid component index in any
     * realistically representable Design. Prevent wraparound
     * from turning the terminating loop into an infinite loop.
     */
    if (component_id == UINT32_MAX) {
      break;
    }
  }

  node_map_free(&map);
  return true;
}

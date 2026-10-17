#include "design_validate.h"

#include <ctype.h>
#include <math.h>
#include <string.h>

#include "component.h"
#include "vec.h"

static int equals_case_insensitive(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

static int requires_two_pins(ComponentKind kind) {
    switch (kind) {
        case COMPONENT_RESISTOR:
        case COMPONENT_CAPACITOR:
        case COMPONENT_INDUCTOR:
        case COMPONENT_DIODE:
        case COMPONENT_LED:
        case COMPONENT_SOURCE:
            return 1;
        default:
            return 0;
    }
}

static int net_has_exact_connection(const Net *net, uint32_t component_id,
                                    uint16_t pin_number) {
    int count = 0;
    if (!net) return 0;
    for (size_t i = 0; i < vec_len(net->connections); ++i) {
        if (net->connections[i].component_id == component_id &&
            net->connections[i].pin_number == pin_number) ++count;
    }
    return count;
}

int design_validate_structure(const Design *design, DiagnosticList *out) {
    if (!design || !out) return -1;
    int errors = 0;

    for (size_t ci = 0; ci < vec_len(design->components); ++ci) {
        const Component *c = &design->components[ci];
        if (!c->reference[0]) {
            if (diagnostic_add(out, DIAGNOSTIC_ERROR, DIAG_TARGET_COMPONENT,
                (uint32_t)ci, "component_reference", "component has an empty reference designator") != 0) return -1;
            ++errors;
        }
        for (size_t cj = ci + 1; cj < vec_len(design->components); ++cj) {
            if (c->reference[0] && equals_case_insensitive(c->reference, design->components[cj].reference)) {
                if (diagnostic_add(out, DIAGNOSTIC_ERROR, DIAG_TARGET_COMPONENT,
                    (uint32_t)cj, "duplicate_reference", "duplicate component reference '%s' (case-insensitive)", c->reference) != 0) return -1;
                ++errors;
            }
        }
        if (requires_two_pins(c->model.kind)) {
            const Pin *pin1 = component_find_pin_const(c, 1);
            const Pin *pin2 = component_find_pin_const(c, 2);
            if (!pin1 || !pin2) {
                if (diagnostic_add(out, DIAGNOSTIC_ERROR, DIAG_TARGET_COMPONENT,
                    (uint32_t)ci, "required_terminal_missing",
                    "%s requires terminal pins 1 and 2 for its current model", c->reference) != 0) return -1;
                ++errors;
            } else if (pin1->net_id >= 0 && pin2->net_id >= 0 &&
                       pin1->net_id == pin2->net_id) {
                if (diagnostic_add(out, DIAGNOSTIC_ERROR, DIAG_TARGET_COMPONENT,
                    (uint32_t)ci, "shorted_two_terminal_component",
                    "%s pins 1 and 2 are connected to the same net ID %u", c->reference,
                    (unsigned)pin1->net_id) != 0) return -1;
                ++errors;
            }
        }
        for (size_t pi = 0; pi < vec_len(c->pins); ++pi) {
            const Pin *pin = &c->pins[pi];
            for (size_t pj = pi + 1; pj < vec_len(c->pins); ++pj) {
                if (pin->number == c->pins[pj].number) {
                    if (diagnostic_add(out, DIAGNOSTIC_ERROR, DIAG_TARGET_COMPONENT,
                        (uint32_t)ci, "duplicate_pin_number", "%s contains duplicate pin number %u", c->reference, (unsigned)pin->number) != 0) return -1;
                    ++errors;
                }
            }
            if (pin->net_id == -1) {
                if (diagnostic_add(out, DIAGNOSTIC_ERROR, DIAG_TARGET_COMPONENT,
                    (uint32_t)ci, "unconnected_pin", "%s pin %u is not connected to a Design net", c->reference, (unsigned)pin->number) != 0) return -1;
                ++errors;
                continue;
            }
            if (pin->net_id < 0 || (size_t)pin->net_id >= vec_len(design->nets)) {
                if (diagnostic_add(out, DIAGNOSTIC_ERROR, DIAG_TARGET_COMPONENT,
                    (uint32_t)ci, "invalid_net_id", "%s pin %u refers to invalid net ID %u", c->reference, (unsigned)pin->number, (unsigned)pin->net_id) != 0) return -1;
                ++errors;
                continue;
            }
            const Net *net = &design->nets[(size_t)pin->net_id];
            if (net->id != (uint32_t)pin->net_id) {
                if (diagnostic_add(out, DIAGNOSTIC_ERROR, DIAG_TARGET_NET,
                    net->id, "net_id_index_mismatch", "net array index and net ID disagree for net '%s'", net->name ? net->name : "(unnamed)") != 0) return -1;
                ++errors;
            }
            if (net_has_exact_connection(net, (uint32_t)ci, pin->number) != 1) {
                if (diagnostic_add(out, DIAGNOSTIC_ERROR, DIAG_TARGET_COMPONENT,
                    (uint32_t)ci, "pin_net_asymmetry", "%s pin %u and net '%s' do not have exactly one reciprocal connection", c->reference, (unsigned)pin->number, net->name ? net->name : "(unnamed)") != 0) return -1;
                ++errors;
            }
        }
    }

    for (size_t ni = 0; ni < vec_len(design->nets); ++ni) {
        const Net *net = &design->nets[ni];
        if (net->id != (uint32_t)ni) {
            if (diagnostic_add(out, DIAGNOSTIC_ERROR, DIAG_TARGET_NET,
                (uint32_t)ni, "net_id_index_mismatch", "net at index %zu has ID %u", ni, (unsigned)net->id) != 0) return -1;
            ++errors;
        }
        if (!net->name || !net->name[0]) {
            if (diagnostic_add(out, DIAGNOSTIC_ERROR, DIAG_TARGET_NET,
                (uint32_t)ni, "empty_net_name", "net at index %zu has an empty name", ni) != 0) return -1;
            ++errors;
        }
        for (size_t nj = ni + 1; nj < vec_len(design->nets); ++nj) {
            const Net *other = &design->nets[nj];
            if (net->name && net->name[0] && other->name && strcmp(net->name, other->name) == 0) {
                if (diagnostic_add(out, DIAGNOSTIC_ERROR, DIAG_TARGET_NET,
                    (uint32_t)nj, "duplicate_net_name", "net name '%s' is assigned to multiple net IDs", net->name) != 0) return -1;
                ++errors;
            }
        }
        if (vec_len(net->connections) == 0) {
            if (diagnostic_add(out, DIAGNOSTIC_ERROR, DIAG_TARGET_NET,
                (uint32_t)ni, "empty_net", "net '%s' has no attached component pins", net->name ? net->name : "(unnamed)") != 0) return -1;
            ++errors;
        }
        for (size_t k = 0; k < vec_len(net->connections); ++k) {
            const NetConnection *conn = &net->connections[k];
            const Component *c = design_get_component_const(design, conn->component_id);
            const Pin *pin = c ? component_find_pin_const(c, conn->pin_number) : NULL;
            if (!pin || pin->net_id != (int32_t)net->id) {
                if (diagnostic_add(out, DIAGNOSTIC_ERROR, DIAG_TARGET_NET,
                    (uint32_t)ni, "orphan_net_connection", "net '%s' contains an orphan or mismatched component-pin connection", net->name ? net->name : "(unnamed)") != 0) return -1;
                ++errors;
            }
        }
        if (vec_len(net->connections) == 1) {
            if (diagnostic_add(out, DIAGNOSTIC_WARNING, DIAG_TARGET_NET,
                (uint32_t)ni, "single_terminal_net", "net '%s' has only one attached component pin; check for an open connection", net->name ? net->name : "(unnamed)") != 0) return -1;
        }
    }

    for (size_t ci = 0; ci < vec_len(design->components); ++ci) {
        const Component *c = &design->components[ci];
        if (c->model.kind == COMPONENT_RESISTOR) {
            double r = c->model.data.resistor.resistance_ohm.nominal;
            if (!(r > 0.0) || !isfinite(r)) {
                if (diagnostic_add(out, DIAGNOSTIC_ERROR, DIAG_TARGET_COMPONENT,
                    (uint32_t)ci, "invalid_resistance", "%s resistance must be finite and positive", c->reference) != 0) return -1;
                ++errors;
            }
        } else if (c->model.kind == COMPONENT_SOURCE) {
            if (!isfinite(c->model.data.source.voltage_dc_v)) {
                if (diagnostic_add(out, DIAGNOSTIC_ERROR, DIAG_TARGET_COMPONENT,
                    (uint32_t)ci, "invalid_source_voltage", "%s source voltage must be finite", c->reference) != 0) return -1;
                ++errors;
            }
        }
    }
    return errors;
}

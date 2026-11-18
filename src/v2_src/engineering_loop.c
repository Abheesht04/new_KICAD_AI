#include "engineering_loop.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static bool text_eq_ci(const char *a, const char *b) {
    if (!a || !b) return false;
    while (*a && *b) {
        unsigned char x = (unsigned char)*a++;
        unsigned char y = (unsigned char)*b++;
        if (x >= 'a' && x <= 'z') x = (unsigned char)(x - 'a' + 'A');
        if (y >= 'a' && y <= 'z') y = (unsigned char)(y - 'a' + 'A');
        if (x != y) return false;
    }
    return *a == '\0' && *b == '\0';
}

static void finding_add(EngineeringLoopResult *r, EngineeringFindingStatus status,
                        const char *code, const char *reference, const char *net,
                        const char *message, bool measured, double value,
                        double limit) {
    if (!r) return;
    if (r->finding_count >= r->finding_capacity) { r->findings_truncated = true; return; }
    EngineeringFinding *f = &r->findings[r->finding_count++];
    f->status = status; f->code = code; f->reference = reference; f->net = net;
    f->message = message; f->has_measurement = measured;
    f->measured = value; f->limit = limit;
}

static const EngineeringComponentBinding *binding_find(
    const EngineeringComponentBinding *bindings, size_t count, const char *ref) {
    for (size_t i = 0; i < count; ++i)
        if (bindings[i].reference && ref && strcmp(bindings[i].reference, ref) == 0)
            return &bindings[i];
    return NULL;
}

static const PartRating *rating_find_bound(const PartRecord *p,
                                            PartRatingName name,
                                            PartRatingBound bound) {
    if (!p) return NULL;
    for (size_t i = 0; i < p->rating_count; ++i)
        if (p->ratings[i].name == name && p->ratings[i].bound == bound)
            return &p->ratings[i];
    return NULL;
}

static bool rating_to_si(const PartRating *r, double *out) {
    double scale;
    if (!r || !out) return false;
    switch (r->unit) {
    case PART_UNIT_UOHM: scale = 1.0e-6; break;
    case PART_UNIT_PF: scale = 1.0e-12; break;
    case PART_UNIT_NH: scale = 1.0e-9; break;
    case PART_UNIT_UV: scale = 1.0e-6; break;
    case PART_UNIT_UA: scale = 1.0e-6; break;
    case PART_UNIT_UW: scale = 1.0e-6; break;
    default: return false;
    }
    *out = (double)r->value * scale;
    return isfinite(*out);
}

static bool part_nominal_value(const PartRecord *p, double *value) {
    const PartRating *rating = rating_find_bound(p,
        p && p->kind == PART_KIND_RESISTOR ? PART_RATING_RESISTANCE :
        p && p->kind == PART_KIND_CAPACITOR ? PART_RATING_CAPACITANCE :
        p && p->kind == PART_KIND_INDUCTOR ? PART_RATING_INDUCTANCE : PART_RATING_INVALID,
        PART_BOUND_NOMINAL);
    if (!rating) {
        rating = rating_find_bound(p,
            p && p->kind == PART_KIND_RESISTOR ? PART_RATING_RESISTANCE :
            p && p->kind == PART_KIND_CAPACITOR ? PART_RATING_CAPACITANCE :
            p && p->kind == PART_KIND_INDUCTOR ? PART_RATING_INDUCTANCE : PART_RATING_INVALID,
            PART_BOUND_TYPICAL);
    }
    return rating_to_si(rating, value) && *value > 0.0;
}

static CircuitElementKind passive_kind(PartKind kind) {
    switch (kind) {
    case PART_KIND_RESISTOR: return CIRCUIT_RESISTOR;
    case PART_KIND_CAPACITOR: return CIRCUIT_CAPACITOR;
    case PART_KIND_INDUCTOR: return CIRCUIT_INDUCTOR;
    default: return CIRCUIT_ELEMENT_INVALID;
    }
}

static bool terminal_node(const CopperTopology *t, const uint32_t *net_nodes,
                          const char *reference, const char *pin,
                          uint32_t *out_node) {
    const CopperTopologyTerminal *terminal =
        copper_topology_find_terminal(t, reference, pin);
    if (!terminal || terminal->net_index == COPPER_TOPOLOGY_NO_NET ||
        terminal->net_index >= t->net_count) return false;
    *out_node = net_nodes[terminal->net_index];
    return true;
}

static size_t element_pin_count(CircuitElementKind kind) {
    switch (kind) {
    case CIRCUIT_RESISTOR: case CIRCUIT_CAPACITOR: case CIRCUIT_INDUCTOR:
    case CIRCUIT_VOLTAGE_SOURCE: case CIRCUIT_CURRENT_SOURCE: case CIRCUIT_DIODE:
        return 2u;
    case CIRCUIT_BJT: case CIRCUIT_JFET: return 3u;
    case CIRCUIT_MOSFET: case CIRCUIT_VCVS: case CIRCUIT_VCCS:
    case CIRCUIT_CCCS: case CIRCUIT_CCVS: return 4u;
    default: return 0u;
    }
}

static bool is_ground_net(const char *name) {
    return text_eq_ci(name, "0") || text_eq_ci(name, "GND") ||
           text_eq_ci(name, "GROUND") || text_eq_ci(name, "GND_POWER");
}

static bool build_circuit(Arena *arena, const CopperTopology *t,
                          const EngineeringComponentBinding *bindings,
                          size_t binding_count, EngineeringLoopResult *out) {
    size_t non_ground = 0;
    out->net_nodes = t->net_count ? (uint32_t *)arena_alloc_array(
        arena, sizeof(uint32_t), t->net_count) : NULL;
    out->net_node_count = t->net_count;
    if (t->net_count && !out->net_nodes) return false;
    for (size_t i = 0; i < t->net_count; ++i) {
        if (is_ground_net(t->nets[i].name) || t->nets[i].connection_count == 0u) out->net_nodes[i] = 0;
        else {
            if (non_ground >= UINT32_MAX) return false;
            out->net_nodes[i] = (uint32_t)(++non_ground);
        }
    }
    if (!non_ground || t->component_count == 0u) {
        finding_add(out, ENGINEERING_FINDING_UNVERIFIED, "CIRCUIT_REFERENCE_MISSING",
                    NULL, NULL, "A DC circuit requires at least one non-ground net and one component.",
                    false, 0.0, 0.0);
        return false;
    }
    if (circuit_init(arena, &out->circuit, (uint32_t)non_ground,
                     t->component_count) != CIRCUIT_OK) return false;

    for (size_t ci = 0; ci < t->component_count; ++ci) {
        const CopperTopologyComponent *component = &t->components[ci];
        const EngineeringComponentBinding *binding = binding_find(
            bindings, binding_count, component->reference);
        if (!binding) {
            finding_add(out, ENGINEERING_FINDING_UNVERIFIED, "COMPONENT_MODEL_MISSING",
                        component->reference, NULL,
                        "No resolved part or explicit circuit model was supplied for this component.",
                        false, 0.0, 0.0);
            return false;
        }
        if (component->part_id && binding->part && binding->part->id &&
            strcmp(component->part_id, binding->part->id) != 0) {
            finding_add(out, ENGINEERING_FINDING_FAIL, "PART_ID_MISMATCH",
                        component->reference, NULL,
                        "Binding part ID does not match the topology component part ID.",
                        false, 0.0, 0.0);
            return false;
        }

        CircuitElement element;
        memset(&element, 0, sizeof(element));
        const char *pin_order[4] = {NULL, NULL, NULL, NULL};
        size_t pin_count = 0;
        if (binding->has_element_template) {
            element = binding->element_template;
            pin_count = binding->pin_count;
            for (size_t i = 0; i < pin_count && i < 4u; ++i)
                pin_order[i] = binding->pin_numbers[i];
        } else {
            if (!binding->part) {
                finding_add(out, ENGINEERING_FINDING_UNVERIFIED, "PART_RECORD_MISSING",
                            component->reference, NULL,
                            "Component has no part record or explicit model template.",
                            false, 0.0, 0.0);
                return false;
            }
            element.kind = passive_kind(binding->part->kind);
            if (element.kind == CIRCUIT_ELEMENT_INVALID) {
                finding_add(out, ENGINEERING_FINDING_UNVERIFIED, "MODEL_UNSUPPORTED",
                            component->reference, NULL,
                            "This part kind needs an explicit CircuitElement model template.",
                            false, 0.0, 0.0);
                return false;
            }
            pin_count = binding->pin_count;
            for (size_t i = 0; i < pin_count && i < 4u; ++i)
                pin_order[i] = binding->pin_numbers[i];
            if (!pin_count && binding->part->pin_count) {
                pin_count = binding->part->pin_count;
                if (pin_count > 4u) pin_count = 4u;
                for (size_t i = 0; i < pin_count; ++i)
                    pin_order[i] = binding->part->pins[i].symbol_pin;
            }
            if (!pin_count && component->terminal_count == 2u &&
                (element.kind == CIRCUIT_RESISTOR || element.kind == CIRCUIT_CAPACITOR ||
                 element.kind == CIRCUIT_INDUCTOR)) {
                pin_count = 2u;
                pin_order[0] = t->terminals[component->first_terminal].number;
                pin_order[1] = t->terminals[component->first_terminal + 1u].number;
                finding_add(out, ENGINEERING_FINDING_UNVERIFIED, "MODEL_PIN_ORDER_ASSUMED",
                            component->reference, NULL,
                            "Two-terminal passive pin order follows the schematic terminal order because the part has no explicit model-pin map.",
                            false, 0.0, 0.0);
            }
            if (binding->has_value_override) element.value_si = binding->value_override_si;
            else if (!part_nominal_value(binding->part, &element.value_si)) {
                finding_add(out, ENGINEERING_FINDING_UNVERIFIED, "NOMINAL_VALUE_MISSING",
                            component->reference, NULL,
                            "Passive part has no nominal/typical SI-convertible rating and no value override.",
                            false, 0.0, 0.0);
                return false;
            }
        }
        if (binding->has_value_override && !binding->has_element_template)
            element.value_si = binding->value_override_si;
        size_t required = element_pin_count(element.kind);
        if (!required || pin_count != required) {
            finding_add(out, ENGINEERING_FINDING_UNVERIFIED, "MODEL_PIN_MAPPING_INCOMPLETE",
                        component->reference, NULL,
                        "Circuit model requires a complete ordered pin list (2, 3 or 4 terminals).",
                        false, 0.0, 0.0);
            return false;
        }
        uint32_t nodes[4] = {0, 0, 0, 0};
        for (size_t pi = 0; pi < required; ++pi) {
            if (!pin_order[pi] || !terminal_node(t, out->net_nodes,
                                                component->reference,
                                                pin_order[pi], &nodes[pi])) {
                finding_add(out, ENGINEERING_FINDING_UNVERIFIED, "MODEL_TERMINAL_UNCONNECTED",
                            component->reference, NULL,
                            "A model terminal is missing from topology or has no assigned net.",
                            false, 0.0, 0.0);
                return false;
            }
        }
        element.reference = component->reference;
        element.n1 = nodes[0]; element.n2 = nodes[1];
        if (required >= 3u) element.n3 = nodes[2];
        if (required >= 4u) element.n4 = nodes[3];
        if (element.kind == CIRCUIT_VOLTAGE_SOURCE &&
            !element.has_dc_value && element.waveform.kind == CIRCUIT_WAVEFORM_NONE)
            element.has_dc_value = true;
        CircuitStatus add_status = circuit_add(&out->circuit, &element);
        if (add_status != CIRCUIT_OK) {
            finding_add(out, ENGINEERING_FINDING_FAIL, "CIRCUIT_ELEMENT_INVALID",
                        component->reference, NULL,
                        circuit_status_name(add_status), false, 0.0, 0.0);
            return false;
        }
    }
    CircuitStatus valid = circuit_validate(&out->circuit);
    if (valid != CIRCUIT_OK) {
        finding_add(out, ENGINEERING_FINDING_FAIL, "CIRCUIT_INVALID", NULL, NULL,
                    circuit_status_name(valid), false, 0.0, 0.0);
        return false;
    }
    out->circuit_built = true;
    finding_add(out, ENGINEERING_FINDING_PASS, "CIRCUIT_BUILT", NULL, NULL,
                "All topology components were mapped to validated circuit elements.",
                false, 0.0, 0.0);
    return true;
}

static double absd(double x) { return x < 0.0 ? -x : x; }

static bool rating_limit_si(const PartRecord *part, PartRatingName name,
                            double *limit) {
    const PartRating *rating = rating_find_bound(part, name, PART_BOUND_MAXIMUM);
    return rating_to_si(rating, limit) && *limit > 0.0;
}

static void assess_component_stress(Arena *arena, const CopperTopology *t,
                                    const EngineeringComponentBinding *bindings,
                                    size_t binding_count, EngineeringLoopResult *out) {
    out->stresses = t->component_count ? (EngineeringComponentStress *)arena_alloc_array(
        arena, sizeof(*out->stresses), t->component_count) : NULL;
    if (!t->component_count || !out->stresses) return;
    out->stress_count = t->component_count;
    for (size_t i = 0; i < t->component_count; ++i) {
        const CopperTopologyComponent *component = &t->components[i];
        const EngineeringComponentBinding *binding = binding_find(bindings, binding_count,
                                                                   component->reference);
        EngineeringComponentStress *stress = &out->stresses[i];
        memset(stress, 0, sizeof(*stress));
        stress->reference = component->reference;
        stress->voltage_status = ENGINEERING_FINDING_UNVERIFIED;
        stress->current_status = ENGINEERING_FINDING_UNVERIFIED;
        stress->power_status = ENGINEERING_FINDING_UNVERIFIED;
        if (!binding || !binding->part || i >= out->circuit.element_count) continue;
        if (binding->part->kind != PART_KIND_RESISTOR) {
            finding_add(out, ENGINEERING_FINDING_UNVERIFIED, "PART_STRESS_MODEL_UNSUPPORTED",
                        component->reference, NULL,
                        "This milestone only evaluates DC voltage, current and dissipation stress for resistors. Capacitor ripple/ESR, inductor DCR/saturation, diode reverse/forward ratings and active-device SOA need component-specific models.",
                        false, 0.0, 0.0);
            continue;
        }
        const CircuitElement *e = &out->circuit.elements[i];
        if (e->n1 >= out->operating_point.node_voltage_count ||
            e->n2 >= out->operating_point.node_voltage_count ||
            i >= out->operating_point.element_current_count) continue;
        stress->voltage_v = absd(out->operating_point.node_voltages_v[e->n1] -
                                 out->operating_point.node_voltages_v[e->n2]);
        stress->current_a = absd(out->operating_point.element_currents_a[i]);
        stress->power_w = stress->voltage_v * stress->current_a;
        stress->has_voltage = stress->has_current = stress->has_power = true;
        double limit = 0.0;
        if (rating_limit_si(binding->part, PART_RATING_VOLTAGE, &limit)) {
            stress->voltage_status = stress->voltage_v <= limit ? ENGINEERING_FINDING_PASS : ENGINEERING_FINDING_FAIL;
            finding_add(out, stress->voltage_status, "PART_VOLTAGE_STRESS",
                        component->reference, NULL, "Operating voltage compared with maximum part rating.",
                        true, stress->voltage_v, limit);
        } else finding_add(out, ENGINEERING_FINDING_UNVERIFIED, "PART_VOLTAGE_RATING_MISSING",
                           component->reference, NULL, "Maximum voltage rating is unavailable.", false, 0.0, 0.0);
        if (rating_limit_si(binding->part, PART_RATING_CURRENT, &limit)) {
            stress->current_status = stress->current_a <= limit ? ENGINEERING_FINDING_PASS : ENGINEERING_FINDING_FAIL;
            finding_add(out, stress->current_status, "PART_CURRENT_STRESS",
                        component->reference, NULL, "Operating current compared with maximum part rating.",
                        true, stress->current_a, limit);
        } else finding_add(out, ENGINEERING_FINDING_UNVERIFIED, "PART_CURRENT_RATING_MISSING",
                           component->reference, NULL, "Maximum current rating is unavailable.", false, 0.0, 0.0);
        if (rating_limit_si(binding->part, PART_RATING_POWER, &limit)) {
            stress->power_status = stress->power_w <= limit ? ENGINEERING_FINDING_PASS : ENGINEERING_FINDING_FAIL;
            finding_add(out, stress->power_status, "PART_POWER_STRESS",
                        component->reference, NULL, "Dissipated power compared with maximum part rating.",
                        true, stress->power_w, limit);
        } else finding_add(out, ENGINEERING_FINDING_UNVERIFIED, "PART_POWER_RATING_MISSING",
                           component->reference, NULL, "Maximum power rating is unavailable.", false, 0.0, 0.0);
    }
}

static const BoardFootprint *find_board_footprint(const Board *board,
                                                  const CopperTopologyComponent *component) {
    if (!board || !component) return NULL;
    for (size_t i = 0; i < board->footprint_count; ++i) {
        const BoardFootprint *fp = &board->footprints[i];
        const char *tail = fp->path ? strrchr(fp->path, '/') : NULL;
        const char *uuid = tail ? tail + 1 : fp->path;
        if (uuid && component->uuid && strcmp(uuid, component->uuid) == 0) return fp;
    }
    for (size_t i = 0; i < board->footprint_count; ++i) {
        const BoardFootprint *fp = &board->footprints[i];
        if ((!fp->path || !fp->path[0]) && fp->reference && component->reference &&
            strcmp(fp->reference, component->reference) == 0) return fp;
    }
    return NULL;
}

static const BoardPad *find_board_pad(const Board *board, size_t footprint_index,
                                      const char *number) {
    if (!board || !number || footprint_index >= board->footprint_count) return NULL;
    const BoardFootprint *fp = &board->footprints[footprint_index];
    if ((size_t)fp->first_pad + fp->pad_count > board->pad_count) return NULL;
    for (size_t i = 0; i < fp->pad_count; ++i) {
        const BoardPad *pad = &board->pads[fp->first_pad + i];
        if (pad->number && strcmp(pad->number, number) == 0) return pad;
    }
    return NULL;
}

static void check_topology_board_parity(const CopperTopology *topology,
                                        const EngineeringComponentBinding *bindings,
                                        size_t binding_count, const Board *board,
                                        EngineeringLoopResult *out) {
    out->parity_checked = true;
    for (size_t ci = 0; ci < topology->component_count; ++ci) {
        const CopperTopologyComponent *component = &topology->components[ci];
        const BoardFootprint *fp = find_board_footprint(board, component);
        if (!fp) {
            ++out->parity_missing_footprints;
            finding_add(out, component->footprint && component->footprint[0] ?
                        ENGINEERING_FINDING_FAIL : ENGINEERING_FINDING_UNVERIFIED,
                        "PCB_FOOTPRINT_MISSING", component->reference, NULL,
                        component->footprint && component->footprint[0] ?
                        "Logical component has an assigned footprint but no matching physical footprint was found." :
                        "No physical footprint is associated with this logical component; verify that it is intentionally virtual.",
                        false, 0.0, 0.0);
            continue;
        }
        ++out->parity_matched_components;
        for (size_t pi = 0; pi < component->terminal_count; ++pi) {
            const CopperTopologyTerminal *term =
                &topology->terminals[component->first_terminal + pi];
            if (term->no_connect) continue;
            const char *pad_number = term->footprint_pad;
            const EngineeringComponentBinding *binding = binding_find(
                bindings, binding_count, component->reference);
            if (!pad_number && binding && binding->part) {
                for (size_t mi = 0; mi < binding->part->pin_count; ++mi) {
                    const PartPinMap *map = &binding->part->pins[mi];
                    if (map->symbol_pin && term->number &&
                        strcmp(map->symbol_pin, term->number) == 0 &&
                        map->footprint_pad && map->footprint_pad[0]) {
                        pad_number = map->footprint_pad;
                        break;
                    }
                }
            }
            if (!pad_number) {
                pad_number = term->number;
                finding_add(out, ENGINEERING_FINDING_UNVERIFIED, "PAD_MAPPING_ASSUMED",
                            component->reference, NULL,
                            "No explicit symbol-pin to footprint-pad mapping was supplied; matching by identical pin/pad number is an assumption.",
                            false, 0.0, 0.0);
            }
            const BoardPad *pad = find_board_pad(board, (size_t)(fp - board->footprints), pad_number);
            if (!pad) {
                finding_add(out, ENGINEERING_FINDING_UNVERIFIED, "PCB_PAD_MAPPING_MISSING",
                            component->reference, NULL,
                            "No physical pad matches the logical terminal mapping.", false, 0.0, 0.0);
                continue;
            }
            if (term->net_index == COPPER_TOPOLOGY_NO_NET) continue;
            const BoardNet *physical_net = board_net_by_id(board, pad->net);
            const char *logical_name = topology->nets[term->net_index].name;
            if (!pad->net || !physical_net || !physical_net->name || !logical_name ||
                strcmp(physical_net->name, logical_name) != 0) {
                ++out->parity_pad_net_mismatches;
                finding_add(out, ENGINEERING_FINDING_FAIL, "PCB_PAD_NET_MISMATCH",
                            component->reference, logical_name,
                            "Physical pad assignment differs from the logical topology net.",
                            false, 0.0, 0.0);
            }
        }
    }
    for (size_t fi = 0; fi < board->footprint_count; ++fi) {
        const BoardFootprint *fp = &board->footprints[fi];
        bool found = false;
        for (size_t ci = 0; ci < topology->component_count; ++ci)
            if (find_board_footprint(board, &topology->components[ci]) == fp) { found = true; break; }
        if (!found)
            finding_add(out, ENGINEERING_FINDING_UNVERIFIED, "PCB_FOOTPRINT_WITHOUT_LOGIC",
                        fp->reference, NULL,
                        "Physical footprint has no matching component in the supplied topology; it may be a mounting/board feature or a missing logical component.",
                        false, 0.0, 0.0);
    }
}

static const BoardNet *board_net_by_name(const Board *board, const char *name) {
    if (!board || !name) return NULL;
    for (size_t i = 0; i < board->net_count; ++i)
        if (board->nets[i].name && strcmp(board->nets[i].name, name) == 0)
            return &board->nets[i];
    return NULL;
}

static void propose_routes(Arena *arena, const CopperTopology *topology,
                           const Board *board, const EngineeringLoopOptions *options,
                           EngineeringLoopResult *out) {
    out->routes_attempted = true;
    out->route_capacity = options->max_route_proposals ? options->max_route_proposals : 64u;
    out->routes = (EngineeringRouteProposal *)arena_alloc_array(
        arena, sizeof(*out->routes), out->route_capacity);
    if (!out->routes && out->route_capacity) {
        finding_add(out, ENGINEERING_FINDING_UNVERIFIED, "ROUTER_MEMORY",
                    NULL, NULL, "Could not allocate route proposal storage.", false, 0.0, 0.0);
        return;
    }
    for (size_t ni = 0; ni < topology->net_count; ++ni) {
        const CopperTopologyNet *net = &topology->nets[ni];
        const BoardNet *pcb_net = board_net_by_name(board, net->name);
        if (!pcb_net) {
            finding_add(out, ENGINEERING_FINDING_UNVERIFIED, "ROUTE_NET_NOT_ON_BOARD",
                        NULL, net->name, "Logical net has no matching physical PCB net.", false, 0.0, 0.0);
            continue;
        }
        const BoardPad *endpoints[3] = {NULL, NULL, NULL};
        size_t endpoint_count = 0;
        for (size_t pi = 0; pi < board->pad_count; ++pi) {
            if (board->pads[pi].net != pcb_net->id) continue;
            if (endpoint_count < 3u) endpoints[endpoint_count] = &board->pads[pi];
            ++endpoint_count;
        }
        if (endpoint_count != 2u) {
            finding_add(out, ENGINEERING_FINDING_UNVERIFIED,
                        endpoint_count == 0u ? "ROUTE_NO_PHYSICAL_ENDPOINTS" : "ROUTE_MULTI_TERMINAL_UNSUPPORTED",
                        NULL, net->name,
                        endpoint_count == 0u ?
                        "No physical pads for this logical net were found on the supplied board." :
                        "This routing proposal backend currently accepts exactly two physical pads per net; multi-terminal routing needs a tree/Steiner strategy.",
                        true, (double)endpoint_count, 2.0);
            continue;
        }
        if (out->route_count >= out->route_capacity) {
            out->findings_truncated = true;
            break;
        }
        RouterOptions ro = options->router_options;
        if (ro.grid_pitch_nm <= 0) ro.grid_pitch_nm = 500000;
        if (ro.trace_width_nm <= 0) ro.trace_width_nm = board->rules.min_track_width;
        if (ro.clearance_nm <= 0) ro.clearance_nm = board->rules.min_clearance;
        if (ro.edge_clearance_nm <= 0) ro.edge_clearance_nm = board->rules.edge_clearance;
        if (ro.trace_width_nm <= 0 || ro.clearance_nm <= 0 || ro.edge_clearance_nm <= 0) {
            finding_add(out, ENGINEERING_FINDING_UNVERIFIED, "ROUTE_CONSTRAINTS_INCOMPLETE",
                        NULL, net->name,
                        "Routing requires explicit positive trace width, copper clearance and edge clearance from board rules or CLI overrides; Copper will not invent fabrication constraints.",
                        false, 0.0, 0.0);
            continue;
        }
        if (ro.max_cells == 0u) ro.max_cells = 1000000u;
        ro.layer = BOARD_LAYER_F_CU;
        ro.net = pcb_net->id;
        EngineeringRouteProposal *proposal = &out->routes[out->route_count++];
        memset(proposal, 0, sizeof(*proposal));
        proposal->topology_net_index = (uint32_t)ni;
        proposal->net_name = net->name;
        proposal->status = copper_route_astar(board, endpoints[0]->pos, endpoints[1]->pos,
                                               &ro, arena, &proposal->route);
        if (proposal->status == ROUTER_OK)
            finding_add(out, ENGINEERING_FINDING_INFO, "ROUTE_PATH_PROPOSED",
                        NULL, net->name,
                        "A two-terminal A* route proposal was generated. It is not committed to the board and has not been included in DFM.",
                        true, (double)proposal->route.point_count, 0.0);
        else
            finding_add(out, ENGINEERING_FINDING_UNVERIFIED, "ROUTE_PATH_UNAVAILABLE",
                        NULL, net->name,
                        proposal->route.message[0] ? proposal->route.message : "Router did not produce a path.",
                        false, 0.0, 0.0);
    }
    finding_add(out, ENGINEERING_FINDING_UNVERIFIED, "ROUTE_PROPOSALS_NOT_COMMITTED",
                NULL, NULL,
                "Route paths are proposals only. Copper does not add their tracks to the physical board in this milestone; run DFM after a future commit/apply stage.",
                false, 0.0, 0.0);
}

static void run_physical_checks(Arena *arena, const CopperTopology *t,
                                const EngineeringComponentBinding *bindings, size_t binding_count,
                                const Board *board, const BoardDiag *provided_diag,
                                const FabRuleSet *rules,
                                const EngineeringLoopOptions *o,
                                EngineeringLoopResult *out) {
    if (!board) {
        finding_add(out, ENGINEERING_FINDING_UNVERIFIED, "PHYSICAL_BOARD_NOT_GENERATED",
                    NULL, NULL, "No physical Board was supplied; placement, routing and DFM cannot be evaluated.",
                    false, 0.0, 0.0);
        return;
    }
    out->physical_board_supplied = true;
    check_topology_board_parity(t, bindings, binding_count, board, out);
    BoardDiag local_diag;
    const BoardDiag *diag = provided_diag;
    if (!diag) {
        if (board_validate(board, &local_diag) != BOARD_OK) {
            finding_add(out, ENGINEERING_FINDING_FAIL, "BOARD_MODEL_INVALID", NULL, NULL,
                        local_diag.message, false, 0.0, 0.0);
            return;
        }
        diag = &local_diag;
    }
    if (diag->unmodelled_records || diag->unmodelled_geometry_records ||
        diag->inexact_rotations || diag->unsupported_layers) {
        finding_add(out, ENGINEERING_FINDING_UNVERIFIED, "BOARD_COVERAGE_INCOMPLETE",
                    NULL, NULL, "Board has unmodelled records, geometry, rotations or layers.",
                    false, 0.0, 0.0);
    } else finding_add(out, ENGINEERING_FINDING_PASS, "BOARD_COVERAGE_COMPLETE",
                       NULL, NULL, "Board diagnostic reports complete supported coverage.", false, 0.0, 0.0);

    if (o->run_placement_proposal) {
        out->placement_attempted = true;
        PlacerOptions po = o->placer_options;
        if (po.grid_pitch_nm <= 0) {
            po.grid_pitch_nm = 500000; po.component_gap_nm = 200000;
            po.edge_clearance_nm = 200000; po.max_candidates = 100000u;
        }
        PlacerStatus ps = copper_place_grid(board, &po, arena, &out->placement);
        finding_add(out, ps == PLACER_OK ? ENGINEERING_FINDING_PASS :
                    ps == PLACER_PARTIAL ? ENGINEERING_FINDING_UNVERIFIED : ENGINEERING_FINDING_UNVERIFIED,
                    "PLACEMENT_PROPOSAL", NULL, NULL, out->placement.message,
                    false, 0.0, 0.0);
    }
    if (o->run_router_proposals) propose_routes(arena, t, board, o, out);
    if (o->run_dfm) {
        out->dfm_attempted = true;
        if (!rules) {
            finding_add(out, ENGINEERING_FINDING_UNVERIFIED, "FAB_RULES_MISSING",
                        NULL, NULL, "DFM was requested but no validated fabrication rule set was supplied.",
                        false, 0.0, 0.0);
        } else {
            report_init(&out->dfm_report, arena, o->dfm_finding_capacity ? o->dfm_finding_capacity : 8192u);
            DfmStatus ds = dfm_check_board(board, diag, rules, &o->dfm_options,
                                           arena, &out->dfm_report);
            if (ds != DFM_OK)
                finding_add(out, ENGINEERING_FINDING_UNVERIFIED, "DFM_EXECUTION_FAILED",
                            NULL, NULL, "DFM engine could not complete the requested checks.",
                            false, 0.0, 0.0);
            else {
                ReportStatus rs = report_overall(&out->dfm_report);
                finding_add(out, rs == REPORT_PASS ? ENGINEERING_FINDING_PASS :
                            rs == REPORT_FAIL ? ENGINEERING_FINDING_FAIL : ENGINEERING_FINDING_UNVERIFIED,
                            "DFM_RESULT", NULL, NULL,
                            "DFM findings apply to the supplied board revision, not to uncommitted placement/routing proposals.",
                            false, 0.0, 0.0);
            }
        }
    }
}

EngineeringLoopOptions engineering_loop_default_options(void) {
    EngineeringLoopOptions o;
    memset(&o, 0, sizeof(o));
    o.mna_options = mna_default_options();
    o.finding_capacity = 256u;
    o.dfm_finding_capacity = 8192u;
    o.max_route_proposals = 64u;
    return o;
}

EngineeringLoopStatus engineering_loop_run(
    Arena *arena, const CopperTopology *topology,
    const EngineeringComponentBinding *bindings, size_t binding_count,
    const Board *board, const BoardDiag *board_diag,
    const FabRuleSet *fab_rules, const EngineeringLoopOptions *options,
    EngineeringLoopResult *out) {
    if (!arena || !topology || !out || (binding_count && !bindings))
        return ENGINEERING_LOOP_ERR_ARGUMENT;
    memset(out, 0, sizeof(*out));
    out->status = ENGINEERING_LOOP_PARTIAL;
    out->iterations = 1u;
    EngineeringLoopOptions defaults = engineering_loop_default_options();
    const EngineeringLoopOptions *o = options ? options : &defaults;
    size_t minimum_findings = 64u;
    if (topology->component_count <= (SIZE_MAX - minimum_findings) / 8u)
        minimum_findings += topology->component_count * 8u;
    out->finding_capacity = o->finding_capacity > minimum_findings ? o->finding_capacity : minimum_findings;
    out->findings = (EngineeringFinding *)arena_alloc_array(
        arena, sizeof(*out->findings), out->finding_capacity);
    if (!out->findings) return ENGINEERING_LOOP_ERR_MEMORY;
    out->resolved_components = topology->component_count ?
        (EngineeringResolvedComponent *)arena_alloc_array(
            arena, sizeof(*out->resolved_components), topology->component_count) : NULL;
    if (topology->component_count && !out->resolved_components)
        return ENGINEERING_LOOP_ERR_MEMORY;
    out->resolved_component_count = topology->component_count;
    for (size_t i = 0; i < topology->component_count; ++i) {
        const CopperTopologyComponent *component = &topology->components[i];
        const EngineeringComponentBinding *binding = binding_find(
            bindings, binding_count, component->reference);
        EngineeringResolvedComponent *resolved = &out->resolved_components[i];
        memset(resolved, 0, sizeof(*resolved));
        resolved->reference = component->reference;
        resolved->part_id = binding && binding->part ? binding->part->id : component->part_id;
        resolved->has_catalogue_part = binding && binding->part;
        resolved->has_explicit_model = binding && binding->has_element_template;
        if (binding && binding->part) {
            resolved->manufacturer = binding->part->manufacturer;
            resolved->mpn = binding->part->mpn;
            resolved->symbol_id = binding->part->symbol_id;
            resolved->footprint_id = binding->part->footprint_id;
            resolved->model_id = binding->part->model_id;
            resolved->part_record = binding->part;
            resolved->part_kind = binding->part->kind;
            resolved->circuit_kind = passive_kind(binding->part->kind);
        }
        if (binding && binding->has_element_template)
            resolved->circuit_kind = binding->element_template.kind;
    }

    CopperTopologyStatus tv = copper_topology_validate(topology);
    if (tv != COPPER_TOPOLOGY_OK) {
        finding_add(out, ENGINEERING_FINDING_UNVERIFIED, "TOPOLOGY_INCOMPLETE",
                    NULL, NULL, copper_topology_status_name(tv), false, 0.0, 0.0);
    }
    for (size_t i = 0; i < topology->terminal_count; ++i) {
        const CopperTopologyTerminal *term = &topology->terminals[i];
        if (term->net_index == COPPER_TOPOLOGY_NO_NET && !term->no_connect) {
            finding_add(out, ENGINEERING_FINDING_UNVERIFIED, "TERMINAL_UNCONNECTED",
                        NULL, NULL, "A terminal has no net and is not explicitly marked no-connect.",
                        false, 0.0, 0.0);
        }
    }

    bool circuit_ok = tv == COPPER_TOPOLOGY_OK;
    if (circuit_ok) circuit_ok = build_circuit(arena, topology, bindings,
                                               binding_count, out);
    if (circuit_ok) {
        out->simulation_attempted = true;
        out->simulation_status = mna_solve_dc(arena, &out->circuit,
                                               &o->mna_options,
                                               &out->operating_point);
        out->simulation_converged = out->simulation_status == MNA_OK &&
                                    out->operating_point.converged;
        if (out->simulation_converged) {
            finding_add(out, ENGINEERING_FINDING_PASS, "DC_OPERATING_POINT",
                        NULL, NULL, "DC operating point converged.",
                        true, out->operating_point.scaled_residual,
                        o->mna_options.residual_tolerance);
            assess_component_stress(arena, topology, bindings, binding_count, out);
        } else {
            finding_add(out, ENGINEERING_FINDING_FAIL, "DC_SIMULATION_FAILED",
                        NULL, NULL, mna_status_name(out->simulation_status),
                        false, 0.0, 0.0);
        }
    }
    run_physical_checks(arena, topology, bindings, binding_count, board, board_diag, fab_rules, o, out);
    finding_add(out, ENGINEERING_FINDING_UNVERIFIED, "PHYSICAL_PARASITICS_NOT_EXTRACTED",
                NULL, NULL, "Track/via parasitic extraction and post-layout re-simulation are not yet integrated; the MNA result is pre-layout electrical analysis.",
                false, 0.0, 0.0);
    finding_add(out, ENGINEERING_FINDING_UNVERIFIED, "ROUTING_NOT_GENERATED",
                NULL, NULL, "The engineering loop does not yet generate or commit a complete multi-terminal routed design.",
                false, 0.0, 0.0);
    finding_add(out, ENGINEERING_FINDING_UNVERIFIED, "MANUFACTURING_EXPORT_NOT_INTEGRATED",
                NULL, NULL, "Gerber/Excellon generation and read-back are not part of this engineering-loop milestone.",
                false, 0.0, 0.0);

    bool fail = false, unverified = out->findings_truncated;
    for (size_t i = 0; i < out->finding_count; ++i) {
        if (out->findings[i].status == ENGINEERING_FINDING_FAIL) fail = true;
        if (out->findings[i].status == ENGINEERING_FINDING_UNVERIFIED) unverified = true;
    }
    out->design_ready = out->circuit_built && out->simulation_converged &&
                        out->physical_board_supplied && out->dfm_attempted &&
                        report_overall(&out->dfm_report) == REPORT_PASS && !fail && !unverified;
    out->status = fail ? ENGINEERING_LOOP_REJECTED :
                  out->design_ready ? ENGINEERING_LOOP_OK : ENGINEERING_LOOP_PARTIAL;
    return ENGINEERING_LOOP_OK;
}

const char *engineering_loop_status_name(EngineeringLoopStatus s) {
    switch (s) {
    case ENGINEERING_LOOP_OK: return "OK";
    case ENGINEERING_LOOP_PARTIAL: return "PARTIAL";
    case ENGINEERING_LOOP_REJECTED: return "REJECTED";
    case ENGINEERING_LOOP_ERR_ARGUMENT: return "ERR_ARGUMENT";
    case ENGINEERING_LOOP_ERR_MEMORY: return "ERR_MEMORY";
    case ENGINEERING_LOOP_ERR_TOPOLOGY: return "ERR_TOPOLOGY";
    case ENGINEERING_LOOP_ERR_CIRCUIT: return "ERR_CIRCUIT";
    case ENGINEERING_LOOP_ERR_SIMULATION: return "ERR_SIMULATION";
    default: return "UNKNOWN";
    }
}
const char *engineering_finding_status_name(EngineeringFindingStatus s) {
    switch (s) {
    case ENGINEERING_FINDING_INFO: return "INFO";
    case ENGINEERING_FINDING_PASS: return "PASS";
    case ENGINEERING_FINDING_FAIL: return "FAIL";
    case ENGINEERING_FINDING_UNVERIFIED: return "UNVERIFIED";
    default: return "UNKNOWN";
    }
}

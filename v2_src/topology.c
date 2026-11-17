#include "topology.h"

#include <string.h>

static bool streq(const char *a, const char *b) {
    return a && b && strcmp(a, b) == 0;
}

static void issue_add(CopperTopology *t, const char *code,
                      const char *ref, const char *pin, const char *net,
                      const char *message) {
    if (!t || !t->issues || t->issue_count >= t->issue_capacity) return;
    CopperTopologyIssue *i = &t->issues[t->issue_count++];
    i->code = code;
    i->reference = ref;
    i->pin = pin;
    i->net = net;
    i->message = message;
    t->complete = false;
}

const CopperTopologyComponent *copper_topology_find_component(
    const CopperTopology *t, const char *reference) {
    if (!t || !reference) return NULL;
    for (size_t i = 0; i < t->component_count; ++i)
        if (streq(t->components[i].reference, reference)) return &t->components[i];
    return NULL;
}

static size_t component_index(const CopperTopology *t, const char *reference) {
    if (!t || !reference) return SIZE_MAX;
    for (size_t i = 0; i < t->component_count; ++i)
        if (streq(t->components[i].reference, reference)) return i;
    return SIZE_MAX;
}

static size_t terminal_global_index(const CopperTopology *t, size_t ci,
                                    const char *pin) {
    if (!t || ci >= t->component_count || !pin) return SIZE_MAX;
    const CopperTopologyComponent *c = &t->components[ci];
    for (size_t i = 0; i < c->terminal_count; ++i) {
        size_t ti = (size_t)c->first_terminal + i;
        if (streq(t->terminals[ti].number, pin)) return ti;
    }
    return SIZE_MAX;
}

const CopperTopologyTerminal *copper_topology_find_terminal(
    const CopperTopology *t, const char *reference, const char *pin_number) {
    size_t ci = component_index(t, reference);
    size_t ti = terminal_global_index(t, ci, pin_number);
    return ti == SIZE_MAX ? NULL : &t->terminals[ti];
}

CopperTopologyStatus copper_topology_init(
    Arena *arena, size_t component_capacity, size_t terminal_capacity,
    size_t net_capacity, size_t connection_capacity, CopperTopology *out) {
    if (!arena || !out) return COPPER_TOPOLOGY_ERR_ARGUMENT;
    memset(out, 0, sizeof(*out));
    out->arena = arena;
    out->component_capacity = component_capacity;
    out->terminal_capacity = terminal_capacity;
    out->net_capacity = net_capacity;
    out->connection_capacity = connection_capacity;
    if (component_capacity > UINT32_MAX ||
        terminal_capacity > UINT32_MAX || net_capacity > UINT32_MAX ||
        net_capacity > SIZE_MAX - 8u ||
        component_capacity > SIZE_MAX - terminal_capacity ||
        component_capacity + terminal_capacity > SIZE_MAX - connection_capacity ||
        component_capacity + terminal_capacity + connection_capacity >
            SIZE_MAX - net_capacity - 8u)
        return COPPER_TOPOLOGY_ERR_MEMORY;
    out->issue_capacity = component_capacity + terminal_capacity +
                          connection_capacity + net_capacity + 8u;
    if (component_capacity)
        out->components = (CopperTopologyComponent *)arena_alloc_array(
            arena, sizeof(*out->components), component_capacity);
    if (terminal_capacity)
        out->terminals = (CopperTopologyTerminal *)arena_alloc_array(
            arena, sizeof(*out->terminals), terminal_capacity);
    if (net_capacity)
        out->nets = (CopperTopologyNet *)arena_alloc_array(
            arena, sizeof(*out->nets), net_capacity);
    if (connection_capacity)
        out->connections = (CopperTopologyConnection *)arena_alloc_array(
            arena, sizeof(*out->connections), connection_capacity);
    out->issues = (CopperTopologyIssue *)arena_alloc_array(
        arena, sizeof(*out->issues), out->issue_capacity);
    if ((component_capacity && !out->components) ||
        (terminal_capacity && !out->terminals) ||
        (net_capacity && !out->nets) ||
        (connection_capacity && !out->connections) || !out->issues)
        return COPPER_TOPOLOGY_ERR_MEMORY;
    out->complete = true;
    return COPPER_TOPOLOGY_OK;
}

CopperTopologyStatus copper_topology_add_component(
    CopperTopology *t, const CopperTopologyComponentSpec *spec,
    const CopperTopologyPinSpec *pins, size_t pin_count,
    size_t *out_component_index) {
    if (!t || !t->arena || !spec || !spec->reference ||
        (pin_count && !pins)) return COPPER_TOPOLOGY_ERR_ARGUMENT;
    if (t->component_count >= t->component_capacity ||
        pin_count > t->terminal_capacity - t->terminal_count)
        return COPPER_TOPOLOGY_ERR_MEMORY;
    if (component_index(t, spec->reference) != SIZE_MAX) {
        issue_add(t, "DUPLICATE_REFERENCE", spec->reference, NULL, NULL,
                  "Component reference already exists in this topology.");
        return COPPER_TOPOLOGY_ERR_DUPLICATE_COMPONENT;
    }
    for (size_t i = 0; i < pin_count; ++i) {
        if (!pins[i].number) return COPPER_TOPOLOGY_ERR_ARGUMENT;
        for (size_t j = 0; j < i; ++j) {
            if (streq(pins[i].number, pins[j].number)) {
                issue_add(t, "DUPLICATE_PIN_NUMBER", spec->reference,
                          pins[i].number, NULL,
                          "Component definition contains duplicate pin numbers.");
                return COPPER_TOPOLOGY_ERR_UNKNOWN_PIN;
            }
        }
    }
    if (pin_count == 0u) {
        issue_add(t, "PIN_DEFINITION_MISSING", spec->reference, NULL, NULL,
                  "Component has no resolved pin definitions.");
    }
    size_t ci = t->component_count++;
    CopperTopologyComponent *c = &t->components[ci];
    memset(c, 0, sizeof(*c));
    c->uuid = spec->uuid;
    c->reference = spec->reference;
    c->value = spec->value;
    c->part_id = spec->part_id;
    c->lib_id = spec->lib_id;
    c->footprint = spec->footprint;
    c->first_terminal = (uint32_t)t->terminal_count;
    for (size_t i = 0; i < pin_count; ++i) {
        CopperTopologyTerminal *term = &t->terminals[t->terminal_count++];
        term->number = pins[i].number;
        term->name = pins[i].name;
        term->electrical_type = pins[i].electrical_type;
        term->footprint_pad = pins[i].footprint_pad;
        term->net_index = COPPER_TOPOLOGY_NO_NET;
        term->no_connect = false;
        ++c->terminal_count;
    }
    if (out_component_index) *out_component_index = ci;
    return COPPER_TOPOLOGY_OK;
}

CopperTopologyStatus copper_topology_add_resolved_part(
    CopperTopology *t, const PartRecord *part, const char *reference,
    const char *uuid, const char *value, size_t *out_component_index) {
    if (!t || !part || !reference) return COPPER_TOPOLOGY_ERR_ARGUMENT;
    CopperTopologyComponentSpec component;
    memset(&component, 0, sizeof(component));
    component.uuid = uuid;
    component.reference = reference;
    component.value = value;
    component.part_id = part->id;
    component.lib_id = part->symbol_id;
    component.footprint = part->footprint_id;
    CopperTopologyPinSpec *pins = NULL;
    if (part->pin_count) {
        pins = (CopperTopologyPinSpec *)arena_alloc_array(
            t->arena, sizeof(*pins), part->pin_count);
        if (!pins) return COPPER_TOPOLOGY_ERR_MEMORY;
        for (size_t i = 0; i < part->pin_count; ++i) {
            pins[i].number = part->pins[i].symbol_pin;
            pins[i].name = part->pins[i].terminal;
            pins[i].electrical_type = NULL;
            pins[i].footprint_pad = part->pins[i].footprint_pad;
        }
    }
    return copper_topology_add_component(t, &component, pins,
                                          part->pin_count,
                                          out_component_index);
}

CopperTopologyStatus copper_topology_add_net(
    CopperTopology *t, const CopperTopologyNetSpec *spec,
    size_t *out_net_index) {
    if (!t || !spec || !spec->name) return COPPER_TOPOLOGY_ERR_ARGUMENT;
    if (t->net_count >= t->net_capacity) return COPPER_TOPOLOGY_ERR_MEMORY;
    for (size_t i = 0; i < t->net_count; ++i) {
        if (streq(t->nets[i].name, spec->name)) {
            issue_add(t, "DUPLICATE_NET_NAME", NULL, NULL, spec->name,
                      "Net name already exists in this topology.");
            return COPPER_TOPOLOGY_ERR_ARGUMENT;
        }
    }
    size_t ni = t->net_count++;
    t->nets[ni].code = spec->code;
    t->nets[ni].name = spec->name;
    t->nets[ni].net_class = spec->net_class;
    t->nets[ni].connection_count = 0;
    if (out_net_index) *out_net_index = ni;
    return COPPER_TOPOLOGY_OK;
}

CopperTopologyStatus copper_topology_connect(
    CopperTopology *t, const char *reference, const char *pin_number,
    size_t net_index) {
    if (!t || !reference || !pin_number) return COPPER_TOPOLOGY_ERR_ARGUMENT;
    if (net_index >= t->net_count) return COPPER_TOPOLOGY_ERR_ARGUMENT;
    if (t->connection_count >= t->connection_capacity)
        return COPPER_TOPOLOGY_ERR_MEMORY;
    size_t ci = component_index(t, reference);
    if (ci == SIZE_MAX) {
        issue_add(t, "UNKNOWN_COMPONENT", reference, pin_number,
                  t->nets[net_index].name,
                  "Connection references an unknown component.");
        return COPPER_TOPOLOGY_ERR_UNKNOWN_COMPONENT;
    }
    size_t ti = terminal_global_index(t, ci, pin_number);
    if (ti == SIZE_MAX) {
        issue_add(t, "UNKNOWN_PIN", reference, pin_number,
                  t->nets[net_index].name,
                  "Connection references an unknown component pin.");
        return COPPER_TOPOLOGY_ERR_UNKNOWN_PIN;
    }
    CopperTopologyTerminal *term = &t->terminals[ti];
    if (term->net_index != COPPER_TOPOLOGY_NO_NET) {
        issue_add(t, "PIN_ASSIGNED_MULTIPLE_NETS", reference, pin_number,
                  t->nets[net_index].name,
                  "A component pin is already assigned to another net.");
        return COPPER_TOPOLOGY_ERR_DUPLICATE_PIN_ASSIGNMENT;
    }
    term->net_index = (uint32_t)net_index;
    CopperTopologyConnection *conn = &t->connections[t->connection_count++];
    conn->component_index = (uint32_t)ci;
    conn->terminal_index = (uint32_t)ti;
    conn->net_index = (uint32_t)net_index;
    ++t->nets[net_index].connection_count;
    return COPPER_TOPOLOGY_OK;
}

static size_t unique_symbol_pin_count(const SchematicIR *s,
                                      const SchematicSymbol *symbol) {
    const SchematicLibrarySymbol *lib = schematic_find_library_symbol(s, symbol->lib_id);
    if (!lib) return 0;
    size_t count = 0;
    for (size_t i = 0; i < lib->pin_count; ++i) {
        const SchematicLibPin *p = &s->library_pins[lib->first_pin + i];
        bool seen = false;
        for (size_t j = 0; j < i; ++j) {
            const SchematicLibPin *q = &s->library_pins[lib->first_pin + j];
            if (streq(p->number, q->number)) { seen = true; break; }
        }
        if (!seen) ++count;
    }
    return count;
}

CopperTopologyStatus copper_topology_build(
    Arena *arena, const SchematicIR *s, const KicadNetlist *n,
    CopperTopology *out) {
    if (!arena || !s || !out) return COPPER_TOPOLOGY_ERR_ARGUMENT;
    size_t terminal_capacity = 0;
    for (size_t i = 0; i < s->symbol_count; ++i) {
        size_t add = unique_symbol_pin_count(s, &s->symbols[i]);
        if (add > SIZE_MAX - terminal_capacity) return COPPER_TOPOLOGY_ERR_MEMORY;
        terminal_capacity += add;
    }
    size_t connection_capacity = n ? n->node_count : 0u;
    CopperTopologyStatus init_status = copper_topology_init(
        arena, s->symbol_count, terminal_capacity, n ? n->net_count : 0u,
        connection_capacity, out);
    if (init_status != COPPER_TOPOLOGY_OK) return init_status;
    out->has_netlist = n != NULL;

    for (size_t i = 0; i < s->symbol_count; ++i) {
        const SchematicSymbol *sym = &s->symbols[i];
        const SchematicLibrarySymbol *lib = schematic_find_library_symbol(s, sym->lib_id);
        CopperTopologyComponentSpec spec;
        spec.uuid = sym->uuid; spec.reference = sym->reference;
        spec.value = sym->value; spec.part_id = NULL;
        spec.lib_id = sym->lib_id; spec.footprint = sym->footprint;
        CopperTopologyPinSpec *pins = NULL;
        size_t pin_count = unique_symbol_pin_count(s, sym);
        if (pin_count) {
            pins = (CopperTopologyPinSpec *)arena_alloc_array(
                arena, sizeof(*pins), pin_count);
            if (!pins) return COPPER_TOPOLOGY_ERR_MEMORY;
            size_t at = 0;
            for (size_t p = 0; p < lib->pin_count; ++p) {
                const SchematicLibPin *lp = &s->library_pins[lib->first_pin + p];
                bool seen = false;
                for (size_t q = 0; q < p; ++q) {
                    const SchematicLibPin *prior = &s->library_pins[lib->first_pin + q];
                    if (streq(lp->number, prior->number)) { seen = true; break; }
                }
                if (seen) continue;
                pins[at].number = lp->number;
                pins[at].name = lp->name;
                pins[at].electrical_type = lp->electrical_type;
                ++at;
            }
        }
        CopperTopologyStatus add_status = copper_topology_add_component(
            out, &spec, pins, pin_count, NULL);
        if (add_status != COPPER_TOPOLOGY_OK) return add_status;
    }

    if (!n) {
        issue_add(out, "NETLIST_REQUIRED", NULL, NULL, NULL,
                  "No netlist supplied; terminal inventory is available but net assignments are unknown.");
        return COPPER_TOPOLOGY_INCOMPLETE;
    }
    for (size_t i = 0; i < n->net_count; ++i) {
        CopperTopologyNetSpec spec = {n->nets[i].code, n->nets[i].name,
                                      n->nets[i].net_class};
        CopperTopologyStatus add_status = copper_topology_add_net(out, &spec, NULL);
        if (add_status != COPPER_TOPOLOGY_OK) return add_status;
    }
    for (size_t i = 0; i < n->node_count; ++i) {
        const KicadNetlistNode *node = &n->nodes[i];
        if (node->net_index >= n->net_count) {
            issue_add(out, "NET_INDEX_OUT_OF_RANGE", node->reference, node->pin, NULL,
                      "Netlist node has an invalid net index.");
            continue;
        }
        CopperTopologyStatus connect_status = copper_topology_connect(
            out, node->reference, node->pin, node->net_index);
        if (connect_status == COPPER_TOPOLOGY_ERR_UNKNOWN_COMPONENT ||
            connect_status == COPPER_TOPOLOGY_ERR_UNKNOWN_PIN)
            continue;
        if (connect_status != COPPER_TOPOLOGY_OK) return connect_status;
    }
    return out->complete ? COPPER_TOPOLOGY_OK : COPPER_TOPOLOGY_INCOMPLETE;
}

CopperTopologyStatus copper_topology_validate(const CopperTopology *t) {
    if (!t || !t->arena || (t->component_count && !t->components) ||
        (t->terminal_count && !t->terminals) || (t->net_count && !t->nets))
        return COPPER_TOPOLOGY_ERR_ARGUMENT;
    for (size_t i = 0; i < t->component_count; ++i) {
        const CopperTopologyComponent *c = &t->components[i];
        if (!c->reference) return COPPER_TOPOLOGY_ERR_ARGUMENT;
        for (size_t j = 0; j < i; ++j)
            if (streq(c->reference, t->components[j].reference))
                return COPPER_TOPOLOGY_ERR_DUPLICATE_COMPONENT;
    }
    for (size_t i = 0; i < t->terminal_count; ++i) {
        if (t->terminals[i].net_index != COPPER_TOPOLOGY_NO_NET &&
            t->terminals[i].net_index >= t->net_count)
            return COPPER_TOPOLOGY_ERR_ARGUMENT;
    }
    return t->complete ? COPPER_TOPOLOGY_OK : COPPER_TOPOLOGY_INCOMPLETE;
}

const char *copper_topology_status_name(CopperTopologyStatus s) {
    switch (s) {
    case COPPER_TOPOLOGY_OK: return "OK";
    case COPPER_TOPOLOGY_INCOMPLETE: return "INCOMPLETE";
    case COPPER_TOPOLOGY_ERR_ARGUMENT: return "ERR_ARGUMENT";
    case COPPER_TOPOLOGY_ERR_MEMORY: return "ERR_MEMORY";
    case COPPER_TOPOLOGY_ERR_DUPLICATE_COMPONENT: return "ERR_DUPLICATE_COMPONENT";
    case COPPER_TOPOLOGY_ERR_DUPLICATE_PIN_ASSIGNMENT: return "ERR_DUPLICATE_PIN_ASSIGNMENT";
    case COPPER_TOPOLOGY_ERR_UNKNOWN_COMPONENT: return "ERR_UNKNOWN_COMPONENT";
    case COPPER_TOPOLOGY_ERR_UNKNOWN_PIN: return "ERR_UNKNOWN_PIN";
    default: return "ERR_UNKNOWN";
    }
}

#ifndef COPPER_TOPOLOGY_H
#define COPPER_TOPOLOGY_H

#include "netlist_kicad.h"
#include "schematic.h"
#include "part_catalogue.h"
#include "arena.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COPPER_TOPOLOGY_NO_NET UINT32_MAX

typedef enum CopperTopologyStatus {
    COPPER_TOPOLOGY_OK = 0,
    COPPER_TOPOLOGY_INCOMPLETE,
    COPPER_TOPOLOGY_ERR_ARGUMENT,
    COPPER_TOPOLOGY_ERR_MEMORY,
    COPPER_TOPOLOGY_ERR_DUPLICATE_COMPONENT,
    COPPER_TOPOLOGY_ERR_DUPLICATE_PIN_ASSIGNMENT,
    COPPER_TOPOLOGY_ERR_UNKNOWN_COMPONENT,
    COPPER_TOPOLOGY_ERR_UNKNOWN_PIN
} CopperTopologyStatus;

typedef struct CopperTopologyTerminal {
    const char *number;
    const char *name;
    const char *electrical_type;
    const char *footprint_pad;
    uint32_t net_index; /* COPPER_TOPOLOGY_NO_NET when not connected */
    bool no_connect;
} CopperTopologyTerminal;

typedef struct CopperTopologyComponent {
    const char *uuid;
    const char *reference;
    const char *value;
    const char *part_id;
    const char *lib_id;
    const char *footprint;
    uint32_t first_terminal;
    uint32_t terminal_count;
} CopperTopologyComponent;

typedef struct CopperTopologyNet {
    const char *code;
    const char *name;
    const char *net_class;
    uint32_t connection_count;
} CopperTopologyNet;

typedef struct CopperTopologyConnection {
    uint32_t component_index;
    uint32_t terminal_index; /* global terminal index */
    uint32_t net_index;
} CopperTopologyConnection;

typedef struct CopperTopologyIssue {
    const char *code;
    const char *reference;
    const char *pin;
    const char *net;
    const char *message;
} CopperTopologyIssue;

typedef struct CopperTopology {
    Arena *arena;
    CopperTopologyComponent *components;
    size_t component_count;
    CopperTopologyTerminal *terminals;
    size_t terminal_count;
    CopperTopologyNet *nets;
    size_t net_count;
    CopperTopologyConnection *connections;
    size_t connection_count;
    CopperTopologyIssue *issues;
    size_t issue_count;
    size_t component_capacity;
    size_t terminal_capacity;
    size_t net_capacity;
    size_t connection_capacity;
    size_t issue_capacity;
    bool has_netlist;
    bool complete;
} CopperTopology;

typedef struct CopperTopologyComponentSpec {
    const char *uuid;
    const char *reference;
    const char *value;
    const char *part_id;
    const char *lib_id;
    const char *footprint;
} CopperTopologyComponentSpec;

typedef struct CopperTopologyPinSpec {
    const char *number;
    const char *name;
    const char *electrical_type;
    const char *footprint_pad;
} CopperTopologyPinSpec;

typedef struct CopperTopologyNetSpec {
    const char *code;
    const char *name;
    const char *net_class;
} CopperTopologyNetSpec;

/* Fixed-capacity arena-backed builder for generated designs. */
CopperTopologyStatus copper_topology_init(
    Arena *arena, size_t component_capacity, size_t terminal_capacity,
    size_t net_capacity, size_t connection_capacity,
    CopperTopology *out);
CopperTopologyStatus copper_topology_add_component(
    CopperTopology *topology, const CopperTopologyComponentSpec *spec,
    const CopperTopologyPinSpec *pins, size_t pin_count,
    size_t *out_component_index);
/* Adds a catalogue-resolved component using its explicit symbol-pin to
 * footprint-pad mapping. Missing mappings are retained as an incomplete
 * component; they are never inferred from matching numbers here. */
CopperTopologyStatus copper_topology_add_resolved_part(
    CopperTopology *topology, const PartRecord *part,
    const char *reference, const char *uuid, const char *value,
    size_t *out_component_index);
CopperTopologyStatus copper_topology_add_net(
    CopperTopology *topology, const CopperTopologyNetSpec *spec,
    size_t *out_net_index);
CopperTopologyStatus copper_topology_connect(
    CopperTopology *topology, const char *reference, const char *pin_number,
    size_t net_index);

/* Imports a schematic terminal inventory and assigns connectivity from a
 * fresh KiCad netlist. Without a netlist, the result is explicitly incomplete. */
CopperTopologyStatus copper_topology_build(
    Arena *arena, const SchematicIR *schematic,
    const KicadNetlist *netlist, CopperTopology *out);

CopperTopologyStatus copper_topology_validate(const CopperTopology *topology);
const char *copper_topology_status_name(CopperTopologyStatus status);
const CopperTopologyComponent *copper_topology_find_component(
    const CopperTopology *topology, const char *reference);
const CopperTopologyTerminal *copper_topology_find_terminal(
    const CopperTopology *topology, const char *reference,
    const char *pin_number);

#ifdef __cplusplus
}
#endif
#endif

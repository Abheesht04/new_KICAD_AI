#ifndef COPPER_NETLIST_KICAD_H
#define COPPER_NETLIST_KICAD_H

#include "IR.h"
#include "arena.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum KicadNetlistStatus {
    KICAD_NETLIST_OK = 0,
    KICAD_NETLIST_ERR_ARGUMENT,
    KICAD_NETLIST_ERR_IO,
    KICAD_NETLIST_ERR_PARSE,
    KICAD_NETLIST_ERR_ROOT,
    KICAD_NETLIST_ERR_MEMORY,
    KICAD_NETLIST_ERR_RANGE
} KicadNetlistStatus;

typedef struct KicadNetlistComponent {
    const char *reference;
    const char *value;
    const char *footprint;
    const char *library;
    const char *part;
    const char *uuid;
} KicadNetlistComponent;

typedef struct KicadNetlistNode {
    const char *reference;
    const char *pin;
    const char *pin_function;
    const char *pin_type;
    uint32_t net_index;
} KicadNetlistNode;

typedef struct KicadNetlistNet {
    const char *code;
    const char *name;
    const char *net_class;
    uint32_t first_node;
    uint32_t node_count;
} KicadNetlistNet;

typedef struct KicadNetlist {
    Arena *arena;
    KicadNetlistComponent *components;
    size_t component_count;
    KicadNetlistNet *nets;
    size_t net_count;
    KicadNetlistNode *nodes;
    size_t node_count;
    const IRNode *source_root;
} KicadNetlist;

typedef struct KicadNetlistDiag {
    KicadNetlistStatus status;
    char message[160];
} KicadNetlistDiag;

KicadNetlistStatus kicad_netlist_parse(Arena *arena, const char *source,
                                       size_t source_length,
                                       KicadNetlist *out,
                                       KicadNetlistDiag *diag);
KicadNetlistStatus kicad_netlist_load(const char *path, Arena *arena,
                                     KicadNetlist *out,
                                     KicadNetlistDiag *diag);
const KicadNetlistNode *kicad_netlist_find_node(const KicadNetlist *netlist,
                                                const char *reference,
                                                const char *pin);
const char *kicad_netlist_status_name(KicadNetlistStatus status);

#ifdef __cplusplus
}
#endif
#endif

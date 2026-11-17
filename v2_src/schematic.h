#ifndef COPPER_SCHEMATIC_H
#define COPPER_SCHEMATIC_H

#include "IR.h"
#include "arena.h"
#include "g2_types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum SchematicStatus {
    SCHEMATIC_OK = 0,
    SCHEMATIC_ERR_ARGUMENT,
    SCHEMATIC_ERR_IO,
    SCHEMATIC_ERR_PARSE,
    SCHEMATIC_ERR_ROOT,
    SCHEMATIC_ERR_MEMORY,
    SCHEMATIC_ERR_RANGE
} SchematicStatus;

typedef enum SchematicLabelKind {
    SCHEMATIC_LABEL_LOCAL = 0,
    SCHEMATIC_LABEL_GLOBAL,
    SCHEMATIC_LABEL_HIERARCHICAL
} SchematicLabelKind;

typedef struct SchematicPoint {
    G2Point position;
} SchematicPoint;

typedef struct SchematicLibPin {
    const char *number;
    const char *name;
    const char *electrical_type;
    uint32_t unit;
} SchematicLibPin;

typedef struct SchematicLibrarySymbol {
    const char *lib_id;
    const char *reference_prefix;
    const char *default_value;
    const char *description;
    uint32_t first_pin;
    uint32_t pin_count;
    const IRNode *raw_node;
} SchematicLibrarySymbol;

typedef struct SchematicSymbolPin {
    const char *number;
    const char *uuid;
} SchematicSymbolPin;

typedef struct SchematicSymbol {
    const char *uuid;
    const char *lib_id;
    const char *reference;
    const char *value;
    const char *footprint;
    const char *datasheet;
    const char *description;
    const char *sheet_path;
    G2Point position;
    int32_t rotation_mdeg;
    uint32_t unit;
    bool mirrored_x;
    bool mirrored_y;
    bool in_bom;
    bool on_board;
    bool dnp;
    uint32_t first_pin;
    uint32_t pin_count;
    const IRNode *raw_node;
} SchematicSymbol;

typedef struct SchematicWire {
    const char *uuid;
    uint32_t first_point;
    uint32_t point_count;
    const IRNode *raw_node;
} SchematicWire;

typedef struct SchematicLabel {
    SchematicLabelKind kind;
    const char *text;
    const char *uuid;
    G2Point position;
    int32_t rotation_mdeg;
    const IRNode *raw_node;
} SchematicLabel;

typedef struct SchematicJunction {
    const char *uuid;
    G2Point position;
} SchematicJunction;

typedef struct SchematicNoConnect {
    const char *uuid;
    G2Point position;
} SchematicNoConnect;

typedef struct SchematicSheetInstance {
    const char *path;
    const char *page;
} SchematicSheetInstance;

typedef struct SchematicDiag {
    SchematicStatus status;
    char message[192];
    size_t unsupported_top_level_records;
    char first_unsupported[48];
} SchematicDiag;

typedef struct SchematicIR {
    Arena *arena;
    uint32_t file_version;
    const char *generator;
    const char *generator_version;
    const char *root_uuid;
    const char *paper;

    SchematicLibrarySymbol *library_symbols;
    size_t library_symbol_count;
    SchematicLibPin *library_pins;
    size_t library_pin_count;

    SchematicSymbol *symbols;
    size_t symbol_count;
    SchematicSymbolPin *symbol_pins;
    size_t symbol_pin_count;

    SchematicWire *wires;
    size_t wire_count;
    SchematicPoint *wire_points;
    size_t wire_point_count;

    SchematicLabel *labels;
    size_t label_count;
    SchematicJunction *junctions;
    size_t junction_count;
    SchematicNoConnect *no_connects;
    size_t no_connect_count;
    SchematicSheetInstance *sheet_instances;
    size_t sheet_instance_count;

    size_t unmodelled_top_level_records;
    const char *first_unmodelled_top_level;
    const IRNode *source_root; /* complete parsed source tree; arena-owned */
} SchematicIR;

SchematicStatus schematic_init(SchematicIR *out, Arena *arena);
SchematicStatus schematic_parse(Arena *arena, const char *source,
                                size_t source_length, SchematicIR *out,
                                SchematicDiag *diag);
SchematicStatus schematic_load_kicad(const char *path, Arena *arena,
                                     SchematicIR *out, SchematicDiag *diag);
SchematicStatus schematic_validate(const SchematicIR *schematic,
                                   SchematicDiag *diag);
const SchematicLibrarySymbol *schematic_find_library_symbol(
    const SchematicIR *schematic, const char *lib_id);
const SchematicSymbol *schematic_find_symbol_uuid(
    const SchematicIR *schematic, const char *uuid);
const char *schematic_status_name(SchematicStatus status);

#ifdef __cplusplus
}
#endif
#endif

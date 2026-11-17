#ifndef COPPER_DESIGN_H
#define COPPER_DESIGN_H

#include "board.h"
#include "copper_project.h"
#include "design_parity.h"
#include "netlist_kicad.h"
#include "project_kicad.h"
#include "report.h"
#include "schematic.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum CopperDesignStatus {
    COPPER_DESIGN_OK = 0,
    COPPER_DESIGN_ERR_ARGUMENT,
    COPPER_DESIGN_ERR_PROJECT,
    COPPER_DESIGN_ERR_MISSING_SOURCE,
    COPPER_DESIGN_ERR_PROJECT_SETTINGS,
    COPPER_DESIGN_ERR_SCHEMATIC,
    COPPER_DESIGN_ERR_BOARD,
    COPPER_DESIGN_ERR_NETLIST,
    COPPER_DESIGN_ERR_MEMORY,
    COPPER_DESIGN_ERR_HASH
} CopperDesignStatus;

typedef struct CopperDesignSource {
    const char *kind;
    const char *path;
    const char *sha256;
    bool present;
} CopperDesignSource;

typedef struct CopperDesignImportOptions {
    const char *project_directory;
    const char *netlist_path; /* optional explicit fresh .kicad_net */
    size_t diagnostic_capacity;
} CopperDesignImportOptions;

typedef struct CopperDesign {
    Arena *arena;
    const char *name;
    CopperProject project;
    ProjectSettingsIR project_settings;
    SchematicIR schematic;
    Board board;
    KicadNetlist netlist;
    bool has_board;
    bool has_netlist;
    bool has_project_settings;
    bool has_schematic;
    BoardDiag board_diag;
    DesignParitySummary parity;
    CopperDesignSource sources[4];
    size_t source_count;
    CopperDiagnosticReport diagnostics;
} CopperDesign;

CopperDesignStatus copper_design_import_project(
    Arena *arena, const CopperDesignImportOptions *options,
    CopperDesign *out);
const char *copper_design_status_name(CopperDesignStatus status);

#ifdef __cplusplus
}
#endif
#endif

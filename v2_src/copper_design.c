#include "copper_design.h"
#include "filesystem.h"

#include <stdio.h>
#include <string.h>

static char *design_strdup(Arena *arena, const char *s) {
    size_t n; char *copy;
    if (!arena || !s) return NULL;
    n = strlen(s); copy = (char *)arena_alloc(arena, n + 1u);
    if (!copy) return NULL;
    memcpy(copy, s, n + 1u);
    return copy;
}
static void add_diagnostic(CopperDesign *d, DiagnosticSeverity severity,
                           const char *code, const char *file,
                           const char *object, const char *message) {
    CopperDiagnostic item;
    memset(&item, 0, sizeof(item)); item.severity = severity;
    item.code = code; item.file = file; item.object = object; item.message = message;
    (void)diagnostic_report_add(&d->diagnostics, &item);
}
static bool add_source(CopperDesign *d, const char *kind, const char *path,
                       bool required) {
    if (d->source_count >= sizeof(d->sources) / sizeof(d->sources[0])) return false;
    CopperDesignSource *s = &d->sources[d->source_count++];
    s->kind = kind; s->path = path; s->present = path && fs_file_exists(path);
    if (!s->present) {
        if (required) add_diagnostic(d, DIAG_ERROR, "SOURCE_FILE_MISSING", path, NULL,
                                    "Required KiCad project source file is missing.");
        return !required;
    }
    char hash[FS_SHA256_HEX_SIZE];
    if (fs_sha256_file_hex(path, hash) != FS_OK) {
        add_diagnostic(d, DIAG_ERROR, "SOURCE_HASH_FAILED", path, NULL,
                       "Could not calculate SHA-256 for source file.");
        return false;
    }
    s->sha256 = design_strdup(d->arena, hash);
    if (!s->sha256) return false;
    add_diagnostic(d, DIAG_INFO, "SOURCE_HASHED", path, NULL,
                   "Source file was read and its SHA-256 recorded.");
    return true;
}
CopperDesignStatus copper_design_import_project(
    Arena *arena, const CopperDesignImportOptions *options,
    CopperDesign *out) {
    CopperProjectError project_error;
    CopperProjectStatus project_status;
    SchematicDiag schematic_diag;
    ProjectSettingsDiag settings_diag;
    if (!arena || !options || !options->project_directory || !out)
        return COPPER_DESIGN_ERR_ARGUMENT;
    memset(out, 0, sizeof(*out)); out->arena = arena;
    diagnostic_report_init(&out->diagnostics, arena,
                           options->diagnostic_capacity ? options->diagnostic_capacity : 256u);
    project_status = copper_project_open(arena, options->project_directory,
                                         &out->project, &project_error);
    if (project_status != COPPER_PROJECT_OK) {
        add_diagnostic(out, DIAG_ERROR, "PROJECT_OPEN_FAILED", options->project_directory,
                       NULL, project_error.message ? project_error.message : copper_project_status_message(project_status));
        return COPPER_DESIGN_ERR_PROJECT;
    }
    out->name = out->project.name;
    add_diagnostic(out, DIAG_INFO, "PROJECT_OPENED", out->project.root_dir,
                   out->name, "KiCad project directory and project settings file were located.");

    if (!add_source(out, "project_settings", out->project.project_file, true))
        return COPPER_DESIGN_ERR_HASH;
    if (!add_source(out, "schematic", out->project.schematic_file, out->project.has_schematic))
        return COPPER_DESIGN_ERR_HASH;
    if (!add_source(out, "pcb", out->project.board_file, out->project.has_board))
        return COPPER_DESIGN_ERR_HASH;

    ProjectSettingsStatus ps = project_settings_load_kicad(out->project.project_file,
                                                            arena, &out->project_settings,
                                                            &settings_diag);
    if (ps != PROJECT_SETTINGS_OK) {
        add_diagnostic(out, DIAG_ERROR, "PROJECT_SETTINGS_PARSE_FAILED",
                       out->project.project_file, NULL, settings_diag.message);
        return COPPER_DESIGN_ERR_PROJECT_SETTINGS;
    }
    out->has_project_settings = true;
    add_diagnostic(out, DIAG_INFO, "PROJECT_SETTINGS_PARSED", out->project.project_file,
                   NULL, "KiCad net classes, board rule settings and ERC/DRC rule severities were imported.");

    if (!out->project.has_schematic) {
        add_diagnostic(out, DIAG_ERROR, "SCHEMATIC_MISSING", out->project.schematic_file,
                       NULL, "This project has no matching .kicad_sch file.");
        return COPPER_DESIGN_ERR_MISSING_SOURCE;
    }
    SchematicStatus ss = schematic_load_kicad(out->project.schematic_file, arena,
                                               &out->schematic, &schematic_diag);
    if (ss != SCHEMATIC_OK) {
        add_diagnostic(out, DIAG_ERROR, "SCHEMATIC_PARSE_FAILED",
                       out->project.schematic_file, NULL, schematic_diag.message);
        return COPPER_DESIGN_ERR_SCHEMATIC;
    }
    out->has_schematic = true;
    add_diagnostic(out, DIAG_INFO, "SCHEMATIC_PARSED", out->project.schematic_file,
                   NULL, "Schematic S-expression was parsed into typed symbol, pin, wire, label and junction records; the complete source tree is retained.");
    if (out->schematic.unmodelled_top_level_records) {
        char message[192];
        (void)snprintf(message, sizeof(message),
                       "Schematic contains %zu unmodelled top-level records; first unsupported record: %s. Semantic coverage is incomplete.",
                       out->schematic.unmodelled_top_level_records,
                       out->schematic.first_unmodelled_top_level ?
                           out->schematic.first_unmodelled_top_level : "unknown");
        add_diagnostic(out, DIAG_UNVERIFIED, "SCHEMATIC_FEATURES_UNMODELLED",
                       out->project.schematic_file, NULL, message);
    }

    if (out->project.has_board) {
        BoardStatus bs = board_load_kicad(out->project.board_file, arena,
                                          &out->board, &out->board_diag);
        if (bs != BOARD_OK) {
            add_diagnostic(out, DIAG_ERROR, "PCB_PARSE_FAILED", out->project.board_file,
                           NULL, out->board_diag.message);
            return COPPER_DESIGN_ERR_BOARD;
        }
        bs = board_validate(&out->board, &out->board_diag);
        if (bs != BOARD_OK) {
            add_diagnostic(out, DIAG_ERROR, "PCB_MODEL_INVALID", out->project.board_file,
                           NULL, out->board_diag.message);
            return COPPER_DESIGN_ERR_BOARD;
        }
        out->has_board = true;
        add_diagnostic(out, DIAG_INFO, "PCB_PARSED", out->project.board_file,
                       NULL, "PCB S-expression was parsed and its represented typed model passed structural validation.");
        if (out->board_diag.unmodelled_records || out->board_diag.unmodelled_geometry_records ||
            out->board_diag.inexact_rotations || out->board_diag.unsupported_layers) {
            add_diagnostic(out, DIAG_UNVERIFIED, "PCB_IMPORT_COVERAGE_INCOMPLETE",
                           out->project.board_file, NULL,
                           "The PCB importer reported unmodelled records, geometry, rotations or layers. Structural model validity does not imply complete physical feature coverage.");
        }
    } else {
        add_diagnostic(out, DIAG_UNVERIFIED, "PCB_NOT_PRESENT", NULL, NULL,
                       "Project has no matching .kicad_pcb; physical design and schematic/PCB parity are unavailable.");
    }

    const char *netlist_path = options->netlist_path;
    char default_netlist[FS_PATH_MAX];
    if (!netlist_path) {
        char filename[COPPER_PROJECT_NAME_MAX + 32];
        int n = snprintf(filename, sizeof(filename), "%s.kicad_net", out->name);
        if (n > 0 && (size_t)n < sizeof(filename) &&
            fs_path_join(default_netlist, sizeof(default_netlist), out->project.root_dir, filename) == FS_OK &&
            fs_file_exists(default_netlist)) netlist_path = default_netlist;
    }
    if (netlist_path) {
        KicadNetlistDiag nd;
        KicadNetlistStatus ns = kicad_netlist_load(netlist_path, arena, &out->netlist, &nd);
        if (ns != KICAD_NETLIST_OK) {
            add_diagnostic(out, DIAG_ERROR, "NETLIST_PARSE_FAILED", netlist_path, NULL, nd.message);
            return COPPER_DESIGN_ERR_NETLIST;
        }
        out->has_netlist = true;
        if (out->source_count < 4u) (void)add_source(out, "netlist", netlist_path, true);
        add_diagnostic(out, DIAG_INFO, "NETLIST_PARSED", netlist_path, NULL,
                       "KiCad S-expression netlist components, nets and pin nodes were imported.");
    }

    if (out->has_board)
        (void)design_parity_check(&out->schematic, &out->board,
                                  out->has_netlist ? &out->netlist : NULL,
                                  &out->diagnostics, &out->parity);
    else
        add_diagnostic(out, DIAG_UNVERIFIED, "DESIGN_PARITY_NOT_RUN", NULL, NULL,
                       "Schematic-to-PCB component and pad-net parity requires a PCB source file.");
    return COPPER_DESIGN_OK;
}
const char *copper_design_status_name(CopperDesignStatus s) {
    static const char *const names[] = {"OK", "ARGUMENT", "PROJECT", "MISSING_SOURCE", "PROJECT_SETTINGS", "SCHEMATIC", "BOARD", "NETLIST", "MEMORY", "HASH"};
    return (unsigned)s < sizeof(names) / sizeof(names[0]) ? names[s] : "UNKNOWN";
}

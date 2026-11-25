#include "IR.h"
#include "design_board.h"
#include "copper_design.h"
#include "copper_design_json.h"
#include "dfm_check.h"
#include "fab_rules.h"
#include "report.h"
#include "filesystem.h"
#include "kicad_assets.h"
#include "part_catalogue.h"
#include "design_resolver.h"
#include "kicad_asset_resolver.h"
#include "design_resolution_json.h"
#include "jlc_catalogue.h"
#include "topology.h"
#include "topology_json.h"
#include "engineering_loop.h"
#include "engineering_loop_json.h"
#include "optimization_loop.h"
#include "optimization_loop_json.h"
#include "schematic.h"
#include "netlist_kicad.h"
#include "intent_check.h"
#include "template_gen.h"
#include "autoroute.h"
#include "json.h"
#include "parts_check.h"
#include "blocks.h"
#include "compose.h"
#include "place_intent.h"
#include "sch_rules.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <math.h>

#define CLI_PATH_MAX FS_PATH_MAX

static bool cli_copy_environment_value(const char *name, char *out,
                                       size_t capacity) {
    if (!name || !out || capacity == 0) return false;
    out[0] = '\0';
#if defined(_WIN32) && (defined(_MSC_VER) || defined(_UCRT)) /* _dupenv_s: MSVC and UCRT runtimes */
    char *value = NULL;
    size_t value_size = 0;
    if (_dupenv_s(&value, &value_size, name) != 0 || value == NULL) {
        free(value);
        return false;
    }
    size_t length = strlen(value);
    bool fits = length < capacity;
    if (fits) memcpy(out, value, length + 1u);
    free(value);
    return fits;
#else
    const char *value = getenv(name);
    if (value == NULL) return false;
    size_t length = strlen(value);
    if (length >= capacity) return false;
    memcpy(out, value, length + 1u);
    return true;
#endif
}

static void print_usage(void) {
    puts("Copper V2 — deterministic PCB engineering core");
    puts("");
    puts("User commands:");
    puts("  copperv2 user board-inspect <board.kicad_pcb>");
    puts("  copperv2 user parts-used <board.kicad_pcb> [--catalogue FILE.parts] [--jlc-catalogue FILE.jlc] [--jlc-max-age-days N]");
    puts("  copperv2 user dfm-check <board.kicad_pcb> --rules FILE.rules [--copper-oz 1|2|2.5|3.5|4.5] [--edge routed|vcut] [--mask-color green|red|yellow|blue|purple|black|white] [--report FILE]");
    puts("  copperv2 user manufacturing-instructions <board.kicad_pcb> --rules FILE.rules --out FILE.md [--copper-oz 1|2|2.5|3.5|4.5] [--edge routed|vcut] [--mask-color green|red|yellow|blue|purple|black|white]");
    puts("");
    puts("Design commands:");
    puts("  copper design import <project-directory> [--out FILE] [--netlist FILE] [--report FILE] [--manifest FILE]");
    puts("  copper design resolve-part <parts.sx> [--part-id ID | --kind R|C|L|D|Q|M|U|J|S] [--mpn MPN] [--text QUERY] [--out design.json]");
    puts("  copper design check-intent <intent.json> --symbol-root DIR --footprint-root DIR [--out report.json]");
    puts("      grounding gate for a model-written design intent: exit 0 GROUNDED, 2 REJECTED (findings listed), 64 usage");
    puts("  copper design generate <requirements.json> --templates FILE --symbol-root DIR --footprint-root DIR --out DIR [--prompt FILE]");
    puts("  copper design route <board.kicad_pcb> --rules FILE --fab-profile FILE --out FILE [--routing routing.json] [--report FILE]");
    puts("  copper design compose <design.compose.json> --blocks DIR --parts FILE.parts [--parts ...] --symbol-root DIR --out DIR [--prompt FILE]");
    puts("      join checked blocks into intent.json; exit 0 COMPOSED, 3 NEEDS_INPUT, 4 NOT_IMPLEMENTED, 1 REFUSED, 2 INVALID_INPUT");
    puts("  copper design place-problem <intent.json> --hints layout_hints.json --rules FILE --footprint-root DIR --out PROBLEM");
    puts("  copper design place-apply <intent.json> --hints layout_hints.json --placer PLACER.json --out-intent FILE --out-routing FILE");
    puts("      placement with the C placer (`copper-core place PROBLEM ITERATIONS SEEDS` in between); exit 5 NO_LEGAL_POSITION");
    puts("  copper design schematic-rules <project dir> --symbol-root DIR [--hints layout_hints.json]");
    puts("      PWR_FLAGs on the hints' external-supply nets, and the project's sym-lib-table and fp-lib-table (Step 3d)");
    puts("      deterministic design from analysed requirements (circuit templates): writes DIR/intent.json and DIR/generation.json;");
    puts("      exit 0 GENERATED, 2 INVALID_INPUT, 3 NEEDS_INPUT (questions), 4 NOT_IMPLEMENTED, 5 TEMPLATE_ERROR, 1 error, 64 usage");
    puts("  copper user design prompt \"<human requirements>\" [--out request.json]");
    puts("  copper user topology show <design.kicad_sch> --netlist <fresh.kicad_net> [--out topology.json]");
    puts("  copper dev topology build <design.kicad_sch> --netlist <fresh.kicad_net> --out topology.json");
    puts("  copper dev topology validate <design.kicad_sch> --netlist <fresh.kicad_net>");
    puts("  copper dev engineering run <design.kicad_sch> --netlist FILE --catalogue FILE.parts [--part REF=ID] [--voltage-source REF=PIN+,PIN-:V] [--board FILE --rules FILE] [--propose-placement] [--propose-routes] [--route-grid-nm N --trace-width-nm N --clearance-nm N --edge-clearance-nm N] [--out design.json]");
    puts("  copper user engineering run <design.kicad_sch> --netlist FILE --catalogue FILE.parts [--part REF=ID] [--voltage-source REF=PIN+,PIN-:V] [--board FILE --rules FILE] [--propose-placement] [--propose-routes] [--route-grid-nm N --trace-width-nm N --clearance-nm N --edge-clearance-nm N] [--out design.json]");
    puts("  copper user optimize evaluate <board.kicad_pcb> --rules FILE --today YYYY-MM-DD --copper-oz-x10 N --edge routed|vcut [--step-nm N --component-gap-nm N --move-edge-clearance-nm N] [--propose-routes --route-grid-nm N --trace-width-nm N --clearance-nm N --route-edge-clearance-nm N] [--max-candidates N] [--out optimization.json]");
    puts("  copper user optimize apply <board.kicad_pcb> --candidate-id HEX [same evaluation options] --out action.json");
    puts("      [--user-catalogue FILE.parts] [--supplier-catalogue FILE.parts] [--source user|supplier|kicad]");
    puts("      [--symbol-root DIR] [--footprint-root DIR] [--copper-asset-root DIR] [--project-root DIR]");
    puts("      [--sym-lib-table FILE] [--fp-lib-table FILE]");
    puts("");
    puts("Developer commands:");
    puts("  copperv2 dev board inspect <board.kicad_pcb>");
    puts("  copperv2 dev board validate <board.kicad_pcb>");
    puts("  copperv2 dev rules validate <FILE.rules>");
    puts("  copper dev optimize evaluate <board.kicad_pcb> --rules FILE --today YYYY-MM-DD --copper-oz-x10 N --edge routed|vcut [--step-nm N --component-gap-nm N --move-edge-clearance-nm N] [--propose-routes --route-grid-nm N --trace-width-nm N --clearance-nm N --route-edge-clearance-nm N] [--max-candidates N] [--out optimization.json]");
    puts("  copper dev optimize apply <board.kicad_pcb> --candidate-id HEX --rules FILE --today YYYY-MM-DD --copper-oz-x10 N --edge routed|vcut --out action.json");
    puts("  copper dev optimize run <board.kicad_pcb> --policy greedy --iterations N --rules FILE --today YYYY-MM-DD --copper-oz-x10 N --edge routed|vcut --out optimization-run.json");
    puts("");
    puts("Existing asset/catalogue commands:");
    puts("  copperv2 assets inspect-symbol <library.kicad_sym> [symbol-name]");
    puts("  copperv2 assets inspect-footprint <footprint.kicad_mod>");
    puts("  copperv2 assets import-symbol-library <nickname> <library.kicad_sym> --root DIR");
    puts("  copperv2 assets import-footprint-library <nickname> <library.pretty> --root DIR");
    puts("  copperv2 assets import-kicad <symbols-dir> <footprints-dir> --root DIR");
    puts("  copperv2 catalogue create <file.parts>");
    puts("  copperv2 catalogue validate <file.parts> [--symbols-root DIR --footprints-root DIR]");
    puts("  copperv2 block check <block.json> [--parts FILE.parts]... [--out report.json]");
    puts("  copperv2 block list <block.json>...   (what each block offers, as JSON: ports and parameters)");
    puts("      one copper.block/1 definition; exit 0 VALID, 3 UNVERIFIED (no --parts), 2 INVALID");
    puts("  copperv2 catalogue check <file.parts> [--symbols-root DIR --footprints-root DIR] [--out report.json]");
    puts("      every problem of every part; exit 0 PASS, 3 UNVERIFIED, 1 FAIL, 2 INVALID_INPUT");
    puts("  copperv2 catalogue install <file.parts> --scope project|user [--project-root DIR] [--user-root DIR]");
    puts("  copperv2 catalogue list --scope project|user [--project-root DIR] [--user-root DIR]");
    puts("  copperv2 catalogue resolve <part-id> [--project-root DIR] [--user-root DIR] [--bundled-root DIR]");
    puts("");
    puts("Environment: COPPER_HOME, COPPER_PROJECT, COPPER_FAB_RULES");
    puts("All engineering geometry is integer nanometres internally; CLI displays millimetres.");
}

static const char *cli_option_value(int argc, char **argv, const char *name) {
    for (int i = 0; i + 1 < argc; ++i)
        if (strcmp(argv[i], name) == 0) return argv[i + 1];
    return NULL;
}

static bool cli_has_flag(int argc, char **argv, const char *name) {
    for (int i = 0; i < argc; ++i)
        if (strcmp(argv[i], name) == 0) return true;
    return false;
}

static bool cli_parse_positive_nm(const char *text, G2Nm *out) {
    if (!text || !out || !text[0]) return false;
    char *end = NULL;
    errno = 0;
    long long value = strtoll(text, &end, 10);
    if (errno == ERANGE || !end || end == text || *end != '\0' || value <= 0) return false;
    *out = (G2Nm)value;
    return true;
}

static void current_date_utc(char out[11]) {
    time_t now = time(NULL);
#ifdef _WIN32
    struct tm utc_value;
    struct tm *utc = gmtime_s(&utc_value, &now) == 0 ? &utc_value : NULL;
#else
    struct tm *utc = gmtime(&now);
#endif
    if (!utc || strftime(out, 11, "%Y-%m-%d", utc) != 10) {
        memcpy(out, "1970-01-01", 11);
    }
}

static bool cli_load_board(const char *path, Arena *arena, Board *board,
                           BoardDiag *diag) {
    arena_init(arena, 65536);
    BoardStatus st = board_load_kicad(path, arena, board, diag);
    if (st != BOARD_OK) {
        fprintf(stderr, "ERROR: board load failed: %s (%s)\n",
                diag && diag->message[0] ? diag->message : "no diagnostic", path);
        arena_destroy(arena);
        return false;
    }
    st = board_validate(board, diag);
    if (st != BOARD_OK) {
        fprintf(stderr, "ERROR: board validation failed: %s\n",
                diag && diag->message[0] ? diag->message : "invalid model");
        arena_destroy(arena);
        return false;
    }
    return true;
}

static void print_board_summary(const Board *b, const BoardDiag *d) {
    printf("KiCad board version: %u\n", b->kicad_file_version);
    printf("Copper layers: %u  Total layer records: %zu\n", b->rules.copper_layers, b->layer_count);
    printf("Nets: %zu  Footprints: %zu  Pads: %zu\n", b->net_count, b->footprint_count, b->pad_count);
    printf("Tracks: %zu  Arc tracks: %zu  Vias: %zu  Zones: %zu\n",
           b->track_count, b->arc_track_count, b->via_count, b->zone_count);
    printf("Edge.Cuts primitives: %zu  Board thickness: ", b->edge_count);
    if (b->rules.board_thickness == BOARD_UNSET_NM) puts("not specified");
    else printf("%.6f mm\n", (double)b->rules.board_thickness / 1000000.0);
    printf("Importer coverage: skipped records=%zu, unmodelled geometry=%zu, inexact pad rotations=%zu, unsupported layers=%zu\n",
           d->unmodelled_records, d->unmodelled_geometry_records, d->inexact_rotations, d->unsupported_layers);
    if (d->first_unmodelled[0]) printf("First unmodelled record: %s\n", d->first_unmodelled);
    puts("Raw KiCad syntax tree: retained in arena for loss-preserving consumers");
}

static int command_user_board_inspect(const char *path) {
    Arena arena; Board b; BoardDiag diag;
    if (!cli_load_board(path, &arena, &b, &diag)) return 1;
    print_board_summary(&b, &diag);
    arena_destroy(&arena);
    return 0;
}

static int command_dev_board_validate(const char *path) {
    Arena arena; Board b; BoardDiag diag;
    if (!cli_load_board(path, &arena, &b, &diag)) return 1;
    printf("PASS: board model references and geometry are structurally valid\n");
    print_board_summary(&b, &diag);
    arena_destroy(&arena);
    return 0;
}

static const char *jlc_class_name(JlcClass c) {
    switch (c) {
    case JLC_CLASS_BASIC: return "BASIC";
    case JLC_CLASS_PREFERRED: return "PREFERRED";
    case JLC_CLASS_EXTENDED: return "EXTENDED";
    default: return "UNKNOWN";
    }
}

static int command_user_parts_used(int argc, char **argv) {
    const char *board_path = argv[3];
    const char *catalogue_path = cli_option_value(argc, argv, "--catalogue");
    const char *jlc_path = cli_option_value(argc, argv, "--jlc-catalogue");
    uint32_t max_age = 0;
    const char *age_arg = cli_option_value(argc, argv, "--jlc-max-age-days");
    char age_env[64];
    if (!age_arg && cli_copy_environment_value("COPPER_JLC_MAX_AGE_DAYS",
                                               age_env, sizeof(age_env)))
        age_arg = age_env;
    if (age_arg) {
        char *end = NULL; unsigned long value = strtoul(age_arg, &end, 10);
        if (!end || *end || value > UINT32_MAX) { fprintf(stderr, "ERROR: JLC maximum age must be an unsigned integer\n"); return 64; }
        max_age = (uint32_t)value;
    }
    if (jlc_path && !age_arg) {
        fprintf(stderr, "ERROR: select catalogue freshness explicitly with --jlc-max-age-days N or COPPER_JLC_MAX_AGE_DAYS\n");
        return 64;
    }
    Arena arena; Board b; BoardDiag diag;
    if (!cli_load_board(board_path, &arena, &b, &diag)) return 1;
    PartCatalogue catalogue = {0}; bool have_catalogue = false;
    if (catalogue_path) {
        PartCatalogueError error;
        PartCatalogueStatus st = part_catalogue_parse_file(&arena, catalogue_path, &catalogue, &error);
        if (st != PART_CATALOGUE_OK) {
            fprintf(stderr, "ERROR: cannot load parts catalogue %s: %s\n",
                    catalogue_path, part_catalogue_status_name(st));
            arena_destroy(&arena); return 1;
        }
        have_catalogue = true;
    }
    JlcCatalogue jlc; bool have_jlc = false;
    if (jlc_path) {
        char today[11]; current_date_utc(today);
        JlcCatalogueStatus st = jlc_catalogue_load(jlc_path, &arena, today, max_age, &jlc);
        if (st != JLC_CATALOGUE_OK) {
            fprintf(stderr, "ERROR: cannot load JLC catalogue %s: %s\n", jlc_path, jlc_catalogue_status_name(st));
            arena_destroy(&arena); return 1;
        }
        have_jlc = true;
    }
    puts("REFERENCE  VALUE        FOOTPRINT                         PART ID / MPN                         LCSC ID   CLASS      STOCK       UNIT PRICE");
    for (size_t i = 0; i < b.footprint_count; ++i) {
        const BoardFootprint *fp = &b.footprints[i];
        const PartRecord *match = NULL; size_t matches = 0;
        if (have_catalogue) {
            if (fp->part_id) {
                match = part_catalogue_find(&catalogue, fp->part_id);
                matches = match ? 1u : 0u;
            } else if (fp->mpn) {
                for (size_t p = 0; p < catalogue.part_count; ++p) {
                    const PartRecord *candidate = &catalogue.parts[p];
                    if (candidate->mpn && strcmp(candidate->mpn, fp->mpn) == 0 &&
                        (!fp->manufacturer || !candidate->manufacturer || strcmp(candidate->manufacturer, fp->manufacturer) == 0)) {
                        match = candidate; ++matches;
                    }
                }
            } else if (fp->lib_id) {
                for (size_t p = 0; p < catalogue.part_count; ++p) {
                    const PartRecord *candidate = &catalogue.parts[p];
                    if (candidate->footprint_id && strcmp(candidate->footprint_id, fp->lib_id) == 0) {
                        match = candidate; ++matches;
                    }
                }
            }
        }
        const JlcPart *jp = NULL;
        if (have_jlc) {
            if (fp->lcsc_id) jp = jlc_catalogue_find_lcsc_id(&jlc, fp->lcsc_id);
            if (!jp && fp->part_id) jp = jlc_catalogue_find_part_id(&jlc, fp->part_id);
            if (!jp && matches == 1 && match) jp = jlc_catalogue_find_part_id(&jlc, match->id);
        }
        const char *part_label = matches == 1 && match ? match->id : (fp->mpn ? fp->mpn : "(not resolved)");
        printf("%-10s %-12s %-32s %-36s ",
               fp->reference ? fp->reference : "?", fp->value ? fp->value : "?",
               fp->lib_id ? fp->lib_id : "(unknown)", part_label);
        if (jp) {
            printf("%-9s %-10s ", jp->lcsc_id, jlc_class_name(jp->part_class));
            if (jp->has_stock) printf("%-11u ", jp->stock); else printf("%-11s ", "not listed");
            if (jp->has_price) printf("$%lld.%06lld",
                (long long)(jp->unit_price_micro_usd / 1000000),
                (long long)(jp->unit_price_micro_usd % 1000000));
            else printf("not listed");
            if (jp->observed_date) printf("  as-of %s", jp->observed_date);
            putchar('\n');
        } else {
            printf("%-9s %-10s %-11s %s\n", "-", "-", "-",
                   !have_catalogue ? (have_jlc ? "no board identity to join" : "catalogue not supplied") :
                   matches > 1 ? "AMBIGUOUS footprint mapping; add exact Copper Part ID property" :
                   matches == 1 ? (have_jlc ? "no LCSC mapping in JLC catalogue" : "part resolved; no JLC catalogue supplied") :
                   "NO exact part identity; footprint alone is not an orderable MPN");
        }
    }
    if (b.footprint_count == 0) puts("No footprints found.");
    arena_destroy(&arena);
    return 0;
}

static int run_dfm_command(const char *board_path, const char *rules_path,
                           uint32_t copper_oz_x10, const char *edge_type,
                           const char *mask_color,
                           char **out_text, size_t *out_length,
                           ReportStatus *out_verdict, FabRuleSet *out_rules,
                           Board *out_board, BoardDiag *out_diag, Arena *arena) {
    if (!board_path || !rules_path || !arena || !out_text || !out_length ||
        !out_verdict || !out_rules || !out_board || !out_diag) return 1;
    if (!cli_load_board(board_path, arena, out_board, out_diag)) return 1;
    char today[11]; current_date_utc(today);
    FabStatus fs = fab_rules_load(rules_path, arena, today, 90, out_rules);
    if (fs != FAB_OK) {
        fprintf(stderr, "ERROR: fabrication rules rejected: %s\n", fab_status_name(fs));
        arena_destroy(arena); return 1;
    }
    size_t total_objects = out_board->track_count + out_board->arc_track_count +
                           out_board->via_count + out_board->pad_count + out_board->footprint_count;
    size_t cap = 512u + out_rules->rule_count * 8u + total_objects * 4u;
    if (cap < 512u || cap > 262144u) cap = 262144u;
    CopperReport report;
    report_init(&report, arena, cap);
    if (!report.findings) { fprintf(stderr, "ERROR: report allocation failed\n"); arena_destroy(arena); return 1; }
    DfmOptions options = {copper_oz_x10, edge_type, false, mask_color};
    DfmStatus ds = dfm_check_board(out_board, out_diag, out_rules, &options, arena, &report);
    if (ds != DFM_OK) {
        fprintf(stderr, "ERROR: DFM execution failed: %d\n", (int)ds);
        arena_destroy(arena); return 1;
    }
    size_t text_cap = 4u * 1024u * 1024u;
    char *text = (char *)arena_alloc(arena, text_cap);
    if (!text) { fprintf(stderr, "ERROR: report text allocation failed\n"); arena_destroy(arena); return 1; }
    size_t n = report_format_text(&report, board_path, text, text_cap);
    if (n >= text_cap) { fprintf(stderr, "ERROR: report text exceeded configured size limit\n"); arena_destroy(arena); return 1; }
    *out_text = text; *out_length = n; *out_verdict = report_overall(&report);
    return 0;
}

static int command_user_dfm(int argc, char **argv, bool instructions) {
    const char *board_path = argv[3];
    const char *rules_path = cli_option_value(argc, argv, "--rules");
    char rules_env[CLI_PATH_MAX];
    if (!rules_path && cli_copy_environment_value("COPPER_FAB_RULES",
                                                  rules_env, sizeof(rules_env)))
        rules_path = rules_env;
    const char *edge_type = cli_option_value(argc, argv, "--edge");
    const char *mask_color_arg = cli_option_value(argc, argv, "--mask-color");
    const char *mask_color = NULL;
    if (mask_color_arg) {
        if (!strcmp(mask_color_arg, "green") || !strcmp(mask_color_arg, "red") ||
            !strcmp(mask_color_arg, "yellow") || !strcmp(mask_color_arg, "blue") ||
            !strcmp(mask_color_arg, "purple")) mask_color = "standard";
        else if (!strcmp(mask_color_arg, "black") || !strcmp(mask_color_arg, "white"))
            mask_color = "black_white";
        else { fprintf(stderr, "ERROR: --mask-color must be green, red, yellow, blue, purple, black, or white\n"); return 64; }
    }
    const char *copper_arg = cli_option_value(argc, argv, "--copper-oz");
    const char *output_path = cli_option_value(argc, argv, instructions ? "--out" : "--report");
    if (!rules_path) {
        fprintf(stderr, "ERROR: pass --rules FILE.rules or set COPPER_FAB_RULES\n");
        return 64;
    }
    if (instructions && !output_path) {
        fprintf(stderr, "ERROR: manufacturing-instructions requires --out FILE.md\n");
        return 64;
    }
    if (edge_type && strcmp(edge_type, "routed") && strcmp(edge_type, "vcut")) {
        fprintf(stderr, "ERROR: --edge must be routed or vcut\n"); return 64;
    }
    uint32_t copper_oz_x10 = 0;
    if (copper_arg) {
        if (!strcmp(copper_arg, "1")) copper_oz_x10 = 10;
        else if (!strcmp(copper_arg, "2")) copper_oz_x10 = 20;
        else if (!strcmp(copper_arg, "2.5")) copper_oz_x10 = 25;
        else if (!strcmp(copper_arg, "3.5")) copper_oz_x10 = 35;
        else if (!strcmp(copper_arg, "4.5")) copper_oz_x10 = 45;
        else { fprintf(stderr, "ERROR: --copper-oz must be one of 1, 2, 2.5, 3.5, 4.5\n"); return 64; }
    }
    Arena arena; char *report_text = NULL; size_t report_length = 0;
    ReportStatus verdict; FabRuleSet rules; Board board; BoardDiag diag;
    if (run_dfm_command(board_path, rules_path, copper_oz_x10, edge_type, mask_color,
                        &report_text, &report_length, &verdict,
                        &rules, &board, &diag, &arena) != 0) return 1;
    if (!instructions) {
        if (output_path) {
            FsStatus st = fs_write_file_exclusive(output_path, report_text, report_length);
            if (st != FS_OK) { fprintf(stderr, "ERROR: report output failed: %s\n", fs_status_name(st)); arena_destroy(&arena); return 1; }
            printf("Wrote DFM report: %s\n", output_path);
        } else {
            fwrite(report_text, 1, report_length, stdout);
        }
    } else {
        char native_layers[2048] = {0};
        size_t layer_used = 0;
        for (size_t li = 0; li < board.layer_count; ++li) {
            if (!board_layer_is_copper(board.layers[li].id)) continue;
            const char *name = board_layer_name(board.layers[li].id);
            int wrote = snprintf(native_layers + layer_used, sizeof(native_layers) - layer_used,
                                 "%s%s", layer_used ? "," : "", name);
            if (wrote < 0 || (size_t)wrote >= sizeof(native_layers) - layer_used) {
                fprintf(stderr, "ERROR: board copper layer list too large\n"); arena_destroy(&arena); return 1;
            }
            layer_used += (size_t)wrote;
        }
        const char *standard_fab_layers[] = {"F.Mask", "B.Mask", "F.SilkS", "B.SilkS", "Edge.Cuts"};
        for (size_t li = 0; li < sizeof(standard_fab_layers) / sizeof(standard_fab_layers[0]); ++li) {
            int wrote = snprintf(native_layers + layer_used, sizeof(native_layers) - layer_used,
                                 "%s%s", layer_used ? "," : "", standard_fab_layers[li]);
            if (wrote < 0 || (size_t)wrote >= sizeof(native_layers) - layer_used) {
                fprintf(stderr, "ERROR: board fabrication layer list too large\n"); arena_destroy(&arena); return 1;
            }
            layer_used += (size_t)wrote;
        }
        char copper_weight[32] = "NOT SELECTED";
        if (copper_oz_x10) {
            if (copper_oz_x10 % 10 == 0)
                (void)snprintf(copper_weight, sizeof(copper_weight), "%u oz", copper_oz_x10 / 10);
            else
                (void)snprintf(copper_weight, sizeof(copper_weight), "%u.%u oz", copper_oz_x10 / 10, copper_oz_x10 % 10);
        }
        size_t cap = report_length + 8192u;
        char *doc = (char *)arena_alloc(&arena, cap);
        if (!doc) { arena_destroy(&arena); return 1; }
        int n = snprintf(doc, cap,
            "# Copper manufacturing handoff\n\n"
            "- Board: `%s`\n- Fabricator ruleset: `%s` (%s)\n"
            "- DFM verdict: **%s**\n- Copper weight selection: %s\n- Edge process: %s\n\n"
            "## Manufacturing gate\n\n"
            "This document is a preparation checklist, not a fabrication release. Do not upload the board for fabrication unless Copper reports PASS, native KiCad DRC passes, and the exported Gerber/Excellon files have been independently re-read and compared with the board. An UNVERIFIED result is not approval.\n\n"
            "## Required native KiCad validation and output\n\n"
            "1. Open the project using the target KiCad version and run ERC on the schematic.\n"
            "2. Run PCB DRC and resolve every violation.\n"
            "3. Export one Gerber per manufacturing layer: `%s`. Include any additional layers required by the selected stackup/process.\n"
            "4. Export Excellon drill files; confirm plated/non-plated and slot outputs.\n"
            "5. Export BOM and component-position files when assembly is ordered. Reconcile MPNs against the approved parts catalogue.\n"
            "6. Re-import the generated Gerber and Excellon outputs into Copper and verify copper, outline, drill count, drill sizes and pad correspondence.\n"
            "7. Review JLCPCB's current order page/process options; selected material, layer count, copper weight, finish, thickness and edge method must match this ruleset configuration.\n\n"
            "## Copper DFM findings\n\n%s\n\n"
            "## Release status\n\n"
            "- [ ] Copper DFM PASS\n- [ ] KiCad native DRC PASS\n- [ ] Gerber/Excellon read-back PASS\n- [ ] BOM and placement files reviewed (assembly only)\n- [ ] Selected JLCPCB process options reviewed\n",
            board_path, rules.ruleset ? rules.ruleset : "unknown", rules.fabricator ? rules.fabricator : "unknown",
            report_status_name(verdict), copper_weight,
            edge_type ? edge_type : "NOT SELECTED", native_layers, report_text);
        if (n < 0 || (size_t)n >= cap) { fprintf(stderr, "ERROR: manufacturing handoff text overflow\n"); arena_destroy(&arena); return 1; }
        FsStatus st = fs_write_file_exclusive(output_path, doc, (size_t)n);
        if (st != FS_OK) { fprintf(stderr, "ERROR: output write failed: %s (existing files are never overwritten)\n", fs_status_name(st)); arena_destroy(&arena); return 1; }
        printf("Wrote manufacturing handoff: %s\nDFM verdict: %s\n",
               output_path, report_status_name(verdict));
    }
    int rc = verdict == REPORT_PASS ? 0 : verdict == REPORT_FAIL ? 2 : 3;
    arena_destroy(&arena);
    return rc;
}

static int command_dev_rules_validate(const char *path) {
    Arena arena; arena_init(&arena, 32768);
    FabRuleSet rules; char today[11]; current_date_utc(today);
    FabStatus status = fab_rules_load(path, &arena, today, 90, &rules);
    if (status != FAB_OK) {
        fprintf(stderr, "FAIL: ruleset validation: %s\n", fab_status_name(status));
        arena_destroy(&arena); return 1;
    }
    printf("PASS: %s / %s — %zu rules, all required fields and source dates valid\n",
           rules.fabricator, rules.ruleset, rules.rule_count);
    for (size_t i = 0; i < rules.rule_count; ++i)
        printf("  %-28s %-28s %lld nm  [%s]\n", rules.rules[i].id,
               rules.rules[i].kind, (long long)rules.rules[i].value,
               rules.rules[i].retrieved);
    arena_destroy(&arena); return 0;
}

static bool suffix_is(const char *text, const char *suffix) {
    size_t a = strlen(text), b = strlen(suffix);
    if (a < b) return false;
    text += a - b;
    for (size_t i = 0; i < b; ++i)
        if (tolower((unsigned char)text[i]) != tolower((unsigned char)suffix[i])) return false;
    return true;
}

static int compare_entries(const void *lhs, const void *rhs) {
    const FsDirectoryEntry *a = (const FsDirectoryEntry *)lhs;
    const FsDirectoryEntry *b = (const FsDirectoryEntry *)rhs;
    return strcmp(a->name, b->name);
}

static bool safe_name(const char *name) {
    if (!name || !name[0] || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return false;
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p)
        if (!(isalnum(*p) || *p == '_' || *p == '-' || *p == '.')) return false;
    return strstr(name, "..") == NULL;
}

static FsStatus read_text(const char *path, char **text, size_t *length) {
    void *data = NULL;
    FsStatus status = fs_read_file(path, &data, length);
    if (status != FS_OK) return status;
    if (memchr(data, '\0', *length) != NULL) {
        free(data);
        return FS_ERR_IO;
    }
    *text = (char *)data;
    return FS_OK;
}

static IRNode *named(Arena *arena, const char *head) {
    return ir_node_list_named(arena, head);
}

static bool append_node(IRNode *parent, IRNode *child) {
    return parent && child && ir_list_append(parent, child) == IR_STATUS_OK;
}

static bool append_text(Arena *arena, IRNode *parent, const char *key,
                        const char *value) {
    IRNode *field = named(arena, key);
    return field && append_node(field, ir_node_string(arena, value ? value : "")) &&
           append_node(parent, field);
}

static bool append_u64(Arena *arena, IRNode *parent, const char *key,
                       uint64_t value) {
    IRNode *field = named(arena, key);
    return field && append_node(field, ir_node_atom_u64(arena, value)) &&
           append_node(parent, field);
}

static bool append_i64(Arena *arena, IRNode *parent, const char *key,
                       int64_t value) {
    IRNode *field = named(arena, key);
    return field && append_node(field, ir_node_atom_i64(arena, value)) &&
           append_node(parent, field);
}

static bool write_ir_exclusive(const char *path, const IRNode *root) {
    char *text = NULL;
    size_t length = 0;
    IRError error = {0};
    IRWriteOptions options = ir_write_options_default();
    options.pretty = true;
    options.indent_width = 2;
    options.final_newline = true;
    if (ir_to_string(root, &options, &text, &length, &error) != IR_STATUS_OK)
        return false;
    FsStatus status = fs_write_file_exclusive(path, text, length);
    ir_string_free(text);
    return status == FS_OK;
}

static bool build_symbol_index(Arena *arena, const char *nickname,
                               const char *source_path, const char *stored_path,
                               const KicadSymbolLibrary *library, IRNode **out) {
    IRNode *root = named(arena, "copper_kicad_symbol_library");
    IRNode *schema = named(arena, "schema");
    if (!root || !schema || !append_node(schema, ir_node_atom(arena, "1")) ||
        !append_node(root, schema) ||
        !append_text(arena, root, "nickname", nickname) ||
        !append_text(arena, root, "source_file", source_path) ||
        !append_text(arena, root, "stored_file", stored_path) ||
        !append_u64(arena, root, "kicad_version", library->format_version) ||
        !append_u64(arena, root, "symbol_count", library->symbol_count)) return false;

    for (size_t i = 0; i < library->symbol_count; ++i) {
        const KicadSymbolAsset *symbol = &library->symbols[i];
        IRNode *item = named(arena, "symbol");
        if (!item || !append_node(item, ir_node_string(arena, symbol->name)) ||
            !append_u64(arena, item, "pin_count", symbol->pin_count) ||
            !append_node(item, named(arena, symbol->pins_inherited ? "inherited_pins" : "local_pins")))
            return false;
        if (symbol->extends && !append_text(arena, item, "extends", symbol->extends)) return false;
        if (symbol->footprint_filter && !append_text(arena, item, "default_footprint", symbol->footprint_filter)) return false;
        if (symbol->datasheet && !append_text(arena, item, "datasheet", symbol->datasheet)) return false;
        for (size_t p = 0; p < symbol->pin_count; ++p) {
            IRNode *pin = named(arena, "pin");
            if (!pin || !append_text(arena, pin, "number", symbol->pins[p].number) ||
                !append_text(arena, pin, "name", symbol->pins[p].name) ||
                !append_text(arena, pin, "electrical_type", symbol->pins[p].electrical_type) ||
                !append_u64(arena, pin, "unit", symbol->pins[p].unit) ||
                !append_u64(arena, pin, "body_style", symbol->pins[p].body_style) ||
                !append_node(item, pin)) return false;
        }
        if (!append_node(root, item)) return false;
    }
    *out = root;
    return true;
}

static bool build_footprint_index_entry(Arena *arena, IRNode *root,
                                        const KicadFootprintAsset *fp,
                                        const char *filename) {
    IRNode *item = named(arena, "footprint");
    if (!item || !append_node(item, ir_node_string(arena, fp->name)) ||
        !append_text(arena, item, "file", filename) ||
        !append_u64(arena, item, "pad_count", fp->pad_count)) return false;
    if (fp->description && !append_text(arena, item, "description", fp->description)) return false;
    if (fp->tags && !append_text(arena, item, "tags", fp->tags)) return false;
    for (size_t i = 0; i < fp->pad_count; ++i) {
        const KicadFootprintPad *pad = &fp->pads[i];
        IRNode *p = named(arena, "pad");
        if (!p || !append_text(arena, p, "number", pad->number) ||
            !append_text(arena, p, "type", pad->type) ||
            !append_text(arena, p, "shape", pad->shape) ||
            !append_i64(arena, p, "x_nm", pad->x_nm) ||
            !append_i64(arena, p, "y_nm", pad->y_nm) ||
            !append_i64(arena, p, "rotation_mdeg", pad->rotation_mdeg) ||
            !append_i64(arena, p, "size_x_nm", pad->size_x_nm) ||
            !append_i64(arena, p, "size_y_nm", pad->size_y_nm) ||
            !append_i64(arena, p, "drill_x_nm", pad->drill_x_nm) ||
            !append_i64(arena, p, "drill_y_nm", pad->drill_y_nm) ||
            !append_i64(arena, p, "drill_offset_x_nm", pad->drill_offset_x_nm) ||
            !append_i64(arena, p, "drill_offset_y_nm", pad->drill_offset_y_nm) ||
            !append_node(item, p)) return false;
    }
    return append_node(root, item);
}

static int command_inspect_symbol(const char *path, const char *filter) {
    char *text = NULL;
    size_t length = 0;
    if (read_text(path, &text, &length) != FS_OK) {
        fprintf(stderr, "ERROR: cannot read %s\n", path);
        return 1;
    }
    Arena arena;
    arena_init(&arena, 65536);
    KicadSymbolLibrary library;
    KicadAssetError error;
    KicadAssetStatus status = kicad_symbol_library_parse(&arena, text, length, &library, &error);
    free(text);
    if (status != KICAD_ASSET_OK) {
        fprintf(stderr, "ERROR: %s: %s\n", kicad_asset_status_name(status), error.message ? error.message : "parse failure");
        if (error.ir_error.message) fprintf(stderr, "IR: %s at %zu:%zu\n",
            error.ir_error.message, error.ir_error.line, error.ir_error.column);
        arena_destroy(&arena);
        return 1;
    }
    printf("KiCad symbol library: %s\nVersion: %u\nSymbols: %zu\n",
           path, library.format_version, library.symbol_count);
    size_t shown = 0;
    for (size_t i = 0; i < library.symbol_count; ++i) {
        const KicadSymbolAsset *symbol = &library.symbols[i];
        if (filter && strcmp(filter, symbol->name) != 0) continue;
        printf("\n%s  pins=%zu", symbol->name, symbol->pin_count);
        if (symbol->extends) printf("  extends=%s", symbol->extends);
        if (symbol->pins_inherited) printf("  [parent pins require resolution]");
        puts("");
        for (size_t p = 0; p < symbol->pin_count; ++p)
            printf("  pin %-5s %-18s %-12s unit=%u\n",
                   symbol->pins[p].number, symbol->pins[p].name,
                   symbol->pins[p].electrical_type, symbol->pins[p].unit);
        ++shown;
    }
    if (filter && shown == 0) {
        fprintf(stderr, "ERROR: symbol '%s' not found\n", filter);
        arena_destroy(&arena);
        return 2;
    }
    arena_destroy(&arena);
    return 0;
}

static int command_inspect_footprint(const char *path) {
    char *text = NULL;
    size_t length = 0;
    if (read_text(path, &text, &length) != FS_OK) {
        fprintf(stderr, "ERROR: cannot read %s\n", path);
        return 1;
    }
    Arena arena;
    arena_init(&arena, 32768);
    KicadFootprintAsset fp;
    KicadAssetError error;
    KicadAssetStatus status = kicad_footprint_parse(&arena, text, length, &fp, &error);
    free(text);
    if (status != KICAD_ASSET_OK) {
        fprintf(stderr, "ERROR: %s: %s\n", kicad_asset_status_name(status), error.message ? error.message : "parse failure");
        arena_destroy(&arena);
        return 1;
    }
    printf("Footprint: %s\nPads: %zu\nDescription: %s\nTags: %s\n",
           fp.name, fp.pad_count, fp.description ? fp.description : "",
           fp.tags ? fp.tags : "");
    for (size_t i = 0; i < fp.pad_count; ++i) {
        const KicadFootprintPad *p = &fp.pads[i];
        printf("  pad %-4s %-12s %-10s at=(%lld,%lld)nm size=(%lld,%lld)nm drill=(%lld,%lld)nm\n",
            p->number, p->type, p->shape,
            (long long)p->x_nm, (long long)p->y_nm,
            (long long)p->size_x_nm, (long long)p->size_y_nm,
            (long long)p->drill_x_nm, (long long)p->drill_y_nm);
    }
    arena_destroy(&arena);
    return 0;
}

static int command_import_symbols(const char *nickname, const char *source,
                                  const char *asset_root) {
    if (!safe_name(nickname)) {
        fprintf(stderr, "ERROR: unsafe library nickname\n");
        return 2;
    }
    char *text = NULL;
    size_t length = 0;
    if (read_text(source, &text, &length) != FS_OK) {
        fprintf(stderr, "ERROR: cannot read symbol library\n");
        return 1;
    }
    Arena arena;
    arena_init(&arena, 1024 * 1024);
    KicadSymbolLibrary library;
    KicadAssetError error;
    KicadAssetStatus status = kicad_symbol_library_parse(&arena, text, length, &library, &error);
    if (status != KICAD_ASSET_OK) {
        fprintf(stderr, "ERROR: symbol import refused: %s: %s\n",
                kicad_asset_status_name(status), error.message ? error.message : "invalid library");
        free(text); arena_destroy(&arena); return 1;
    }

    char symbols_dir[CLI_PATH_MAX], index_dir[CLI_PATH_MAX];
    char stored[CLI_PATH_MAX], index_path[CLI_PATH_MAX];
    if (fs_path_join(symbols_dir, sizeof(symbols_dir), asset_root, "symbols") != FS_OK ||
        fs_path_join(index_dir, sizeof(index_dir), asset_root, "index") != FS_OK ||
        fs_create_directories(symbols_dir) != FS_OK ||
        fs_create_directories(index_dir) != FS_OK) {
        fprintf(stderr, "ERROR: could not create asset store directories\n");
        free(text); arena_destroy(&arena); return 1;
    }
    char filename[CLI_PATH_MAX];
    if (snprintf(filename, sizeof(filename), "%s.kicad_sym", nickname) < 0 ||
        fs_path_join(stored, sizeof(stored), symbols_dir, filename) != FS_OK) {
        free(text); arena_destroy(&arena); return 1;
    }
    if (snprintf(filename, sizeof(filename), "symbols-%s.cidx", nickname) < 0 ||
        fs_path_join(index_path, sizeof(index_path), index_dir, filename) != FS_OK) {
        free(text); arena_destroy(&arena); return 1;
    }

    if (fs_file_exists(stored) || fs_file_exists(index_path)) {
        bool complete = fs_file_exists(stored) && fs_file_exists(index_path);
        printf("%s symbol library '%s'\n",
               complete ? "Already imported; skipped" : "Incomplete existing import; refusing overwrite",
               nickname);
        free(text); arena_destroy(&arena);
        return complete ? 0 : 1;
    }

    IRNode *index = NULL;
    if (!build_symbol_index(&arena, nickname, source, stored, &library, &index)) {
        fprintf(stderr, "ERROR: could not build symbol asset index\n");
        free(text); arena_destroy(&arena); return 1;
    }
    if (fs_write_file_exclusive(stored, text, length) != FS_OK) {
        fprintf(stderr, "ERROR: cannot install source file (destination exists or I/O failed): %s\n", stored);
        free(text); arena_destroy(&arena); return 1;
    }
    if (!write_ir_exclusive(index_path, index)) {
        (void)fs_remove_tree(stored);
        fprintf(stderr, "ERROR: cannot write symbol index: %s\n", index_path);
        free(text); arena_destroy(&arena); return 1;
    }
    printf("Imported symbol library '%s': %zu symbols, stored at %s\n",
           nickname, library.symbol_count, stored);
    printf("Index: %s\n", index_path);
    free(text);
    arena_destroy(&arena);
    return 0;
}

static int command_import_footprints(const char *nickname, const char *source_dir,
                                     const char *asset_root) {
    if (!safe_name(nickname) || !fs_directory_exists(source_dir)) {
        fprintf(stderr, "ERROR: invalid nickname or footprint directory\n");
        return 2;
    }
    FsDirectoryListing listing;
    if (fs_list_directory(source_dir, &listing) != FS_OK) {
        fprintf(stderr, "ERROR: cannot list footprint library\n");
        return 1;
    }
    if (listing.count > 1)
        qsort(listing.entries, listing.count, sizeof(*listing.entries), compare_entries);

    char footprint_root[CLI_PATH_MAX], stored_dir[CLI_PATH_MAX];
    char index_dir[CLI_PATH_MAX], index_path[CLI_PATH_MAX];
    if (fs_path_join(footprint_root, sizeof(footprint_root), asset_root, "footprints") != FS_OK ||
        fs_path_join(stored_dir, sizeof(stored_dir), footprint_root, nickname) != FS_OK ||
        fs_path_join(index_dir, sizeof(index_dir), asset_root, "index") != FS_OK ||
        fs_create_directories(footprint_root) != FS_OK ||
        fs_create_directories(index_dir) != FS_OK) {
        fs_free_directory_listing(&listing);
        fprintf(stderr, "ERROR: cannot create footprint asset store\n");
        return 1;
    }
    char pretty_name[CLI_PATH_MAX];
    if (snprintf(pretty_name, sizeof(pretty_name), "%s.pretty", nickname) < 0 ||
        fs_path_join(stored_dir, sizeof(stored_dir), footprint_root, pretty_name) != FS_OK) {
        fs_free_directory_listing(&listing); return 1;
    }
    char idxname[CLI_PATH_MAX];
    if (snprintf(idxname, sizeof(idxname), "footprints-%s.cidx", nickname) < 0 ||
        fs_path_join(index_path, sizeof(index_path), index_dir, idxname) != FS_OK) {
        fs_free_directory_listing(&listing); return 1;
    }
    if (fs_directory_exists(stored_dir) || fs_file_exists(index_path)) {
        bool complete = fs_directory_exists(stored_dir) && fs_file_exists(index_path);
        fs_free_directory_listing(&listing);
        printf("%s footprint library '%s'\n",
               complete ? "Already imported; skipped" : "Incomplete existing import; refusing overwrite",
               nickname);
        return complete ? 0 : 1;
    }

    Arena index_arena;
    arena_init(&index_arena, 1024 * 1024);
    IRNode *index = named(&index_arena, "copper_kicad_footprint_library");
    IRNode *schema = named(&index_arena, "schema");
    if (!index || !schema || !append_node(schema, ir_node_atom(&index_arena, "1")) ||
        !append_node(index, schema) ||
        !append_text(&index_arena, index, "nickname", nickname) ||
        !append_text(&index_arena, index, "source_directory", source_dir)) {
        arena_destroy(&index_arena); fs_free_directory_listing(&listing); return 1;
    }

    size_t imported = 0;
    bool import_failed = false;
    for (size_t i = 0; i < listing.count; ++i) {
        FsDirectoryEntry *entry = &listing.entries[i];
        if (!entry->is_regular_file || !suffix_is(entry->name, ".kicad_mod")) continue;
        char source_file[CLI_PATH_MAX], destination_file[CLI_PATH_MAX];
        if (fs_path_join(source_file, sizeof(source_file), source_dir, entry->name) != FS_OK) {
            import_failed = true; break;
        }
        char *text = NULL; size_t length = 0;
        if (read_text(source_file, &text, &length) != FS_OK) { import_failed = true; break; }
        Arena one_arena; arena_init(&one_arena, 32768);
        KicadFootprintAsset fp; KicadAssetError error;
        KicadAssetStatus parse_status = kicad_footprint_parse(
            &one_arena, text, length, &fp, &error);
        if (parse_status != KICAD_ASSET_OK) {
            fprintf(stderr, "ERROR: invalid footprint %s: %s\n", source_file,
                    error.message ? error.message : kicad_asset_status_name(parse_status));
            arena_destroy(&one_arena); free(text); import_failed = true; break;
        }
        if (!build_footprint_index_entry(&index_arena, index, &fp, entry->name)) {
            arena_destroy(&one_arena); free(text); import_failed = true; break;
        }
        arena_destroy(&one_arena);
        if (fs_create_directories(stored_dir) != FS_OK &&
            !fs_directory_exists(stored_dir)) {
            free(text); import_failed = true; break;
        }
        if (fs_path_join(destination_file, sizeof(destination_file), stored_dir, entry->name) != FS_OK ||
            fs_write_file_exclusive(destination_file, text, length) != FS_OK) {
            free(text); import_failed = true; break;
        }
        free(text);
        ++imported;
    }
    fs_free_directory_listing(&listing);

    if (import_failed || imported == 0) {
        (void)fs_remove_tree(stored_dir);
        arena_destroy(&index_arena);
        fprintf(stderr, "ERROR: no footprints imported; source library was empty or invalid\n");
        return 1;
    }
    if (!append_u64(&index_arena, index, "footprint_count", imported)) {
        (void)fs_remove_tree(stored_dir);
        arena_destroy(&index_arena);
        return 1;
    }
    if (!write_ir_exclusive(index_path, index)) {
        (void)fs_remove_tree(stored_dir);
        arena_destroy(&index_arena);
        fprintf(stderr, "ERROR: failed to write footprint index\n");
        return 1;
    }
    printf("Imported footprint library '%s': %zu footprints\n", nickname, imported);
    printf("Stored at: %s\nIndex: %s\n", stored_dir, index_path);
    arena_destroy(&index_arena);
    return 0;
}

static bool nickname_from_filename(const char *filename, const char *suffix,
                                   char *out, size_t out_capacity) {
    size_t length = strlen(filename);
    size_t suffix_length = strlen(suffix);
    if (length <= suffix_length || !suffix_is(filename, suffix)) return false;
    size_t name_length = length - suffix_length;
    if (name_length >= out_capacity) return false;
    memcpy(out, filename, name_length);
    out[name_length] = '\0';
    return safe_name(out);
}

static int command_import_kicad(const char *symbols_dir,
                                const char *footprints_dir,
                                const char *asset_root) {
    size_t symbol_ok = 0, footprint_ok = 0, failures = 0;
    FsDirectoryListing listing;
    if (fs_list_directory(symbols_dir, &listing) != FS_OK) {
        fprintf(stderr, "ERROR: cannot list KiCad symbol directory: %s\n", symbols_dir);
        return 1;
    }
    if (listing.count > 1)
        qsort(listing.entries, listing.count, sizeof(*listing.entries), compare_entries);
    for (size_t i = 0; i < listing.count; ++i) {
        FsDirectoryEntry *entry = &listing.entries[i];
        if (!entry->is_regular_file || !suffix_is(entry->name, ".kicad_sym")) continue;
        char nickname[CLI_PATH_MAX], source[CLI_PATH_MAX];
        if (!nickname_from_filename(entry->name, ".kicad_sym", nickname, sizeof(nickname)) ||
            fs_path_join(source, sizeof(source), symbols_dir, entry->name) != FS_OK) {
            fprintf(stderr, "SKIP unsafe symbol library filename: %s\n", entry->name);
            ++failures; continue;
        }
        printf("\n[SYMBOL LIBRARY] %s\n", nickname);
        if (command_import_symbols(nickname, source, asset_root) == 0) ++symbol_ok;
        else ++failures;
    }
    fs_free_directory_listing(&listing);

    if (fs_list_directory(footprints_dir, &listing) != FS_OK) {
        fprintf(stderr, "ERROR: cannot list KiCad footprint directory: %s\n", footprints_dir);
        return 1;
    }
    if (listing.count > 1)
        qsort(listing.entries, listing.count, sizeof(*listing.entries), compare_entries);
    for (size_t i = 0; i < listing.count; ++i) {
        FsDirectoryEntry *entry = &listing.entries[i];
        if (!entry->is_directory || !suffix_is(entry->name, ".pretty")) continue;
        char nickname[CLI_PATH_MAX], source[CLI_PATH_MAX];
        if (!nickname_from_filename(entry->name, ".pretty", nickname, sizeof(nickname)) ||
            fs_path_join(source, sizeof(source), footprints_dir, entry->name) != FS_OK) {
            fprintf(stderr, "SKIP unsafe footprint library name: %s\n", entry->name);
            ++failures; continue;
        }
        printf("\n[FOOTPRINT LIBRARY] %s\n", nickname);
        if (command_import_footprints(nickname, source, asset_root) == 0) ++footprint_ok;
        else ++failures;
    }
    fs_free_directory_listing(&listing);

    printf("\nKiCad import summary: %zu symbol libraries, %zu footprint libraries, %zu failures\n",
           symbol_ok, footprint_ok, failures);
    printf("Asset root: %s\n", asset_root);
    return failures == 0 ? 0 : 1;
}

static bool parse_catalogue_file(const char *path, PartCatalogue *catalogue,
                                 Arena *arena) {
    PartCatalogueError error;
    PartCatalogueStatus status = part_catalogue_parse_file(arena, path, catalogue, &error);
    if (status != PART_CATALOGUE_OK) {
        fprintf(stderr, "INVALID: %s: %s (part index %zu, field %s)\n",
            part_catalogue_status_name(status),
            error.message ? error.message : "catalogue parsing failed",
            error.item_index, error.field ? error.field : "-");
        return false;
    }
    return true;
}

static bool option_value(int argc, char **argv, const char *option,
                         const char **out);

static int command_catalogue_create(const char *path) {
    Arena arena;
    arena_init(&arena, 4096);

    IRNode *root = named(&arena, "copper_parts");
    IRNode *schema = named(&arena, "schema");
    bool ok = root && schema &&
              append_node(schema, ir_node_atom_u64(&arena, PART_CATALOGUE_SCHEMA_VERSION)) &&
              append_node(root, schema) &&
              write_ir_exclusive(path, root);
    arena_destroy(&arena);

    if (!ok) {
        fprintf(stderr, "ERROR: cannot create catalogue '%s' (file may already exist or path is invalid)\n", path);
        return 1;
    }
    printf("Created empty Copper part catalogue: %s\n", path);
    puts("Add at least one (part ...) record, then validate before installing; empty catalogues are rejected.");
    return 0;
}

static int command_catalogue_validate(int argc, char **argv) {
    const char *path = argv[3];
    const char *symbols_root = NULL;
    const char *footprints_root = NULL;
    (void)option_value(argc, argv, "--symbols-root", &symbols_root);
    (void)option_value(argc, argv, "--footprints-root", &footprints_root);
    if (!!symbols_root != !!footprints_root) {
        fprintf(stderr, "ERROR: provide both --symbols-root and --footprints-root\n");
        return 2;
    }

    Arena arena; arena_init(&arena, 65536);
    PartCatalogue catalogue;
    bool ok = parse_catalogue_file(path, &catalogue, &arena);
    if (ok) {
        size_t verified = 0;
        for (size_t i = 0; i < catalogue.part_count; ++i)
            if (part_record_sources_verified(&catalogue.parts[i])) ++verified;
        printf("PASS catalogue schema %u: %zu parts (%zu with verified sources)\n",
               catalogue.schema_version, catalogue.part_count, verified);
        if (symbols_root && footprints_root) {
            KicadAssetValidationReport report;
            KicadAssetError error;
            KicadAssetStatus status = kicad_part_catalogue_validate_paths(
                &arena, &catalogue, symbols_root, footprints_root, &report, &error);
            printf("KiCad assets: %zu/%zu parts passed, %zu failed\n",
                   report.passed_parts, report.checked_parts, report.failed_parts);
            if (status != KICAD_ASSET_OK) {
                fprintf(stderr, "ASSET VALIDATION FAILED: %s: %s (part index %zu)\n",
                    kicad_asset_status_name(status),
                    error.message ? error.message : "asset mismatch",
                    error.item_index);
                ok = false;
            }
        } else {
            puts("KiCad asset validation: NOT RUN (no symbol/footprint roots supplied).");
        }
        puts("A schema pass alone is not proof of KiCad asset or electrical-model compatibility.");
    }
    arena_destroy(&arena);
    return ok ? 0 : 2;
}

static bool default_user_root(char *out, size_t capacity) {
    char override[CLI_PATH_MAX];
    if (cli_copy_environment_value("COPPER_HOME", override, sizeof(override)) &&
        override[0])
        return fs_path_join(out, capacity, override, "catalogues") == FS_OK;
#ifdef _WIN32
    char appdata[CLI_PATH_MAX];
    if (!cli_copy_environment_value("APPDATA", appdata, sizeof(appdata)) ||
        !appdata[0]) return false;
    char base[CLI_PATH_MAX];
    return fs_path_join(base, sizeof(base), appdata, "CopperV2") == FS_OK &&
           fs_path_join(out, capacity, base, "catalogues") == FS_OK;
#else
    char xdg[CLI_PATH_MAX], home[CLI_PATH_MAX];
    bool have_xdg = cli_copy_environment_value("XDG_DATA_HOME", xdg, sizeof(xdg));
    bool have_home = cli_copy_environment_value("HOME", home, sizeof(home));
    char base[CLI_PATH_MAX];
    if (have_xdg && xdg[0]) {
        if (fs_path_join(base, sizeof(base), xdg, "copperv2") != FS_OK) return false;
    } else {
        if (!have_home || !home[0] ||
            fs_path_join(base, sizeof(base), home, ".local/share/copperv2") != FS_OK) return false;
    }
    return fs_path_join(out, capacity, base, "catalogues") == FS_OK;
#endif
}

static bool option_value(int argc, char **argv, const char *option,
                         const char **out) {
    for (int i = 0; i + 1 < argc; ++i) {
        if (strcmp(argv[i], option) == 0) { *out = argv[i + 1]; return true; }
    }
    return false;
}

/* copper block list BLOCK.json...: what each block offers, as JSON ("copper.block-list/1"): its summary, its ports
 * (kind, voltage range, currents, pull-ups) and its parameters (unit, required, derived from a connection, default).
 * This is what a model may choose from when it writes a copper.compose/1 file. Each block is checked first (blocks.c;
 * no catalogue); an invalid block is listed with its verdict only. Exit 0 when every block is VALID or UNVERIFIED, 2
 * otherwise, 64 usage. */
static void list_int(JsonWriter *w, const JsonValue *obj, const char *key)
{
    int64_t v = 0;
    const JsonValue *x = json_get(obj, key);
    if (x && x->kind == JSON_NUMBER && json_to_i64(x, &v) == JSON_NUMBER_OK) { json_write_key(w, key); json_write_int(w, v); }
}

static void list_str(JsonWriter *w, const JsonValue *obj, const char *key)
{
    const JsonValue *x = json_get(obj, key);
    if (x && x->kind == JSON_STRING) { json_write_key(w, key); json_write_string(w, x->text); }
}

static int command_block_list(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "block list needs one or more BLOCK.json files\n"); return 64; }
    Arena arena;
    arena_init(&arena, 1u << 20);
    JsonWriter w;
    json_writer_init(&w, &arena);
    json_write_begin_object(&w);
    json_write_key(&w, "format"); json_write_string(&w, "copper.block-list/1");
    json_write_key(&w, "blocks");
    json_write_begin_array(&w);
    int code = 0;
    for (int i = 3; i < argc; ++i) {
        const unsigned char *data = NULL;
        size_t size = 0;
        BlockReport br;
        JsonValue *root = NULL;
        JsonError je;
        bool readable = fs_read_file_arena(&arena, argv[i], &data, &size) == FS_OK;
        BlockStatus st = readable ? block_check_text(&arena, (const char *)data, size, NULL, 0, &br) : BLOCK_INVALID;
        json_write_begin_object(&w);
        json_write_key(&w, "file"); json_write_string(&w, argv[i]);
        json_write_key(&w, "verdict"); json_write_string(&w, block_status_name(st));
        if ((st != BLOCK_VALID && st != BLOCK_UNVERIFIED) || json_parse(&arena, (const char *)data, size, &root, &je) != JSON_OK) {
            json_write_end_object(&w);
            code = 2;
            continue;
        }
        list_str(&w, root, "name");
        list_str(&w, root, "summary");
        json_write_key(&w, "ports");
        json_write_begin_array(&w);
        const JsonValue *ports = json_get(root, "ports");
        for (size_t k = 0; ports && k < ports->count; ++k) {
            const JsonValue *p = ports->items[k];
            json_write_begin_object(&w);
            list_str(&w, p, "name"); list_str(&w, p, "kind");
            list_int(&w, p, "voltage_min"); list_int(&w, p, "voltage_max"); list_str(&w, p, "voltage_parameter");
            list_int(&w, p, "current"); list_int(&w, p, "current_max"); list_str(&w, p, "level_port");
            if (json_get(p, "pullup")) { json_write_key(&w, "pullup"); json_write_bool(&w, json_get(p, "pullup")->kind == JSON_TRUE); }
            list_str(&w, p, "note");
            json_write_end_object(&w);
        }
        json_write_end_array(&w);
        json_write_key(&w, "parameters");
        json_write_begin_array(&w);
        const JsonValue *params = json_get(root, "parameters");
        for (size_t k = 0; params && k < params->count; ++k) {
            const JsonValue *p = params->items[k];
            json_write_begin_object(&w);
            list_str(&w, p, "name"); list_str(&w, p, "unit");
            json_write_key(&w, "required"); json_write_bool(&w, json_get(p, "required") && json_get(p, "required")->kind == JSON_TRUE);
            json_write_key(&w, "derived_from_connection"); json_write_bool(&w, json_get(p, "from") != NULL);
            list_int(&w, p, "default"); list_int(&w, p, "min"); list_int(&w, p, "max");
            json_write_end_object(&w);
        }
        json_write_end_array(&w);
        json_write_end_object(&w);
    }
    json_write_end_array(&w);
    json_write_end_object(&w);
    size_t len = 0;
    const char *text = json_writer_finish(&w, &len);
    if (!text) { fprintf(stderr, "block list: out of memory\n"); arena_destroy(&arena); return 70; }
    fwrite(text, 1, len, stdout);
    putchar('\n');
    arena_destroy(&arena);
    return code;
}

/* copper block check BLOCK.json [--parts FILE.parts]... [--out REPORT.json]
 * Checks one copper.block/1 definition (src/v2_src/blocks.c), against the given part catalogues when there are any.
 * Exit: 0 VALID, 3 UNVERIFIED (no catalogue given), 2 INVALID, 64 usage, 70 internal error. */
static int command_block_check(int argc, char **argv) {
    const char *path = argv[3], *out_path = NULL;
    enum { MAX_CATS = 32 };
    const char *cat_paths[MAX_CATS];
    size_t ncat = 0;
    for (int i = 4; i < argc; i += 2) {
        if (i + 1 >= argc) { fprintf(stderr, "ERROR: %s needs a value\n", argv[i]); return 64; }
        if (!strcmp(argv[i], "--parts")) {
            if (ncat == MAX_CATS) { fprintf(stderr, "ERROR: at most %d --parts files\n", MAX_CATS); return 64; }
            cat_paths[ncat++] = argv[i + 1];
        } else if (!strcmp(argv[i], "--out")) out_path = argv[i + 1];
        else { fprintf(stderr, "ERROR: unknown option %s\n", argv[i]); return 64; }
    }
    Arena arena;
    arena_init(&arena, 1u << 20);
    PartCatalogue cats[MAX_CATS];
    const PartCatalogue *refs[MAX_CATS];
    for (size_t i = 0; i < ncat; ++i) {
        PartCatalogueError e;
        if (part_catalogue_parse_file(&arena, cat_paths[i], &cats[i], &e) != PART_CATALOGUE_OK) {
            printf("%s: catalogue does not parse: %s\n", cat_paths[i], e.message ? e.message : "error");
            printf("STATUS block check: INVALID (a --parts catalogue is unreadable)\n");
            arena_destroy(&arena);
            return 2;
        }
        refs[i] = &cats[i];
    }
    BlockReport r;
    BlockStatus st = block_check_file(&arena, path, refs, ncat, &r);
    if (st == BLOCK_ERR_ARGUMENT || st == BLOCK_ERR_MEMORY) {
        fprintf(stderr, "ERROR: block check: %s\n", block_status_name(st));
        arena_destroy(&arena);
        return 70;
    }
    for (size_t i = 0; i < r.finding_count; ++i)
        printf("  %-20s %-28s %s\n", r.findings[i].code, r.findings[i].path[0] ? r.findings[i].path : "(file)", r.findings[i].message);
    if (!ncat) puts("  catalogue checks NOT RUN: give the part catalogues with --parts");
    int code = st == BLOCK_VALID ? 0 : st == BLOCK_UNVERIFIED ? 3 : 2;
    if (out_path) {
        size_t len = 0;
        const char *json = block_report_json(&arena, &r, &len);
        FILE *f = json ? fopen(out_path, "wb") : NULL;
        bool ok = f && fwrite(json, 1, len, f) == len;
        if (f && fclose(f) != 0) ok = false;
        if (!ok) { fprintf(stderr, "ERROR: cannot write %s\n", out_path); code = 70; }
    }
    printf("STATUS block check: %s (%s: %zu parameters, %zu ports, %zu nets, %zu parts, %zu formulas; %zu findings)\n",
           block_status_name(st), r.name ? r.name : "?", r.parameters, r.ports, r.nets, r.parts, r.formulas, r.finding_count);
    arena_destroy(&arena);
    return code;
}

/* copper catalogue check FILE [--symbols-root DIR --footprints-root DIR] [--out REPORT.json]
 * Every problem of every part (src/v2_src/parts_check.c); `catalogue validate` is unchanged.
 * Exit: 0 PASS, 3 UNVERIFIED, 1 FAIL, 2 INVALID_INPUT, 64 usage, 70 internal error. */
static int command_catalogue_check(int argc, char **argv) {
    const char *path = argv[3], *symbols_root = NULL, *footprints_root = NULL, *out_path = NULL;
    for (int i = 4; i < argc; i += 2) {
        if (i + 1 >= argc) { fprintf(stderr, "ERROR: %s needs a value\n", argv[i]); return 64; }
        if (!strcmp(argv[i], "--symbols-root")) symbols_root = argv[i + 1];
        else if (!strcmp(argv[i], "--footprints-root")) footprints_root = argv[i + 1];
        else if (!strcmp(argv[i], "--out")) out_path = argv[i + 1];
        else { fprintf(stderr, "ERROR: unknown option %s\n", argv[i]); return 64; }
    }
    if (!!symbols_root != !!footprints_root) {
        fprintf(stderr, "ERROR: provide both --symbols-root and --footprints-root, or neither\n");
        return 64;
    }
    Arena arena;
    arena_init(&arena, 1u << 20);
    PartsCheckReport r;
    PartsCheckStatus st = parts_check_file(&arena, path, symbols_root, footprints_root, &r);
    if (st == PARTS_CHECK_ERR_ARGUMENT || st == PARTS_CHECK_ERR_MEMORY) {
        fprintf(stderr, "ERROR: catalogue check: %s\n", parts_check_status_name(st));
        arena_destroy(&arena);
        return 70;
    }
    if (st == PARTS_CHECK_INVALID_INPUT) {
        printf("%s: %s\n", path, r.invalid_message);
    } else {
        printf("%-28s %-2s %-22s %-7s %s\n", "part", "k", "mpn", "kicad", "verified (identity; ratings)");
        for (size_t i = 0; i < r.part_count; ++i) {
            const PartsCheckPart *p = &r.parts[i];
            printf("%-28s %-2s %-22s %-7s %s; %zu/%zu\n", p->id, p->kind, p->mpn ? p->mpn : "-", p->kicad,
                   p->identity_verified ? "yes" : "no", p->ratings_verified, p->ratings);
        }
        for (size_t i = 0; i < r.finding_count; ++i) {
            const PartsFinding *f = &r.findings[i];
            printf("  %-10s %-23s %s: %s\n", parts_finding_level_name(f->level), f->code,
                   f->part_id ? f->part_id : "(catalogue)", f->message);
        }
    }
    int code = st == PARTS_CHECK_PASS ? 0 : st == PARTS_CHECK_UNVERIFIED ? 3 : st == PARTS_CHECK_FAIL ? 1 : 2;
    if (out_path) {
        size_t len = 0;
        const char *json = parts_check_report_json(&arena, &r, &len);
        FILE *f = json ? fopen(out_path, "wb") : NULL;
        bool ok = f && fwrite(json, 1, len, f) == len;
        if (f && fclose(f) != 0) ok = false;
        if (!ok) { fprintf(stderr, "ERROR: cannot write %s\n", out_path); code = 70; }
    }
    printf("STATUS catalogue check: %s (%zu parts, %zu FAIL, %zu UNVERIFIED/NOT_RUN findings)\n",
           parts_check_status_name(st), r.part_count, r.fail_count, r.unverified_count);
    arena_destroy(&arena);
    return code;
}

static int command_catalogue_install(int argc, char **argv) {
    const char *source = argv[3];
    const char *scope = NULL;
    char project_root_env[CLI_PATH_MAX];
    const char *project_root =
        cli_copy_environment_value("COPPER_PROJECT", project_root_env,
                                   sizeof(project_root_env))
            ? project_root_env : NULL;
    const char *user_root_arg = NULL;
    const char *symbols_root = NULL;
    const char *footprints_root = NULL;
    (void)option_value(argc, argv, "--scope", &scope);
    (void)option_value(argc, argv, "--project-root", &project_root);
    (void)option_value(argc, argv, "--user-root", &user_root_arg);
    (void)option_value(argc, argv, "--symbols-root", &symbols_root);
    (void)option_value(argc, argv, "--footprints-root", &footprints_root);
    if (!!symbols_root != !!footprints_root) {
        fprintf(stderr, "ERROR: provide both --symbols-root and --footprints-root\n"); return 2;
    }
    if (!scope || (strcmp(scope, "user") != 0 && strcmp(scope, "project") != 0)) {
        fprintf(stderr, "ERROR: --scope must be user or project\n"); return 2;
    }
    char destination_dir[CLI_PATH_MAX];
    if (strcmp(scope, "project") == 0) {
        char root[CLI_PATH_MAX];
        if (!project_root) {
            if (fs_get_current_directory(root, sizeof(root)) != FS_OK) return 1;
            project_root = root;
            char local[CLI_PATH_MAX];
            if (fs_path_join(local, sizeof(local), project_root, ".copper") != FS_OK ||
                fs_path_join(destination_dir, sizeof(destination_dir), local, "catalogues") != FS_OK) return 1;
        } else {
            char local[CLI_PATH_MAX];
            if (fs_path_join(local, sizeof(local), project_root, ".copper") != FS_OK ||
                fs_path_join(destination_dir, sizeof(destination_dir), local, "catalogues") != FS_OK) return 1;
        }
    } else if (user_root_arg) {
        if (fs_path_join(destination_dir, sizeof(destination_dir), user_root_arg, "catalogues") != FS_OK) return 1;
    } else if (!default_user_root(destination_dir, sizeof(destination_dir))) {
        fprintf(stderr, "ERROR: cannot determine user catalogue directory; set COPPER_HOME or --user-root\n");
        return 1;
    }

    Arena arena; arena_init(&arena, 65536);
    PartCatalogue catalogue;
    if (!parse_catalogue_file(source, &catalogue, &arena)) {
        arena_destroy(&arena); return 2;
    }
    if (symbols_root && footprints_root) {
        KicadAssetValidationReport report;
        KicadAssetError asset_error;
        KicadAssetStatus asset_status = kicad_part_catalogue_validate_paths(
            &arena, &catalogue, symbols_root, footprints_root, &report, &asset_error);
        if (asset_status != KICAD_ASSET_OK) {
            fprintf(stderr, "ERROR: catalogue asset validation failed: %s: %s (part index %zu)\n",
                kicad_asset_status_name(asset_status),
                asset_error.message ? asset_error.message : "asset mismatch",
                asset_error.item_index);
            arena_destroy(&arena); return 2;
        }
        printf("KiCad assets validated: %zu parts\n", report.passed_parts);
    }
    char basename[CLI_PATH_MAX], destination[CLI_PATH_MAX];
    if (fs_path_basename(basename, sizeof(basename), source) != FS_OK ||
        fs_create_directories(destination_dir) != FS_OK ||
        fs_path_join(destination, sizeof(destination), destination_dir, basename) != FS_OK) {
        arena_destroy(&arena); fprintf(stderr, "ERROR: invalid catalogue install path\n"); return 1;
    }
    FsStatus copied = fs_copy_file_exclusive(source, destination);
    if (copied != FS_OK) {
        arena_destroy(&arena);
        fprintf(stderr, "ERROR: install failed (%s). Existing catalogues are never overwritten.\n",
                fs_status_name(copied));
        return 1;
    }
    printf("Installed catalogue '%s' (%zu parts) into %s scope:\n%s\n",
           basename, catalogue.part_count, scope, destination);
    arena_destroy(&arena);
    return 0;
}

static int command_catalogue_list(int argc, char **argv) {
    const char *scope = NULL;
    char project_root_env[CLI_PATH_MAX];
    const char *project_root =
        cli_copy_environment_value("COPPER_PROJECT", project_root_env,
                                   sizeof(project_root_env))
            ? project_root_env : NULL;
    const char *user_root_arg = NULL;
    (void)option_value(argc, argv, "--scope", &scope);
    (void)option_value(argc, argv, "--project-root", &project_root);
    (void)option_value(argc, argv, "--user-root", &user_root_arg);
    if (!scope || (strcmp(scope, "user") != 0 && strcmp(scope, "project") != 0)) {
        fprintf(stderr, "ERROR: --scope must be user or project\n"); return 2;
    }
    char directory[CLI_PATH_MAX], base[CLI_PATH_MAX];
    if (strcmp(scope, "project") == 0) {
        if (!project_root && fs_get_current_directory(base, sizeof(base)) != FS_OK) return 1;
        if (!project_root) project_root = base;
        if (fs_path_join(base, sizeof(base), project_root, ".copper") != FS_OK ||
            fs_path_join(directory, sizeof(directory), base, "catalogues") != FS_OK) return 1;
    } else if (user_root_arg) {
        if (fs_path_join(directory, sizeof(directory), user_root_arg, "catalogues") != FS_OK) return 1;
    } else if (!default_user_root(directory, sizeof(directory))) return 1;

    FsDirectoryListing listing;
    FsStatus status = fs_list_directory(directory, &listing);
    if (status == FS_ERR_NOT_DIRECTORY) {
        printf("No %s catalogues installed at %s\n", scope, directory);
        return 0;
    }
    if (status != FS_OK) { fprintf(stderr, "ERROR: cannot read %s\n", directory); return 1; }
    if (listing.count > 1)
        qsort(listing.entries, listing.count, sizeof(*listing.entries), compare_entries);
    printf("%s catalogues: %s\n", scope, directory);
    size_t count = 0;
    for (size_t i = 0; i < listing.count; ++i) {
        if (listing.entries[i].is_regular_file && suffix_is(listing.entries[i].name, ".parts")) {
            puts(listing.entries[i].name); ++count;
        }
    }
    if (!count) puts("(none)");
    fs_free_directory_listing(&listing);
    return 0;
}

static bool copy_cli_text(char *out, size_t capacity, const char *text) {
    if (!out || capacity == 0 || !text) return false;
    size_t length = strlen(text);
    if (length >= capacity) return false;
    memcpy(out, text, length + 1);
    return true;
}

static int find_part_in_scope(const char *directory, const char *part_id,
                              const char *scope_name, bool *out_found) {
    *out_found = false;
    FsDirectoryListing listing;
    FsStatus fs = fs_list_directory(directory, &listing);
    if (fs == FS_ERR_NOT_DIRECTORY || fs == FS_ERR_NOT_FOUND) return 0;
    if (fs != FS_OK) {
        fprintf(stderr, "ERROR: cannot enumerate %s catalogue scope: %s\n",
                scope_name, directory);
        return 2;
    }
    if (listing.count > 1)
        qsort(listing.entries, listing.count, sizeof(*listing.entries), compare_entries);

    size_t matches = 0;
    char match_path[CLI_PATH_MAX] = {0};
    char match_id[CLI_PATH_MAX] = {0};
    char match_manufacturer[CLI_PATH_MAX] = {0};
    char match_mpn[CLI_PATH_MAX] = {0};
    char match_symbol[CLI_PATH_MAX] = {0};
    char match_footprint[CLI_PATH_MAX] = {0};
    char match_model[CLI_PATH_MAX] = {0};
    bool match_verified = false;

    for (size_t i = 0; i < listing.count; ++i) {
        if (!listing.entries[i].is_regular_file ||
            !suffix_is(listing.entries[i].name, ".parts")) continue;
        char path[CLI_PATH_MAX];
        if (fs_path_join(path, sizeof(path), directory, listing.entries[i].name) != FS_OK) {
            fs_free_directory_listing(&listing);
            return 2;
        }
        Arena arena;
        arena_init(&arena, 65536);
        PartCatalogue catalogue;
        PartCatalogueError error;
        PartCatalogueStatus status = part_catalogue_parse_file(&arena, path, &catalogue, &error);
        if (status != PART_CATALOGUE_OK) {
            fprintf(stderr, "ERROR: invalid installed catalogue %s: %s\n",
                    path, error.message ? error.message : part_catalogue_status_name(status));
            arena_destroy(&arena);
            fs_free_directory_listing(&listing);
            return 2;
        }

        const PartRecord *part = part_catalogue_find(&catalogue, part_id);
        if (part) {
            ++matches;
            if (matches == 1) {
                bool copied =
                    copy_cli_text(match_path, sizeof(match_path), path) &&
                    copy_cli_text(match_id, sizeof(match_id), part->id) &&
                    copy_cli_text(match_manufacturer, sizeof(match_manufacturer), part->manufacturer ? part->manufacturer : "") &&
                    copy_cli_text(match_mpn, sizeof(match_mpn), part->mpn ? part->mpn : "") &&
                    copy_cli_text(match_symbol, sizeof(match_symbol), part->symbol_id ? part->symbol_id : "") &&
                    copy_cli_text(match_footprint, sizeof(match_footprint), part->footprint_id ? part->footprint_id : "") &&
                    copy_cli_text(match_model, sizeof(match_model), part->model_id ? part->model_id : "");
                if (!copied) {
                    arena_destroy(&arena);
                    fs_free_directory_listing(&listing);
                    fprintf(stderr, "ERROR: part record contains a field longer than the CLI output limit\n");
                    return 2;
                }
                match_verified = part_record_sources_verified(part);
            }
        }
        arena_destroy(&arena);
    }

    fs_free_directory_listing(&listing);
    if (matches > 1) {
        fprintf(stderr, "ERROR: part id '%s' appears in %zu catalogues in %s scope; resolve the duplicate explicitly\n",
                part_id, matches, scope_name);
        return 2;
    }
    if (matches == 1) {
        printf("FOUND in %s scope: %s\n", scope_name, match_path);
        printf("  id: %s\n  manufacturer: %s\n  mpn: %s\n  symbol: %s\n  footprint: %s\n  model: %s\n",
               match_id, match_manufacturer, match_mpn, match_symbol, match_footprint, match_model);
        printf("  sources: %s\n", match_verified ? "verified" : "UNVERIFIED");
        *out_found = true;
    }
    return 0;
}

static PartKind cli_parse_part_kind(const char *text) {
    if (!text) return PART_KIND_INVALID;
    if (!strcmp(text, "R") || !strcmp(text, "resistor")) return PART_KIND_RESISTOR;
    if (!strcmp(text, "C") || !strcmp(text, "capacitor")) return PART_KIND_CAPACITOR;
    if (!strcmp(text, "L") || !strcmp(text, "inductor")) return PART_KIND_INDUCTOR;
    if (!strcmp(text, "D") || !strcmp(text, "diode") || !strcmp(text, "led")) return PART_KIND_DIODE;
    if (!strcmp(text, "Q") || !strcmp(text, "transistor")) return PART_KIND_TRANSISTOR;
    if (!strcmp(text, "M") || !strcmp(text, "mosfet")) return PART_KIND_MOSFET;
    if (!strcmp(text, "U") || !strcmp(text, "ic") || !strcmp(text, "mcu")) return PART_KIND_IC;
    if (!strcmp(text, "J") || !strcmp(text, "connector")) return PART_KIND_CONNECTOR;
    if (!strcmp(text, "S") || !strcmp(text, "switch")) return PART_KIND_SWITCH;
    return PART_KIND_INVALID;
}

static int command_catalogue_search(int argc, char **argv) {
    if (argc < 4) return 64;
    const char *path = argv[3];
    const char *kind = cli_option_value(argc, argv, "--kind");
    const char *manufacturer = cli_option_value(argc, argv, "--manufacturer");
    const char *mpn = cli_option_value(argc, argv, "--mpn");
    const char *package = cli_option_value(argc, argv, "--package");
    const char *symbol = cli_option_value(argc, argv, "--symbol");
    const char *footprint = cli_option_value(argc, argv, "--footprint");
    const char *model = cli_option_value(argc, argv, "--model");
    const char *text_query = cli_option_value(argc, argv, "--text");
    const char *source_arg = cli_option_value(argc, argv, "--source");

    DesignCatalogueSource source = DESIGN_CATALOGUE_KICAD;
    if (source_arg) {
        if (!strcmp(source_arg, "kicad")) source = DESIGN_CATALOGUE_KICAD;
        else if (!strcmp(source_arg, "user")) source = DESIGN_CATALOGUE_USER;
        else if (!strcmp(source_arg, "supplier")) source = DESIGN_CATALOGUE_SUPPLIER;
        else {
            fprintf(stderr, "ERROR: --source must be kicad, user, or supplier\n");
            return 64;
        }
    }
    PartKind parsed_kind = cli_parse_part_kind(kind);
    if (kind && parsed_kind == PART_KIND_INVALID) {
        fprintf(stderr, "ERROR: unsupported --kind '%s'\n", kind);
        return 64;
    }

    Arena arena;
    arena_init(&arena, 32768u);
    PartCatalogue catalogue;
    PartCatalogueError error;
    PartCatalogueStatus ps = part_catalogue_parse_file(&arena, path, &catalogue, &error);
    if (ps != PART_CATALOGUE_OK) {
        fprintf(stderr, "ERROR: catalogue load failed: %s field=%s message=%s\n",
                part_catalogue_status_name(ps),
                error.field ? error.field : "-",
                error.message ? error.message : "-");
        arena_destroy(&arena);
        return 1;
    }

    DesignCatalogueView view;
    memset(&view, 0, sizeof(view));
    view.catalogue = &catalogue;
    view.source = source;
    view.name = path;
    view.priority = design_catalogue_default_priority(source);

    DesignPartQuery query;
    memset(&query, 0, sizeof(query));
    query.kind = parsed_kind;
    query.manufacturer = manufacturer;
    query.mpn = mpn;
    query.package = package;
    query.symbol_id = symbol;
    query.footprint_id = footprint;
    query.model_id = model;
    query.text = text_query;
    query.require_symbol = cli_option_value(argc, argv, "--require-symbol") != NULL;
    query.require_footprint = cli_option_value(argc, argv, "--require-footprint") != NULL;
    query.require_model = cli_option_value(argc, argv, "--require-model") != NULL;
    query.require_verified_identity = cli_option_value(argc, argv, "--require-verified") != NULL;

    DesignResolveResult result;
    DesignResolveStatus rs = design_resolve_part(&arena, &view, 1, &query, &result);
    printf("Catalogue: %s\nSource: %s (priority %d)\nCandidates: %zu\nResolution: %s\n",
           path, design_catalogue_source_name(source), view.priority,
           result.candidate_count, design_resolve_status_name(rs));
    for (size_t i = 0; i < result.candidate_count; ++i) {
        const DesignResolveCandidate *c = &result.candidates[i];
        const PartRecord *part = c->part;
        printf("%c %s  score=%d  manufacturer=%s  mpn=%s  package=%s\n",
               (i == 0 ? '*' : ' '), part->id ? part->id : "(no-id)",
               c->score, part->manufacturer ? part->manufacturer : "UNKNOWN",
               part->mpn ? part->mpn : "UNKNOWN",
               part->package ? part->package : "UNKNOWN");
        printf("    symbol=%s footprint=%s model=%s pins=%zu missing=0x%08x\n",
               part->symbol_id ? part->symbol_id : "UNRESOLVED",
               part->footprint_id ? part->footprint_id : "UNRESOLVED",
               part->model_id ? part->model_id : "UNRESOLVED",
               part->pin_count, c->missing_fields);
        printf("    capabilities: symbol=%s footprint=%s simulation=%s manufacturing=%s identity=%s ratings=%s\n",
               c->usable_for_symbol ? "yes" : "no",
               c->usable_for_footprint ? "yes" : "no",
               c->usable_for_simulation ? "yes" : "no",
               c->usable_for_manufacturing ? "yes" : "no",
               c->identity_verified ? "verified" : "UNVERIFIED",
               c->ratings_complete ? "complete" : "INCOMPLETE");
    }
    arena_destroy(&arena);
    return rs == DESIGN_RESOLVE_OK ? 0 :
           rs == DESIGN_RESOLVE_AMBIGUOUS ? 4 :
           rs == DESIGN_RESOLVE_NOT_FOUND ? 3 :
           rs == DESIGN_RESOLVE_INCOMPLETE ? 2 : 1;
}

static int command_catalogue_resolve(int argc, char **argv) {
    const char *part_id = argv[3];
    const char *project_root = NULL;
    const char *user_root_arg = NULL;
    const char *bundled_root_arg = NULL;
    (void)option_value(argc, argv, "--project-root", &project_root);
    (void)option_value(argc, argv, "--user-root", &user_root_arg);
    (void)option_value(argc, argv, "--bundled-root", &bundled_root_arg);

    char cwd[CLI_PATH_MAX], project_copper[CLI_PATH_MAX];
    if (!project_root) {
        if (fs_get_current_directory(cwd, sizeof(cwd)) != FS_OK) return 1;
        project_root = cwd;
    }
    char project_dir[CLI_PATH_MAX];
    if (fs_path_join(project_copper, sizeof(project_copper), project_root, ".copper") != FS_OK ||
        fs_path_join(project_dir, sizeof(project_dir), project_copper, "catalogues") != FS_OK)
        return 1;

    char user_dir[CLI_PATH_MAX];
    if (user_root_arg) {
        char user_home[CLI_PATH_MAX];
        if (fs_path_join(user_home, sizeof(user_home), user_root_arg, "catalogues") != FS_OK) return 1;
        memcpy(user_dir, user_home, strlen(user_home) + 1);
    } else if (!default_user_root(user_dir, sizeof(user_dir))) {
        user_dir[0] = '\0';
    }

    char bundled_dir[CLI_PATH_MAX];
    if (bundled_root_arg) {
        if (!copy_cli_text(bundled_dir, sizeof(bundled_dir), bundled_root_arg)) {
            fprintf(stderr, "ERROR: --bundled-root path is too long\\n");
            return 2;
        }
    } else {
        if (fs_path_join(bundled_dir, sizeof(bundled_dir), ".", "catalogues") != FS_OK) return 1;
    }

    const struct { const char *scope; const char *path; } scopes[] = {
        {"project", project_dir},
        {"user", user_dir},
        {"bundled", bundled_dir}
    };
    for (size_t i = 0; i < sizeof(scopes) / sizeof(scopes[0]); ++i) {
        if (!scopes[i].path || !scopes[i].path[0]) continue;
        bool found = false;
        int status = find_part_in_scope(scopes[i].path, part_id, scopes[i].scope, &found);
        if (status != 0) return status;
        if (found) return 0;
    }
    printf("NOT FOUND: %s in project, user or bundled catalogue scope\n", part_id);
    return 3;
}


static bool cli_ensure_parent_directory(const char *file_path) {
    char parent[CLI_PATH_MAX];
    size_t length;
    if (!file_path || !file_path[0]) return false;
    length = strlen(file_path);
    if (length >= sizeof(parent)) return false;
    memcpy(parent, file_path, length + 1u);
    char *slash = strrchr(parent, '/');
    char *backslash = strrchr(parent, '\\');
    if (!slash || (backslash && backslash > slash)) slash = backslash;
    if (!slash) return true;
    if (slash == parent) return true;
    *slash = '\0';
    FsStatus fs = fs_create_directories(parent);
    return fs == FS_OK || fs == FS_ERR_ALREADY_EXISTS;
}

static int command_design_import(int argc, char **argv) {
    if (argc < 4) return 2;
    const char *project_dir = argv[3];
    const char *out_path = cli_option_value(argc, argv, "--out");
    const char *netlist_path = cli_option_value(argc, argv, "--netlist");
    const char *report_path = cli_option_value(argc, argv, "--report");
    const char *manifest_path = cli_option_value(argc, argv, "--manifest");
    const char *design_path;
    const char *diagnostic_path;
    Arena arena;
    CopperDesign design;
    CopperDesignImportOptions options;
    memset(&options, 0, sizeof(options));
    options.project_directory = project_dir;
    options.netlist_path = netlist_path;
    options.diagnostic_capacity = 256u;
    arena_init(&arena, 65536u);
    CopperDesignStatus status = copper_design_import_project(&arena, &options, &design);
    if (status != COPPER_DESIGN_OK) {
        size_t text_length = 0;
        char *text = diagnostic_report_to_text(&design.diagnostics, &text_length);
        (void)text_length;
        fprintf(stderr, "ERROR: project import failed: %s\n", copper_design_status_name(status));
        if (text) { fputs(text, stderr); free(text); }
        arena_destroy(&arena);
        return 1;
    }
    CopperProjectError workspace_error;
    CopperProjectStatus workspace_status = copper_project_ensure_workspace(
        &design.project, &workspace_error);
    if (workspace_status != COPPER_PROJECT_OK) {
        fprintf(stderr, "ERROR: could not create Copper workspace: %s\n",
                workspace_error.message ? workspace_error.message : copper_project_status_message(workspace_status));
        arena_destroy(&arena);
        return 1;
    }
    design_path = out_path ? out_path : design.project.design_file;
    diagnostic_path = report_path ? report_path : design.project.import_report_file;
    if (!manifest_path) manifest_path = design.project.manifest_file;
    if (!cli_ensure_parent_directory(design_path) ||
        !cli_ensure_parent_directory(diagnostic_path) ||
        !cli_ensure_parent_directory(manifest_path)) {
        fprintf(stderr, "ERROR: could not create output parent directories\n");
        arena_destroy(&arena); return 1;
    }
    FsStatus write_status = copper_design_json_write_file(&design, design_path);
    if (write_status != FS_OK) {
        fprintf(stderr, "ERROR: design JSON write failed: %s\n", fs_status_name(write_status));
        arena_destroy(&arena); return 1;
    }
    size_t report_length = 0;
    char *report_json = diagnostic_report_to_json(&design.diagnostics, &report_length);
    if (!report_json) {
        fprintf(stderr, "ERROR: diagnostic report serialization failed\n");
        arena_destroy(&arena); return 1;
    }
    write_status = fs_write_file_atomic_replace(diagnostic_path, report_json, report_length);
    free(report_json);
    if (write_status != FS_OK) {
        fprintf(stderr, "ERROR: diagnostic report write failed: %s\n", fs_status_name(write_status));
        arena_destroy(&arena); return 1;
    }
    write_status = copper_project_manifest_write_file(&design, design_path,
                                                       diagnostic_path,
                                                       manifest_path);
    if (write_status != FS_OK) {
        fprintf(stderr, "ERROR: project manifest write failed: %s\n", fs_status_name(write_status));
        arena_destroy(&arena);
        return 1;
    }
    size_t text_length = 0;
    char *text = diagnostic_report_to_text(&design.diagnostics, &text_length);
    (void)text_length;
    printf("Copper design import: OK\n");
    printf("Project: %s\n", design.name);
    printf("Schematic symbols: %zu  Library symbols: %zu  Wires: %zu  Labels: %zu\n",
           design.schematic.symbol_count, design.schematic.library_symbol_count,
           design.schematic.wire_count, design.schematic.label_count);
    if (design.has_board)
        printf("PCB footprints: %zu  Pads: %zu  Tracks: %zu  Vias: %zu  Zones: %zu\n",
               design.board.footprint_count, design.board.pad_count,
               design.board.track_count, design.board.via_count, design.board.zone_count);
    printf("Parity: matched=%zu missing-footprints=%zu extra-footprints=%zu pad-net-checks=%zu mismatches=%zu\n",
           design.parity.matched_components, design.parity.missing_footprints,
           design.parity.extra_footprints, design.parity.checked_pad_nets,
           design.parity.pad_net_mismatches);
    printf("Design JSON: %s\nDiagnostics JSON: %s\nManifest: %s\n", design_path, diagnostic_path, manifest_path);
    if (text) { fputs(text, stdout); free(text); }
    arena_destroy(&arena);
    return 0;
}


static int command_design_resolve_part(int argc, char **argv) {
    if (argc < 4) return 64;
    const char *primary_path = argv[3];
    const char *part_id = cli_option_value(argc, argv, "--part-id");
    const char *kind_text = cli_option_value(argc, argv, "--kind");
    const char *mpn = cli_option_value(argc, argv, "--mpn");
    const char *manufacturer = cli_option_value(argc, argv, "--manufacturer");
    const char *package = cli_option_value(argc, argv, "--package");
    const char *symbol_id = cli_option_value(argc, argv, "--symbol");
    const char *footprint_id = cli_option_value(argc, argv, "--footprint");
    const char *model_id = cli_option_value(argc, argv, "--model");
    const char *text_query = cli_option_value(argc, argv, "--text");
    const char *source_text = cli_option_value(argc, argv, "--source");
    const char *out_path = cli_option_value(argc, argv, "--out");
    const char *symbol_root = cli_option_value(argc, argv, "--symbol-root");
    const char *footprint_root = cli_option_value(argc, argv, "--footprint-root");
    const char *copper_asset_root = cli_option_value(argc, argv, "--copper-asset-root");
    const char *project_directory = cli_option_value(argc, argv, "--project-root");
    const char *symbol_table_path = cli_option_value(argc, argv, "--sym-lib-table");
    const char *footprint_table_path = cli_option_value(argc, argv, "--fp-lib-table");

    if (!part_id && !kind_text && !mpn && !text_query && !symbol_id && !footprint_id) {
        fprintf(stderr, "ERROR: provide at least one query field: --part-id, --kind, --mpn, --text, --symbol or --footprint\n");
        return 64;
    }
    DesignCatalogueSource primary_source = DESIGN_CATALOGUE_USER;
    if (source_text) {
        if (strcmp(source_text, "kicad") == 0) primary_source = DESIGN_CATALOGUE_KICAD;
        else if (strcmp(source_text, "user") == 0) primary_source = DESIGN_CATALOGUE_USER;
        else if (strcmp(source_text, "supplier") == 0) primary_source = DESIGN_CATALOGUE_SUPPLIER;
        else { fprintf(stderr, "ERROR: --source must be kicad, user or supplier\n"); return 64; }
    }
    PartKind kind = cli_parse_part_kind(kind_text);
    if (kind_text && kind == PART_KIND_INVALID) {
        fprintf(stderr, "ERROR: unsupported --kind '%s'\n", kind_text);
        return 64;
    }

    /* Repeated supplemental catalogue options are bounded and deterministic. */
    enum { MAX_CATALOGUES = 16 };
    const char *paths[MAX_CATALOGUES];
    DesignCatalogueSource sources[MAX_CATALOGUES];
    const char *names[MAX_CATALOGUES];
    size_t catalogue_count = 1u;
    paths[0] = primary_path; sources[0] = primary_source; names[0] = primary_path;
    for (int i = 4; i + 1 < argc; ++i) {
        DesignCatalogueSource src;
        if (strcmp(argv[i], "--user-catalogue") == 0) src = DESIGN_CATALOGUE_USER;
        else if (strcmp(argv[i], "--supplier-catalogue") == 0) src = DESIGN_CATALOGUE_SUPPLIER;
        else if (strcmp(argv[i], "--kicad-catalogue") == 0) src = DESIGN_CATALOGUE_KICAD;
        else continue;
        if (catalogue_count >= MAX_CATALOGUES) {
            fprintf(stderr, "ERROR: at most %u catalogue files may be combined\n", (unsigned)MAX_CATALOGUES);
            return 64;
        }
        paths[catalogue_count] = argv[++i];
        sources[catalogue_count] = src;
        names[catalogue_count] = paths[catalogue_count];
        ++catalogue_count;
    }

    Arena arena;
    arena_init(&arena, 65536u);
    PartCatalogue catalogues[MAX_CATALOGUES];
    DesignCatalogueView views[MAX_CATALOGUES];
    memset(catalogues, 0, sizeof(catalogues));
    memset(views, 0, sizeof(views));
    for (size_t i = 0; i < catalogue_count; ++i) {
        PartCatalogueError parse_error;
        PartCatalogueStatus ps = part_catalogue_parse_file(&arena, paths[i], &catalogues[i], &parse_error);
        if (ps != PART_CATALOGUE_OK) {
            fprintf(stderr, "ERROR: catalogue '%s' failed: %s (%s: %s)\n",
                    paths[i], part_catalogue_status_name(ps),
                    parse_error.field ? parse_error.field : "field",
                    parse_error.message ? parse_error.message : "parse error");
            arena_destroy(&arena);
            return 1;
        }
        views[i].catalogue = &catalogues[i];
        views[i].source = sources[i];
        views[i].name = names[i];
        views[i].priority = design_catalogue_default_priority(sources[i]);
    }

    DesignPartQuery query;
    memset(&query, 0, sizeof(query));
    query.part_id = part_id;
    query.kind = kind;
    query.manufacturer = manufacturer;
    query.mpn = mpn;
    query.package = package;
    query.symbol_id = symbol_id;
    query.footprint_id = footprint_id;
    query.model_id = model_id;
    query.text = text_query;
    query.require_symbol = cli_option_value(argc, argv, "--require-symbol") != NULL;
    query.require_footprint = cli_option_value(argc, argv, "--require-footprint") != NULL;
    query.require_model = cli_option_value(argc, argv, "--require-model") != NULL;
    query.require_verified_identity = cli_option_value(argc, argv, "--require-verified") != NULL;

    DesignResolveResult result;
    DesignResolveStatus resolve_status = design_resolve_part(&arena, views,
        catalogue_count, &query, &result);
    KicadAssetRootOptions root_options;
    memset(&root_options, 0, sizeof(root_options));
    root_options.symbol_root = symbol_root;
    root_options.footprint_root = footprint_root;
    root_options.copper_asset_root = copper_asset_root;
    root_options.project_directory = project_directory;
    root_options.symbol_table_path = symbol_table_path;
    root_options.footprint_table_path = footprint_table_path;
    KicadAssetRoots roots;
    KicadAssetDiscoveryStatus discovery = kicad_asset_roots_discover(&arena,
        &root_options, &roots);

    KicadPartAssetResolution asset_result;
    memset(&asset_result, 0, sizeof(asset_result));
    KicadAssetError asset_error;
    bool has_assets = result.best && result.best->part;
    if (has_assets) (void)kicad_part_asset_resolve(&arena, result.best->part,
                                                  &roots, &asset_result,
                                                  &asset_error);

    char default_output[CLI_PATH_MAX];
    if (!out_path) {
        if (fs_get_current_directory(default_output, sizeof(default_output)) != FS_OK ||
            fs_path_join(default_output, sizeof(default_output), default_output,
                         "design.json") != FS_OK) {
            arena_destroy(&arena); return 1;
        }
        out_path = default_output;
    }
    if (!cli_ensure_parent_directory(out_path)) {
        fprintf(stderr, "ERROR: cannot create output directory for '%s'\n", out_path);
        arena_destroy(&arena); return 1;
    }

    DesignResolutionJsonInput json_input;
    memset(&json_input, 0, sizeof(json_input));
    json_input.catalogue_path = primary_path;
    json_input.query_kind = kind_text;
    json_input.query_mpn = mpn;
    json_input.query_manufacturer = manufacturer;
    json_input.query_text = text_query;
    json_input.resolution = &result;
    json_input.asset_roots = &roots;
    json_input.assets = has_assets ? &asset_result : NULL;
    FsStatus write_status = design_resolution_json_write_file(&json_input, out_path);

    printf("Catalogue resolution: %s\n", design_resolve_status_name(resolve_status));
    printf("KiCad asset discovery: %s\n", kicad_asset_discovery_status_name(discovery));
    printf("  Symbol root: %s [%s]\n", roots.symbol_root ? roots.symbol_root : "LIBRARY TABLE",
           roots.symbol_source ? roots.symbol_source : (roots.symbol_table_path ? roots.symbol_table_path : "no source"));
    printf("  Footprint root: %s [%s]\n", roots.footprint_root ? roots.footprint_root : "LIBRARY TABLE",
           roots.footprint_source ? roots.footprint_source : (roots.footprint_table_path ? roots.footprint_table_path : "no source"));
    printf("Candidates: %zu\n", result.candidate_count);
    if (result.best && result.best->part) {
        const PartRecord *part = result.best->part;
        printf("Selected: %s  %s  %s\n", part->id ? part->id : "(no id)",
               part->manufacturer ? part->manufacturer : "UNKNOWN",
               part->mpn ? part->mpn : "UNKNOWN");
        printf("  Symbol: %s -> %s\n", part->symbol_id ? part->symbol_id : "UNASSIGNED",
               asset_result.symbol_found ? (asset_result.symbol_is_suggestion ? "SUGGESTED (not assigned)" : "FOUND") : "NOT RESOLVED");
        printf("  Footprint: %s -> %s\n", part->footprint_id ? part->footprint_id : "UNASSIGNED",
               asset_result.footprint_found ? (asset_result.footprint_is_suggestion ? "SUGGESTED (not assigned)" : "FOUND") : "NOT RESOLVED");
        if (asset_result.suggested_symbol_id)
            printf("  Symbol suggestion (not assigned): %s\n", asset_result.suggested_symbol_id);
        if (asset_result.suggested_footprint_filter)
            printf("  KiCad footprint filter: %s\n", asset_result.suggested_footprint_filter);
        printf("  Pin mapping: %s\n", kicad_pin_map_resolution_name(asset_result.pin_map_resolution));
        if (asset_result.message) printf("  Feedback: %s\n", asset_result.message);
    }
    if (write_status != FS_OK) {
        fprintf(stderr, "ERROR: could not write JSON '%s': %s\n", out_path, fs_status_name(write_status));
        arena_destroy(&arena); return 1;
    }
    printf("Design preview JSON: %s\n", out_path);

    int exit_status;
    if (resolve_status == DESIGN_RESOLVE_NOT_FOUND) exit_status = 3;
    else if (resolve_status == DESIGN_RESOLVE_AMBIGUOUS) exit_status = 4;
    else if (resolve_status != DESIGN_RESOLVE_OK) exit_status = 2;
    else if (!has_assets || !asset_result.symbol_found || !asset_result.footprint_found ||
             !asset_result.explicit_mapping_valid) exit_status = 2;
    else exit_status = 0;
    arena_destroy(&arena);
    return exit_status;
}


static int command_topology(int argc, char **argv, bool validate_only) {
    if (argc < 5) return 64;
    const char *schematic_path = argv[4];
    const char *netlist_path = cli_option_value(argc, argv, "--netlist");
    const char *out_path = cli_option_value(argc, argv, "--out");
    if (!netlist_path) {
        fprintf(stderr, "ERROR: --netlist is required for connectivity topology; schematic geometry alone is not treated as a proven net graph.\n");
        return 64;
    }
    Arena arena;
    SchematicIR schematic;
    SchematicDiag schematic_diag;
    KicadNetlist netlist;
    KicadNetlistDiag netlist_diag;
    CopperTopology topology;
    arena_init(&arena, 65536u);
    SchematicStatus ss = schematic_load_kicad(schematic_path, &arena, &schematic, &schematic_diag);
    if (ss != SCHEMATIC_OK) {
        fprintf(stderr, "ERROR: schematic load failed: %s\n", schematic_diag.message);
        arena_destroy(&arena); return 1;
    }
    KicadNetlistStatus ns = kicad_netlist_load(netlist_path, &arena, &netlist, &netlist_diag);
    if (ns != KICAD_NETLIST_OK) {
        fprintf(stderr, "ERROR: netlist load failed: %s\n", netlist_diag.message);
        arena_destroy(&arena); return 1;
    }
    CopperTopologyStatus ts = copper_topology_build(&arena, &schematic, &netlist, &topology);
    if (ts != COPPER_TOPOLOGY_OK && ts != COPPER_TOPOLOGY_INCOMPLETE) {
        fprintf(stderr, "ERROR: topology build failed: %s\n", copper_topology_status_name(ts));
        arena_destroy(&arena); return 1;
    }
    printf("Copper topology: %s\n", copper_topology_status_name(ts));
    printf("Components: %zu  Terminals: %zu  Nets: %zu  Connections: %zu  Issues: %zu\n",
           topology.component_count, topology.terminal_count, topology.net_count,
           topology.connection_count, topology.issue_count);
    for (size_t i = 0; i < topology.component_count; ++i) {
        const CopperTopologyComponent *component = &topology.components[i];
        printf("\n%s  %s  [%s]\n", component->reference ? component->reference : "?",
               component->value ? component->value : "",
               component->lib_id ? component->lib_id : "symbol-unresolved");
        for (size_t j = 0; j < component->terminal_count; ++j) {
            const CopperTopologyTerminal *pin =
                &topology.terminals[component->first_terminal + j];
            const char *net_name = pin->net_index == COPPER_TOPOLOGY_NO_NET
                ? "UNCONNECTED_OR_NC" : topology.nets[pin->net_index].name;
            printf("  pin %-6s %-12s -> %s\n", pin->number ? pin->number : "?",
                   pin->name ? pin->name : "", net_name ? net_name : "(unnamed)");
        }
    }
    for (size_t i = 0; i < topology.issue_count; ++i)
        printf("WARNING [%s] %s\n", topology.issues[i].code, topology.issues[i].message);
    if (out_path) {
        FsStatus ws = copper_topology_json_write_file(&topology, out_path);
        if (ws != FS_OK) {
            fprintf(stderr, "ERROR: topology JSON write failed: %s\n", fs_status_name(ws));
            arena_destroy(&arena); return 1;
        }
        printf("\nTopology JSON: %s\n", out_path);
    }
    arena_destroy(&arena);
    if (validate_only && ts != COPPER_TOPOLOGY_OK) return 2;
    return ts == COPPER_TOPOLOGY_OK ? 0 : 2;
}

static size_t topology_component_index_by_ref(const CopperTopology *topology,
                                              const char *reference) {
    for (size_t i = 0; i < topology->component_count; ++i)
        if (topology->components[i].reference && reference &&
            strcmp(topology->components[i].reference, reference) == 0) return i;
    return SIZE_MAX;
}

static bool parse_assignment(const char *text, char *left, size_t left_cap,
                             char *right, size_t right_cap) {
    if (!text || !left || !right || left_cap == 0u || right_cap == 0u) return false;
    const char *eq = strchr(text, '=');
    if (!eq || eq == text || !eq[1]) return false;
    size_t ln = (size_t)(eq - text), rn = strlen(eq + 1);
    if (ln >= left_cap || rn >= right_cap) return false;
    memcpy(left, text, ln); left[ln] = '\0';
    memcpy(right, eq + 1, rn + 1u);
    return true;
}

static const char *cli_arena_copy(Arena *arena, const char *text) {
    if (!arena || !text) return NULL;
    size_t n = strlen(text);
    char *copy = (char *)arena_alloc(arena, n + 1u);
    if (!copy) return NULL;
    memcpy(copy, text, n + 1u);
    return copy;
}

static bool parse_source_binding_value(const char *text, char *reference,
                                       size_t reference_cap,
                                       char *pin1, size_t pin1_cap,
                                       char *pin2, size_t pin2_cap,
                                       double *value) {
    char rhs[256];
    if (!parse_assignment(text, reference, reference_cap, rhs, sizeof(rhs))) return false;
    char *colon = strrchr(rhs, ':');
    if (!colon) return false;
    *colon++ = '\0';
    char *comma = strchr(rhs, ',');
    if (!comma) return false;
    *comma++ = '\0';
    if (!rhs[0] || !comma[0] || !colon[0]) return false;
    if (strlen(rhs) >= pin1_cap || strlen(comma) >= pin2_cap) return false;
    memcpy(pin1, rhs, strlen(rhs) + 1u);
    memcpy(pin2, comma, strlen(comma) + 1u);
    char *end = NULL;
    *value = strtod(colon, &end);
    return end && *end == '\0' && isfinite(*value);
}

static int command_engineering_run(int argc, char **argv) {
    if (argc < 5) return 64;
    const char *schematic_path = argv[4];
    const char *netlist_path = cli_option_value(argc, argv, "--netlist");
    const char *catalogue_path = cli_option_value(argc, argv, "--catalogue");
    const char *board_path = cli_option_value(argc, argv, "--board");
    const char *rules_path = cli_option_value(argc, argv, "--rules");
    const char *out_path = cli_option_value(argc, argv, "--out");
    const char *report_path = cli_option_value(argc, argv, "--report");
    const char *today_arg = cli_option_value(argc, argv, "--today");
    bool request_placement = cli_has_flag(argc, argv, "--propose-placement");
    bool request_routes = cli_has_flag(argc, argv, "--propose-routes");
    if ((request_placement || request_routes) && !board_path) {
        fprintf(stderr, "ERROR: --propose-placement and --propose-routes require --board FILE\n");
        return 64;
    }
    if (!netlist_path || !catalogue_path) {
        fprintf(stderr, "ERROR: engineering run requires --netlist and --catalogue; circuit connectivity and component models are never guessed.\n");
        return 64;
    }
    if (!out_path) out_path = "design.json";
    if (!report_path) report_path = "engineering_report.txt";

    Arena arena; arena_init(&arena, 131072u);
    SchematicIR schematic; SchematicDiag schematic_diag;
    KicadNetlist netlist; KicadNetlistDiag netlist_diag;
    CopperTopology topology;
    SchematicStatus ss = schematic_load_kicad(schematic_path, &arena, &schematic, &schematic_diag);
    if (ss != SCHEMATIC_OK) {
        fprintf(stderr, "ERROR: schematic import failed: %s\n", schematic_diag.message);
        arena_destroy(&arena); return 1;
    }
    KicadNetlistStatus ns = kicad_netlist_load(netlist_path, &arena, &netlist, &netlist_diag);
    if (ns != KICAD_NETLIST_OK) {
        fprintf(stderr, "ERROR: netlist import failed: %s\n", netlist_diag.message);
        arena_destroy(&arena); return 1;
    }
    CopperTopologyStatus ts = copper_topology_build(&arena, &schematic, &netlist, &topology);
    if (ts != COPPER_TOPOLOGY_OK && ts != COPPER_TOPOLOGY_INCOMPLETE) {
        fprintf(stderr, "ERROR: topology build failed: %s\n", copper_topology_status_name(ts));
        arena_destroy(&arena); return 1;
    }

    PartCatalogue catalogue; PartCatalogueError catalogue_error;
    PartCatalogueStatus pcs = part_catalogue_parse_file(&arena, catalogue_path,
                                                         &catalogue, &catalogue_error);
    if (pcs != PART_CATALOGUE_OK) {
        fprintf(stderr, "ERROR: catalogue load failed: %s (%s)\n",
                part_catalogue_status_name(pcs), catalogue_error.message ? catalogue_error.message : "unknown");
        arena_destroy(&arena); return 1;
    }
    EngineeringComponentBinding *bindings = topology.component_count ?
        (EngineeringComponentBinding *)arena_alloc_array(&arena, sizeof(*bindings), topology.component_count) : NULL;
    if (topology.component_count && !bindings) { arena_destroy(&arena); return 1; }
    if (bindings) memset(bindings, 0, sizeof(*bindings) * topology.component_count);
    for (size_t i = 0; i < topology.component_count; ++i)
        bindings[i].reference = topology.components[i].reference;

    for (int i = 5; i + 1 < argc; ++i) {
        if (strcmp(argv[i], "--part") == 0) {
            char ref[128], id[256];
            if (!parse_assignment(argv[++i], ref, sizeof(ref), id, sizeof(id))) {
                fprintf(stderr, "ERROR: --part must be REF=PART_ID\n"); arena_destroy(&arena); return 64;
            }
            size_t ci = topology_component_index_by_ref(&topology, ref);
            const PartRecord *part = part_catalogue_find(&catalogue, id);
            if (ci == SIZE_MAX || !part) {
                fprintf(stderr, "ERROR: component '%s' or catalogue part '%s' was not found\n", ref, id);
                arena_destroy(&arena); return 2;
            }
            bindings[ci].part = part;
            if (part->kind == PART_KIND_RESISTOR || part->kind == PART_KIND_CAPACITOR ||
                part->kind == PART_KIND_INDUCTOR) {
                if (part->pin_count >= 2u) {
                    bindings[ci].pin_count = 2u;
                    bindings[ci].pin_numbers[0] = part->pins[0].symbol_pin;
                    bindings[ci].pin_numbers[1] = part->pins[1].symbol_pin;
                } else {
                    const CopperTopologyComponent *component = &topology.components[ci];
                    if (component->terminal_count == 2u) {
                        bindings[ci].pin_count = 2u;
                        bindings[ci].pin_numbers[0] = topology.terminals[component->first_terminal].number;
                        bindings[ci].pin_numbers[1] = topology.terminals[component->first_terminal + 1u].number;
                    }
                }
            }
        } else if (strcmp(argv[i], "--voltage-source") == 0 ||
                   strcmp(argv[i], "--current-source") == 0) {
            bool is_voltage = strcmp(argv[i], "--voltage-source") == 0;
            char ref[128], pin1[64], pin2[64]; double value = 0.0;
            if (!parse_source_binding_value(argv[++i], ref, sizeof(ref), pin1, sizeof(pin1),
                                             pin2, sizeof(pin2), &value)) {
                fprintf(stderr, "ERROR: source must be REF=PIN1,PIN2:VALUE_SI\n");
                arena_destroy(&arena); return 64;
            }
            size_t ci = topology_component_index_by_ref(&topology, ref);
            if (ci == SIZE_MAX) {
                fprintf(stderr, "ERROR: source component '%s' not found in topology\n", ref);
                arena_destroy(&arena); return 2;
            }
            bindings[ci].has_element_template = true;
            bindings[ci].element_template.kind = is_voltage ? CIRCUIT_VOLTAGE_SOURCE : CIRCUIT_CURRENT_SOURCE;
            bindings[ci].element_template.value_si = value;
            bindings[ci].element_template.has_dc_value = true;
            bindings[ci].pin_count = 2u;
            bindings[ci].pin_numbers[0] = cli_arena_copy(&arena, pin1);
            bindings[ci].pin_numbers[1] = cli_arena_copy(&arena, pin2);
            if (!bindings[ci].pin_numbers[0] || !bindings[ci].pin_numbers[1]) {
                arena_destroy(&arena); return 1;
            }
        }
    }

    Board board; BoardDiag board_diag; bool has_board = false;
    if (board_path) {
        BoardStatus bs = board_load_kicad(board_path, &arena, &board, &board_diag);
        if (bs != BOARD_OK) {
            fprintf(stderr, "ERROR: board load failed: %s\n", board_diag.message);
            arena_destroy(&arena); return 1;
        }
        has_board = true;
    }
    FabRuleSet rules; bool has_rules = false;
    if (rules_path) {
        char today[11];
        if (today_arg) {
            if (strlen(today_arg) != 10u) { fprintf(stderr, "ERROR: --today must be YYYY-MM-DD\n"); arena_destroy(&arena); return 64; }
            memcpy(today, today_arg, 11u);
        } else current_date_utc(today);
        FabStatus fs = fab_rules_load(rules_path, &arena, today, 90u, &rules);
        if (fs != FAB_OK) {
            fprintf(stderr, "ERROR: fabrication rules failed validation: %s\n", fab_status_name(fs));
            arena_destroy(&arena); return 1;
        }
        has_rules = true;
    }

    EngineeringLoopOptions options = engineering_loop_default_options();
    options.finding_capacity = 512u;
    if (has_board && request_placement)
        options.run_placement_proposal = true;
    if (has_board && request_routes)
        options.run_router_proposals = true;
    const char *route_grid_arg = cli_option_value(argc, argv, "--route-grid-nm");
    const char *trace_width_arg = cli_option_value(argc, argv, "--trace-width-nm");
    const char *clearance_arg = cli_option_value(argc, argv, "--clearance-nm");
    const char *edge_clearance_arg = cli_option_value(argc, argv, "--edge-clearance-nm");
    if ((route_grid_arg && !cli_parse_positive_nm(route_grid_arg, &options.router_options.grid_pitch_nm)) ||
        (trace_width_arg && !cli_parse_positive_nm(trace_width_arg, &options.router_options.trace_width_nm)) ||
        (clearance_arg && !cli_parse_positive_nm(clearance_arg, &options.router_options.clearance_nm)) ||
        (edge_clearance_arg && !cli_parse_positive_nm(edge_clearance_arg, &options.router_options.edge_clearance_nm))) {
        fprintf(stderr, "ERROR: route geometry options must be positive integer nanometres\n");
        arena_destroy(&arena); return 64;
    }
    if (has_board && has_rules) {
        options.run_dfm = true;
        options.dfm_options.copper_oz_x10 = 10u;
        options.dfm_options.edge_type = cli_option_value(argc, argv, "--edge-type");
        if (!options.dfm_options.edge_type) options.dfm_options.edge_type = "routed";
        options.dfm_options.mask_color = cli_option_value(argc, argv, "--mask-color");
        if (!options.dfm_options.mask_color) options.dfm_options.mask_color = "standard";
        options.dfm_options.check_same_net_clearance = false;
    }
    EngineeringLoopResult result;
    EngineeringLoopStatus es = engineering_loop_run(&arena, &topology, bindings,
        topology.component_count, has_board ? &board : NULL,
        has_board ? &board_diag : NULL, has_rules ? &rules : NULL,
        &options, &result);
    if (es != ENGINEERING_LOOP_OK) {
        fprintf(stderr, "ERROR: engineering loop execution failed: %s\n", engineering_loop_status_name(es));
        arena_destroy(&arena); return 1;
    }
    if (!cli_ensure_parent_directory(out_path) || !cli_ensure_parent_directory(report_path)) {
        fprintf(stderr, "ERROR: cannot create output directories\n"); arena_destroy(&arena); return 1;
    }
    FsStatus ws = engineering_loop_json_write_file(&topology, &result, out_path);
    if (ws != FS_OK) {
        fprintf(stderr, "ERROR: engineering JSON write failed: %s\n", fs_status_name(ws));
        arena_destroy(&arena); return 1;
    }
    size_t report_length = 0;
    char *report = engineering_loop_report_to_text(&result, &report_length);
    if (!report) { arena_destroy(&arena); return 1; }
    ws = fs_write_file_atomic_replace(report_path, report, report_length);
    free(report);
    if (ws != FS_OK) {
        fprintf(stderr, "ERROR: engineering report write failed: %s\n", fs_status_name(ws));
        arena_destroy(&arena); return 1;
    }
    printf("Engineering loop: %s\n", engineering_loop_status_name(result.status));
    printf("Topology: components=%zu nets=%zu terminals=%zu connections=%zu\n",
           topology.component_count, topology.net_count, topology.terminal_count, topology.connection_count);
    printf("Circuit: %s; DC simulation: %s\n", result.circuit_built ? "built" : "incomplete",
           result.simulation_attempted ? mna_status_name(result.simulation_status) : "not run");
    printf("Feedback findings: %zu\nDesign ready: %s\n", result.finding_count,
           result.design_ready ? "YES" : "NO");
    printf("Design JSON: %s\nEngineering report: %s\n", out_path, report_path);
    int exit_code = result.status == ENGINEERING_LOOP_OK ? 0 :
                    result.status == ENGINEERING_LOOP_REJECTED ? 2 : 3;
    arena_destroy(&arena); return exit_code;
}


static bool cli_parse_size_value(const char *text, size_t *out) {
    if (!text || !out || !text[0]) return false;
    char *end = NULL;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno == ERANGE || !end || end == text || *end != '\0' || value > SIZE_MAX) return false;
    *out = (size_t)value;
    return true;
}

static bool cli_parse_u32_value(const char *text, uint32_t *out) {
    size_t value = 0;
    if (!cli_parse_size_value(text, &value) || value > UINT32_MAX) return false;
    *out = (uint32_t)value;
    return true;
}

static bool cli_parse_candidate_id(const char *text, uint64_t *out) {
    if (!text || !out || !text[0]) return false;
    if (strncmp(text, "0x", 2) == 0 || strncmp(text, "0X", 2) == 0) text += 2;
    char *end = NULL;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 16);
    if (errno == ERANGE || !end || end == text || *end != '\0' || value == 0u) return false;
    *out = (uint64_t)value;
    return true;
}

static bool cli_load_optimization_inputs(int argc, char **argv,
                                         Arena *arena, Board *board,
                                         BoardDiag *diag, FabRuleSet *rules,
                                         CopperOptOptions *options) {
    const char *rules_path = cli_option_value(argc, argv, "--rules");
    const char *today = cli_option_value(argc, argv, "--today");
    const char *oz_text = cli_option_value(argc, argv, "--copper-oz-x10");
    const char *edge = cli_option_value(argc, argv, "--edge");
    const char *mask = cli_option_value(argc, argv, "--mask-color");
    if (!rules_path || !today || !oz_text || !edge) {
        fprintf(stderr, "ERROR: optimization requires --rules, --today, --copper-oz-x10 and --edge. No fabrication assumptions are inferred.\n");
        return false;
    }
    uint32_t oz = 0;
    if (!cli_parse_u32_value(oz_text, &oz) || oz == 0u ||
        (strcmp(edge, "routed") != 0 && strcmp(edge, "vcut") != 0)) {
        fprintf(stderr, "ERROR: invalid copper weight or edge process. Use integer copper-oz-x10 and routed|vcut.\n");
        return false;
    }
    if (mask && strcmp(mask, "standard") != 0 && strcmp(mask, "black_white") != 0) {
        fprintf(stderr, "ERROR: --mask-color must be standard or black_white.\n");
        return false;
    }
    size_t max_age = 3650u;
    const char *age_text = cli_option_value(argc, argv, "--max-rule-age-days");
    if (age_text && (!cli_parse_size_value(age_text, &max_age) || max_age > UINT32_MAX)) {
        fprintf(stderr, "ERROR: invalid --max-rule-age-days value.\n");
        return false;
    }
    const char *board_path = argc > 4 ? argv[4] : NULL;
    if (!board_path || board_load_kicad(board_path, arena, board, diag) != BOARD_OK) {
        fprintf(stderr, "ERROR: could not load PCB board for optimization.\n");
        return false;
    }
    FabStatus fs = fab_rules_load(rules_path, arena, today, (uint32_t)max_age, rules);
    if (fs != FAB_OK) {
        fprintf(stderr, "ERROR: fabrication rules could not be loaded: %s\n", fab_status_name(fs));
        return false;
    }
    *options = copper_opt_default_options();
    options->dfm_options.copper_oz_x10 = oz;
    options->dfm_options.edge_type = edge;
    options->dfm_options.mask_color = mask;
    options->dfm_options.check_same_net_clearance = false;
    const char *value = cli_option_value(argc, argv, "--step-nm");
    if (value && !cli_parse_positive_nm(value, &options->move_step_nm)) return false;
    value = cli_option_value(argc, argv, "--component-gap-nm");
    if (value && !cli_parse_positive_nm(value, &options->component_gap_nm)) return false;
    value = cli_option_value(argc, argv, "--move-edge-clearance-nm");
    if (value && !cli_parse_positive_nm(value, &options->move_edge_clearance_nm)) return false;
    value = cli_option_value(argc, argv, "--max-candidates");
    if (value && !cli_parse_size_value(value, &options->max_candidates)) return false;
    options->propose_moves = !cli_has_flag(argc, argv, "--no-moves");
    options->propose_routes = cli_has_flag(argc, argv, "--propose-routes");
    if (options->propose_routes) {
        G2Nm grid = 0, width = 0, clearance = 0, edge_clearance = 0;
        if (!cli_parse_positive_nm(cli_option_value(argc, argv, "--route-grid-nm"), &grid) ||
            !cli_parse_positive_nm(cli_option_value(argc, argv, "--trace-width-nm"), &width) ||
            !cli_parse_positive_nm(cli_option_value(argc, argv, "--clearance-nm"), &clearance) ||
            !cli_parse_positive_nm(cli_option_value(argc, argv, "--route-edge-clearance-nm"), &edge_clearance)) {
            fprintf(stderr, "ERROR: route proposals require --route-grid-nm, --trace-width-nm, --clearance-nm and --route-edge-clearance-nm.\n");
            return false;
        }
        options->router_options.layer = BOARD_LAYER_F_CU;
        options->router_options.grid_pitch_nm = grid;
        options->router_options.trace_width_nm = width;
        options->router_options.clearance_nm = clearance;
        options->router_options.edge_clearance_nm = edge_clearance;
        options->router_options.max_cells = 1000000u;
    }
    return true;
}

static int command_optimize_evaluate(int argc, char **argv) {
    Arena arena;
    arena_init(&arena, 65536u);
    Board board; BoardDiag diag; FabRuleSet rules; CopperOptOptions options;
    memset(&board, 0, sizeof(board)); memset(&diag, 0, sizeof(diag));
    memset(&rules, 0, sizeof(rules));
    if (!cli_load_optimization_inputs(argc, argv, &arena, &board, &diag, &rules, &options)) {
        arena_destroy(&arena); return 64;
    }
    CopperOptEvaluation evaluation;
    CopperOptStatus status = copper_opt_evaluate(&arena, &board, &diag, &rules,
                                                  &options, &evaluation);
    if (status < 0) {
        fprintf(stderr, "ERROR: optimization evaluation failed: %s\n", copper_opt_status_name(status));
        arena_destroy(&arena); return 1;
    }
    size_t json_length = 0;
    char *json = copper_opt_evaluation_json(&evaluation, &json_length);
    const char *out_path = cli_option_value(argc, argv, "--out");
    if (!out_path) out_path = "optimization.json";
    if (!json || copper_opt_write_json(out_path, json, json_length) != FS_OK) {
        fprintf(stderr, "ERROR: could not write optimization evaluation JSON.\n");
        free(json); arena_destroy(&arena); return 1;
    }
    printf("Optimization evaluation: %s\n", copper_opt_status_name(status));
    printf("Board: %zu footprints, %zu tracks, HPWL=%llu nm\n", board.footprint_count,
           board.track_count, (unsigned long long)evaluation.baseline.hpwl_nm);
    printf("DFM: %zu FAIL, %zu UNVERIFIED; candidates=%zu, eligible=%zu\n",
           evaluation.baseline.dfm_fail_count, evaluation.baseline.dfm_unverified_count,
           evaluation.candidate_count, evaluation.eligible_count);
    printf("JSON: %s\n", out_path);
    puts("Decision authority: user/agent. Copper evaluates candidates but does not choose one.");
    free(json); arena_destroy(&arena); return 0;
}

static int command_optimize_apply(int argc, char **argv) {
    uint64_t candidate_id = 0;
    if (!cli_parse_candidate_id(cli_option_value(argc, argv, "--candidate-id"), &candidate_id)) {
        fprintf(stderr, "ERROR: --candidate-id must be a non-zero hexadecimal candidate ID.\n");
        return 64;
    }
    Arena arena; arena_init(&arena, 65536u);
    Board board; BoardDiag diag; FabRuleSet rules; CopperOptOptions options;
    memset(&board, 0, sizeof(board)); memset(&diag, 0, sizeof(diag)); memset(&rules, 0, sizeof(rules));
    if (!cli_load_optimization_inputs(argc, argv, &arena, &board, &diag, &rules, &options)) {
        arena_destroy(&arena); return 64;
    }
    CopperOptEvaluation evaluation;
    CopperOptStatus es = copper_opt_evaluate(&arena, &board, &diag, &rules, &options, &evaluation);
    if (es < 0) { arena_destroy(&arena); return 1; }
    const CopperOptCandidate *selected = NULL;
    for (size_t i = 0; i < evaluation.candidate_count; ++i)
        if (evaluation.candidates[i].action.candidate_id == candidate_id) {
            selected = &evaluation.candidates[i]; break;
        }
    if (!selected || !selected->eligible) {
        fprintf(stderr, "ERROR: candidate is unknown, stale or ineligible for this board/rules revision.\n");
        arena_destroy(&arena); return 2;
    }
    CopperOptApplyResult applied;
    CopperOptStatus as = copper_opt_apply(&arena, &board, &diag, &rules,
                                           &options, &selected->action, &applied);
    if (as != COPPER_OPT_OK) {
        fprintf(stderr, "ERROR: candidate apply rejected: %s\n", applied.message ? applied.message : copper_opt_status_name(as));
        arena_destroy(&arena); return 2;
    }
    size_t json_length = 0;
    char *json = copper_opt_apply_json(&applied, &json_length);
    const char *out_path = cli_option_value(argc, argv, "--out");
    if (!out_path) out_path = "optimization-apply.json";
    if (!json || copper_opt_write_json(out_path, json, json_length) != FS_OK) {
        fprintf(stderr, "ERROR: could not write candidate-board JSON snapshot.\n");
        free(json); arena_destroy(&arena); return 1;
    }
    printf("Candidate committed in Copper's in-memory board model: %s\n", out_path);
    printf("Tracks added: %zu; HPWL: %llu -> %llu nm\n", applied.added_track_count,
           (unsigned long long)applied.before.hpwl_nm, (unsigned long long)applied.after.hpwl_nm);
    puts("Native .kicad_pcb export is not part of this optimizer milestone; JSON is a candidate snapshot, not a replacement PCB file.");
    free(json); arena_destroy(&arena); return 0;
}

static bool greedy_optimizer_decision(void *context, size_t iteration,
                                      const CopperOptMetrics *baseline,
                                      const CopperOptCandidate *candidates,
                                      size_t candidate_count,
                                      uint64_t *selected_candidate_id) {
    (void)context; (void)iteration;
    const CopperOptCandidate *best = NULL;
    for (size_t i = 0; i < candidate_count; ++i) {
        if (!candidates[i].eligible) continue;
        if (!best || copper_opt_metrics_preferred(&candidates[i].metrics, &best->metrics))
            best = &candidates[i];
    }
    if (!best || !copper_opt_metrics_preferred(&best->metrics, baseline)) {
        *selected_candidate_id = 0;
        return true;
    }
    *selected_candidate_id = best->action.candidate_id;
    return true;
}

static int command_optimize_run(int argc, char **argv) {
    const char *policy = cli_option_value(argc, argv, "--policy");
    if (!policy || strcmp(policy, "greedy") != 0) {
        fprintf(stderr, "ERROR: developer run currently requires --policy greedy; production decisions are expected from an external AI/user callback.\n");
        return 64;
    }
    Arena arena; arena_init(&arena, 65536u);
    Board board; BoardDiag diag; FabRuleSet rules; CopperOptOptions options;
    memset(&board, 0, sizeof(board)); memset(&diag, 0, sizeof(diag)); memset(&rules, 0, sizeof(rules));
    if (!cli_load_optimization_inputs(argc, argv, &arena, &board, &diag, &rules, &options)) {
        arena_destroy(&arena); return 64;
    }
    options.max_iterations = 3u;
    const char *iterations = cli_option_value(argc, argv, "--iterations");
    if (iterations && (!cli_parse_size_value(iterations, &options.max_iterations) || options.max_iterations == 0u)) {
        arena_destroy(&arena); return 64;
    }
    CopperOptRunResult run;
    CopperOptStatus status = copper_opt_run(&arena, &board, &diag, &rules, &options,
                                            greedy_optimizer_decision, NULL, &run);
    if (status < 0) {
        fprintf(stderr, "ERROR: optimization run failed: %s\n", copper_opt_status_name(status));
        arena_destroy(&arena); return 1;
    }
    size_t json_length = 0;
    char *json = copper_opt_run_json(&run, &json_length);
    const char *out_path = cli_option_value(argc, argv, "--out");
    if (!out_path) out_path = "optimization-run.json";
    if (!json || copper_opt_write_json(out_path, json, json_length) != FS_OK) {
        free(json); arena_destroy(&arena); return 1;
    }
    printf("Optimization run: %s; iterations=%zu; accepted=%zu\n",
           copper_opt_status_name(status), run.history_count, run.accepted_actions);
    printf("Result: %s\n", out_path);
    puts("NOTE: greedy policy is a developer test harness only. The production tool interface exposes candidates to the AI for final selection.");
    free(json); arena_destroy(&arena); return 0;
}

static void prompt_json_escape(FILE *f, const char *s) {
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        switch (*p) {
        case '"': fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        case '\n': fputs("\\n", f); break;
        case '\r': fputs("\\r", f); break;
        case '\t': fputs("\\t", f); break;
        default:
            if (*p < 0x20) fprintf(f, "\\u%04x", (unsigned)*p);
            else fputc(*p, f);
            break;
        }
    }
    fputc('"', f);
}

static int command_user_design_prompt(int argc, char **argv) {
    const char *out_path = cli_option_value(argc, argv, "--out");
    if (!out_path) out_path = "design_request.draft.json";
    char prompt[8192];
    size_t used = 0;
    for (int i = 3; i < argc; ++i) {
        if (strcmp(argv[i], "--out") == 0) break;
        size_t n = strlen(argv[i]);
        if (n > sizeof(prompt) - used - 2u) {
            fprintf(stderr, "ERROR: prompt exceeds %zu bytes\n", sizeof(prompt) - 1u);
            return 64;
        }
        if (used) prompt[used++] = ' ';
        memcpy(prompt + used, argv[i], n);
        used += n;
    }
    prompt[used] = '\0';
    if (!used) {
        fprintf(stderr, "ERROR: provide a human design prompt\n");
        return 64;
    }
    FILE *f = fopen(out_path, "wb");
    if (!f) {
        fprintf(stderr, "ERROR: cannot open '%s' for writing\n", out_path);
        return 1;
    }
    fputs("{\n  \"schema\": \"copper.request-draft\",\n  \"schema_version\": 1,\n  \"status\": \"needs_structuring\",\n  \"raw_prompt\": ", f);
    prompt_json_escape(f, prompt);
    fputs(",\n  \"note\": \"Raw user intent only; no components, nets, or electrical assumptions have been inferred.\"\n}\n", f);
    if (fclose(f) != 0) {
        fprintf(stderr, "ERROR: write failed for '%s'\n", out_path);
        return 1;
    }
    printf("Design prompt captured: %s\n", out_path);
    printf("Bytes: %zu\n", used);
    puts("Status: needs_structuring (no AI interpretation or engineering design has been run)");
    return 0;
}

/* copper design check-intent: every claim in a model-written design intent checked against the installed KiCad
 * libraries (src/v2_src/intent_check.c). Exit 0 GROUNDED, 2 REJECTED, 1 I/O or memory error, 64 usage. */
static int command_design_check_intent(int argc, char **argv) {
    const char *intent_path = argv[3], *symbols = NULL, *footprints = NULL, *out_path = NULL;
    for (int i = 4; i < argc; i++) {
        if (i + 1 < argc && strcmp(argv[i], "--symbol-root") == 0) symbols = argv[++i];
        else if (i + 1 < argc && strcmp(argv[i], "--footprint-root") == 0) footprints = argv[++i];
        else if (i + 1 < argc && strcmp(argv[i], "--out") == 0) out_path = argv[++i];
        else { fprintf(stderr, "check-intent: unknown option %s\n", argv[i]); return 64; }
    }
    if (!symbols || !footprints) { fprintf(stderr, "check-intent needs --symbol-root and --footprint-root\n"); return 64; }
    Arena arena;
    arena_init(&arena, 1u << 20);
    const unsigned char *data = NULL;
    size_t size = 0;
    if (fs_read_file_arena(&arena, intent_path, &data, &size) != FS_OK) {
        fprintf(stderr, "check-intent: cannot read %s\n", intent_path);
        arena_destroy(&arena);
        return 1;
    }
    IntentCheckReport report;
    IntentCheckStatus st = intent_check(&arena, (const char *)data, size, symbols, footprints, &report);
    if (st == INTENT_CHECK_ERR_ARGUMENT || st == INTENT_CHECK_ERR_MEMORY) {
        fprintf(stderr, "check-intent: %s\n", intent_check_status_name(st));
        arena_destroy(&arena);
        return 1;
    }
    size_t len = 0;
    const char *json = intent_check_report_json(&arena, &report, &len);
    if (!json) { fprintf(stderr, "check-intent: out of memory\n"); arena_destroy(&arena); return 1; }
    if (out_path) {
        FILE *f = fopen(out_path, "wb");
        if (!f || fwrite(json, 1, len, f) != len) { if (f) fclose(f); fprintf(stderr, "check-intent: cannot write %s\n", out_path); arena_destroy(&arena); return 1; }
        fclose(f);
    }
    printf("Design intent: %s (%zu component(s), %zu pin(s), %zu net(s))\n", intent_check_status_name(st), report.components, report.pins, report.nets);
    for (size_t i = 0; i < report.count; i++) {
        const IntentFinding *f = &report.findings[i];
        printf("  [%s] %s%s%s%s%s\n", f->code, f->reference ? f->reference : "", f->item ? " " : "", f->item ? f->item : "",
               (f->reference || f->item) ? ": " : "", f->message);
    }
    if (report.truncated) printf("  ... more findings were not listed (limit %d)\n", INTENT_CHECK_MAX_FINDINGS);
    if (st == INTENT_CHECK_REJECTED) printf("Nothing was compiled. Fix the intent from the installed library data (pin numbers and names), then check again.\n");
    arena_destroy(&arena);
    return st == INTENT_CHECK_GROUNDED ? 0 : 2;
}

/* copper design compose: checked blocks -> design intent (compose.c). Writes compose_report.json always, and
 * intent.json only when the design composed. Exit 0 COMPOSED, 3 NEEDS_INPUT, 4 NOT_IMPLEMENTED, 1 REFUSED,
 * 2 INVALID_INPUT, 64 usage, 70 internal error. */
static int write_text_file(const char *dir, const char *name, const char *text, size_t len);
static int command_design_compose(int argc, char **argv) {
    const char *path = argv[3], *blocks = NULL, *symbols = NULL, *out_dir = NULL, *prompt_path = NULL;
    enum { MAX_CATS = 32 };
    const char *cat_paths[MAX_CATS];
    size_t ncat = 0;
    for (int i = 4; i < argc; i += 2) {
        if (i + 1 >= argc) { fprintf(stderr, "compose: %s needs a value\n", argv[i]); return 64; }
        if (!strcmp(argv[i], "--blocks")) blocks = argv[i + 1];
        else if (!strcmp(argv[i], "--prompt")) prompt_path = argv[i + 1];
        else if (!strcmp(argv[i], "--symbol-root")) symbols = argv[i + 1];
        else if (!strcmp(argv[i], "--out")) out_dir = argv[i + 1];
        else if (!strcmp(argv[i], "--parts")) {
            if (ncat == MAX_CATS) { fprintf(stderr, "compose: at most %d --parts files\n", MAX_CATS); return 64; }
            cat_paths[ncat++] = argv[i + 1];
        } else { fprintf(stderr, "compose: unknown option %s\n", argv[i]); return 64; }
    }
    if (!blocks || !symbols || !out_dir || !ncat) {
        fprintf(stderr, "compose needs --blocks DIR, --parts FILE (one or more), --symbol-root DIR and --out DIR\n");
        return 64;
    }
    Arena arena;
    arena_init(&arena, 1u << 22);
    PartCatalogue cats[MAX_CATS];
    const PartCatalogue *refs[MAX_CATS];
    for (size_t i = 0; i < ncat; ++i) {
        PartCatalogueError e;
        if (part_catalogue_parse_file(&arena, cat_paths[i], &cats[i], &e) != PART_CATALOGUE_OK) {
            printf("compose: %s does not parse as a part catalogue: %s\nSTATUS compose: INVALID_INPUT\n", cat_paths[i], e.message ? e.message : "error");
            arena_destroy(&arena);
            return 2;
        }
        refs[i] = &cats[i];
    }
    const unsigned char *data = NULL;
    size_t size = 0;
    if (fs_read_file_arena(&arena, path, &data, &size) != FS_OK) {
        printf("compose: cannot read %s\nSTATUS compose: INVALID_INPUT\n", path);
        arena_destroy(&arena);
        return 2;
    }
    const char *prompt = NULL;
    if (prompt_path) {
        const unsigned char *pdata = NULL;
        size_t psize = 0;
        char *ptext = NULL;
        if (fs_read_file_arena(&arena, prompt_path, &pdata, &psize) != FS_OK || !(ptext = arena_alloc(&arena, psize + 1))) {
            printf("compose: cannot read the prompt %s\nSTATUS compose: INVALID_INPUT\n", prompt_path);
            arena_destroy(&arena);
            return 2;
        }
        memcpy(ptext, pdata, psize);
        ptext[psize] = 0;
        prompt = ptext;
    }
    ComposeInputs in = {blocks, refs, ncat, symbols, prompt};
    ComposeReport r;
    ComposeStatus st = compose_design(&arena, (const char *)data, size, &in, &r);
    if (st == COMPOSE_ERR_ARGUMENT || st == COMPOSE_ERR_MEMORY) {
        fprintf(stderr, "compose: %s\n", compose_status_name(st));
        arena_destroy(&arena);
        return 70;
    }
    for (size_t i = 0; i < r.value_count; ++i)
        printf("  %-28s %-34s [%s] %s\n", r.values[i].where, r.values[i].value, r.values[i].origin, r.values[i].source);
    for (size_t i = 0; i < r.finding_count; ++i)
        printf("  %-15s %-24s %s: %s\n", compose_level_name(r.findings[i].level), r.findings[i].code, r.findings[i].where, r.findings[i].message);
    size_t rlen = 0;
    const char *report = compose_report_json(&arena, &r, &rlen);
    bool ok = fs_create_directories(out_dir) == FS_OK && report && write_text_file(out_dir, "compose_report.json", report, rlen);
    if (ok && st == COMPOSE_COMPOSED) ok = write_text_file(out_dir, "intent.json", r.intent_json, r.intent_len) &&
                                           write_text_file(out_dir, "layout_hints.json", r.hints_json, r.hints_len);
    if (!ok) { fprintf(stderr, "compose: cannot write into %s\n", out_dir); arena_destroy(&arena); return 70; }
    printf("STATUS compose: %s (%zu blocks, %zu nets, %zu components)%s\n", compose_status_name(st), r.blocks, r.nets, r.components,
           st == COMPOSE_COMPOSED ? "; intent.json written: run `copper design check-intent` on it next" : "; no intent written");
    arena_destroy(&arena);
    switch (st) {
    case COMPOSE_COMPOSED: return 0;
    case COMPOSE_NEEDS_INPUT: return 3;
    case COMPOSE_NOT_IMPLEMENTED: return 4;
    case COMPOSE_REFUSED: return 1;
    case COMPOSE_INVALID_INPUT: return 2;
    default: return 70;
    }
}

/* copper design place-problem INTENT --hints H --rules R --footprint-root D --out PROBLEM
 * copper design place-apply   INTENT --hints H --placer PLACER.json --out-intent I --out-routing R
 * Placement of a composed intent with the C placer (place_intent.c); `copper-core place` runs in between.
 * Exit 0 PLACED, 2 INVALID_INPUT, 5 NO_LEGAL_POSITION, 64 usage, 70 internal error. */
static bool read_all(Arena *a, const char *path, const char **text, size_t *len) {
    const unsigned char *d = NULL;
    if (!path || fs_read_file_arena(a, path, &d, len) != FS_OK) return false;
    *text = (const char *)d;
    return true;
}

static bool write_file_path(const char *path, const char *text, size_t len) {
    FILE *f = fopen(path, "wb");
    bool ok = f && fwrite(text, 1, len, f) == len;
    if (f && fclose(f) != 0) ok = false;
    return ok;
}

static int place_exit(PlaceStatus st) {
    return st == PLACE_OK ? 0 : st == PLACE_INVALID_INPUT ? 2 : st == PLACE_NO_LEGAL_POSITION ? 5 : 70;
}

static int command_design_place(int argc, char **argv, bool apply) {
    const char *intent_path = argv[3], *hints = NULL, *rules = NULL, *fps = NULL, *out = NULL, *placer = NULL, *out_intent = NULL, *out_routing = NULL;
    for (int i = 4; i < argc; i += 2) {
        if (i + 1 >= argc) { fprintf(stderr, "place: %s needs a value\n", argv[i]); return 64; }
        if (!strcmp(argv[i], "--hints")) hints = argv[i + 1];
        else if (!apply && !strcmp(argv[i], "--rules")) rules = argv[i + 1];
        else if (!apply && !strcmp(argv[i], "--footprint-root")) fps = argv[i + 1];
        else if (!apply && !strcmp(argv[i], "--out")) out = argv[i + 1];
        else if (apply && !strcmp(argv[i], "--placer")) placer = argv[i + 1];
        else if (apply && !strcmp(argv[i], "--out-intent")) out_intent = argv[i + 1];
        else if (apply && !strcmp(argv[i], "--out-routing")) out_routing = argv[i + 1];
        else { fprintf(stderr, "place: unknown option %s\n", argv[i]); return 64; }
    }
    if (!hints || (!apply && (!rules || !fps || !out)) || (apply && (!placer || !out_intent || !out_routing))) {
        fprintf(stderr, apply ? "place-apply needs --hints, --placer, --out-intent and --out-routing\n"
                              : "place-problem needs --hints, --rules, --footprint-root and --out\n");
        return 64;
    }
    Arena arena;
    arena_init(&arena, 1u << 22);
    const char *it = NULL, *ht = NULL, *rt = NULL, *pt = NULL;
    size_t il = 0, hl = 0, rl = 0, ptl = 0;
    if (!read_all(&arena, intent_path, &it, &il) || !read_all(&arena, hints, &ht, &hl) ||
        (!apply && !read_all(&arena, rules, &rt, &rl)) || (apply && !read_all(&arena, placer, &pt, &ptl))) {
        printf("STATUS place: INVALID_INPUT (an input file cannot be read)\n");
        arena_destroy(&arena);
        return 2;
    }
    PlaceReport r;
    PlaceStatus st;
    if (!apply) {
        const char *problem = NULL;
        size_t plen = 0;
        st = place_problem(&arena, it, il, ht, hl, rt, rl, fps, &problem, &plen, &r);
        if (st == PLACE_OK && !write_file_path(out, problem, plen)) { fprintf(stderr, "place: cannot write %s\n", out); arena_destroy(&arena); return 70; }
        printf("STATUS place-problem: %s (%zu parts, %zu keep-outs, %zu decoupling pairs)%s%s\n", place_status_name(st), r.parts, r.keepouts,
               r.decoupling, st == PLACE_OK ? "" : ": ", st == PLACE_OK ? "" : r.message);
    } else {
        const char *ni = NULL, *nr = NULL;
        size_t nil = 0, nrl = 0;
        st = place_apply(&arena, it, il, ht, hl, pt, ptl, &ni, &nil, &nr, &nrl, &r);
        if (st == PLACE_OK && (!write_file_path(out_intent, ni, nil) || !write_file_path(out_routing, nr, nrl))) {
            fprintf(stderr, "place: cannot write the outputs\n");
            arena_destroy(&arena);
            return 70;
        }
        printf("STATUS place-apply: %s (%zu parts placed from %s, %zu routing keep-outs)%s%s\n", place_status_name(st), r.parts,
               r.seed ? r.seed : "-", r.keepouts, st == PLACE_OK ? "" : ": ", st == PLACE_OK ? "" : r.message);
    }
    arena_destroy(&arena);
    return place_exit(st);
}

/* copper design schematic-rules PROJECT_DIR --symbol-root DIR [--hints layout_hints.json] (Step 3d, sch_rules.c):
 * PWR_FLAGs on the nets the hints list (external supplies), then the project's library tables. A library table that
 * Copper did not write is never overwritten. Exit 0 OK, 2 INVALID_INPUT, 1 I/O error, 64 usage. */
static int command_design_schematic_rules(int argc, char **argv) {
    const char *dir = argv[3], *symbols = NULL, *hints = NULL;
    for (int i = 4; i < argc; i += 2) {
        if (i + 1 >= argc) { fprintf(stderr, "schematic-rules: %s needs a value\n", argv[i]); return 64; }
        if (!strcmp(argv[i], "--symbol-root")) symbols = argv[i + 1];
        else if (!strcmp(argv[i], "--hints")) hints = argv[i + 1];
        else { fprintf(stderr, "schematic-rules: unknown option %s\n", argv[i]); return 64; }
    }
    if (!symbols) { fprintf(stderr, "schematic-rules needs --symbol-root\n"); return 64; }
    /* the project: the one .kicad_pro in the directory */
    FsDirectoryListing ls;
    memset(&ls, 0, sizeof ls);
    if (fs_list_directory(dir, &ls) != FS_OK) { printf("STATUS schematic-rules: INVALID_INPUT (cannot list %s)\n", dir); return 2; }
    char name[256] = "";
    size_t pros = 0;
    for (size_t i = 0; i < ls.count; i++) {
        const char *n = ls.entries[i].name;
        size_t nl = strlen(n);
        if (ls.entries[i].is_regular_file && nl > 10 && !strcmp(n + nl - 10, ".kicad_pro") && nl - 10 < sizeof name) {
            memcpy(name, n, nl - 10); name[nl - 10] = 0; pros++;
        }
    }
    fs_free_directory_listing(&ls);
    if (pros != 1) { printf("STATUS schematic-rules: INVALID_INPUT (%s holds %zu .kicad_pro files, not one)\n", dir, pros); return 2; }
    Arena arena;
    arena_init(&arena, 1u << 22);
    char sch_path[FS_PATH_MAX], pcb_path[FS_PATH_MAX], file[300], sym_path[FS_PATH_MAX], fp_path[FS_PATH_MAX], power_path[FS_PATH_MAX];
    snprintf(file, sizeof file, "%s.kicad_sch", name);
    bool paths = fs_path_join(sch_path, sizeof sch_path, dir, file) == FS_OK;
    snprintf(file, sizeof file, "%s.kicad_pcb", name);
    paths = paths && fs_path_join(pcb_path, sizeof pcb_path, dir, file) == FS_OK &&
            fs_path_join(sym_path, sizeof sym_path, dir, "sym-lib-table") == FS_OK &&
            fs_path_join(fp_path, sizeof fp_path, dir, "fp-lib-table") == FS_OK &&
            fs_path_join(power_path, sizeof power_path, symbols, "power.kicad_sym") == FS_OK;
    const char *sch = NULL, *pcb = NULL, *ht = NULL, *power = NULL, *old = NULL;
    size_t sl = 0, pl = 0, hl = 0, pwl = 0, ol = 0;
    if (!paths || !read_all(&arena, sch_path, &sch, &sl) || !read_all(&arena, pcb_path, &pcb, &pl) ||
        (hints && (!read_all(&arena, hints, &ht, &hl) || !read_all(&arena, power_path, &power, &pwl)))) {
        printf("STATUS schematic-rules: INVALID_INPUT (cannot read the schematic, board, hints or %s)\n", power_path);
        arena_destroy(&arena);
        return 2;
    }
    /* never overwrite a library table Copper did not write: checked before anything is written */
    const char *tables[2] = {sym_path, fp_path};
    for (int k = 0; k < 2; k++)
        if (read_all(&arena, tables[k], &old, &ol) && !sch_lib_table_is_copper(old, ol)) {
            printf("STATUS schematic-rules: INVALID_INPUT (%s exists and was not written by Copper; it is never overwritten)\n", tables[k]);
            arena_destroy(&arena);
            return 2;
        }
    SchRulesReport r;
    memset(&r, 0, sizeof r);
    size_t added = 0, present = 0;
    if (hints) {
        const char *ns = NULL;
        size_t nsl = 0;
        if (sch_power_flags(&arena, sch, sl, ht, hl, power, pwl, &ns, &nsl, &r) != SCH_RULES_OK) {
            printf("STATUS schematic-rules: %s (%s)\n", sch_rules_status_name(r.status), r.message);
            arena_destroy(&arena);
            return r.status == SCH_RULES_INVALID_INPUT ? 2 : 70;
        }
        added = r.flags_added; present = r.flags_present;
        if (added && fs_write_file_atomic_replace(sch_path, ns, nsl) != FS_OK) { fprintf(stderr, "schematic-rules: cannot write %s\n", sch_path); arena_destroy(&arena); return 1; }
        sch = ns; sl = nsl;
    }
    const char *st = NULL, *ft = NULL;
    size_t stl = 0, ftl = 0;
    if (sch_lib_tables(&arena, sch, sl, pcb, pl, &st, &stl, &ft, &ftl, &r) != SCH_RULES_OK) {
        printf("STATUS schematic-rules: %s (%s)\n", sch_rules_status_name(r.status), r.message);
        arena_destroy(&arena);
        return r.status == SCH_RULES_INVALID_INPUT ? 2 : 70;
    }
    if (fs_write_file_atomic_replace(sym_path, st, stl) != FS_OK || fs_write_file_atomic_replace(fp_path, ft, ftl) != FS_OK) {
        fprintf(stderr, "schematic-rules: cannot write the library tables\n");
        arena_destroy(&arena);
        return 1;
    }
    printf("STATUS schematic-rules: OK (%zu power flag(s) added, %zu already present; %zu symbol and %zu footprint libraries in the project tables)\n",
           added, present, r.sym_libs, r.fp_libs);
    arena_destroy(&arena);
    return 0;
}

/* copper design generate: requirements (from the prompt analysis) -> design intent, by template (template_gen.c). */
static int write_text_file(const char *dir, const char *name, const char *text, size_t len) {
    char path[FS_PATH_MAX];
    if (fs_path_join(path, sizeof path, dir, name) != FS_OK) return 0;
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    int ok = fwrite(text, 1, len, f) == len;
    return fclose(f) == 0 && ok;
}

static int command_design_generate(int argc, char **argv) {
    const char *req_path = argv[3], *templates = NULL, *symbols = NULL, *footprints = NULL, *out_dir = NULL, *prompt_path = NULL;
    for (int i = 4; i < argc; i++) {
        if (i + 1 < argc && strcmp(argv[i], "--templates") == 0) templates = argv[++i];
        else if (i + 1 < argc && strcmp(argv[i], "--symbol-root") == 0) symbols = argv[++i];
        else if (i + 1 < argc && strcmp(argv[i], "--footprint-root") == 0) footprints = argv[++i];
        else if (i + 1 < argc && strcmp(argv[i], "--out") == 0) out_dir = argv[++i];
        else if (i + 1 < argc && strcmp(argv[i], "--prompt") == 0) prompt_path = argv[++i];
        else { fprintf(stderr, "generate: unknown option %s\n", argv[i]); return 64; }
    }
    if (!templates || !symbols || !footprints || !out_dir) { fprintf(stderr, "generate needs --templates, --symbol-root, --footprint-root and --out\n"); return 64; }
    Arena arena;
    arena_init(&arena, 1u << 20);
    const unsigned char *req = NULL, *tpl = NULL, *prompt = NULL;
    size_t req_len = 0, tpl_len = 0, prompt_len = 0;
    if (fs_read_file_arena(&arena, req_path, &req, &req_len) != FS_OK || fs_read_file_arena(&arena, templates, &tpl, &tpl_len) != FS_OK ||
        (prompt_path && fs_read_file_arena(&arena, prompt_path, &prompt, &prompt_len) != FS_OK)) {
        fprintf(stderr, "generate: cannot read the requirements, templates or prompt file\n");
        arena_destroy(&arena);
        return 1;
    }
    (void)prompt_len;
    if (fs_create_directories(out_dir) != FS_OK) { fprintf(stderr, "generate: cannot create %s\n", out_dir); arena_destroy(&arena); return 1; }
    TemplateGenOutput out;
    TemplateGenStatus st = template_generate(&arena, (const char *)req, req_len, prompt ? (const char *)prompt : NULL, (const char *)tpl, tpl_len,
                                             symbols, footprints, &out);
    if (st == TGEN_ERR_ARGUMENT || st == TGEN_ERR_MEMORY || !out.report_json) {
        fprintf(stderr, "generate: %s\n", template_gen_status_name(st));
        arena_destroy(&arena);
        return 1;
    }
    if (!write_text_file(out_dir, "generation.json", out.report_json, out.report_len) ||
        (out.intent_json && !write_text_file(out_dir, "intent.json", out.intent_json, out.intent_len)) ||
        (out.routing_json && !write_text_file(out_dir, "routing.json", out.routing_json, out.routing_len))) {
        fprintf(stderr, "generate: cannot write into %s\n", out_dir);
        arena_destroy(&arena);
        return 1;
    }
    printf("Design generation: %s%s\n", template_gen_status_name(st), out.intent_json ? " (intent.json written)" : " (no design written)");
    printf("Report: generation.json in %s\n", out_dir);
    arena_destroy(&arena);
    switch (st) {
    case TGEN_GENERATED: return 0;
    case TGEN_INVALID_INPUT: return 2;
    case TGEN_NEEDS_INPUT: return 3;
    case TGEN_NOT_IMPLEMENTED: return 4;
    case TGEN_TEMPLATE_ERROR: return 5;
    default: return 1;
    }
}

/* copper design route: route a placed .kicad_pcb on two layers (autoroute.c) and write the tracks into it. */
static bool rule_mm(const JsonValue *rules, const char *key, G2Nm *out) {
    const JsonValue *v = json_get(json_get(rules, key), "value");
    return v && json_to_nm(v, out) == JSON_NUMBER_OK;
}
static bool rule_int(const JsonValue *rules, const char *key, int64_t *out) {
    const JsonValue *v = json_get(json_get(rules, key), "value");
    return v && json_to_i64(v, out) == JSON_NUMBER_OK;
}
/* A fab limit in mm from a copper.fab_profile/1 file; false when absent or null. */
static bool fab_limit(const JsonValue *fab, const char *key, G2Nm *out) {
    const JsonValue *v = json_get(json_get(json_get(fab, "limits"), key), "value");
    return v && v->kind == JSON_NUMBER && json_to_nm(v, out) == JSON_NUMBER_OK;
}
static int route_rule_below(const char *what, G2Nm have, const char *limit, G2Nm need) {
    fprintf(stderr, "route: %s (%lld nm) is below the fab profile's %s (%lld nm): INVALID_INPUT\n", what, (long long)have, limit, (long long)need);
    return 2;
}

static int command_design_route(int argc, char **argv) {
    const char *board_path = argv[3], *rules_path = NULL, *routing_path = NULL, *fab_path = NULL, *out_path = NULL, *report_path = NULL;
    for (int i = 4; i < argc; i++) {
        if (i + 1 < argc && strcmp(argv[i], "--rules") == 0) rules_path = argv[++i];
        else if (i + 1 < argc && strcmp(argv[i], "--routing") == 0) routing_path = argv[++i];
        else if (i + 1 < argc && strcmp(argv[i], "--fab-profile") == 0) fab_path = argv[++i];
        else if (i + 1 < argc && strcmp(argv[i], "--out") == 0) out_path = argv[++i];
        else if (i + 1 < argc && strcmp(argv[i], "--report") == 0) report_path = argv[++i];
        else { fprintf(stderr, "route: unknown option %s\n", argv[i]); return 64; }
    }
    if (!rules_path || !fab_path || !out_path) { fprintf(stderr, "route needs --rules, --fab-profile and --out\n"); return 64; }
    Arena arena;
    arena_init(&arena, 1u << 22);
    const unsigned char *rules_text = NULL, *fab_text = NULL, *routing_text = NULL, *pcb_text = NULL;
    size_t rules_len = 0, fab_len = 0, routing_len = 0, pcb_len = 0;
    if (fs_read_file_arena(&arena, rules_path, &rules_text, &rules_len) != FS_OK || fs_read_file_arena(&arena, fab_path, &fab_text, &fab_len) != FS_OK ||
        fs_read_file_arena(&arena, board_path, &pcb_text, &pcb_len) != FS_OK ||
        (routing_path && fs_read_file_arena(&arena, routing_path, &routing_text, &routing_len) != FS_OK)) {
        fprintf(stderr, "route: cannot read the board, rules, fab profile or routing file\n");
        arena_destroy(&arena);
        return 1;
    }
    JsonValue *rj = NULL, *fj = NULL, *nj = NULL;
    JsonError je;
    memset(&je, 0, sizeof je);
    if (json_parse(&arena, (const char *)rules_text, rules_len, &rj, &je) != JSON_OK ||
        json_parse(&arena, (const char *)fab_text, fab_len, &fj, &je) != JSON_OK ||
        (routing_text && json_parse(&arena, (const char *)routing_text, routing_len, &nj, &je) != JSON_OK)) {
        fprintf(stderr, "route: a JSON input is invalid (line %zu)\n", je.line);
        arena_destroy(&arena);
        return 2;
    }
    const JsonValue *rules = json_get(rj, "rules");
    AutorouteRules r;
    memset(&r, 0, sizeof r);
    int64_t via_cost = 0, passes = 0, max_cells = 0;
    if (!rule_mm(rules, "grid_mm", &r.grid_nm) || !rule_mm(rules, "track_mm", &r.track_nm) || !rule_mm(rules, "clearance_mm", &r.clearance_nm) ||
        !rule_mm(rules, "edge_clearance_mm", &r.edge_clearance_nm) || !rule_mm(rules, "via_size_mm", &r.via_size_nm) ||
        !rule_mm(rules, "via_drill_mm", &r.via_drill_nm) || !rule_int(rules, "via_cost", &via_cost) || !rule_int(rules, "passes", &passes) ||
        !rule_int(rules, "max_cells", &max_cells) || via_cost <= 0 || passes <= 0 || max_cells <= 0) {
        fprintf(stderr, "route: the rules file lacks a value (grid_mm, track_mm, clearance_mm, edge_clearance_mm, via_size_mm, via_drill_mm, via_cost, passes, max_cells)\n");
        arena_destroy(&arena);
        return 2;
    }
    r.via_cost = (uint32_t)via_cost;
    r.passes = (uint32_t)passes;
    r.max_cells = (size_t)max_cells;
    /* Fine-pitch escapes (Step 3c, LAYOUT_RULES.md R5): optional, both or neither. */
    bool has_fine = json_get(rules, "fine_pitch_clearance_mm") != NULL, has_neck = json_get(rules, "fine_pitch_neck_mm") != NULL;
    if (has_fine != has_neck || (has_fine && (!rule_mm(rules, "fine_pitch_clearance_mm", &r.fine_clearance_nm) ||
                                              !rule_mm(rules, "fine_pitch_neck_mm", &r.fine_neck_nm) || r.fine_clearance_nm <= 0 || r.fine_neck_nm <= 0))) {
        fprintf(stderr, "route: give both fine_pitch_clearance_mm and fine_pitch_neck_mm (positive), or neither\n");
        arena_destroy(&arena);
        return 2;
    }
    /* Per-net widths and keep-outs from the template (routing.json). */
    const JsonValue *widths = json_get(nj, "net_widths_mm"), *kos = json_get(nj, "keepouts");
    AutorouteNetWidth *nw = NULL;
    AutorouteKeepout *ko = NULL;
    if (widths && widths->kind == JSON_OBJECT && widths->count) {
        nw = arena_alloc_array(&arena, sizeof *nw, widths->count);
        for (size_t i = 0; nw && i < widths->count; i++) {
            nw[i].net = widths->keys[i];
            if (json_to_nm(widths->items[i], &nw[i].width_nm) != JSON_NUMBER_OK) { fprintf(stderr, "route: width of %s is not a number\n", widths->keys[i]); arena_destroy(&arena); return 2; }
        }
        r.widths = nw;
        r.width_count = nw ? widths->count : 0;
    }
    if (kos && kos->kind == JSON_ARRAY && kos->count) {
        ko = arena_alloc_array(&arena, sizeof *ko, kos->count);
        for (size_t i = 0; ko && i < kos->count; i++) {
            const JsonValue *rect = json_get(kos->items[i], "rect_mm"), *layers = json_get(kos->items[i], "layers");
            G2Nm v[4];
            if (!rect || rect->kind != JSON_ARRAY || rect->count != 4 || !layers || layers->kind != JSON_ARRAY) { fprintf(stderr, "route: keep-out %zu is malformed\n", i); arena_destroy(&arena); return 2; }
            for (int k = 0; k < 4; k++) if (json_to_nm(rect->items[k], &v[k]) != JSON_NUMBER_OK) { fprintf(stderr, "route: keep-out %zu is malformed\n", i); arena_destroy(&arena); return 2; }
            ko[i].box.min.x = v[0] < v[2] ? v[0] : v[2]; ko[i].box.max.x = v[0] < v[2] ? v[2] : v[0];
            ko[i].box.min.y = v[1] < v[3] ? v[1] : v[3]; ko[i].box.max.y = v[1] < v[3] ? v[3] : v[1];
            ko[i].front = ko[i].back = false;
            for (size_t k = 0; k < layers->count; k++) {
                if (layers->items[k]->kind != JSON_STRING) continue;
                if (!strcmp(layers->items[k]->text, "F.Cu")) ko[i].front = true;
                if (!strcmp(layers->items[k]->text, "B.Cu")) ko[i].back = true;
            }
        }
        r.keepouts = ko;
        r.keepout_count = ko ? kos->count : 0;
    }
    /* Every rule must meet the fab profile (fail closed: a missing limit is not a pass for that rule). */
    G2Nm lim = 0, need = 0;
    if (!fab_limit(fj, "track_width_min_mm", &lim)) { fprintf(stderr, "route: the fab profile has no track_width_min_mm\n"); arena_destroy(&arena); return 2; }
    if (r.track_nm < lim) { arena_destroy(&arena); return route_rule_below("track_mm", r.track_nm, "track_width_min_mm", lim); }
    for (size_t i = 0; i < r.width_count; i++) if (r.widths[i].width_nm < lim) { arena_destroy(&arena); return route_rule_below(r.widths[i].net, r.widths[i].width_nm, "track_width_min_mm", lim); }
    static const char *const clearance_limits[] = {"track_spacing_min_mm", "clearance_default_min_mm", "pad_to_track_min_mm", "pth_to_track_min_mm",
                                                   "via_hole_to_track_min_mm", "smd_pad_to_pad_min_mm", NULL};
    for (const char *const *k = clearance_limits; *k; k++)
        if (fab_limit(fj, *k, &lim) && r.clearance_nm < lim) { arena_destroy(&arena); return route_rule_below("clearance_mm", r.clearance_nm, *k, lim); }
    if (fab_limit(fj, "copper_to_edge_min_mm", &lim) && r.edge_clearance_nm < lim) { arena_destroy(&arena); return route_rule_below("edge_clearance_mm", r.edge_clearance_nm, "copper_to_edge_min_mm", lim); }
    if (fab_limit(fj, "via_diameter_min_mm", &lim) && r.via_size_nm < lim) { arena_destroy(&arena); return route_rule_below("via_size_mm", r.via_size_nm, "via_diameter_min_mm", lim); }
    if (fab_limit(fj, "via_hole_min_mm", &lim) && r.via_drill_nm < lim) { arena_destroy(&arena); return route_rule_below("via_drill_mm", r.via_drill_nm, "via_hole_min_mm", lim); }
    if (fab_limit(fj, "drill_min_mm", &lim) && r.via_drill_nm < lim) { arena_destroy(&arena); return route_rule_below("via_drill_mm", r.via_drill_nm, "drill_min_mm", lim); }
    if (fab_limit(fj, "via_diameter_over_hole_min_mm", &need) && r.via_size_nm - r.via_drill_nm < need) { arena_destroy(&arena); return route_rule_below("via_size_mm - via_drill_mm", r.via_size_nm - r.via_drill_nm, "via_diameter_over_hole_min_mm", need); }
    if (has_fine) {
        /* the escape stub is a track next to SMD pads: every limit that could apply to it, the SMD pad-to-pad one too */
        static const char *const fine_limits[] = {"track_spacing_min_mm", "clearance_default_min_mm", "pad_to_track_min_mm", "smd_pad_to_pad_min_mm", NULL};
        for (const char *const *k = fine_limits; *k; k++)
            if (fab_limit(fj, *k, &lim) && r.fine_clearance_nm < lim) { arena_destroy(&arena); return route_rule_below("fine_pitch_clearance_mm", r.fine_clearance_nm, *k, lim); }
        if (fab_limit(fj, "track_width_min_mm", &lim) && r.fine_neck_nm < lim) { arena_destroy(&arena); return route_rule_below("fine_pitch_neck_mm", r.fine_neck_nm, "track_width_min_mm", lim); }
    }

    Board board;
    BoardDiag diag;
    memset(&diag, 0, sizeof diag);
    if (board_load_kicad(board_path, &arena, &board, &diag) != BOARD_OK) {
        fprintf(stderr, "route: cannot load %s: %s\n", board_path, diag.message);
        arena_destroy(&arena);
        return 2;
    }
    AutorouteResult res;
    AutorouteStatus st = autoroute_board(&arena, &board, &r, &res);
    printf("Routing: %s - %s\n", autoroute_status_name(st), res.message);
    for (size_t i = 0; i < res.net_count; i++) {
        const AutorouteNetResult *n = &res.nets[i];
        if (n->status == AUTOROUTE_NET_SINGLE_PAD) continue;
        printf("  %-10s %-14s pads %zu, %zu segment(s), %zu via(s)%s%s\n", autoroute_net_status_name(n->status), n->name ? n->name : "?",
               n->pads, n->segments, n->vias, n->message[0] ? ": " : "", n->message);
    }
    if (st == AUTOROUTE_ROUTED || st == AUTOROUTE_PARTIAL) {
        const char *seed = board_path, *text = NULL;
        size_t len = 0;
        for (const char *p = board_path; *p; p++) if (*p == '/' || *p == '\\') seed = p + 1;
        if (autoroute_write_kicad(&arena, (const char *)pcb_text, pcb_len, &board, &res, seed, &text, &len) != AUTOROUTE_ROUTED) {
            fprintf(stderr, "route: cannot write the routed board\n");
            arena_destroy(&arena);
            return 1;
        }
        FILE *f = fopen(out_path, "wb");
        if (!f || fwrite(text, 1, len, f) != len) { if (f) fclose(f); fprintf(stderr, "route: cannot write %s\n", out_path); arena_destroy(&arena); return 1; }
        if (fclose(f) != 0) { fprintf(stderr, "route: cannot write %s\n", out_path); arena_destroy(&arena); return 1; }
    }
    if (report_path) {
        JsonWriter w;
        json_writer_init(&w, &arena);
        json_write_begin_object(&w);
        json_write_key(&w, "format"); json_write_string(&w, "copper.routing_report/1");
        json_write_key(&w, "status"); json_write_string(&w, autoroute_status_name(st));
        json_write_key(&w, "message"); json_write_string(&w, res.message);
        json_write_key(&w, "rules"); json_write_string(&w, rules_path);
        json_write_key(&w, "fab_profile"); json_write_string(&w, fab_path);
        json_write_key(&w, "tracks"); json_write_uint(&w, res.track_count);
        json_write_key(&w, "vias"); json_write_uint(&w, res.via_count);
        json_write_key(&w, "nets_routed"); json_write_uint(&w, res.nets_routed);
        json_write_key(&w, "nets_failed"); json_write_uint(&w, res.nets_failed);
        json_write_key(&w, "pass"); json_write_uint(&w, res.pass_used);
        json_write_key(&w, "nets");
        json_write_begin_array(&w);
        for (size_t i = 0; i < res.net_count; i++) {
            const AutorouteNetResult *n = &res.nets[i];
            json_write_begin_object(&w);
            json_write_key(&w, "net"); json_write_string(&w, n->name ? n->name : "");
            json_write_key(&w, "status"); json_write_string(&w, autoroute_net_status_name(n->status));
            json_write_key(&w, "pads"); json_write_uint(&w, n->pads);
            json_write_key(&w, "width_nm"); json_write_int(&w, n->width_nm);
            json_write_key(&w, "segments"); json_write_uint(&w, n->segments);
            json_write_key(&w, "vias"); json_write_uint(&w, n->vias);
            json_write_key(&w, "message"); json_write_string(&w, n->message);
            json_write_end_object(&w);
        }
        json_write_end_array(&w);
        json_write_end_object(&w);
        size_t len = 0;
        const char *json = json_writer_finish(&w, &len);
        FILE *f = json ? fopen(report_path, "wb") : NULL;
        if (!f || fwrite(json, 1, len, f) != len) { if (f) fclose(f); fprintf(stderr, "route: cannot write %s\n", report_path); arena_destroy(&arena); return 1; }
        fclose(f);
    }
    arena_destroy(&arena);
    switch (st) {
    case AUTOROUTE_ROUTED: return 0;
    case AUTOROUTE_PARTIAL: return 6;
    case AUTOROUTE_INVALID_INPUT: return 2;
    case AUTOROUTE_UNSUPPORTED: return 7;
    default: return 1;
    }
}

int main(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "help") == 0 || strcmp(argv[1], "--help") == 0) {
        print_usage(); return argc < 2 ? 64 : 0;
    }
    if (strcmp(argv[1], "design") == 0) {
        if (argc >= 4 && strcmp(argv[2], "import") == 0)
            return command_design_import(argc, argv);
        if (argc >= 4 && strcmp(argv[2], "resolve-part") == 0)
            return command_design_resolve_part(argc, argv);
        if (argc >= 4 && strcmp(argv[2], "check-intent") == 0)
            return command_design_check_intent(argc, argv);
        if (argc >= 4 && strcmp(argv[2], "generate") == 0)
            return command_design_generate(argc, argv);
        if (argc >= 4 && strcmp(argv[2], "compose") == 0)
            return command_design_compose(argc, argv);
        if (argc >= 4 && strcmp(argv[2], "place-problem") == 0)
            return command_design_place(argc, argv, false);
        if (argc >= 4 && strcmp(argv[2], "place-apply") == 0)
            return command_design_place(argc, argv, true);
        if (argc >= 4 && strcmp(argv[2], "schematic-rules") == 0)
            return command_design_schematic_rules(argc, argv);
        if (argc >= 4 && strcmp(argv[2], "route") == 0)
            return command_design_route(argc, argv);
        print_usage(); return 64;
    }
    if (strcmp(argv[1], "user") == 0) {
        if (argc >= 4 && strcmp(argv[2], "design") == 0 && strcmp(argv[3], "prompt") == 0)
            return command_user_design_prompt(argc, argv);
        if (argc >= 5 && strcmp(argv[2], "engineering") == 0 && strcmp(argv[3], "run") == 0)
            return command_engineering_run(argc, argv);
        if (argc >= 5 && strcmp(argv[2], "optimize") == 0 && strcmp(argv[3], "evaluate") == 0)
            return command_optimize_evaluate(argc, argv);
        if (argc >= 5 && strcmp(argv[2], "optimize") == 0 && strcmp(argv[3], "apply") == 0)
            return command_optimize_apply(argc, argv);
        if (argc >= 5 && strcmp(argv[2], "topology") == 0 && strcmp(argv[3], "show") == 0)
            return command_topology(argc, argv, false);
        if (argc >= 4 && strcmp(argv[2], "board-inspect") == 0)
            return command_user_board_inspect(argv[3]);
        if (argc >= 4 && strcmp(argv[2], "parts-used") == 0)
            return command_user_parts_used(argc, argv);
        if (argc >= 4 && strcmp(argv[2], "dfm-check") == 0)
            return command_user_dfm(argc, argv, false);
        if (argc >= 4 && strcmp(argv[2], "manufacturing-instructions") == 0)
            return command_user_dfm(argc, argv, true);
        print_usage(); return 64;
    }
    if (strcmp(argv[1], "dev") == 0) {
        if (argc >= 5 && strcmp(argv[2], "engineering") == 0 && strcmp(argv[3], "run") == 0)
            return command_engineering_run(argc, argv);
        if (argc >= 5 && strcmp(argv[2], "optimize") == 0 && strcmp(argv[3], "evaluate") == 0)
            return command_optimize_evaluate(argc, argv);
        if (argc >= 5 && strcmp(argv[2], "optimize") == 0 && strcmp(argv[3], "apply") == 0)
            return command_optimize_apply(argc, argv);
        if (argc >= 5 && strcmp(argv[2], "optimize") == 0 && strcmp(argv[3], "run") == 0)
            return command_optimize_run(argc, argv);
        if (argc >= 5 && strcmp(argv[2], "topology") == 0 && strcmp(argv[3], "build") == 0)
            return command_topology(argc, argv, false);
        if (argc >= 5 && strcmp(argv[2], "topology") == 0 && strcmp(argv[3], "validate") == 0)
            return command_topology(argc, argv, true);
        if (argc == 5 && strcmp(argv[2], "board") == 0 && strcmp(argv[3], "inspect") == 0)
            return command_user_board_inspect(argv[4]);
        if (argc == 5 && strcmp(argv[2], "board") == 0 && strcmp(argv[3], "validate") == 0)
            return command_dev_board_validate(argv[4]);
        if (argc == 5 && strcmp(argv[2], "rules") == 0 && strcmp(argv[3], "validate") == 0)
            return command_dev_rules_validate(argv[4]);
        print_usage(); return 64;
    }
    if (strcmp(argv[1], "assets") == 0) {
        if (argc >= 4 && strcmp(argv[2], "inspect-symbol") == 0)
            return command_inspect_symbol(argv[3], argc >= 5 ? argv[4] : NULL);
        if (argc == 4 && strcmp(argv[2], "inspect-footprint") == 0)
            return command_inspect_footprint(argv[3]);
        if (argc == 7 && strcmp(argv[2], "import-symbol-library") == 0 &&
            strcmp(argv[5], "--root") == 0)
            return command_import_symbols(argv[3], argv[4], argv[6]);
        if (argc == 7 && strcmp(argv[2], "import-footprint-library") == 0 &&
            strcmp(argv[5], "--root") == 0)
            return command_import_footprints(argv[3], argv[4], argv[6]);
        if (argc == 7 && strcmp(argv[2], "import-kicad") == 0 &&
            strcmp(argv[5], "--root") == 0)
            return command_import_kicad(argv[3], argv[4], argv[6]);
        print_usage(); return 64;
    }
    if (strcmp(argv[1], "block") == 0) {
        if (argc >= 4 && strcmp(argv[2], "check") == 0)
            return command_block_check(argc, argv);
        if (argc >= 3 && strcmp(argv[2], "list") == 0)
            return command_block_list(argc, argv);
        print_usage(); return 64;
    }
    if (strcmp(argv[1], "catalogue") == 0) {
        if (argc == 4 && strcmp(argv[2], "create") == 0)
            return command_catalogue_create(argv[3]);
        if (argc >= 4 && strcmp(argv[2], "validate") == 0)
            return command_catalogue_validate(argc, argv);
        if (argc >= 5 && strcmp(argv[2], "install") == 0)
            return command_catalogue_install(argc, argv);
        if (argc >= 4 && strcmp(argv[2], "list") == 0)
            return command_catalogue_list(argc, argv);
        if (argc >= 4 && strcmp(argv[2], "resolve") == 0)
            return command_catalogue_resolve(argc, argv);
        if (argc >= 4 && strcmp(argv[2], "search") == 0)
            return command_catalogue_search(argc, argv);
        if (argc >= 4 && strcmp(argv[2], "check") == 0)
            return command_catalogue_check(argc, argv);
        print_usage(); return 64;
    }
    print_usage();
    return 64;
}

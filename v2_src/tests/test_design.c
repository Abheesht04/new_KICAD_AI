#ifdef NDEBUG
#undef NDEBUG
#endif
#include "copper_design.h"
#include "copper_design_json.h"
#include "project_kicad.h"
#include "netlist_kicad.h"
#include "report.h"
#include "design_resolver.h"
#include "design_parity.h"
#include "part_catalogue.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool contains(const char *text, const char *needle) {
    return text && needle && strstr(text, needle) != NULL;
}

static void test_sample_project_import_and_json(void) {
    Arena arena;
    CopperDesign design;
    CopperDesignImportOptions options;
    memset(&options, 0, sizeof(options));
    options.project_directory = COPPER_TEST_SOURCE_DIR "/tests/fixtures/sample_resistor_lol";
    options.diagnostic_capacity = 256;
    arena_init(&arena, 65536);
    assert(copper_design_import_project(&arena, &options, &design) == COPPER_DESIGN_OK);
    assert(design.has_project_settings && design.has_schematic && design.has_board);
    assert(!design.has_netlist);
    assert(design.schematic.symbol_count == 8);
    assert(design.schematic.library_symbol_count == 4);
    assert(design.schematic.wire_count == 23);
    assert(design.schematic.label_count == 6);
    assert(design.schematic.junction_count == 1);
    assert(design.schematic.unmodelled_top_level_records == 0);
    const SchematicLibrarySymbol *bc107 = schematic_find_library_symbol(
        &design.schematic, "Transistor_BJT:BC107");
    assert(bc107 && bc107->pin_count == 3);
    bool found_bc107_base = false;
    for (size_t i = 0; i < bc107->pin_count; ++i) {
        const SchematicLibPin *pin = &design.schematic.library_pins[bc107->first_pin + i];
        if (pin->number && strcmp(pin->number, "2") == 0) {
            assert(pin->unit == 1);
            assert(strcmp(pin->name, "B") == 0);
            found_bc107_base = true;
        }
    }
    assert(found_bc107_base);
    assert(design.project_settings.net_class_count == 1);
    assert(design.project_settings.net_classes[0].clearance_nm == 200000);
    assert(design.project_settings.net_classes[0].track_width_nm == 200000);
    assert(design.project_settings.board_rule_count > 0);
    assert(design.project_settings.board_rule_severity_count > 0);
    assert(design.project_settings.erc_rule_severity_count > 0);
    assert(design.board.footprint_count == 5);
    assert(design.board.pad_count == 10);
    assert(design.parity.matched_components == 5);
    assert(design.parity.missing_footprints == 1);
    assert(design.parity.symbols_without_footprint_assignment == 2);
    assert(design.board.edge_count == 1);
    assert(design.board.rules.board_thickness == 1600000);

    size_t json_length_a = 0, json_length_b = 0;
    char *json_a = copper_design_json_to_string(&design, &json_length_a);
    char *json_b = copper_design_json_to_string(&design, &json_length_b);
    assert(json_a && json_b && json_length_a == json_length_b);
    assert(memcmp(json_a, json_b, json_length_a) == 0);
    assert(contains(json_a, "\"schema\":\"copper.design\""));
    assert(contains(json_a, "\"reference\":\"Q1\""));
    assert(contains(json_a, "\"name\":\"Default\""));
    assert(contains(json_a, "\"PCB_FOOTPRINT_MISSING\""));
    assert(contains(json_a, "\"SYMBOL_FOOTPRINT_UNASSIGNED\""));
    assert(contains(json_a, "\"typed_coverage_complete\":false"));

    /* The project JSON parser is also the current strict JSON syntax validator. */
    ProjectSettingsIR syntax_check;
    ProjectSettingsDiag syntax_diag;
    ProjectSettingsStatus syntax_status = project_settings_parse(&arena, json_a, json_length_a,
                                                                  &syntax_check, &syntax_diag);
    if (syntax_status != PROJECT_SETTINGS_OK) fprintf(stderr, "design JSON parse failed: %s at %zu:%zu: %s\n", project_settings_status_name(syntax_status), syntax_diag.line, syntax_diag.column, syntax_diag.message);
    assert(syntax_status == PROJECT_SETTINGS_OK);
    free(json_a);
    free(json_b);
    arena_destroy(&arena);
}


static void test_forgiving_catalogue_and_resolution(void) {
    static const char catalogue_text[] =
        "(copper_parts (schema 1)"
        " (part (id \"generic_red_led\") (kind D)"
        "   (description \"Generic red indicator LED\")"
        "   (aliases (alias \"red led\") (alias \"indicator\")))"
        " (part (id \"exact_led\") (kind D) (manufacturer \"Acme\")"
        "   (mpn \"LED-RED-0603\") (package \"0603\")"
        "   (symbol \"Device:LED\") (footprint \"LED_SMD:LED_0603_1608Metric\")"
        "   (identity_source (url \"https://example.invalid/ds.pdf\")"
        "     (document \"LED datasheet\") (status \"verified\")"
        "     (verified_by \"test\") (verified_on \"2026-11-16\"))"
        "   (pins (pin \"anode\" \"1\" \"1\") (pin \"cathode\" \"2\" \"2\"))))";
    Arena arena;
    PartCatalogue catalogue;
    PartCatalogueError error;
    DesignCatalogueView view;
    DesignPartQuery query;
    DesignResolveResult result;
    memset(&view, 0, sizeof(view));
    memset(&query, 0, sizeof(query));
    arena_init(&arena, 8192);
    PartCatalogueStatus parse_status = part_catalogue_parse(&arena, catalogue_text,
        sizeof(catalogue_text) - 1u, &catalogue, &error);
    if (parse_status != PART_CATALOGUE_OK)
        fprintf(stderr, "catalogue parse: %s field=%s message=%s item=%zu\\n",
                part_catalogue_status_name(parse_status), error.field ? error.field : "-",
                error.message ? error.message : "-", error.item_index);
    assert(parse_status == PART_CATALOGUE_OK);
    assert(catalogue.part_count == 2);
    assert(catalogue.parts[0].symbol_id == NULL);
    assert(catalogue.parts[0].footprint_id == NULL);
    assert(catalogue.parts[0].pin_count == 0);
    assert(catalogue.parts[0].alias_count == 2);
    view.catalogue = &catalogue;
    view.source = DESIGN_CATALOGUE_USER;
    view.name = "test-user";
    view.priority = 100;
    query.kind = PART_KIND_DIODE;
    query.text = "red led";
    query.require_symbol = true;
    query.require_footprint = true;
    assert(design_resolve_part(&arena, &view, 1, &query, &result) ==
           DESIGN_RESOLVE_INCOMPLETE);
    assert(result.best != NULL);
    assert(strcmp(result.best->part->id, "generic_red_led") == 0);
    assert((result.best->missing_fields & DESIGN_MISSING_SYMBOL) != 0);
    assert(!result.best->usable_for_footprint);
    query.text = NULL;
    query.mpn = "LED-RED-0603";
    query.require_symbol = true;
    query.require_footprint = true;
    assert(design_resolve_part(&arena, &view, 1, &query, &result) ==
           DESIGN_RESOLVE_OK);
    assert(strcmp(result.best->part->id, "exact_led") == 0);
    assert(design_candidate_meets_requirements(result.best, &query));

    DesignCatalogueView layered[2] = {view, view};
    layered[0].source = DESIGN_CATALOGUE_USER;
    layered[0].priority = design_catalogue_default_priority(DESIGN_CATALOGUE_USER);
    layered[0].name = "user-overlay";
    layered[1].source = DESIGN_CATALOGUE_KICAD;
    layered[1].priority = design_catalogue_default_priority(DESIGN_CATALOGUE_KICAD);
    layered[1].name = "kicad-default";
    assert(design_resolve_part(&arena, layered, 2, &query, &result) ==
           DESIGN_RESOLVE_OK);
    assert(result.candidate_count == 1); /* duplicate canonical IDs collapse across sources */
    assert(strcmp(result.best->catalogue_name, "kicad-default") == 0);

    static const char sparse_text[] =
        "(copper_parts (schema 1)"
        " (part (mpn \"VENDOR-ONLY-42\") (vendor_extra \"opaque metadata\")))";
    PartCatalogue sparse;
    PartCatalogueStatus sparse_status = part_catalogue_parse(&arena, sparse_text,
        sizeof(sparse_text) - 1u, &sparse, &error);
    if (sparse_status != PART_CATALOGUE_OK)
        fprintf(stderr, "sparse parse: %s field=%s msg=%s\\n",
                part_catalogue_status_name(sparse_status),
                error.field ? error.field : "-", error.message ? error.message : "-");
    assert(sparse_status == PART_CATALOGUE_OK);
    assert(sparse.part_count == 1);
    assert(sparse.parts[0].id_generated);
    assert(sparse.parts[0].source_record != NULL);
    assert(sparse.parts[0].kind == PART_KIND_INVALID);
    assert(sparse.parts[0].symbol_id == NULL);
    arena_destroy(&arena);
}

static void test_schematic_rejects_trailing_expression(void) {
    Arena arena;
    SchematicIR schematic;
    SchematicDiag diag;
    const char *bad = "(kicad_sch (version 20250114) (uuid \"x\")) (extra)";
    arena_init(&arena, 4096);
    assert(schematic_parse(&arena, bad, strlen(bad), &schematic, &diag) == SCHEMATIC_ERR_PARSE);
    arena_destroy(&arena);
}

static void test_project_json_rejects_trailing_content(void) {
    Arena arena;
    ProjectSettingsIR settings;
    ProjectSettingsDiag diag;
    const char *bad = "{\"meta\":{\"version\":1}} garbage";
    arena_init(&arena, 4096);
    assert(project_settings_parse(&arena, bad, strlen(bad), &settings, &diag) == PROJECT_SETTINGS_ERR_JSON);
    arena_destroy(&arena);
}



static void test_project_json_source_is_arena_owned(void) {
    static const char source[] =
        "{\"meta\":{\"version\":1,\"filename\":\"owned.kicad_pro\"},"
        "\"net_settings\":{\"classes\":[]}}";
    Arena arena;
    ProjectSettingsIR settings;
    ProjectSettingsDiag diag;
    char *temporary = (char *)malloc(sizeof(source));
    assert(temporary);
    memcpy(temporary, source, sizeof(source));
    arena_init(&arena, 4096);
    assert(project_settings_parse(&arena, temporary, sizeof(source) - 1u,
                                  &settings, &diag) == PROJECT_SETTINGS_OK);
    memset(temporary, 'x', sizeof(source) - 1u);
    free(temporary);
    assert(settings.source_json != NULL);
    assert(settings.source_json_length == sizeof(source) - 1u);
    assert(memcmp(settings.source_json, source, sizeof(source) - 1u) == 0);
    assert(strcmp(settings.filename, "owned.kicad_pro") == 0);
    arena_destroy(&arena);
}

static void test_kicad_netlist_parse(void) {
    static const char netlist_text[] =
        "(export (version \"E\")"
        " (components (comp (ref \"R1\") (value \"10k\")"
        " (footprint \"Resistor_SMD:R_0603_1608Metric\")"
        " (tstamps \"uuid-r1\")"
        " (libsource (lib \"Device\") (part \"R\"))))"
        " (nets (net (code \"1\") (name \"SIGNAL\") (class \"Default\")"
        " (node (ref \"R1\") (pin \"1\") (pinfunction \"~\") (pintype \"passive\")))))";
    Arena arena;
    KicadNetlist netlist;
    KicadNetlistDiag diag;
    arena_init(&arena, 4096);
    assert(kicad_netlist_parse(&arena, netlist_text, sizeof(netlist_text) - 1u,
                              &netlist, &diag) == KICAD_NETLIST_OK);
    assert(netlist.component_count == 1);
    assert(netlist.net_count == 1);
    assert(netlist.node_count == 1);
    assert(strcmp(netlist.components[0].reference, "R1") == 0);
    assert(strcmp(netlist.nets[0].name, "SIGNAL") == 0);
    const KicadNetlistNode *node = kicad_netlist_find_node(&netlist, "R1", "1");
    assert(node && node->net_index == 0);
    arena_destroy(&arena);
}

static void test_netlist_json_serialization(void) {
    static const char netlist_text[] =
        "(export (version \"E\")"
        " (components (comp (ref \"R1\") (value \"10k\")))"
        " (nets (net (code \"1\") (name \"SIGNAL\") (class \"Default\")"
        " (node (ref \"R1\") (pin \"1\") (pintype \"passive\")))))";
    Arena arena;
    CopperDesign design;
    KicadNetlistDiag netlist_diag;
    ProjectSettingsIR parsed;
    ProjectSettingsDiag parsed_diag;
    memset(&design, 0, sizeof(design));
    arena_init(&arena, 8192);
    design.arena = &arena;
    design.name = "netlist_json_test";
    design.has_project_settings = true;
    design.has_schematic = true;
    design.has_netlist = true;
    design.parity.netlist_available = true;
    design.schematic.root_uuid = "root-uuid";
    diagnostic_report_init(&design.diagnostics, &arena, 4);
    assert(kicad_netlist_parse(&arena, netlist_text, sizeof(netlist_text) - 1u,
                              &design.netlist, &netlist_diag) == KICAD_NETLIST_OK);
    size_t length = 0;
    char *json = copper_design_json_to_string(&design, &length);
    assert(json && length > 0);
    assert(contains(json, "\"name\":\"SIGNAL\""));
    ProjectSettingsStatus parse_status = project_settings_parse(&arena, json, length, &parsed,
                                                                 &parsed_diag);
    assert(parse_status == PROJECT_SETTINGS_OK);
    free(json);
    arena_destroy(&arena);
}

static void test_diagnostic_report_json_and_text(void) {
    Arena arena;
    CopperDiagnosticReport report;
    CopperDiagnostic diagnostic;
    size_t json_length = 0, text_length = 0;
    arena_init(&arena, 4096);
    diagnostic_report_init(&report, &arena, 2);
    memset(&diagnostic, 0, sizeof(diagnostic));
    diagnostic.severity = DIAG_UNVERIFIED;
    diagnostic.code = "TEST_UNVERIFIED";
    diagnostic.file = "board.kicad_pcb";
    diagnostic.object = "R1";
    diagnostic.message = "Coverage is incomplete: \"custom pad\"\\zone";
    diagnostic.line = 7;
    diagnostic.column = 11;
    diagnostic.offset = 29;
    assert(diagnostic_report_add(&report, &diagnostic));
    char *json = diagnostic_report_to_json(&report, &json_length);
    char *text = diagnostic_report_to_text(&report, &text_length);
    assert(json && json_length > 0 && text && text_length > 0);
    assert(contains(json, "TEST_UNVERIFIED"));
    assert(contains(text, "Coverage is incomplete"));
    ProjectSettingsIR parsed;
    ProjectSettingsDiag parse_diag;
    assert(project_settings_parse(&arena, json, json_length, &parsed,
                                  &parse_diag) == PROJECT_SETTINGS_OK);
    free(json);
    free(text);
    arena_destroy(&arena);
}

static void test_root_sheet_net_name_parity(void) {
    SchematicSymbol symbol;
    SchematicIR schematic;
    BoardFootprint footprint;
    BoardPad pad;
    BoardNet pcb_net;
    Board board;
    KicadNetlistNode node;
    KicadNetlistNet net;
    KicadNetlist netlist;
    DesignParitySummary summary;
    CopperDiagnosticReport report;
    Arena arena;

    memset(&symbol, 0, sizeof(symbol));
    memset(&schematic, 0, sizeof(schematic));
    memset(&footprint, 0, sizeof(footprint));
    memset(&pad, 0, sizeof(pad));
    memset(&pcb_net, 0, sizeof(pcb_net));
    memset(&board, 0, sizeof(board));
    memset(&node, 0, sizeof(node));
    memset(&net, 0, sizeof(net));
    memset(&netlist, 0, sizeof(netlist));

    symbol.uuid = "symbol-uuid";
    symbol.reference = "R1";
    symbol.footprint = "Resistor_SMD:R_0603_1608Metric";
    symbol.on_board = true;
    schematic.symbols = &symbol;
    schematic.symbol_count = 1;

    footprint.reference = "R1";
    footprint.lib_id = symbol.footprint;
    footprint.path = "/symbol-uuid";
    footprint.pad_count = 1;
    board.footprints = &footprint;
    board.footprint_count = 1;

    pad.footprint = 0;
    pad.number = "1";
    pad.net = 1;
    board.pads = &pad;
    board.pad_count = 1;
    pcb_net.id = 1;
    pcb_net.name = "VIN";
    board.nets = &pcb_net;
    board.net_count = 1;

    node.reference = "R1";
    node.pin = "1";
    node.net_index = 0;
    net.name = "/VIN";
    net.first_node = 0;
    net.node_count = 1;
    netlist.nodes = &node;
    netlist.node_count = 1;
    netlist.nets = &net;
    netlist.net_count = 1;

    arena_init(&arena, 4096);
    diagnostic_report_init(&report, &arena, 8);
    assert(design_parity_check(&schematic, &board, &netlist, &report, &summary));
    assert(summary.checked_pad_nets == 1);
    assert(summary.pad_net_mismatches == 0);

    /* Do not erase the hierarchy: /Power/VIN is not the root net VIN. */
    net.name = "/Power/VIN";
    assert(!design_parity_check(&schematic, &board, &netlist, &report, &summary));
    assert(summary.pad_net_mismatches == 1);
    arena_destroy(&arena);
}

int main(void) {
    test_sample_project_import_and_json();
    test_forgiving_catalogue_and_resolution();
    test_schematic_rejects_trailing_expression();
    test_project_json_rejects_trailing_content();
    test_project_json_source_is_arena_owned();
    test_kicad_netlist_parse();
    test_netlist_json_serialization();
    test_diagnostic_report_json_and_text();
    test_root_sheet_net_name_parity();
    puts("Copper design tests: PASS");
    return 0;
}

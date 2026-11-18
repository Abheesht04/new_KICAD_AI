#ifdef NDEBUG
#undef NDEBUG
#endif
#include "design_resolution_json.h"
#include "design_resolver.h"
#include "kicad_asset_resolver.h"
#include "part_catalogue.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool has_text(const char *text, const char *needle) {
    return text && needle && strstr(text, needle) != NULL;
}

static void test_kicad_asset_resolution_and_design_json(void) {
    Arena arena;
    PartCatalogue catalogue;
    PartCatalogueError parse_error;
    DesignCatalogueView view;
    DesignPartQuery query;
    DesignResolveResult result;
    KicadAssetRootOptions root_options;
    KicadAssetRoots roots;
    KicadPartAssetResolution assets;
    KicadAssetError asset_error;
    DesignResolutionJsonInput json_input;
    const char *fixture = COPPER_TEST_SOURCE_DIR "/tests/fixtures/catalogue_resolution";
    char path[4096];
    char symbol_root[4096];
    char footprint_root[4096];
    char project_root[4096];
    size_t json_length = 0;
    char *json;

    arena_init(&arena, 32768u);
    (void)snprintf(path, sizeof(path), "%s/parts.sx", fixture);
    PartCatalogueStatus parse_status = part_catalogue_parse_file(&arena, path, &catalogue, &parse_error);
    if (parse_status != PART_CATALOGUE_OK) fprintf(stderr, "catalogue parse failed: %s field=%s message=%s item=%zu\n", part_catalogue_status_name(parse_status), parse_error.field ? parse_error.field : "-", parse_error.message ? parse_error.message : "-", parse_error.item_index);
    assert(parse_status == PART_CATALOGUE_OK);
    assert(catalogue.part_count == 3u);

    memset(&view, 0, sizeof(view));
    view.catalogue = &catalogue;
    view.source = DESIGN_CATALOGUE_USER;
    view.name = "fixture-user-catalogue";
    view.priority = design_catalogue_default_priority(DESIGN_CATALOGUE_USER);

    memset(&query, 0, sizeof(query));
    query.part_id = "resistor_test_10k";
    assert(design_resolve_part(&arena, &view, 1u, &query, &result) == DESIGN_RESOLVE_OK);
    assert(result.best && strcmp(result.best->part->id, "resistor_test_10k") == 0);

    memset(&root_options, 0, sizeof(root_options));
    (void)snprintf(symbol_root, sizeof(symbol_root), "%s/kicad/symbols", fixture);
    root_options.symbol_root = symbol_root;
    (void)snprintf(footprint_root, sizeof(footprint_root), "%s/kicad/footprints", fixture);
    root_options.footprint_root = footprint_root;
    assert(kicad_asset_roots_discover(&arena, &root_options, &roots) == KICAD_DISCOVERY_OK);
    assert(roots.symbols_available && roots.footprints_available);

    KicadAssetStatus asset_status = kicad_part_asset_resolve(&arena, result.best->part, &roots, &assets, &asset_error);
    if (asset_status != KICAD_ASSET_OK) fprintf(stderr, "asset resolve failed: %s: %s symbol=%s footprint=%s mapping=%s\n", kicad_asset_status_name(asset_status), asset_error.message ? asset_error.message : "-", kicad_asset_status_name(assets.symbol_status), kicad_asset_status_name(assets.footprint_status), kicad_asset_status_name(assets.mapping_status));
    assert(asset_status == KICAD_ASSET_OK);
    assert(assets.symbol_found && assets.footprint_found);
    assert(assets.explicit_mapping_valid);
    assert(assets.pin_map_resolution == KICAD_PIN_MAP_EXPLICIT_VALID);
    assert(assets.symbol && assets.symbol->pin_count == 2u);
    assert(assets.symbol->footprint_default &&
           strcmp(assets.symbol->footprint_default, "Resistor_SMD:R_0603_1608Metric") == 0);
    assert(assets.symbol->footprint_filter && strcmp(assets.symbol->footprint_filter, "R_* 0603*") == 0);
    assert(assets.footprint && assets.footprint->pad_count == 2u);
    assert(assets.unique_symbol_pin_count == 2u);
    assert(assets.matching_pad_count == 2u);
    assert(assets.missing_pad_count == 0u);
    assert(assets.suggested_footprint_id == NULL);

    memset(&json_input, 0, sizeof(json_input));
    json_input.catalogue_path = "parts.sx";
    json_input.query_kind = "R";
    json_input.resolution = &result;
    json_input.asset_roots = &roots;
    json_input.assets = &assets;
    json = design_resolution_json_to_string(&json_input, &json_length);
    assert(json && json_length > 0u);
    assert(has_text(json, "copper.design"));
    assert(has_text(json, "PART_ASSETS_RESOLVED"));
    assert(has_text(json, "EXPLICIT_VALID"));
    assert(has_text(json, "CTR-10K-0603"));
    assert(has_text(json, "R_0603_1608Metric"));
    assert(has_text(json, "missing_fields"));
    free(json);

    /* No explicit mapping: exact pin/pad number equality is only an inference. */
    query.part_id = "resistor_inferred_10k";
    assert(design_resolve_part(&arena, &view, 1u, &query, &result) == DESIGN_RESOLVE_OK);
    assert(kicad_part_asset_resolve(&arena, result.best->part, &roots,
                                    &assets, &asset_error) == KICAD_ASSET_ERR_SCHEMA);
    assert(assets.inferred_mapping_possible);
    assert(!assets.explicit_mapping_valid);
    assert(assets.pin_map_resolution == KICAD_PIN_MAP_INFERRED_UNVERIFIED);

    /* Missing symbol/footprint metadata stays missing; KiCad can only suggest. */
    query.part_id = "resistor_sparse_10k";
    assert(design_resolve_part(&arena, &view, 1u, &query, &result) == DESIGN_RESOLVE_OK);
    assert(kicad_part_asset_resolve(&arena, result.best->part, &roots,
                                    &assets, &asset_error) != KICAD_ASSET_OK);
    assert(assets.suggested_symbol_id && strcmp(assets.suggested_symbol_id, "Device:R") == 0);
    assert(assets.symbol_found && assets.symbol_is_suggestion);
    assert(assets.suggested_footprint_filter != NULL);
    assert(assets.suggested_footprint_id &&
           strcmp(assets.suggested_footprint_id, "Resistor_SMD:R_0603_1608Metric") == 0);
    assert(assets.footprint_found && assets.footprint_is_suggestion);
    assert(!assets.explicit_mapping_valid);
    assert(assets.pin_map_resolution == KICAD_PIN_MAP_INFERRED_UNVERIFIED);

    /* JSON must make incomplete/suggested assets visible rather than imply readiness. */
    memset(&json_input, 0, sizeof(json_input));
    json_input.catalogue_path = "parts.sx";
    json_input.query_kind = "R";
    json_input.resolution = &result;
    json_input.asset_roots = &roots;
    json_input.assets = &assets;
    json = design_resolution_json_to_string(&json_input, &json_length);
    assert(json && json_length > 0u);
    assert(has_text(json, "ASSET_RESOLUTION_INCOMPLETE"));
    assert(has_text(json, "SUGGESTION_RESOLVED"));
    assert(has_text(json, "pin_map_verified\":false"));
    free(json);

    /* Project-local library tables take precedence and expand KIPRJMOD. */
    query.part_id = "resistor_test_10k";
    assert(design_resolve_part(&arena, &view, 1u, &query, &result) == DESIGN_RESOLVE_OK);
    memset(&root_options, 0, sizeof(root_options));
    (void)snprintf(project_root, sizeof(project_root), "%s/project", fixture);
    root_options.project_directory = project_root;
    assert(kicad_asset_roots_discover(&arena, &root_options, &roots) == KICAD_DISCOVERY_OK);
    assert(roots.symbol_table_path && roots.footprint_table_path);
    assert(kicad_part_asset_resolve(&arena, result.best->part, &roots,
                                    &assets, &asset_error) == KICAD_ASSET_OK);
    assert(assets.symbol_found && assets.footprint_found && assets.explicit_mapping_valid);
    assert(has_text(assets.symbol_path, "local_symbols"));
    assert(has_text(assets.footprint_path, "local_footprints"));
    arena_destroy(&arena);
}

int main(void) {
    test_kicad_asset_resolution_and_design_json();
    puts("Catalogue asset resolution tests passed.");
    return 0;
}

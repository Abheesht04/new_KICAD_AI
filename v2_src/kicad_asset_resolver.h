#ifndef COPPER_KICAD_ASSET_RESOLVER_H
#define COPPER_KICAD_ASSET_RESOLVER_H

#include "arena.h"
#include "kicad_assets.h"
#include "part_catalogue.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum KicadAssetDiscoveryStatus {
    KICAD_DISCOVERY_OK = 0,
    KICAD_DISCOVERY_PARTIAL,
    KICAD_DISCOVERY_NOT_FOUND,
    KICAD_DISCOVERY_ERR_ARGUMENT,
    KICAD_DISCOVERY_ERR_MEMORY
} KicadAssetDiscoveryStatus;

typedef struct KicadAssetRootOptions {
    const char *symbol_root;       /* optional explicit KiCad symbols directory */
    const char *footprint_root;    /* optional explicit KiCad footprints directory */
    const char *copper_asset_root; /* optional imported Copper asset store */
    const char *project_directory; /* optional project for KIPRJMOD and library tables */
    const char *symbol_table_path;  /* optional explicit sym-lib-table */
    const char *footprint_table_path; /* optional explicit fp-lib-table */
} KicadAssetRootOptions;

typedef struct KicadAssetRoots {
    const char *symbol_root;
    const char *footprint_root;
    const char *symbol_source;
    const char *footprint_source;
    const char *project_directory;
    const char *symbol_table_path;
    const char *footprint_table_path;
    bool symbols_available;
    bool footprints_available;
} KicadAssetRoots;

typedef enum KicadPinMapResolution {
    KICAD_PIN_MAP_NOT_CHECKED = 0,
    KICAD_PIN_MAP_EXPLICIT_VALID,
    KICAD_PIN_MAP_EXPLICIT_INVALID,
    KICAD_PIN_MAP_INFERRED_UNVERIFIED,
    KICAD_PIN_MAP_INCOMPLETE,
    KICAD_PIN_MAP_NOT_APPLICABLE
} KicadPinMapResolution;

typedef struct KicadPartAssetResolution {
    KicadAssetStatus symbol_status;
    KicadAssetStatus footprint_status;
    KicadAssetStatus mapping_status;
    KicadPinMapResolution pin_map_resolution;
    const char *symbol_path;
    const char *footprint_path;
    const char *suggested_symbol_id;
    const char *suggested_footprint_id;
    const char *suggested_footprint_filter;
    const char *effective_symbol_id;
    const char *effective_footprint_id;
    const char *message;
    const KicadSymbolAsset *symbol;
    const KicadFootprintAsset *footprint;
    const KicadSymbolLibrary *symbol_library;
    size_t unique_symbol_pin_count;
    size_t matching_pad_count;
    size_t missing_pad_count;
    bool symbol_found;
    bool footprint_found;
    bool symbol_is_suggestion;
    bool footprint_is_suggestion;
    bool explicit_mapping_valid;
    bool inferred_mapping_possible;
} KicadPartAssetResolution;

KicadAssetDiscoveryStatus kicad_asset_roots_discover(
    Arena *arena, const KicadAssetRootOptions *options, KicadAssetRoots *out);
const char *kicad_asset_discovery_status_name(KicadAssetDiscoveryStatus status);
const char *kicad_pin_map_resolution_name(KicadPinMapResolution status);

/* Resolve existing KiCad IDs against the discovered/default library roots.
 * Missing IDs are not silently assigned. A safe generic symbol suggestion may
 * be returned, but it is explicitly marked as a suggestion. */
KicadAssetStatus kicad_part_asset_resolve(
    Arena *arena, const PartRecord *part, const KicadAssetRoots *roots,
    KicadPartAssetResolution *out, KicadAssetError *error);

#ifdef __cplusplus
}
#endif
#endif

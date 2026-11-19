#ifndef COPPER_KICAD_ASSETS_H
#define COPPER_KICAD_ASSETS_H

#include "IR.h"
#include "part_catalogue.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum KicadAssetStatus {
    KICAD_ASSET_OK = 0,
    KICAD_ASSET_ERR_ARGUMENT,
    KICAD_ASSET_ERR_PARSE,
    KICAD_ASSET_ERR_SCHEMA,
    KICAD_ASSET_ERR_MEMORY,
    KICAD_ASSET_ERR_NOT_FOUND,
    KICAD_ASSET_ERR_DUPLICATE,
    KICAD_ASSET_ERR_IO,
    KICAD_ASSET_ERR_RANGE
} KicadAssetStatus;

typedef struct KicadSymbolPin {
    const char *number;
    const char *name;
    const char *electrical_type;
    uint32_t unit;             /* 0 for common pins / root-symbol pins */
    uint32_t body_style;       /* KiCad alternate body style, usually 1 */
} KicadSymbolPin;

typedef struct KicadSymbolAsset {
    const char *name;
    const char *extends;       /* NULL for a non-derived symbol */
    const char *footprint_default; /* exact default Nickname:Footprint when supplied */
    const char *footprint_filter;  /* ki_fp_filters wildcard expression */
    const char *datasheet;
    KicadSymbolPin *pins;
    size_t pin_count;
    bool pins_inherited;       /* true when (extends ...) requires parent resolution */
} KicadSymbolAsset;

typedef struct KicadSymbolLibrary {
    uint32_t format_version;
    KicadSymbolAsset *symbols;
    size_t symbol_count;
} KicadSymbolLibrary;

typedef struct KicadFootprintPad {
    const char *number;
    const char *type;          /* smd, thru_hole, np_thru_hole, connect */
    const char *shape;         /* rect, roundrect, circle, oval, custom, ... */
    int64_t x_nm;
    int64_t y_nm;
    int64_t rotation_mdeg;   /* exact milli-degrees */
    int64_t size_x_nm;
    int64_t size_y_nm;
    int64_t drill_x_nm;        /* zero when absent */
    int64_t drill_y_nm;
    int64_t drill_offset_x_nm;
    int64_t drill_offset_y_nm;
} KicadFootprintPad;

typedef struct KicadFootprintAsset {
    uint32_t format_version;
    const char *name;
    const char *description;
    const char *tags;
    KicadFootprintPad *pads;
    size_t pad_count;
} KicadFootprintAsset;

typedef struct KicadAssetError {
    KicadAssetStatus code;
    const char *message;       /* static diagnostic */
    size_t item_index;
    IRError ir_error;
} KicadAssetError;

/* Parse a packed .kicad_sym library. The complete source tree is retained by
 * the caller's Arena; this typed view indexes top-level symbols and their pins.
 * Derived symbols report `pins_inherited` until a parent library is resolved. */
KicadAssetStatus kicad_symbol_library_parse(
    Arena *arena, const char *text, size_t length,
    KicadSymbolLibrary *out, KicadAssetError *error);

const KicadSymbolAsset *kicad_symbol_find(
    const KicadSymbolLibrary *library, const char *name);

/* Parse one .kicad_mod footprint file. Coordinates and sizes are exact integer nm. */
KicadAssetStatus kicad_footprint_parse(
    Arena *arena, const char *text, size_t length,
    KicadFootprintAsset *out, KicadAssetError *error);

bool kicad_footprint_has_pad(const KicadFootprintAsset *footprint,
                             const char *pad_number);

/* Verify every physical-part mapping against imported native assets.
 * This checks existence and pin/pad identity, not electrical equivalence. */
KicadAssetStatus kicad_part_assets_validate(
    const PartRecord *part,
    const KicadSymbolLibrary *symbol_library,
    const KicadFootprintAsset *footprint,
    KicadAssetError *error);

typedef struct KicadAssetValidationReport {
    size_t checked_parts;
    size_t passed_parts;
    size_t failed_parts;
} KicadAssetValidationReport;

/* Resolve Nickname:Entry IDs using KiCad-style library directories:
 * <symbol_root>/<Nickname>.kicad_sym
 * <footprint_root>/<Nickname>.pretty/<Entry>.kicad_mod
 * Symbol libraries are cached for the duration of this call. */
KicadAssetStatus kicad_part_catalogue_validate_paths(
    Arena *arena,
    const PartCatalogue *catalogue,
    const char *symbol_root,
    const char *footprint_root,
    KicadAssetValidationReport *report,
    KicadAssetError *error);

const char *kicad_asset_status_name(KicadAssetStatus status);

#ifdef __cplusplus
}
#endif

#endif

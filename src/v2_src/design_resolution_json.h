#ifndef COPPER_DESIGN_RESOLUTION_JSON_H
#define COPPER_DESIGN_RESOLUTION_JSON_H

#include "design_resolver.h"
#include "kicad_asset_resolver.h"
#include "filesystem.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DesignResolutionJsonInput {
    const char *project_name;
    const char *catalogue_path;
    const char *query_text;
    const char *query_kind;
    const char *query_mpn;
    const char *query_manufacturer;
    const DesignResolveResult *resolution;
    const KicadAssetRoots *asset_roots;
    const KicadPartAssetResolution *assets;
} DesignResolutionJsonInput;

/* Returned JSON is malloc-owned. The output is deterministic for identical
 * catalogue contents, query, asset roots and library files. */
char *design_resolution_json_to_string(const DesignResolutionJsonInput *input,
                                       size_t *out_length);
FsStatus design_resolution_json_write_file(const DesignResolutionJsonInput *input,
                                           const char *output_path);

#ifdef __cplusplus
}
#endif
#endif

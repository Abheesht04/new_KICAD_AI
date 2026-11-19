#ifndef COPPER_DESIGN_RESOLVER_H
#define COPPER_DESIGN_RESOLVER_H

#include "part_catalogue.h"
#include "arena.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Catalogue source identifies where a candidate came from. KiCad is the
 * default asset authority; user catalogues may override/enrich identities. */
typedef enum DesignCatalogueSource {
    DESIGN_CATALOGUE_KICAD = 0,
    DESIGN_CATALOGUE_USER = 1,
    DESIGN_CATALOGUE_SUPPLIER = 2
} DesignCatalogueSource;

typedef enum DesignResolveStatus {
    DESIGN_RESOLVE_OK = 0,
    DESIGN_RESOLVE_NOT_FOUND,
    DESIGN_RESOLVE_AMBIGUOUS,
    DESIGN_RESOLVE_INCOMPLETE,
    DESIGN_RESOLVE_ERR_ARGUMENT,
    DESIGN_RESOLVE_ERR_MEMORY
} DesignResolveStatus;

typedef struct DesignRatingRequirement {
    PartRatingName name;
    PartRatingBound bound;
    PartRatingUnit unit;
    int64_t minimum_value; /* inclusive; zero means no numeric constraint */
    int64_t maximum_value; /* inclusive; zero means no numeric constraint */
    bool required;          /* missing rating makes candidate incomplete */
} DesignRatingRequirement;

typedef struct DesignPartQuery {
    const char *part_id;            /* exact canonical ID when supplied */
    PartKind kind;                 /* INVALID means any kind */
    const char *manufacturer;
    const char *mpn;               /* exact match preferred */
    const char *package;
    const char *symbol_id;
    const char *footprint_id;
    const char *model_id;
    const char *text;              /* case-insensitive substring across metadata */
    const DesignRatingRequirement *ratings;
    size_t rating_count;
    bool require_symbol;
    bool require_footprint;
    bool require_model;
    bool require_verified_identity;
} DesignPartQuery;

typedef struct DesignCatalogueView {
    const PartCatalogue *catalogue;
    DesignCatalogueSource source;
    const char *name;
    int32_t priority; /* larger wins only after match score */
} DesignCatalogueView;

typedef struct DesignResolveCandidate {
    const PartRecord *part;
    const char *catalogue_name;
    DesignCatalogueSource source;
    int32_t score;
    int32_t priority;
    uint32_t missing_fields;
    bool identity_verified;
    bool ratings_complete;
    bool usable_for_symbol;
    bool usable_for_footprint;
    bool usable_for_simulation;
    bool usable_for_manufacturing;
} DesignResolveCandidate;

enum {
    DESIGN_MISSING_MANUFACTURER = 1u << 0,
    DESIGN_MISSING_MPN          = 1u << 1,
    DESIGN_MISSING_SYMBOL       = 1u << 2,
    DESIGN_MISSING_FOOTPRINT    = 1u << 3,
    DESIGN_MISSING_MODEL        = 1u << 4,
    DESIGN_MISSING_PIN_MAP      = 1u << 5,
    DESIGN_MISSING_PROVENANCE   = 1u << 6,
    DESIGN_MISSING_KIND        = 1u << 7,
    DESIGN_GENERATED_ID        = 1u << 8
};

typedef struct DesignResolveResult {
    DesignResolveStatus status;
    DesignResolveCandidate *candidates; /* sorted best-first */
    size_t candidate_count;
    const DesignResolveCandidate *best;
    size_t ambiguous_count;              /* tied at best score and priority */
} DesignResolveResult;

const char *design_resolve_status_name(DesignResolveStatus status);
const char *design_catalogue_source_name(DesignCatalogueSource source);
/* Recommended tie-break priorities: KiCad libraries first by default,
 * then user overlays, then supplier feeds. Explicit query matches still win. */
int32_t design_catalogue_default_priority(DesignCatalogueSource source);

/* Deterministic, integer-scored candidate search. No candidate is silently
 * promoted to verified: missing data is represented in missing_fields and
 * capability flags. Candidate storage belongs to the supplied Arena. */
DesignResolveStatus design_resolve_part(
    Arena *arena,
    const DesignCatalogueView *catalogues,
    size_t catalogue_count,
    const DesignPartQuery *query,
    DesignResolveResult *out);

/* Capability gate for downstream generation. A part may be useful for symbol
 * discovery while remaining unsuitable for simulation or manufacturing. */
bool design_candidate_meets_requirements(
    const DesignResolveCandidate *candidate,
    const DesignPartQuery *query);

#ifdef __cplusplus
}
#endif
#endif

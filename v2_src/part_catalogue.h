#ifndef COPPER_PART_CATALOGUE_H
#define COPPER_PART_CATALOGUE_H

/*
 * Deterministic, data-only part catalogue.
 *
 * Units are fixed-point integer subunits. No floating-point value is stored
 * in a catalogue record. All strings and arrays are owned by the caller's
 * Arena. The parsed IR tree and the typed view share that Arena lifetime.
 */

#include "IR.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PART_CATALOGUE_SCHEMA_VERSION 1u
#define PART_CATALOGUE_MAX_RATINGS 64u
#define PART_CATALOGUE_MAX_PINS 128u

typedef enum PartCatalogueStatus {
    PART_CATALOGUE_OK = 0,
    PART_CATALOGUE_ERR_ARGUMENT,
    PART_CATALOGUE_ERR_PARSE,
    PART_CATALOGUE_ERR_SCHEMA,
    PART_CATALOGUE_ERR_DUPLICATE,
    PART_CATALOGUE_ERR_RANGE,
    PART_CATALOGUE_ERR_MEMORY
} PartCatalogueStatus;

typedef enum PartKind {
    PART_KIND_INVALID = 0,
    PART_KIND_RESISTOR,
    PART_KIND_CAPACITOR,
    PART_KIND_INDUCTOR,
    PART_KIND_DIODE,
    PART_KIND_TRANSISTOR,
    PART_KIND_MOSFET,
    PART_KIND_IC,
    PART_KIND_CONNECTOR,
    PART_KIND_SWITCH
} PartKind;

typedef enum PartRatingName {
    PART_RATING_INVALID = 0,
    PART_RATING_RESISTANCE,
    PART_RATING_CAPACITANCE,
    PART_RATING_INDUCTANCE,
    PART_RATING_VOLTAGE,
    PART_RATING_CURRENT,
    PART_RATING_POWER,
    PART_RATING_TOLERANCE,
    PART_RATING_HEIGHT,
    PART_RATING_DROPOUT,
    PART_RATING_QUIESCENT_CURRENT,
    PART_RATING_TEMPERATURE
} PartRatingName;

typedef enum PartRatingBound {
    PART_BOUND_INVALID = 0,
    PART_BOUND_NOMINAL,
    PART_BOUND_MINIMUM,
    PART_BOUND_TYPICAL,
    PART_BOUND_MAXIMUM
} PartRatingBound;

typedef enum PartRatingUnit {
    PART_UNIT_INVALID = 0,
    PART_UNIT_UOHM,
    PART_UNIT_PF,
    PART_UNIT_NH,
    PART_UNIT_UV,
    PART_UNIT_UA,
    PART_UNIT_UW,
    PART_UNIT_PPM,
    PART_UNIT_UM,
    PART_UNIT_MC
} PartRatingUnit;

typedef enum PartSourceStatus {
    PART_SOURCE_UNVERIFIED = 0,
    PART_SOURCE_VERIFIED
} PartSourceStatus;

typedef struct PartSource {
    const char *url;
    const char *document;
    const char *revision;
    uint32_t page;              /* 0 means the source did not supply a page */
    PartSourceStatus status;
    const char *verified_by;    /* required only when status == VERIFIED */
    const char *verified_on;    /* ISO YYYY-MM-DD, required when verified */
} PartSource;

typedef struct PartRating {
    PartRatingName name;
    PartRatingBound bound;
    int64_t value;              /* exact integer in `unit` */
    PartRatingUnit unit;
    PartSource source;
} PartRating;

typedef struct PartPinMap {
    const char *terminal;       /* model/logical terminal, e.g. "anode" */
    const char *symbol_pin;     /* KiCad symbol pin number */
    const char *footprint_pad;  /* KiCad footprint pad number */
} PartPinMap;

typedef struct PartRecord {
    const char *id;
    bool id_generated;             /* deterministic identity synthesized from available metadata */
    const IRNode *source_record;   /* original record retained for forward compatibility */             /* stable, lowercase machine identifier */
    PartKind kind;
    const char *manufacturer;
    const char *mpn;
    const char *symbol_id;      /* Nickname:Symbol */
    const char *footprint_id;   /* Nickname:Footprint */
    const char *model_id;       /* optional copper-core device model */
    const char *package;
    const char *description;       /* optional human-readable description */
    const char *datasheet;         /* optional datasheet URL/document */
    const char *supplier;          /* optional catalogue/provider name */
    const char **aliases;          /* optional MPN/name/search aliases */
    size_t alias_count;
    PartSource identity_source;    /* zeroed means provenance unavailable */

    PartRating *ratings;
    size_t rating_count;
    PartPinMap *pins;
    size_t pin_count;
} PartRecord;

typedef struct PartCatalogue {
    uint32_t schema_version;
    PartRecord *parts;
    size_t part_count;
    PartRecord **by_id; /* sorted pointer index; parts retain source order */
} PartCatalogue;

typedef struct PartCatalogueError {
    PartCatalogueStatus code;
    size_t item_index;          /* zero-based part index; SIZE_MAX if not applicable */
    const char *field;          /* static field name */
    const char *message;        /* static diagnostic */
    IRError ir_error;
} PartCatalogueError;

const char *part_catalogue_status_name(PartCatalogueStatus status);
const char *part_kind_name(PartKind kind);
const char *part_rating_name(PartRatingName name);
const char *part_rating_bound_name(PartRatingBound bound);
const char *part_rating_unit_name(PartRatingUnit unit);

/* Parse one `(copper_parts ...)` document from memory. */
PartCatalogueStatus part_catalogue_parse(
    Arena *arena,
    const char *text,
    size_t length,
    PartCatalogue *out,
    PartCatalogueError *error);

/* Read and parse a catalogue file. File I/O remains separate from parsing. */
PartCatalogueStatus part_catalogue_parse_file(
    Arena *arena,
    const char *path,
    PartCatalogue *out,
    PartCatalogueError *error);

/* Stable linear lookup. Catalogue order is preserved exactly as authored. */
const PartRecord *part_catalogue_find(
    const PartCatalogue *catalogue, const char *part_id);

/* Find one exact rating (name + bound); returns NULL when absent. */
const PartRating *part_record_find_rating(
    const PartRecord *part, PartRatingName name, PartRatingBound bound);

/* Data is fabrication/electrical-ready only when all identity/rating sources
 * used by the part are explicitly verified. This does not verify KiCad
 * symbol/footprint pad parity or the model contract; those are separate gates. */
bool part_record_sources_verified(const PartRecord *part);

#ifdef __cplusplus
}
#endif

#endif /* COPPER_PART_CATALOGUE_H */

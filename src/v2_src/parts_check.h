/*
 * parts_check.h - full report on a Copper part catalogue (copper_parts schema 1), for curated part libraries.
 *
 * `copper catalogue validate` stops at the first KiCad problem. This check reports every problem of every
 * part and says which data is verified, so a library can be reviewed part by part (PIPELINE_HARD_RULES.md H2).
 * It never fills in, guesses or repairs a value: it only reads the catalogue (part_catalogue.c) and the
 * installed KiCad libraries (kicad_assets.c).
 *
 * Checks per part:
 *   MISSING_IDENTITY        FAIL        manufacturer, mpn, datasheet, symbol or footprint is absent
 *   NO_IDENTITY_SOURCE      FAIL        no (identity_source ...): nothing says where the identity comes from
 *   CITATION_INCOMPLETE     FAIL        a source has no (revision ...), or a rating source has no (page ...)
 *   DUPLICATE_MPN           FAIL        another part has the same manufacturer and MPN
 *   KICAD_ASSET             FAIL        symbol or footprint missing, or a mapped pin/pad absent (kicad_assets.c)
 *   REQUIRED_RATING_MISSING UNVERIFIED  a rating this kind of part needs is absent (resistor: resistance and
 *                                       power; capacitor: capacitance and voltage; inductor: inductance and
 *                                       current; diode, transistor, MOSFET: voltage and current)
 *   SOURCE_UNVERIFIED       UNVERIFIED  the identity or some ratings are transcribed but not verified by a person
 *   KICAD_NOT_RUN           NOT_RUN     no KiCad library roots were given (one finding for the whole catalogue)
 *
 * Verdict, worst first: INVALID_INPUT (the catalogue does not parse) > FAIL > UNVERIFIED (also when the KiCad
 * check did not run) > PASS. PASS means every part's identity and ratings are verified by a person and every
 * KiCad mapping exists; it does not mean the part suits a design.
 */
#ifndef COPPER_PARTS_CHECK_H
#define COPPER_PARTS_CHECK_H

#include "arena.h"
#include "part_catalogue.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum PartsCheckStatus {
    PARTS_CHECK_PASS = 0,
    PARTS_CHECK_UNVERIFIED,
    PARTS_CHECK_FAIL,
    PARTS_CHECK_INVALID_INPUT,
    PARTS_CHECK_ERR_ARGUMENT,
    PARTS_CHECK_ERR_MEMORY
} PartsCheckStatus;

typedef enum PartsFindingLevel {
    PARTS_FINDING_FAIL = 0,
    PARTS_FINDING_UNVERIFIED,
    PARTS_FINDING_NOT_RUN
} PartsFindingLevel;

typedef struct PartsFinding {
    PartsFindingLevel level;
    const char *code;           /* static, e.g. "KICAD_ASSET" */
    const char *part_id;        /* NULL for a catalogue-wide finding */
    size_t part_index;          /* SIZE_MAX for a catalogue-wide finding */
    const char *message;        /* Arena-owned */
} PartsFinding;

typedef struct PartsCheckPart {
    const char *id;
    const char *kind;           /* "R", "C", ... */
    const char *mpn;            /* NULL when absent */
    const char *kicad;          /* "PASS", "FAIL" or "NOT_RUN" */
    bool identity_verified;
    size_t ratings;
    size_t ratings_verified;
    size_t fails;
    size_t unverified;
} PartsCheckPart;

typedef struct PartsCheckReport {
    PartsCheckStatus verdict;
    const char *source;         /* the catalogue path, or "text" */
    char invalid_message[256];  /* set when verdict == PARTS_CHECK_INVALID_INPUT */
    bool kicad_checked;
    PartsCheckPart *parts;
    size_t part_count;
    PartsFinding *findings;
    size_t finding_count;
    size_t fail_count;
    size_t unverified_count;
} PartsCheckReport;

/* Check a catalogue held in memory. symbols_root and footprints_root are both given or both NULL (then the KiCad
 * check is NOT_RUN). Returns the verdict, or PARTS_CHECK_ERR_ARGUMENT / PARTS_CHECK_ERR_MEMORY. */
PartsCheckStatus parts_check_text(Arena *arena, const char *text, size_t length, const char *symbols_root,
                                  const char *footprints_root, PartsCheckReport *out);

/* The same for a file; a file that cannot be read is INVALID_INPUT. */
PartsCheckStatus parts_check_file(Arena *arena, const char *path, const char *symbols_root,
                                  const char *footprints_root, PartsCheckReport *out);

/* The report as JSON (format "copper.parts-check/1"), Arena-owned. */
const char *parts_check_report_json(Arena *arena, const PartsCheckReport *report, size_t *length);

const char *parts_check_status_name(PartsCheckStatus status);
const char *parts_finding_level_name(PartsFindingLevel level);

#ifdef __cplusplus
}
#endif

#endif

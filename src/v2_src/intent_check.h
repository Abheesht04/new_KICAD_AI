#ifndef COPPER_INTENT_CHECK_H
#define COPPER_INTENT_CHECK_H

/*
 * intent_check.h - grounding gate for model-written design intents (copper.design-intent v1).
 *
 * A language model (Manus) writes the design intent; this gate checks every claim in it against the KiCad libraries
 * that are actually installed, before anything is compiled. A design that names a part, pin, pad or pin name that
 * does not exist, leaves a net with one end, or carries a field outside the schema (an invented MPN, rating,
 * "routed" flag...) is REJECTED with one finding per problem. Nothing is guessed, repaired or substituted.
 *
 * Checks:
 *   - the shape and the exact key set of schema v1 (unknown keys are rejected anywhere), plus one required key per
 *     pin, "pin_name", the name the author believes the pin has;
 *   - references (R1, D12...) and their uniqueness; non-empty values;
 *   - each symbol_id exists in <symbols_root>/<Lib>.kicad_sym, is not derived (extends) and has a single unit;
 *   - each footprint_id exists as <footprints_root>/<Lib>.pretty/<Name>.kicad_mod;
 *   - every symbol pin is mapped exactly once, every mapped pin exists, and its pin_name equals the library's name
 *     (this catches a swapped LED/diode polarity even when the pin numbers themselves are valid);
 *   - every numbered footprint pad is used exactly once, and every mapped pad exists;
 *   - net names; every named net has at least two pins; no power-input pin is left unconnected;
 *   - optional positions lie at least 2.5 mm inside the board (exact nm arithmetic).
 *
 * Memory: everything in the caller's Arena. No printf, no exit.
 */

#include "arena.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum IntentCheckStatus {
    INTENT_CHECK_GROUNDED = 0,   /* every claim matched the installed libraries */
    INTENT_CHECK_REJECTED,       /* at least one finding */
    INTENT_CHECK_ERR_ARGUMENT,
    INTENT_CHECK_ERR_MEMORY
} IntentCheckStatus;

typedef struct IntentFinding {
    const char *code;            /* e.g. "PIN_NAME_MISMATCH" */
    const char *reference;       /* component, or NULL for a design-level finding */
    const char *item;            /* pin / pad / net / key, or NULL */
    const char *message;         /* what is wrong, with the library's actual value where there is one */
} IntentFinding;

typedef struct IntentCheckReport {
    IntentFinding *findings;
    size_t count, capacity;
    size_t components, pins, nets; /* counted from the intent */
    bool truncated;                /* more than INTENT_CHECK_MAX_FINDINGS */
} IntentCheckReport;

#define INTENT_CHECK_MAX_FINDINGS 256

IntentCheckStatus intent_check(Arena *arena, const char *intent_json, size_t length, const char *symbols_root,
                               const char *footprints_root, IntentCheckReport *out);

/* {"format":"copper.intent-check/1","verdict":"GROUNDED"|"REJECTED","components":..,"findings":[..]} */
const char *intent_check_report_json(Arena *arena, const IntentCheckReport *report, size_t *length);

const char *intent_check_status_name(IntentCheckStatus status);

#ifdef __cplusplus
}
#endif

#endif /* COPPER_INTENT_CHECK_H */

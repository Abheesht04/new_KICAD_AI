#ifndef COPPER_V2_PROJECT_RULES_H
#define COPPER_V2_PROJECT_RULES_H

/*
 * project_rules.h - the KiCad project's design rules, written from the target fab profile and Copper's routing rules
 * (LAYOUT_RULES.md section 6, Step 3b), so that KiCad DRC checks the fab's limits and Copper's design rules, never
 * KiCad's built-in defaults. Integer nanometres; no floating point.
 *
 * Writes:
 *   - <project>.kicad_pro: board.design_settings.rules (board minimums, from the fab profile) and the "Default" net
 *     class (clearance, track width, via, from Copper's routing rules). Every other member of an existing file is
 *     kept as it is.
 *   - <project>.kicad_dru: one rule per fine-pitch footprint (R5): a footprint whose own pads of different nets are
 *     closer than the Default clearance gets clearance = the fab's pad-to-pad limit between its own pads
 *     (memberOfFootprint on both items). Never below the fab's limit; a footprint whose pads are closer than that
 *     limit is FAIL for the fab, and the rule still makes KiCad report it.
 *
 * Mapping (KiCad 9.0 key <- source), from a real KiCad 9.0 project file and checked with kicad-cli:
 *   min_clearance <- clearance_default_min_mm        min_track_width <- track_width_min_mm
 *   min_through_hole_diameter <- drill_min_mm        min_hole_to_hole <- via_hole_to_hole_min_mm
 *   min_hole_clearance <- via_hole_to_track_min_mm   min_copper_edge_clearance <- copper_to_edge_min_mm
 *   min_via_diameter <- via_diameter_min_mm          min_via_annular_width <- via_diameter_over_hole_min_mm / 2 (up)
 *   min_text_height <- silk_text_height_min_mm       min_text_thickness <- silk_line_width_min_mm
 * A limit the fab does not publish is not written (KiCad's default then stands, and the report says that it is not
 * fab evidence). Not written, and reported: KiCad's min_silk_clearance (silk to silk, not the fab's pad to silk),
 * the solder mask web (a .kicad_pcb setup value), pad hole to hole, and the copper limits KiCad has no board
 * minimum for (pad to track, PTH to track, SMD pad to pad): each is reported as covered by the Default clearance
 * when that is at least the limit, otherwise as not checked by KiCad.
 */

#include "arena.h"
#include "board.h"
#include "dfm.h"
#include "fab_global.h"
#include "json.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum PrStatus {
    PR_OK = 0,
    PR_INVALID_ARGUMENT,
    PR_INVALID_RULES,      /* routing rules missing a value, or below the fab's limit: key names it */
    PR_INVALID_PROJECT,    /* the existing .kicad_pro is not a JSON object */
    PR_OUT_OF_MEMORY
} PrStatus;

const char *pr_status_name(PrStatus status);

typedef struct PrError {
    PrStatus code;
    const char *key;
    const char *message;   /* static string */
} PrError;

/* Copper's routing rules (copper.routing_rules/1): the values the Default net class gets. */
typedef struct PrDesignRules {
    const char *name;
    G2Nm track, clearance, via_diameter, via_drill;
    bool has_fine;            /* Step 3c: fine_pitch_clearance_mm given (the router's escape stubs) */
    G2Nm fine_clearance;      /* a fine-pitch footprint's escape stubs to its own pads; written as a second rule:
                                 A.memberOfFootprint('REF') && B.Type == 'Track' && B.intersectsCourtyard('REF') */
} PrDesignRules;

PrStatus pr_design_rules_from(const JsonValue *root, PrDesignRules *out, PrError *error);

typedef struct PrSetting {
    const char *kicad_key;
    bool written;
    G2Nm nm;
    const char *from;      /* e.g. "fab:drill_min_mm" */
    const char *note;      /* why not written, or how it was derived; may be NULL */
} PrSetting;

typedef struct PrFinePitch {
    const char *reference;
    const char *pad_a, *pad_b;   /* the closest pair of pads on different nets */
    G2Nm min_gap;                /* edge to edge of their boxes (a lower bound when a pad is round or not at 90) */
    bool approximate;
    bool floor_present;          /* false: the fab publishes no pad limit; nothing is relaxed */
    G2Nm floor;                  /* the clearance the rule sets */
    const char *floor_from;      /* "fab:smd_pad_to_pad_min_mm" or "fab:clearance_default_min_mm" */
    bool rule_written;           /* false when the reference cannot be quoted in a rule, or no floor */
    DfmVerdict verdict;          /* PASS: gap >= floor; FAIL: below; UNVERIFIED: no floor or no rule */
} PrFinePitch;

typedef struct PrResult {
    const char *pro_text; size_t pro_len;
    const char *dru_text; size_t dru_len;
    const char *report_text; size_t report_len;
    PrSetting *settings; size_t setting_count;
    PrFinePitch *fine; size_t fine_count;
    DfmVerdict verdict;          /* FAIL if any fine-pitch footprint fails the fab, UNVERIFIED if any is unverified */
} PrResult;

/* First line of every .kicad_dru this module writes; a file without it is not Copper's and is never overwritten. */
#define PR_DRU_HEADER "# Written by copper-core project-rules (src/v2_src/project_rules.c). Do not edit; regenerate."

/* existing_pro: the project's current .kicad_pro, parsed (NULL when there is none); pro_filename: its file name,
   used for meta.filename only when a new file is made. */
PrStatus pr_build(Arena *arena, const Board *board, const DfmProfile *fab, const FabMaskSilk *mask_silk,
                  const PrDesignRules *rules, const JsonValue *existing_pro, const char *pro_filename,
                  PrResult *out, PrError *error);

#ifdef __cplusplus
}
#endif

#endif

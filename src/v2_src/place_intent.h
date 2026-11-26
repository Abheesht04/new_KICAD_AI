/*
 * place_intent.h - placement of a design intent with the C placer (Step 2e).
 *
 *   place_problem: intent.json + layout_hints.json + placement rules + the KiCad footprints -> the placer's problem
 *                  text (version 1, src/v2_src/placement.h): every part's courtyard (body), silkscreen envelope and pads
 *                  with their nets at 0/90/180/270 degrees, the parts' keep-outs (no bodies, no pads), the decoupling
 *                  pairs, and keep-out strips along the board edge (rule body_margin_mm). A fine-pitch part (two of its
 *                  own pads on different nets closer than fine_pitch_gap_mm) has its body grown by
 *                  fine_pitch_escape_mm on every side, so its pads keep room for their escape stubs (Step 3c, R5).
 *   (the caller runs `copper-core place PROBLEM ITERATIONS SEEDS`: placement.c is in another library, G-044)
 *   place_apply:   intent.json + hints + the placer's JSON -> intent.json with a position for every part (the
 *                  lowest-cost legal annealed placement, else the constructive one), and routing.json
 *                  (copper.routing/1) with every part keep-out in board coordinates on F.Cu and B.Cu.
 *
 * Geometry is exact integer nm. Rotations follow KiCad: a point (x, y) of a footprint rotated by r degrees is at
 * (x cos r + y sin r, -x sin r + y cos r) (tools/copper.py rotate). A pad at a rotation that is not a multiple of
 * 90 degrees is replaced by the square of its larger side (conservative). Arcs and circles take their bounding square.
 */
#ifndef COPPER_PLACE_INTENT_H
#define COPPER_PLACE_INTENT_H

#include "arena.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum PlaceStatus {
    PLACE_OK = 0,
    PLACE_INVALID_INPUT,         /* intent, hints, rules or placer output malformed; a footprint missing */
    PLACE_NO_LEGAL_POSITION,     /* the placer found no legal position for some part (board too small?) */
    PLACE_ERR_ARGUMENT,
    PLACE_ERR_MEMORY
} PlaceStatus;

typedef struct PlaceReport {
    PlaceStatus status;
    char message[400];
    size_t parts, keepouts, decoupling;
    size_t fine_pitch;           /* place_problem: parts given escape room (Step 3c, rules fine_pitch_*) */
    size_t labels_moved;         /* Step 3e: Reference labels moved off their own part's silkscreen */
    const char *seed;            /* place_apply: which result was used ("seed 3", "constructive") */
} PlaceReport;

PlaceStatus place_problem(Arena *arena, const char *intent, size_t intent_len, const char *hints, size_t hints_len,
                          const char *rules, size_t rules_len, const char *footprints_root,
                          const char **problem, size_t *problem_len, PlaceReport *report);

/* Step 3e (P4): in a compiled .kicad_pcb, move each Reference label that is still at its library position and lies
 * closer than silk_to_pad_mm to its own part's silkscreen by the same smallest move place_problem used (up, down,
 * left or right, in the footprint frame). A label not at its library position was moved by someone: it is kept.
 * Without the three silk_* rules nothing changes. Only the two numbers of a moved label's (at) are rewritten. */
PlaceStatus place_labels(Arena *arena, const char *pcb, size_t pcb_len, const char *rules, size_t rules_len,
                         const char *footprints_root, const char **pcb_out, size_t *pcb_out_len, PlaceReport *report);

PlaceStatus place_apply(Arena *arena, const char *intent, size_t intent_len, const char *hints, size_t hints_len,
                        const char *placer_json, size_t placer_len,
                        const char **intent_out, size_t *intent_out_len, const char **routing_out, size_t *routing_out_len,
                        PlaceReport *report);

const char *place_status_name(PlaceStatus status);

#ifdef __cplusplus
}
#endif

#endif

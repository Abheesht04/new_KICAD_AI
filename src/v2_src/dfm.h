#ifndef COPPER_V2_DFM_H
#define COPPER_V2_DFM_H

/*
 * dfm.h - DFM checks of a board model against a fab profile (Copper V2 track C), exact integer nanometres.
 *
 * Not the legacy src/dfm.h (rule registry, double mm): a separate module with its own guard and no shared
 * symbols. It sits in src/v2_src, which comes first on copper_v2's include path.
 *
 * Profiles are data (fab_profiles/NAME.json, format "copper.fab_profile/1"): every limit is present, carries the
 * fabricator's own row label and wording, and may be null when the fabricator publishes no value; a null limit makes
 * its check UNVERIFIED, never a guess. No limit is a C constant.
 *
 * Checks: track width; copper clearance between different nets (BVH, exact shape distances); zone clearance (not
 * implemented: zone fills are not in the board model); annular rings; drill and slot sizes; hole to hole; hole to
 * track; copper to board edge; hole to board edge; board size.
 *
 * Exactness: tracks are capsules, vias and round pads circles, oval pads capsules, rect and roundrect pads rounded
 * rectangles, every size rounded outward to whole nm (never smaller than the copper). A violation is reported only
 * when the exact distance is below the limit. Items with no exact shape (arc tracks, Edge.Cuts arcs, pads at angles
 * that are not multiples of 90 degrees, trapezoid pads) are replaced by a shape that contains them: if that shape
 * keeps the limit the pair passes, otherwise the pair is UNVERIFIED. Custom pads are UNVERIFIED as a whole.
 * A distance equal to the limit passes.
 */

#include "arena.h"
#include "board.h"
#include "g2_types.h"
#include "json.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum DfmStatus {
    DFM_OK = 0,
    DFM_INVALID_ARGUMENT,
    DFM_IO_ERROR,
    DFM_PARSE_ERROR,      /* the profile is not valid JSON: line/column set */
    DFM_INVALID_PROFILE,  /* valid JSON, but not a complete copper.fab_profile/1: key names the field */
    DFM_OUT_OF_MEMORY,
    DFM_GEOMETRY_ERROR    /* a board coordinate the exact geometry refuses */
} DfmStatus;

const char *dfm_status_name(DfmStatus status);

typedef enum DfmLimit {
    DFM_TRACK_WIDTH_MIN = 0,
    DFM_TRACK_SPACING_MIN,
    DFM_CLEARANCE_DEFAULT_MIN,
    DFM_PAD_TO_TRACK_MIN,
    DFM_PTH_TO_TRACK_MIN,
    DFM_SMD_PAD_TO_PAD_MIN,
    DFM_PTH_ANNULAR_RING_MIN,
    DFM_NPTH_ANNULAR_RING_MIN,
    DFM_VIA_DIAMETER_OVER_HOLE_MIN,
    DFM_VIA_HOLE_MIN,
    DFM_VIA_DIAMETER_MIN,
    DFM_DRILL_MIN,
    DFM_NPTH_DRILL_MIN,
    DFM_PLATED_SLOT_WIDTH_MIN,
    DFM_NPTH_SLOT_WIDTH_MIN,
    DFM_VIA_HOLE_TO_HOLE_MIN,
    DFM_PAD_HOLE_TO_HOLE_MIN,
    DFM_VIA_HOLE_TO_TRACK_MIN,
    DFM_NPTH_TO_TRACK_MIN,
    DFM_COPPER_TO_EDGE_MIN,
    DFM_HOLE_TO_EDGE_MIN,
    DFM_BOARD_MIN_X,
    DFM_BOARD_MIN_Y,
    DFM_BOARD_MIN_SIZE_THICKNESS_MIN,
    DFM_BOARD_MAX_X,
    DFM_BOARD_MAX_Y,
    DFM_BOARD_MAX_THICKNESS_MIN,
    DFM_BOARD_MAX_THIN_X,
    DFM_BOARD_MAX_THIN_Y,
    DFM_LIMIT_COUNT
} DfmLimit;

/* The profile key of a limit, e.g. "track_width_min_mm". */
const char *dfm_limit_key(DfmLimit limit);

typedef struct DfmLimitValue {
    bool present;          /* false: the fabricator publishes no value (null in the profile) */
    G2Nm nm;
    const char *row;       /* the fabricator's row label (may be NULL when not on the page) */
    const char *quote;     /* the fabricator's wording */
    const char *note;      /* the profile author's interpretation, or NULL */
} DfmLimitValue;

typedef struct DfmProfile {
    const char *name, *fabricator, *process;
    const char *source_url, *source_title, *retrieved;
    DfmLimitValue limits[DFM_LIMIT_COUNT];
} DfmProfile;

typedef struct DfmError {
    DfmStatus code;
    size_t line, column, offset;   /* JSON parse errors */
    const char *key;               /* the profile field at fault */
    const char *message;
} DfmError;

DfmStatus dfm_profile_load(Arena *arena, const char *path, DfmProfile *out, DfmError *error);
DfmStatus dfm_profile_parse(Arena *arena, const char *text, size_t len, DfmProfile *out, DfmError *error);

typedef enum DfmRule {
    DFM_RULE_TRACK_WIDTH = 0,
    DFM_RULE_CLEARANCE,
    DFM_RULE_ZONE_CLEARANCE,
    DFM_RULE_ANNULAR_RING,
    DFM_RULE_DRILL,
    DFM_RULE_HOLE_TO_HOLE,
    DFM_RULE_HOLE_TO_TRACK,
    DFM_RULE_COPPER_TO_EDGE,
    DFM_RULE_HOLE_TO_EDGE,
    DFM_RULE_BOARD_SIZE,
    DFM_RULE_COUNT
} DfmRule;

const char *dfm_rule_name(DfmRule rule);

typedef enum DfmVerdict { DFM_PASS = 0, DFM_FAIL, DFM_UNVERIFIED, DFM_NOT_IMPLEMENTED } DfmVerdict;

const char *dfm_verdict_name(DfmVerdict verdict);

typedef enum DfmItemKind { DFM_ITEM_NONE = 0, DFM_ITEM_TRACK, DFM_ITEM_VIA, DFM_ITEM_PAD, DFM_ITEM_EDGE, DFM_ITEM_BOARD } DfmItemKind;

typedef struct DfmItemRef {
    DfmItemKind kind;
    size_t index;          /* into Board.tracks / vias / pads / edges */
} DfmItemRef;

typedef struct DfmViolation {
    DfmRule rule;
    DfmLimit limit;
    G2Nm limit_nm;
    G2Nm measured_nm;      /* the measured size; -1 for a distance, which is known only to be below the limit */
    DfmItemRef a, b;       /* b.kind == DFM_ITEM_NONE for a single-item check */
    G2Point at;            /* a representative point of item a */
    uint64_t layers;       /* shared copper layers, for clearances */
} DfmViolation;

typedef struct DfmRuleResult {
    DfmRule rule;
    DfmVerdict verdict;
    size_t checked;        /* items or pairs examined exactly */
    size_t violations;
    size_t unverified;     /* items or pairs that could not be decided */
    const char *note;      /* why UNVERIFIED / NOT_IMPLEMENTED, or NULL */
} DfmRuleResult;

typedef struct DfmReport {
    DfmVerdict verdict;            /* FAIL if any rule fails, else UNVERIFIED if any is not PASS, else PASS */
    DfmRuleResult rules[DFM_RULE_COUNT];
    DfmViolation *violations;      /* sorted: rule, limit, y, x, items */
    size_t violation_count;
} DfmReport;

DfmStatus dfm_check(Arena *arena, const Board *board, const DfmProfile *profile, DfmReport *out, DfmError *error);

#ifdef __cplusplus
}
#endif

#endif

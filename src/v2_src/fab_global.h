#ifndef COPPER_V2_FAB_GLOBAL_H
#define COPPER_V2_FAB_GLOBAL_H

/*
 * fab_global.h - the "global" fab profile: the strictest published value of each limit over several
 * copper.fab_profile/1 files (LAYOUT_RULES.md section 2, Step 3a). Integer nanometres; no floating point.
 *
 * Beside dfm.c, not in it: dfm.c parses the "limits" object and is unchanged. This module also reads the
 * optional "mask_silk_limits" section (solder mask web, silkscreen line width, text height, pad to silk),
 * with the same rules as dfm.c: every key present, a quote for each, null when the fab publishes no value,
 * an unknown key is an error.
 *
 * Strictest: a minimum takes the largest published value; a maximum board size takes the smallest. A limit
 * that some profiles do not publish still takes the strictest published value, and its note names the
 * fabricators that publish none (for them the limit is UNVERIFIED, not assumed). A limit no profile publishes
 * stays null. Ties go to the earlier profile, so the result depends only on the order given.
 */

#include "arena.h"
#include "dfm.h"
#include "json.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum FabGlobalStatus {
    FAB_GLOBAL_OK = 0,
    FAB_GLOBAL_INVALID_ARGUMENT,
    FAB_GLOBAL_INVALID_PROFILE,   /* a profile (or its mask_silk_limits) is incomplete or wrong: key names the field */
    FAB_GLOBAL_OUT_OF_MEMORY
} FabGlobalStatus;

const char *fab_global_status_name(FabGlobalStatus status);

typedef enum FabMaskSilkLimit {
    FAB_SOLDER_MASK_WEB_MIN = 0,
    FAB_SILK_LINE_WIDTH_MIN,
    FAB_SILK_TEXT_HEIGHT_MIN,
    FAB_PAD_TO_SILK_MIN,
    FAB_MASK_SILK_COUNT
} FabMaskSilkLimit;

const char *fab_mask_silk_key(FabMaskSilkLimit limit);

typedef struct FabMaskSilk {
    bool present;                                 /* false: the profile has no mask_silk_limits section */
    const char *retrieved;
    DfmLimitValue limits[FAB_MASK_SILK_COUNT];
} FabMaskSilk;

typedef struct FabGlobalError {
    FabGlobalStatus code;
    size_t profile;          /* index of the profile at fault */
    const char *key;
    const char *message;     /* static string */
} FabGlobalError;

/* Reads the mask_silk_limits section of a parsed profile (root object). Absent section: OK, present = false. */
FabGlobalStatus fab_mask_silk_from(const JsonValue *root, FabMaskSilk *out, FabGlobalError *error);

/* One input: a profile and its mask/silk section (as read above). */
typedef struct FabGlobalInput {
    DfmProfile profile;
    FabMaskSilk mask_silk;
} FabGlobalInput;

typedef struct FabGlobal {
    size_t count;                                     /* profiles combined */
    DfmLimitValue limits[DFM_LIMIT_COUNT];            /* row/quote/note name the deciding fabricator */
    int from[DFM_LIMIT_COUNT];                        /* deciding profile index, -1 when no one publishes it */
    size_t missing[DFM_LIMIT_COUNT];                  /* profiles that publish no value */
    DfmLimitValue mask_silk[FAB_MASK_SILK_COUNT];
    int mask_silk_from[FAB_MASK_SILK_COUNT];
    size_t mask_silk_missing[FAB_MASK_SILK_COUNT];
} FabGlobal;

/* Reads profile files (dfm_profile_load + mask_silk_limits) into inputs[i]. */
FabGlobalStatus fab_global_load(Arena *arena, const char *const *paths, size_t count, FabGlobalInput *inputs,
                                FabGlobalError *error);

FabGlobalStatus fab_global_build(Arena *arena, const FabGlobalInput *inputs, size_t count, FabGlobal *out,
                                 FabGlobalError *error);

/* The global profile as a copper.fab_profile/1 document that dfm_profile_parse accepts, with "derived_from"
   listing each input's name, fabricator, url and retrieved date. */
FabGlobalStatus fab_global_write(Arena *arena, const FabGlobalInput *inputs, const FabGlobal *g, const char *name,
                                 const char **text, size_t *len);

#ifdef __cplusplus
}
#endif

#endif

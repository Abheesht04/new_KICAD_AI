#ifndef COPPER_SPICE_EXPAND_H
#define COPPER_SPICE_EXPAND_H

#include "arena.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Include callback contract:
 * - The callback must copy both returned values into scratch_arena.
 * - Returned paths and file bytes remain valid until expansion completes.
 * - The expansion engine never frees callback memory individually.
 *
 * including_path is the resolved path of the source containing .INCLUDE.
 * requested_path is the spelling from the deck.
 */
typedef int (*CspIncludeReadFn)(void *context, Arena *scratch_arena,
                                const char *including_path,
                                const char *requested_path,
                                const char **resolved_path,
                                const char **data, size_t *data_size);

typedef struct CspExpandOptions {
    CspIncludeReadFn read_include;
    void *read_context;
    unsigned max_include_depth;       /* zero selects 16; hard maximum 31 */
    unsigned max_subcircuit_depth;    /* zero selects 16; hard maximum 64 */
} CspExpandOptions;

/*
 * Expand .INCLUDE, source-order .PARAM/{expr}, and .SUBCKT/.ENDS/X instances.
 * All temporary storage is allocated in a private scratch Arena and released
 * in one operation. The NUL-terminated result is allocated in `arena`.
 * `error_line` is the physical line number in the file containing the error.
 */
int csp_expand_deck(Arena *arena, const char *source_name, const char *text,
                    size_t text_size, const CspExpandOptions *options,
                    char **out_text, size_t *out_size, size_t *error_line);

#ifdef __cplusplus
}
#endif
#endif

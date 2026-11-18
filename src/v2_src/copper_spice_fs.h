#ifndef COPPER_SPICE_FS_H
#define COPPER_SPICE_FS_H

#include "copper_spice.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Parse a deck using Copper's UTF-8 filesystem layer. Includes are restricted
 * to the top-level deck's physical parent directory by default. Existing
 * include targets and the allowed root are physically resolved before the
 * containment check; symlink/junction escapes are rejected on supported
 * platforms.
 */
CspStatus csp_parse_file(Arena *arena, const char *path, CspNetlist *out,
                         size_t *error_line);

/* Parse while allowing includes only beneath the physically resolved root. */
CspStatus csp_parse_file_in_root(Arena *arena, const char *path,
                                 const char *allowed_root, CspNetlist *out,
                                 size_t *error_line);

#ifdef __cplusplus
}
#endif
#endif

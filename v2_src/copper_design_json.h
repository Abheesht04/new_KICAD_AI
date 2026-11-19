#ifndef COPPER_DESIGN_JSON_H
#define COPPER_DESIGN_JSON_H

#include "copper_design.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* JSON buffers returned by copper_design_json_to_string are malloc-owned. */
char *copper_design_json_to_string(const CopperDesign *design,
                                   size_t *out_length);
/* Atomic replacement: an existing design.json is replaced only after the
 * complete JSON document has been written successfully. */
FsStatus copper_design_json_write_file(const CopperDesign *design,
                                       const char *output_path);

/* Manifest records imported source hashes and hashes of the generated design
 * and diagnostic report. Paths are recorded exactly as supplied by the CLI. */
char *copper_project_manifest_to_string(const CopperDesign *design,
                                        const char *design_path,
                                        const char *report_path,
                                        size_t *out_length);
FsStatus copper_project_manifest_write_file(const CopperDesign *design,
                                             const char *design_path,
                                             const char *report_path,
                                             const char *manifest_path);

#ifdef __cplusplus
}
#endif
#endif

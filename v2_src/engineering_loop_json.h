#ifndef COPPER_ENGINEERING_LOOP_JSON_H
#define COPPER_ENGINEERING_LOOP_JSON_H

#include "engineering_loop.h"
#include "filesystem.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returned strings are malloc-owned and must be freed by the caller. */
char *engineering_loop_json_to_string(const CopperTopology *topology,
                                      const EngineeringLoopResult *result,
                                      size_t *out_length);
FsStatus engineering_loop_json_write_file(const CopperTopology *topology,
                                          const EngineeringLoopResult *result,
                                          const char *path);
char *engineering_loop_report_to_text(const EngineeringLoopResult *result,
                                      size_t *out_length);

#ifdef __cplusplus
}
#endif
#endif

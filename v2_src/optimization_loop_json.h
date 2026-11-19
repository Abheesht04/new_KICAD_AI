#ifndef COPPER_OPTIMIZATION_LOOP_JSON_H
#define COPPER_OPTIMIZATION_LOOP_JSON_H

#include "optimization_loop.h"
#include "filesystem.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

char *copper_opt_evaluation_json(const CopperOptEvaluation *evaluation,
                                 size_t *out_length);
char *copper_opt_apply_json(const CopperOptApplyResult *result,
                            size_t *out_length);
char *copper_opt_run_json(const CopperOptRunResult *result,
                          size_t *out_length);
FsStatus copper_opt_write_json(const char *path, const char *json,
                               size_t json_length);

#ifdef __cplusplus
}
#endif
#endif

#ifndef COPPER_FAB_RULES_H
#define COPPER_FAB_RULES_H
#include "IR.h"
#include "arena.h"
#include "g2_types.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
  FAB_OK = 0, FAB_ERR_ARGUMENT = -1, FAB_ERR_IO = -2, FAB_ERR_PARSE = -3,
  FAB_ERR_MISSING_FIELD = -4, FAB_ERR_STALE = -5, FAB_ERR_NO_RULE = -6,
  FAB_ERR_MEMORY = -7, FAB_ERR_DUPLICATE = -8, FAB_ERR_RANGE = -9
} FabStatus;
typedef enum { FAB_SEV_REQUIRED = 0, FAB_SEV_RECOMMENDED = 1 } FabSeverity;
typedef struct {
  const char *id;
  const char *kind;
  G2Nm value;
  uint32_t layers_min, layers_max;
  uint32_t copper_oz_x10;
  const char *edge;
  const char *mask_color; /* optional selector: standard or black_white */
  FabSeverity severity;
  const char *note;
  const char *source_url;
  const char *retrieved;
} FabRule;
typedef struct {
  const char *ruleset;
  const char *fabricator;
  FabRule *rules;
  size_t rule_count;
} FabRuleSet;
FabStatus fab_rules_load(const char *path, Arena *arena, const char *today_iso,
                         uint32_t max_age_days, FabRuleSet *out);
FabStatus fab_rules_select(const FabRuleSet *rs, const char *kind,
                           uint32_t copper_layers, uint32_t copper_oz_x10,
                           const char *edge, const FabRule **out,
                           size_t max_out, size_t *out_count);
FabStatus fab_rules_select_ex(const FabRuleSet *rs, const char *kind,
                              uint32_t copper_layers, uint32_t copper_oz_x10,
                              const char *edge, const char *mask_color,
                              const FabRule **out, size_t max_out,
                              size_t *out_count);
const char *fab_status_name(FabStatus status);
#endif

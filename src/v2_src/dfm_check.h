#ifndef COPPER_DFM_CHECK_H
#define COPPER_DFM_CHECK_H
#include "design_board.h"
#include "fab_rules.h"
#include "report.h"
#include <stdint.h>

typedef enum { DFM_OK = 0, DFM_ERR_ARGUMENT = -1, DFM_ERR_MEMORY = -2,
               DFM_ERR_GEOMETRY = -3, DFM_ERR_REPORT_CAPACITY = -4 } DfmStatus;
typedef struct {
  uint32_t copper_oz_x10;       /* 10 = 1 oz, 20 = 2 oz */
  const char *edge_type;        /* routed or vcut */
  bool check_same_net_clearance;
  const char *mask_color; /* standard, black_white; required for colour-specific rules */
} DfmOptions;
DfmStatus dfm_check_board(const Board *board, const BoardDiag *diag,
                          const FabRuleSet *rules, const DfmOptions *options,
                          Arena *arena, CopperReport *report);
#endif

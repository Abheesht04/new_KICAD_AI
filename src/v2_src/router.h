#ifndef COPPER_ROUTER_H
#define COPPER_ROUTER_H
#include "design_board.h"
#include <stddef.h>
#include <stdint.h>

typedef enum {
  ROUTER_OK = 0,
  ROUTER_ERR_ARGUMENT = -1,
  ROUTER_ERR_MEMORY = -2,
  ROUTER_ERR_RANGE = -3,
  ROUTER_ERR_NO_PATH = -4,
  ROUTER_ERR_UNVERIFIED = -5,
  ROUTER_ERR_GEOMETRY = -6
} RouterStatus;

typedef struct {
  BoardLayer layer;
  uint32_t net;
  G2Nm grid_pitch_nm;
  G2Nm trace_width_nm;
  G2Nm clearance_nm;
  G2Nm edge_clearance_nm;
  size_t max_cells;
} RouterOptions;

typedef struct {
  RouterStatus status;
  G2Point *points;
  size_t point_count;
  size_t expanded_nodes;
  uint64_t cost;
  char message[160];
} RouterResult;

/* Deterministic 8-neighbour A*. Unsupported copper geometry fails closed. */
RouterStatus copper_route_astar(const Board *board, G2Point start, G2Point goal,
                                const RouterOptions *options, Arena *arena,
                                RouterResult *out);
#endif

#ifndef COPPER_PLACER_H
#define COPPER_PLACER_H
#include "design_board.h"
#include <stddef.h>

typedef enum {
  PLACER_OK = 0,
  PLACER_PARTIAL = 1,
  PLACER_ERR_ARGUMENT = -1,
  PLACER_ERR_MEMORY = -2,
  PLACER_ERR_UNVERIFIED = -3,
  PLACER_ERR_GEOMETRY = -4
} PlacerStatus;

typedef struct {
  G2Nm grid_pitch_nm;
  G2Nm component_gap_nm;
  G2Nm edge_clearance_nm;
  size_t max_candidates;
} PlacerOptions;

typedef struct {
  uint32_t footprint_index;
  G2Point position;
  int32_t rotation_mdeg;
  bool fixed;
  bool placed;
} PlacerPlacement;

typedef struct {
  PlacerStatus status;
  PlacerPlacement *placements;
  size_t placement_count;
  size_t placed_count;
  size_t unplaced_count;
  size_t candidates_tested;
  char message[160];
} PlacerResult;

/* Deterministic first-fit courtyard-grid placer. It returns proposals only. */
PlacerStatus copper_place_grid(const Board *board, const PlacerOptions *options,
                               Arena *arena, PlacerResult *out);
#endif

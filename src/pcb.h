#ifndef PCB_H
#define PCB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vec.h"

/* KiCad S-expression layer identifiers; not stackup indices. */
typedef enum {
    PCB_LAYER_F_CU = 0,
    PCB_LAYER_B_CU = 2,
    PCB_LAYER_IN1_CU = 4,
    PCB_LAYER_IN2_CU = 6,
    PCB_LAYER_IN3_CU = 8,
    PCB_LAYER_IN4_CU = 10,
    PCB_LAYER_IN5_CU = 12,
    PCB_LAYER_IN6_CU = 14,
    PCB_LAYER_IN7_CU = 16,
    PCB_LAYER_IN8_CU = 18,
    PCB_LAYER_IN9_CU = 20,
    PCB_LAYER_IN10_CU = 22,
    PCB_LAYER_IN11_CU = 24,
    PCB_LAYER_IN12_CU = 26,
    PCB_LAYER_IN13_CU = 28,
    PCB_LAYER_IN14_CU = 30,
    PCB_LAYER_IN15_CU = 32,
    PCB_LAYER_IN16_CU = 34,
    PCB_LAYER_IN17_CU = 36,
    PCB_LAYER_IN18_CU = 38,
    PCB_LAYER_IN19_CU = 40,
    PCB_LAYER_IN20_CU = 42,
    PCB_LAYER_IN21_CU = 44,
    PCB_LAYER_IN22_CU = 46,
    PCB_LAYER_IN23_CU = 48,
    PCB_LAYER_IN24_CU = 50,
    PCB_LAYER_IN25_CU = 52,
    PCB_LAYER_IN26_CU = 54,
    PCB_LAYER_IN27_CU = 56,
    PCB_LAYER_IN28_CU = 58,
    PCB_LAYER_IN29_CU = 60,
    PCB_LAYER_IN30_CU = 62,
} PcbCopperLayer;
typedef struct {
    double width_mm;
    double height_mm;
    double board_thickness_mm;
    double copper_thickness_mm;
    uint8_t layer_count;
} PcbBoardSpec;

typedef struct {
    uint32_t net_id;
    uint8_t layer;
    double x1_mm;
    double y1_mm;
    double x2_mm;
    double y2_mm;
    double width_mm;
} PcbTrace;

typedef struct {
    uint32_t component_id;
    uint16_t pin_number;
    uint32_t net_id;
    double x_mm;
    double y_mm;
    double pad_diameter_mm;
    double drill_mm;
    bool plated;
    uint8_t layer;
    /* V2: axis-aligned rectangular copper; zero keeps legacy circular pads. */
    double width_mm, height_mm, corner_radius_mm;
    bool rectangular;
} PcbPad;

typedef struct {
    uint32_t net_id;
    double x_mm;
    double y_mm;
    double diameter_mm;
    double drill_mm;
    uint8_t layer_start;
    uint8_t layer_end;
    bool plated;
} PcbVia;

typedef struct {
    uint32_t component_id;
    double x_mm;
    double y_mm;
    double rotation_deg;
    double width_mm;
    double height_mm;
    double height_z_mm;
} PcbPlacement;

typedef struct {
    PcbBoardSpec spec;
    PcbTrace *traces;
    PcbPad *pads;
    PcbVia *vias;
    PcbPlacement *placements;
} PcbBoard;

void pcb_board_init(PcbBoard *board, PcbBoardSpec spec);
void pcb_board_free(PcbBoard *board);

int pcb_add_trace(PcbBoard *board, PcbTrace trace);
int pcb_add_pad(PcbBoard *board, PcbPad pad);
int pcb_add_via(PcbBoard *board, PcbVia via);
int pcb_add_placement(PcbBoard *board, PcbPlacement placement);

const PcbPlacement *pcb_find_placement(
    const PcbBoard *board,
    uint32_t component_id
);

#endif

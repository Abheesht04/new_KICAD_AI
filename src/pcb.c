#include "pcb.h"

#include <string.h>

void pcb_board_init(PcbBoard *board, PcbBoardSpec spec)
{
    if (!board) {
        return;
    }

    memset(board, 0, sizeof(*board));
    board->spec = spec;
}

void pcb_board_free(PcbBoard *board)
{
    if (!board) {
        return;
    }

    vec_free(board->traces);
    vec_free(board->pads);
    vec_free(board->vias);
    vec_free(board->placements);

    memset(board, 0, sizeof(*board));
}

int pcb_add_trace(PcbBoard *board, PcbTrace trace)
{
    if (!board || trace.width_mm <= 0.0) {
        return -1;
    }

    return vec_push(board->traces, trace);
}

int pcb_add_pad(PcbBoard *board, PcbPad pad)
{
    if (!board || pad.pad_diameter_mm <= 0.0) {
        return -1;
    }

    if (pad.plated && pad.drill_mm <= 0.0) {
        return -1;
    }

    return vec_push(board->pads, pad);
}

int pcb_add_via(PcbBoard *board, PcbVia via)
{
    if (!board || via.diameter_mm <= 0.0 || via.drill_mm <= 0.0 ||
        via.diameter_mm <= via.drill_mm) {
        return -1;
    }

    return vec_push(board->vias, via);
}

int pcb_add_placement(PcbBoard *board, PcbPlacement placement)
{
    if (!board || placement.width_mm <= 0.0 ||
        placement.height_mm <= 0.0 || placement.height_z_mm < 0.0) {
        return -1;
    }

    return vec_push(board->placements, placement);
}

const PcbPlacement *pcb_find_placement(
    const PcbBoard *board,
    uint32_t component_id
)
{
    if (!board) {
        return NULL;
    }

    for (size_t i = 0; i < vec_len(board->placements); ++i) {
        const PcbPlacement *placement = &board->placements[i];

        if (placement->component_id == component_id) {
            return placement;
        }
    }

    return NULL;
}

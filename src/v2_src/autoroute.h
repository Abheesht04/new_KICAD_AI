#ifndef COPPER_AUTOROUTE_H
#define COPPER_AUTOROUTE_H

/*
 * autoroute.h - deterministic two-layer board router (Copper V2, design overlay).
 *
 * Routes every net of a placed board (design_board.h model, loaded from the .kicad_pcb that Copper's compiler wrote)
 * on F.Cu and B.Cu with through vias, and writes the tracks and vias back into the KiCad file.
 *
 *   - Grid search: per net, a multi-source Dijkstra/A* over (layer, cell) grows a tree from the first pad to every
 *     other pad of the net. F.Cu prefers horizontal and B.Cu vertical moves; a via costs `via_cost` grid steps.
 *   - Obstacles: every copper item of another net (pads, including unconnected ones, tracks, vias), keep-out
 *     rectangles and the board edge, inflated by half the track width plus the clearance, plus a slack of 3/4 of a
 *     grid step so that a straight move between two free cell centres can never come closer than the clearance.
 *   - Exact check: every produced segment and via is then checked with exact integer geometry (g2_types) against all
 *     other-net copper and the edge, with no slack. A net with any failing item is removed whole and reported
 *     FAILED: Copper never writes copper it has not verified. Pads are modelled by their bounding boxes, which is
 *     conservative for round and oval pads.
 *   - Order: nets are routed shortest first; if any fail, the failed nets move to the front and everything is routed
 *     again, up to `passes` times. The pass with the most routed nets (then the fewest vias) is kept.
 *
 * Lengths are int64 nanometres; no floating point. Arena memory; status enums; no printf, no exit.
 */

#include "arena.h"
#include "design_board.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum AutorouteStatus {
    AUTOROUTE_ROUTED = 0,         /* every net with two or more pads is connected */
    AUTOROUTE_PARTIAL,            /* some nets could not be routed: see the per-net results */
    AUTOROUTE_INVALID_INPUT,      /* rules out of range, board already has tracks, no rectangular outline, ... */
    AUTOROUTE_UNSUPPORTED,        /* board features the router does not model (custom pads, zones, arcs, >2 layers) */
    AUTOROUTE_ERR_ARGUMENT,
    AUTOROUTE_ERR_MEMORY
} AutorouteStatus;

typedef struct AutorouteNetWidth { const char *net; G2Nm width_nm; } AutorouteNetWidth;
typedef struct AutorouteKeepout { G2Aabb box; bool front, back; } AutorouteKeepout;

typedef struct AutorouteRules {
    G2Nm grid_nm;                 /* cell pitch */
    G2Nm track_nm;                /* default track width */
    G2Nm clearance_nm;            /* copper to copper of different nets */
    G2Nm edge_clearance_nm;       /* copper to the board outline */
    G2Nm via_size_nm, via_drill_nm;
    uint32_t via_cost;            /* in grid steps */
    uint32_t passes;              /* rip-up-and-reroute passes, at least 1 */
    size_t max_cells;             /* per layer */
    const AutorouteNetWidth *widths; size_t width_count;   /* per-net track widths (others: track_nm) */
    const AutorouteKeepout *keepouts; size_t keepout_count;
    /* Fine-pitch escapes (Step 3c, LAYOUT_RULES.md R5; added beside the base fields, 0 = off, both or neither).
     * A footprint is fine-pitch when two of its own pads on different nets are closer than clearance_nm. Each of its
     * pads connects through a straight stub fine_neck_nm wide (or the net's width, if narrower) from the pad centre
     * to a free grid cell; that stub keeps fine_clearance_nm to the other SMD pads of the same footprint and the
     * normal clearance to everything else. Nothing else is relaxed. The caller checks both against the fab profile. */
    G2Nm fine_clearance_nm;
    G2Nm fine_neck_nm;
} AutorouteRules;

typedef enum AutorouteNetStatus { AUTOROUTE_NET_ROUTED, AUTOROUTE_NET_FAILED, AUTOROUTE_NET_SINGLE_PAD } AutorouteNetStatus;

typedef struct AutorouteNetResult {
    const char *name;
    uint32_t net;
    size_t pads;
    AutorouteNetStatus status;
    G2Nm width_nm;
    size_t segments, vias;
    const char *message;          /* why it failed, or "" */
} AutorouteNetResult;

typedef struct AutorouteResult {
    AutorouteStatus status;
    char message[200];
    BoardTrack *tracks; size_t track_count;
    BoardVia *vias; size_t via_count;
    AutorouteNetResult *nets; size_t net_count;
    size_t nets_routed, nets_failed;
    uint32_t pass_used;
} AutorouteResult;

/* Route `board` (which must have no tracks or vias yet). The result's arrays are arena-owned. */
AutorouteStatus autoroute_board(Arena *arena, const Board *board, const AutorouteRules *rules, AutorouteResult *out);

/* `pcb_text` with the result's tracks and vias inserted before the board's closing parenthesis. UUIDs are derived
 * from `seed` (deterministic). */
AutorouteStatus autoroute_write_kicad(Arena *arena, const char *pcb_text, size_t pcb_len, const Board *board,
                                      const AutorouteResult *result, const char *seed, const char **out_text,
                                      size_t *out_len);

const char *autoroute_status_name(AutorouteStatus status);
const char *autoroute_net_status_name(AutorouteNetStatus status);

#ifdef __cplusplus
}
#endif

#endif /* COPPER_AUTOROUTE_H */

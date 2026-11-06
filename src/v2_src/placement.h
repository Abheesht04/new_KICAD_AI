#ifndef COPPER_V2_PLACEMENT_H
#define COPPER_V2_PLACEMENT_H

/*
 * placement.h - deterministic component placement in exact integer nanometres (Copper V2, CLAUDE.md section 2).
 *
 * The C port of tools/layout.py's placer (initial_placement, anneal, Board.legal), made stronger:
 *   - legality is exact integer arithmetic on nm rectangles, never less strict than the Python rules it replaces
 *     (outline, pad-to-edge, courtyard gap, silkscreen over silkscreen or pads, board and footprint keepouts);
 *   - the objective is integer: half-perimeter wirelength (nm) plus weighted occupied area, decoupling distance and
 *     keepout-to-edge distance, weights given as data in parts per million;
 *   - annealing is seeded (splitmix64), uses an integer cooling schedule and a fixed-point exp table (no libm), moves
 *     a part by shift, rotation, swap or a pull toward the median of its nets' bounding boxes, and keeps only legal
 *     states; nets touched by a move are re-measured incrementally;
 *   - a greedy refinement at half the grid step polishes the best annealed state.
 * Same problem, seed and iteration count always give the same placement, on any machine.
 *
 * The problem is an S-expression (version 1, lengths in mm with at most 6 decimals, converted exactly to nm):
 *   (placement_problem (version 1)
 *     (board W H) (edge E) (gap G) (step S)
 *     (weights (placement_area_mm2 0.01) (decoupling_mm 1) (keepout_edge_mm 0.5))
 *     (nets "GND" "VIN" ...)
 *     (keepout X1 Y1 X2 Y2 FLAGS) ...                         board keepouts; FLAGS 8 = no bodies, 4 = no pads
 *     (part "R1" [(fixed)] [(at X Y ROT)]
 *       (shape ROT (body X1 Y1 X2 Y2) [(silk X1 Y1 X2 Y2)]
 *              (pad NET CX CY X1 Y1 X2 Y2) ... (keepout X1 Y1 X2 Y2 FLAGS) ...) ...)   one shape per 0/90/180/270
 *     (decoupling "C1" NET "U1") ...)                          NET is an index into (nets ...), -1 for no net
 * Shapes are in the part's own frame at that rotation; a placement adds the part origin (x, y).
 */

#include "arena.h"
#include "g2_types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum PlStatus {
    PL_OK = 0,
    PL_INVALID_INPUT,      /* a NULL argument or an inconsistent problem */
    PL_PARSE_ERROR,        /* the problem text is not a version-1 placement problem; see PlError line/offset */
    PL_OUT_OF_MEMORY,
    PL_RANGE_ERROR,        /* a length outside the exact-arithmetic range (|v| <= 1 m) */
    PL_NO_LEGAL_POSITION,  /* constructive placement found no legal position for at least one part */
    PL_ILLEGAL_START,      /* annealing was given a placement with an unplaced or illegal movable part */
    PL_IO_ERROR
} PlStatus;

const char *pl_status_name(PlStatus status);

typedef struct PlError {
    PlStatus code;
    size_t line;          /* 1-based line in the problem text, 0 when not from parsing */
    size_t offset;        /* byte offset in the problem text, for parse errors */
    size_t item;          /* 1-based index of the top-level form at fault (0 when none), for content errors */
    const char *message;  /* static string */
} PlError;

enum { PL_KEEP_PADS = 4, PL_KEEP_BODIES = 8 };

typedef struct PlRect { G2Nm x1, y1, x2, y2; } PlRect;
typedef struct PlKeepout { PlRect r; uint32_t flags; } PlKeepout;
typedef struct PlPad { PlRect r; G2Nm cx, cy; int32_t net; } PlPad;

typedef struct PlShape {
    PlRect body;
    bool has_silk;
    PlRect silk;
    PlPad *pads;
    size_t pad_count;
    bool has_env;         /* the pads' envelope; false when the part has no pads */
    PlRect env;
    PlKeepout *keepouts;
    size_t keepout_count;
    PlRect extent;        /* union of body, silk, pads and keepouts: two parts whose extents are farther apart than
                             the courtyard gap cannot break any pairwise rule (an exact early-out) */
} PlShape;

typedef struct PlPart {
    const char *ref;
    bool fixed;
    bool has_given;
    G2Nm gx, gy;
    uint8_t grot;         /* rotation index: 0, 1, 2, 3 = 0, 90, 180, 270 degrees */
    PlShape shape[4];
    int32_t *nets;        /* distinct nets of this part's pads, ascending */
    size_t net_count;
} PlPart;

typedef struct PlDecoupling { size_t cap; int32_t net; size_t mcu; } PlDecoupling;

typedef struct PlWeights {
    int64_t area_ppm;          /* cost (mm) per mm^2 of occupied area, x 1e6 */
    int64_t decoupling_ppm;    /* cost (mm) per mm of capacitor-to-MCU supply pad distance, x 1e6 */
    int64_t keepout_edge_ppm;  /* cost (mm) per mm between a footprint keepout and the nearest board edge, x 1e6 */
} PlWeights;

typedef struct PlProblem {
    G2Nm width, height, edge, gap, step;
    PlWeights weights;
    const char **net_names;
    size_t net_count;
    PlKeepout *keepouts;
    size_t keepout_count;
    PlPart *parts;
    size_t part_count;
    PlDecoupling *decoupling;
    size_t decoupling_count;
    size_t **net_parts;        /* per net: the parts with a pad on it, ascending */
    size_t *net_part_count;
} PlProblem;

typedef struct PlPlacement {
    G2Nm *x, *y;
    uint8_t *rot;
    bool *placed;
} PlPlacement;

typedef struct PlStats {
    uint64_t moves, legal_moves, accepted, improved, refine_moves;
    int64_t start_cost, best_cost;
} PlStats;

PlStatus pl_problem_parse(Arena *arena, const char *text, size_t len, PlProblem *out, PlError *error);
PlStatus pl_problem_load(Arena *arena, const char *path, PlProblem *out, PlError *error);

PlStatus pl_placement_alloc(Arena *arena, const PlProblem *problem, PlPlacement *out);
void pl_placement_copy(const PlProblem *problem, PlPlacement *dst, const PlPlacement *src);

/* True when part i, where `placement` puts it, breaks no rule against the board and the other placed parts. */
bool pl_part_legal(const PlProblem *problem, const PlPlacement *placement, size_t i);
/* The objective of a full placement (every part placed), in nm of cost. */
int64_t pl_cost(const PlProblem *problem, const PlPlacement *placement);

/* Constructive start (tools/layout.py initial_placement): fixed parts at their given positions, then parts with
 * keepouts first, the rest by connectivity to what is placed, each at the legal grid position and rotation with the
 * least cost. Unplaceable parts stay unplaced (placement->placed[i] false) and the result is PL_NO_LEGAL_POSITION. */
PlStatus pl_constructive(Arena *arena, const PlProblem *problem, PlPlacement *out);

/* Seeded simulated annealing from a complete legal start, then refinement. `iterations` temperature steps, each with
 * one move per movable part. `out` receives the best legal placement seen; stats may be NULL. */
PlStatus pl_anneal(Arena *arena, const PlProblem *problem, const PlPlacement *start, uint64_t seed,
                   uint32_t iterations, PlPlacement *out, PlStats *stats);

/* exp(-q / 256) in Q32 for q >= 0 (0 beyond q = 17 * 256): the annealing acceptance table. Exposed for tests. */
uint64_t pl_exp_neg_q32(uint64_t q);

#ifdef __cplusplus
}
#endif

#endif

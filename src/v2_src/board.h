#ifndef COPPER_V2_BOARD_H
#define COPPER_V2_BOARD_H

/*
 * board.h - the KiCad board model (Copper V2 track B), read through IR in exact integer nanometres.
 *
 * board_load reads a .kicad_pcb (KiCad 9 format 20241229; later formats read the same way) into:
 *   - nets (from the board's net table, and by name when an item names a net the table lacks);
 *   - footprints with their reference, value, position, rotation and side;
 *   - pads at absolute board positions: exact for footprint rotations that are multiples of 90 degrees, otherwise
 *     rounded to the nearest nm from a 62-bit fixed-point sine (error < 1 nm) and flagged `exact = false`;
 *   - tracks (segments and arcs), vias, zones (outline vertices; keepouts flagged);
 *   - Edge.Cuts from gr_line / gr_arc / gr_rect / gr_circle / gr_poly and footprint fp_* items on Edge.Cuts, chained
 *     into loops by exact endpoint equality, with the outline's bounds (an arc's extremes from its circle, computed
 *     with 128-bit integers and widened by 1 nm: conservative, never smaller than the true outline).
 * Coordinates are KiCad's: x right, y down, mm in the file, nm here. Angles are micro-degrees (exact decimals).
 * Things read but not modelled exactly are listed as BoardIssue entries, never silently dropped.
 *
 * Memory: everything lives in the caller's Arena. Errors: BoardStatus with line/column/offset for parse errors
 * and the 1-based top-level item plus a context string for content errors. No printf, no exit, no floating point.
 */

#include "arena.h"
#include "g2_types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum BoardStatus {
    BOARD_OK = 0,
    BOARD_INVALID_ARGUMENT,
    BOARD_IO_ERROR,
    BOARD_PARSE_ERROR,        /* not a well-formed S-expression; line/column/offset are set */
    BOARD_NOT_A_BOARD,        /* well-formed, but not (kicad_pcb ...) */
    BOARD_INVALID_CONTENT,    /* a board item is malformed; item and context say which */
    BOARD_RANGE_ERROR,        /* a coordinate beyond +-1 m or a number out of range */
    BOARD_OUT_OF_MEMORY
} BoardStatus;

const char *board_status_name(BoardStatus status);

typedef struct BoardError {
    BoardStatus code;
    size_t line, column, offset;  /* parse errors */
    size_t item;                  /* 1-based top-level form of (kicad_pcb ...) at fault; 0 when none */
    const char *context;          /* e.g. "footprint R3 pad 2: size"; arena or static string, may be NULL */
    const char *message;          /* static string */
} BoardError;

/* Layer bits: copper F.Cu = bit 0, In1..In30.Cu = bits 1..30, B.Cu = bit 31; technical layers from bit 32. */
#define BOARD_LAYER_F_CU      (UINT64_C(1) << 0)
#define BOARD_LAYER_IN_CU(n)  (UINT64_C(1) << (n))      /* n = 1..30 */
#define BOARD_LAYER_B_CU      (UINT64_C(1) << 31)
#define BOARD_LAYERS_CU       UINT64_C(0xFFFFFFFF)
#define BOARD_LAYER_F_MASK    (UINT64_C(1) << 32)
#define BOARD_LAYER_B_MASK    (UINT64_C(1) << 33)
#define BOARD_LAYER_F_PASTE   (UINT64_C(1) << 34)
#define BOARD_LAYER_B_PASTE   (UINT64_C(1) << 35)
#define BOARD_LAYER_F_SILK    (UINT64_C(1) << 36)
#define BOARD_LAYER_B_SILK    (UINT64_C(1) << 37)
#define BOARD_LAYER_EDGE_CUTS (UINT64_C(1) << 38)
#define BOARD_LAYER_F_CRTYD   (UINT64_C(1) << 39)
#define BOARD_LAYER_B_CRTYD   (UINT64_C(1) << 40)
#define BOARD_LAYER_F_FAB     (UINT64_C(1) << 41)
#define BOARD_LAYER_B_FAB     (UINT64_C(1) << 42)
#define BOARD_LAYER_OTHER     (UINT64_C(1) << 63)      /* any layer this model does not name */

/* The bits for one KiCad layer name, including wildcards ("*.Cu", "F&B.Cu", "*.Mask"); 0 for an empty name. */
uint64_t board_layer_bits(const char *name);

typedef struct BoardNet {
    int32_t number;     /* KiCad's net number, or -1 when the net was only named */
    const char *name;   /* "" for KiCad's unconnected net 0 */
} BoardNet;

typedef enum BoardPadType { BOARD_PAD_SMD = 0, BOARD_PAD_THRU_HOLE, BOARD_PAD_NP_THRU_HOLE, BOARD_PAD_CONNECT } BoardPadType;

typedef enum BoardPadShape {
    BOARD_SHAPE_RECT = 0, BOARD_SHAPE_ROUNDRECT, BOARD_SHAPE_CIRCLE, BOARD_SHAPE_OVAL, BOARD_SHAPE_TRAPEZOID,
    BOARD_SHAPE_CUSTOM, BOARD_SHAPE_OTHER
} BoardPadShape;

typedef struct BoardPad {
    const char *number;          /* "" for an unnumbered pad */
    size_t footprint;            /* index into Board.footprints */
    BoardPadType type;
    BoardPadShape shape;
    G2Point local;               /* footprint frame, as in the file */
    G2Point at;                  /* board position */
    bool exact;                  /* false: the footprint rotation is not a multiple of 90 degrees (rounded, < 1 nm) */
    int64_t angle_udeg;          /* the pad's absolute orientation, as in the file */
    G2Nm size_x, size_y;
    bool has_drill, drill_oval;
    G2Nm drill_x, drill_y;       /* drill_y == drill_x for a round drill */
    int64_t roundrect_rratio_ppm;
    uint64_t layers;
    int32_t net;                 /* index into Board.nets; -1 for no net */
} BoardPad;

typedef struct BoardFootprint {
    const char *lib_id, *reference, *value;
    G2Point at;
    int64_t angle_udeg;
    bool back;                   /* on B.Cu (KiCad stores back-side pads already mirrored) */
    size_t first_pad, pad_count; /* Board.pads[first_pad .. first_pad + pad_count) */
} BoardFootprint;

typedef struct BoardTrack {
    bool is_arc;
    G2Point start, mid, end;     /* mid only for arcs */
    G2Nm width;
    uint64_t layer;
    int32_t net;
} BoardTrack;

typedef enum BoardViaType { BOARD_VIA_THROUGH = 0, BOARD_VIA_BLIND, BOARD_VIA_MICRO } BoardViaType;

typedef struct BoardVia {
    G2Point at;
    G2Nm size, drill;
    BoardViaType type;
    uint64_t layers;
    int32_t net;
} BoardVia;

typedef struct BoardZone {
    int32_t net;
    uint64_t layers;
    bool keepout;
    bool in_footprint;
    const G2Point *outline;      /* the first (outline) polygon's vertices, board coordinates */
    size_t outline_count;
    size_t filled_polygon_count;
} BoardZone;

typedef enum BoardEdgeKind { BOARD_EDGE_LINE = 0, BOARD_EDGE_ARC, BOARD_EDGE_CIRCLE } BoardEdgeKind;

typedef struct BoardEdge {
    BoardEdgeKind kind;
    G2Point start, mid, end;     /* line: start/end; arc: start/mid/end; circle: start = centre, end = a point on it */
    bool from_footprint;
    bool exact;                  /* false when it came from a footprint at a non-90-degree rotation */
    size_t loop;                 /* index of the loop it belongs to (SIZE_MAX for an unchained edge) */
} BoardEdge;

typedef struct BoardOutline {
    size_t loop_count;           /* chained loops, closed or not; a circle is a loop by itself */
    size_t open_loops;           /* loops whose ends do not meet */
    bool closed;                 /* at least one edge, and every loop closed with exactly equal end points */
    size_t dangling_ends;        /* end points not shared by two edges */
    G2Nm max_gap;                /* dangling ends paired nearest-first: the widest pair (Euclidean, rounded up);
                                    0 when closed, -1 when the ends cannot be paired (an odd count) */
    bool has_bounds;
    G2Aabb bounds;               /* union of all Edge.Cuts items (arcs and circles conservative, +1 nm) */
} BoardOutline;

typedef enum BoardIssueCode {
    BOARD_ISSUE_INEXACT_ROTATION = 0,  /* footprint rotation not a multiple of 90: positions rounded (< 1 nm) */
    BOARD_ISSUE_ZONE_ARC,              /* an arc inside a zone outline: only its end points are kept */
    BOARD_ISSUE_EDGE_UNSUPPORTED,      /* an Edge.Cuts item that is not a line, arc, rect, circle or polygon */
    BOARD_ISSUE_ARC_DEGENERATE,        /* an arc whose three points are collinear: treated as a line */
    BOARD_ISSUE_OUTLINE_OPEN,          /* Edge.Cuts does not close */
    BOARD_ISSUE_NO_OUTLINE,
    BOARD_ISSUE_ARC_BOUNDS_UNAVAILABLE /* no 128-bit integers on this compiler: arc bounds use the end points only */
} BoardIssueCode;

const char *board_issue_name(BoardIssueCode code);

typedef struct BoardIssue {
    BoardIssueCode code;
    size_t item;                 /* 1-based top-level form */
    const char *context;
} BoardIssue;

typedef struct Board {
    int64_t version;
    const char *generator, *generator_version;
    G2Nm thickness;              /* (general (thickness ...)); 0 when absent */
    size_t copper_layers;        /* signal/power layers declared in (layers ...) */
    BoardNet *nets;            size_t net_count;
    BoardFootprint *footprints; size_t footprint_count;
    BoardPad *pads;            size_t pad_count;
    BoardTrack *tracks;        size_t track_count;
    BoardVia *vias;            size_t via_count;
    BoardZone *zones;          size_t zone_count;
    BoardEdge *edges;          size_t edge_count;
    BoardOutline outline;
    BoardIssue *issues;        size_t issue_count;
} Board;

BoardStatus board_load(Arena *arena, const char *path, Board *out, BoardError *error);
BoardStatus board_parse(Arena *arena, const char *text, size_t len, Board *out, BoardError *error);

/* True when Edge.Cuts closes once ends at most `tolerance` nm apart are joined. KiCad joins outline ends within a
 * small chaining tolerance; this model keeps exact closure in outline.closed and takes the tolerance as data from
 * the caller (a fab or rules profile), never as a built-in constant. */
bool board_outline_closed_within(const Board *board, G2Nm tolerance);

/* Lookups (linear; NULL / SIZE_MAX / -1 when absent). */
const BoardFootprint *board_find_footprint(const Board *board, const char *reference);
const BoardPad *board_find_pad(const Board *board, const char *reference, const char *number);
int32_t board_find_net(const Board *board, const char *name);
/* Pads on net `net`, in board order: writes up to `capacity` pad indices and returns the total count. */
size_t board_pads_on_net(const Board *board, int32_t net, size_t *out, size_t capacity);

/* A box containing the arc start -> mid -> end (its circle's extremes where the arc sweeps them, +1 nm): never
 * smaller than the arc. Collinear points give the points' box. BOARD_RANGE_ERROR without 128-bit integers. */
BoardStatus board_arc_bounds(G2Point start, G2Point mid, G2Point end, G2Aabb *out);

/* Rotate (x, y) by `udeg` micro-degrees the way KiCad turns a footprint (y down): x' = x cos + y sin,
 * y' = -x sin + y cos. Exact for multiples of 90 degrees (*exact = true); otherwise rounded to the nearest nm.
 * Returns BOARD_RANGE_ERROR if the result leaves int64 or the platform cannot rotate inexactly. */
BoardStatus board_rotate(G2Nm x, G2Nm y, int64_t udeg, G2Nm *ox, G2Nm *oy, bool *exact);

#ifdef __cplusplus
}
#endif

#endif

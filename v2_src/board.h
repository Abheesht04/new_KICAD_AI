/*
 * board.h - loss-aware, exact-unit in-memory PCB model.
 *
 * Coordinates and dimensions are integer nanometres.  Angles are integer
 * millidegrees.  Standard KiCad layers use stable canonical IDs (0..58);
 * a uint64_t layer set can therefore represent every KiCad board layer.
 *
 * The original KiCad IR tree is retained for lossless round-tripping and for
 * features not yet projected into the typed model.  A typed-model DFM PASS is
 * forbidden whenever an unmodelled manufacturing-relevant feature remains.
 */
#ifndef COPPER_BOARD_H
#define COPPER_BOARD_H

#include "IR.h"
#include "arena.h"
#include "g2_types.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  BOARD_OK = 0,
  BOARD_ERR_ARGUMENT = -1,
  BOARD_ERR_IO = -2,
  BOARD_ERR_PARSE = -3,
  BOARD_ERR_UNSUPPORTED = -4,
  BOARD_ERR_INVALID = -5,
  BOARD_ERR_MEMORY = -6,
  BOARD_ERR_RANGE = -7,
  BOARD_ERR_REFERENCE = -8
} BoardStatus;

/* KiCad canonical layer IDs. InN.Cu are contiguous for fast lookup. */
typedef enum {
  BOARD_LAYER_F_CU = 0,
  BOARD_LAYER_IN1_CU, BOARD_LAYER_IN2_CU, BOARD_LAYER_IN3_CU,
  BOARD_LAYER_IN4_CU, BOARD_LAYER_IN5_CU, BOARD_LAYER_IN6_CU,
  BOARD_LAYER_IN7_CU, BOARD_LAYER_IN8_CU, BOARD_LAYER_IN9_CU,
  BOARD_LAYER_IN10_CU, BOARD_LAYER_IN11_CU, BOARD_LAYER_IN12_CU,
  BOARD_LAYER_IN13_CU, BOARD_LAYER_IN14_CU, BOARD_LAYER_IN15_CU,
  BOARD_LAYER_IN16_CU, BOARD_LAYER_IN17_CU, BOARD_LAYER_IN18_CU,
  BOARD_LAYER_IN19_CU, BOARD_LAYER_IN20_CU, BOARD_LAYER_IN21_CU,
  BOARD_LAYER_IN22_CU, BOARD_LAYER_IN23_CU, BOARD_LAYER_IN24_CU,
  BOARD_LAYER_IN25_CU, BOARD_LAYER_IN26_CU, BOARD_LAYER_IN27_CU,
  BOARD_LAYER_IN28_CU, BOARD_LAYER_IN29_CU, BOARD_LAYER_IN30_CU,
  BOARD_LAYER_B_CU,
  BOARD_LAYER_B_ADHES, BOARD_LAYER_F_ADHES,
  BOARD_LAYER_B_PASTE, BOARD_LAYER_F_PASTE,
  BOARD_LAYER_B_SILK, BOARD_LAYER_F_SILK,
  BOARD_LAYER_B_MASK, BOARD_LAYER_F_MASK,
  BOARD_LAYER_DWGS_USER, BOARD_LAYER_CMTS_USER,
  BOARD_LAYER_ECO1_USER, BOARD_LAYER_ECO2_USER,
  BOARD_LAYER_EDGE_CUTS, BOARD_LAYER_MARGIN,
  BOARD_LAYER_B_CRTYD, BOARD_LAYER_F_CRTYD,
  BOARD_LAYER_B_FAB, BOARD_LAYER_F_FAB,
  BOARD_LAYER_USER1, BOARD_LAYER_USER2, BOARD_LAYER_USER3,
  BOARD_LAYER_USER4, BOARD_LAYER_USER5, BOARD_LAYER_USER6,
  BOARD_LAYER_USER7, BOARD_LAYER_USER8, BOARD_LAYER_USER9,
  BOARD_LAYER_COUNT
} BoardLayer;

typedef uint64_t BoardLayerMask;
#define BOARD_LAYER_BIT(l) (UINT64_C(1) << (unsigned)(l))
#define BOARD_NO_INDEX UINT32_MAX
#define BOARD_UNSET_NM INT64_MIN

const char *board_layer_name(BoardLayer layer);
BoardStatus board_layer_from_name(const char *name, BoardLayer *out);
bool board_layer_is_copper(BoardLayer layer);

typedef enum { BOARD_LAYER_TYPE_SIGNAL, BOARD_LAYER_TYPE_POWER,
               BOARD_LAYER_TYPE_MIXED, BOARD_LAYER_TYPE_USER,
               BOARD_LAYER_TYPE_JUMPER, BOARD_LAYER_TYPE_UNKNOWN } BoardLayerType;
typedef struct {
  uint32_t ordinal;             /* native KiCad layer ordinal */
  BoardLayer id;                /* canonical identity */
  BoardLayerType type;
  const char *user_name;        /* optional display override */
} BoardLayerInfo;

typedef struct { uint32_t id; const char *name; } BoardNet;

typedef enum { BOARD_PAD_SMD, BOARD_PAD_THRU, BOARD_PAD_NPTH,
               BOARD_PAD_CONNECT } BoardPadType;
typedef enum { BOARD_PAD_CIRCLE, BOARD_PAD_RECT, BOARD_PAD_OVAL,
               BOARD_PAD_ROUNDRECT, BOARD_PAD_TRAPEZOID,
               BOARD_PAD_CUSTOM } BoardPadShape;

typedef struct {
  uint32_t footprint;
  const char *number;
  BoardPadType type;
  BoardPadShape shape;
  G2Point pos;                  /* absolute board position */
  int32_t rot_mdeg;             /* absolute orientation */
  G2Point size;
  G2Nm roundrect_ratio_ppm;
  G2Point drill;                /* x/y; 0,0 means no drill */
  G2Point drill_offset;         /* absolute board offset from pad centre */
  BoardLayerMask layers;
  uint32_t net;
  G2Nm clearance;
  G2Nm mask_margin;
  G2Nm paste_margin;
  bool rotation_exact;
  bool locked;
  const IRNode *raw_node;       /* custom/chamfer primitives and future fields */
} BoardPad;

typedef struct {
  const char *reference;
  const char *value;
  const char *lib_id;
  const char *part_id;       /* optional Copper catalogue identity property */
  const char *manufacturer;  /* optional assembly metadata property */
  const char *mpn;           /* optional exact orderable manufacturer part number */
  const char *lcsc_id;       /* optional JLCPCB/LCSC identity property */
  const char *uuid;
  const char *path;
  G2Point pos;
  int32_t rot_mdeg;
  BoardLayer layer;
  bool on_back;
  bool rotation_exact;
  bool locked;
  bool placed;
  bool dnp;
  bool exclude_from_bom;
  bool exclude_from_pos;
  bool courtyard_exact;       /* false when the footprint courtyard has unresolved arcs/circles */
  uint32_t first_pad, pad_count;
  uint32_t first_court, court_count;
  const IRNode *raw_node;
} BoardFootprint;

typedef struct {
  G2Point a, b;
  G2Nm width;
  BoardLayer layer;
  uint32_t net;
  bool locked;
  const char *uuid;
} BoardTrack;
typedef struct {
  G2Point start, mid, end;
  G2Nm width;
  BoardLayer layer;
  uint32_t net;
  bool locked;
  const char *uuid;
} BoardArcTrack;
typedef enum { BOARD_VIA_THROUGH, BOARD_VIA_BLIND_BURIED,
               BOARD_VIA_MICRO, BOARD_VIA_UNKNOWN } BoardViaType;
typedef struct {
  G2Point pos;
  G2Nm size, drill;
  BoardLayer from, to;
  uint32_t net;
  bool plated;
  BoardViaType type;
  bool locked;
  const char *uuid;
} BoardVia;

typedef struct {
  uint32_t net;
  BoardLayer layer;             /* first/native primary layer */
  BoardLayerMask layers;        /* full layer set if available */
  uint32_t first_pt, pt_count;
  G2Nm clearance, min_thickness;
  bool is_keepout;
  bool no_tracks, no_vias, no_pads, no_copper, no_footprints;
  bool locked;
  const IRNode *raw_node;       /* fill settings, thermal, priority, etc. */
} BoardZone;

typedef enum { BOARD_EDGE_LINE, BOARD_EDGE_ARC, BOARD_EDGE_CIRCLE,
               BOARD_EDGE_RECT, BOARD_EDGE_POLYGON } BoardEdgeKind;
typedef struct {
  BoardEdgeKind kind;
  G2Point p0, p1, mid;
  G2Nm width;
  uint32_t first_pt, pt_count;   /* polygon points in Board.edge_pts */
  const IRNode *raw_node;
} BoardEdge;

typedef struct {
  uint32_t copper_layers;
  G2Nm board_thickness;
  G2Nm min_clearance, min_track_width, min_via_size, min_via_drill,
       min_hole_to_hole, min_annular, min_through_hole, edge_clearance,
       mask_margin, paste_margin;
} BoardRules;

typedef struct {
  Arena *arena;
  BoardRules rules;
  BoardLayerInfo *layers; size_t layer_count;
  BoardNet *nets; size_t net_count;
  BoardFootprint *footprints; size_t footprint_count;
  BoardPad *pads; size_t pad_count;
  BoardTrack *tracks; size_t track_count;
  BoardArcTrack *arc_tracks; size_t arc_track_count;
  BoardVia *vias; size_t via_count;
  BoardZone *zones; size_t zone_count;
  BoardEdge *edges; size_t edge_count;
  G2Point *court_pts; size_t court_pt_count;
  G2Point *zone_pts; size_t zone_pt_count;
  G2Point *edge_pts; size_t edge_pt_count;
  const IRNode *source_root;    /* complete source tree; remains arena-owned */
  uint32_t kicad_file_version;
} Board;

typedef struct {
  BoardStatus status;
  char message[160];
  size_t unmodelled_records;
  size_t unmodelled_geometry_records;
  size_t inexact_rotations;
  size_t unsupported_layers;
  char first_unmodelled[48];
} BoardDiag;

BoardStatus board_init(Board *b, Arena *arena);
BoardStatus board_load_kicad(const char *path, Arena *arena, Board *out,
                             BoardDiag *diag);
BoardStatus board_validate(const Board *b, BoardDiag *diag);
const BoardNet *board_net_by_id(const Board *b, uint32_t id);
/* Deterministic integer rotation. Cardinal rotations are exact; all other
 * angles use Q30 CORDIC and report exact=false. */
BoardStatus board_rotate(G2Point local, int32_t rot_mdeg, G2Point *out,
                         bool *exact);
BoardStatus board_pad_aabb(const BoardPad *p, G2Aabb *out);
BoardStatus board_bounds(const Board *b, G2Aabb *out);
BoardStatus board_hole_count(const Board *b, size_t *plated, size_t *unplated);

#ifdef __cplusplus
}
#endif
#endif

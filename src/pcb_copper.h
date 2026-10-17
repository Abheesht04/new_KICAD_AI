#ifndef PCB_COPPER_H
#define PCB_COPPER_H
/*
 * pcb_copper.h - adapter between the physical PCB IR (PcbBoard, double mm,
 * KiCad layer IDs) and the integer copper kernel (copper.h).
 *
 * This is the ONE place where:
 *   - mm becomes integer nm        (positions: nearest; sizes: round UP)
 *   - KiCad layer IDs become stackup bit masks
 *   - copper becomes OUTWARD rounded (radius = ceil(diameter / 2))
 *
 * Everything after this boundary is exact integer arithmetic.
 */
#include <stddef.h>
#include <stdint.h>

#include "copper.h"
#include "pcb.h"

typedef enum {
    PCB_COPPER_OK = 0,
    PCB_COPPER_SKIP = 1,            /* valid object that is not copper (NPTH, unplated via) */
    PCB_COPPER_ERR_ARGUMENT = -1,
    PCB_COPPER_ERR_GEOMETRY = -2,   /* non-finite, out of range, or non-positive size */
    PCB_COPPER_ERR_MEMORY = -3
} PcbCopperStatus;

/* Position: nearest nm, ties away from zero, must be within +-500 mm. */
PcbCopperStatus pcb_copper_mm_to_nm_coordinate(double mm, Nm *out_nm);
/* Size or requirement: rounded UP to nm, must be 0 <= mm <= 501. */
PcbCopperStatus pcb_copper_mm_to_nm_ceil(double mm, Nm *out_nm);

/* KiCad layer ID (F.Cu 0, B.Cu 2, In1.Cu 4, ...) -> one stackup bit. */
PcbCopperStatus pcb_copper_mask_from_kicad_layer(uint8_t kicad_layer_id,
                                                 uint8_t board_layer_count,
                                                 CopperLayerMask *out_mask);

/* Build one CopperItem from a board object. kind is TRACE, PAD or VIA. */
PcbCopperStatus pcb_copper_item(const PcbBoard *board, CopperItemKind kind,
                                size_t object_index, CopperItem *out_item);

typedef struct {
    CopperItem a;
    CopperItem b;
    CopperResult result;          /* COPPER_RESULT_TOO_CLOSE or COPPER_RESULT_SHORT */
    double gap_mm;                /* signed, report only */
    Nm required_nm;
} PcbCopperFinding;

typedef struct {
    size_t objects_seen;
    size_t active_copper_objects;
    size_t pairs_considered;      /* pairs that reached the exact test */
    size_t filtered_by_layer;
    size_t filtered_same_net;
    size_t invalid_objects;
    size_t unsupported_objects;   /* > 0 means the result is UNVERIFIED, not PASS */
    size_t violations;
} PcbCopperStats;

/* Return 0 to continue, nonzero to abort. */
typedef int (*PcbCopperVisitor)(const PcbCopperFinding *finding, void *user);

/*
 * Check every pair of copper objects against one clearance rule.
 * Findings are visited in ascending (i, j) object order, so output is
 * reproducible. Returns 0 when the scan completed (violations are not errors),
 * < 0 on bad input, allocation failure or visitor abort. With `stats` set,
 * invalid objects are counted and skipped; without it they are an error.
 */
int pcb_copper_check_clearance(const PcbBoard *board, Nm required_nm,
                               PcbCopperVisitor visitor, void *user,
                               PcbCopperStats *stats);

#endif

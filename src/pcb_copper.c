#include "pcb_copper.h"

#include <stdlib.h>
#include <string.h>

#include "nmath.h"
#include "vec.h"

/* ------------------------------------------------------------------------ *
 * Unit conversion (no libm: nm_from_mm / nm_from_mm_ceil live in geom.c)
 * ------------------------------------------------------------------------ */

PcbCopperStatus pcb_copper_mm_to_nm_coordinate(double mm, Nm *out_nm) {
  if (!out_nm) return PCB_COPPER_ERR_ARGUMENT;
  /* NaN fails both comparisons; +-inf fail the range. */
  if (!(mm >= -500.0 && mm <= 500.0)) return PCB_COPPER_ERR_GEOMETRY;
  *out_nm = nm_from_mm(mm);
  return pt_in_bounds((Pt){*out_nm, 0}) ? PCB_COPPER_OK
                                        : PCB_COPPER_ERR_GEOMETRY;
}

PcbCopperStatus pcb_copper_mm_to_nm_ceil(double mm, Nm *out_nm) {
  if (!out_nm) return PCB_COPPER_ERR_ARGUMENT;
  if (!(mm >= 0.0 && mm <= 501.0)) return PCB_COPPER_ERR_GEOMETRY;
  *out_nm = nm_from_mm_ceil(mm);
  return PCB_COPPER_OK;
}

static PcbCopperStatus mm_point(double x_mm, double y_mm, Pt *out) {
  PcbCopperStatus s = pcb_copper_mm_to_nm_coordinate(x_mm, &out->x);
  if (s != PCB_COPPER_OK) return s;
  return pcb_copper_mm_to_nm_coordinate(y_mm, &out->y);
}

/* A copper diameter/width in mm -> outward-rounded RADIUS in nm, > 0. */
static PcbCopperStatus radius_from_size_mm(double size_mm, Nm *out_radius) {
  if (!(size_mm > 0.0)) return PCB_COPPER_ERR_GEOMETRY; /* also rejects NaN */
  Nm size_nm;
  PcbCopperStatus s = pcb_copper_mm_to_nm_ceil(size_mm, &size_nm);
  if (s != PCB_COPPER_OK || size_nm <= 0) return PCB_COPPER_ERR_GEOMETRY;
  *out_radius = (size_nm + 1) / 2; /* ceil(size / 2): copper grows outward */
  return PCB_COPPER_OK;
}

/* ------------------------------------------------------------------------ *
 * Layers
 * ------------------------------------------------------------------------ */

PcbCopperStatus pcb_copper_mask_from_kicad_layer(uint8_t id, uint8_t count,
                                                 CopperLayerMask *out_mask) {
  if (!out_mask || count == 0 || count > 32) return PCB_COPPER_ERR_ARGUMENT;

  unsigned index;
  if (id == PCB_LAYER_F_CU) {
    index = 0;
  } else if (id == PCB_LAYER_B_CU) {
    if (count < 2) return PCB_COPPER_ERR_GEOMETRY;
    index = (unsigned)count - 1u;
  } else if (id >= PCB_LAYER_IN1_CU && id <= PCB_LAYER_IN30_CU && (id % 2u) == 0u) {
    index = (unsigned)id / 2u - 1u;
    if (index >= (unsigned)count - 1u) return PCB_COPPER_ERR_GEOMETRY;
  } else {
    return PCB_COPPER_ERR_GEOMETRY;
  }
  return copper_layer_mask_single(index, out_mask) ? PCB_COPPER_OK
                                                   : PCB_COPPER_ERR_GEOMETRY;
}

static unsigned lowest_bit(CopperLayerMask m) {
  unsigned i = 0;
  while (i < COPPER_MAX_LAYERS && (m & (UINT64_C(1) << i)) == 0) ++i;
  return i;
}

static PcbCopperStatus span_from_kicad_layers(uint8_t first_id, uint8_t last_id,
                                              uint8_t count,
                                              CopperLayerMask *out) {
  CopperLayerMask m1, m2;
  if (pcb_copper_mask_from_kicad_layer(first_id, count, &m1) != PCB_COPPER_OK ||
      pcb_copper_mask_from_kicad_layer(last_id, count, &m2) != PCB_COPPER_OK) {
    return PCB_COPPER_ERR_GEOMETRY;
  }
  return copper_layer_mask_span(lowest_bit(m1), lowest_bit(m2), out)
             ? PCB_COPPER_OK
             : PCB_COPPER_ERR_GEOMETRY;
}

/* ------------------------------------------------------------------------ *
 * Board object -> CopperItem
 * ------------------------------------------------------------------------ */

PcbCopperStatus pcb_copper_item(const PcbBoard *board, CopperItemKind kind,
                                size_t index, CopperItem *out) {
  if (!board || !out || board->spec.layer_count == 0 ||
      board->spec.layer_count > 32) {
    return PCB_COPPER_ERR_ARGUMENT;
  }
  memset(out, 0, sizeof(*out));
  out->kind = kind;
  out->source_index = (uint32_t)index;
  const uint8_t layers = board->spec.layer_count;
  PcbCopperStatus s;

  switch (kind) {
  case COPPER_ITEM_TRACE: {
    if (index >= vec_len(board->traces)) return PCB_COPPER_ERR_ARGUMENT;
    const PcbTrace *t = &board->traces[index];
    Pt p0, p1;
    Nm radius;
    if ((s = mm_point(t->x1_mm, t->y1_mm, &p0)) != PCB_COPPER_OK) return s;
    if ((s = mm_point(t->x2_mm, t->y2_mm, &p1)) != PCB_COPPER_OK) return s;
    if ((s = radius_from_size_mm(t->width_mm, &radius)) != PCB_COPPER_OK) return s;
    if ((s = pcb_copper_mask_from_kicad_layer(t->layer, layers, &out->layers)) !=
        PCB_COPPER_OK) {
      return s;
    }
    out->shape = copper_shape_segment(p0, p1, radius);
    out->net = (CopperNet)t->net_id;
    return PCB_COPPER_OK;
  }

  case COPPER_ITEM_PAD: {
    if (index >= vec_len(board->pads)) return PCB_COPPER_ERR_ARGUMENT;
    const PcbPad *p = &board->pads[index];
    /* A drilled, non-plated item is a mechanical hole, not copper. */
    if (p->drill_mm > 0.0 && !p->plated) return PCB_COPPER_SKIP;
    Pt c;
    Nm radius;
    if ((s = mm_point(p->x_mm, p->y_mm, &c)) != PCB_COPPER_OK) return s;
    if ((s = radius_from_size_mm(p->pad_diameter_mm, &radius)) != PCB_COPPER_OK) return s;
    if (p->plated && p->drill_mm > 0.0) {
      if (!copper_layer_mask_span(0, (unsigned)layers - 1u, &out->layers)) {
        return PCB_COPPER_ERR_GEOMETRY;
      }
    } else if ((s = pcb_copper_mask_from_kicad_layer(p->layer, layers, &out->layers)) !=
               PCB_COPPER_OK) {
      return s;
    }
    if (p->rectangular) {
      Nm w, h;
      if (!(p->width_mm > 0 && p->height_mm > 0) ||
          pcb_copper_mm_to_nm_ceil(p->width_mm, &w) != PCB_COPPER_OK ||
          pcb_copper_mm_to_nm_ceil(p->height_mm, &h) != PCB_COPPER_OK)
        return PCB_COPPER_ERR_GEOMETRY;
      w=(w+1)/2; h=(h+1)/2;
      Nm cr;
      if (pcb_copper_mm_to_nm_ceil(p->corner_radius_mm, &cr) != PCB_COPPER_OK || cr>w || cr>h)
        return PCB_COPPER_ERR_GEOMETRY;
      out->shape=copper_shape_rect((Pt){c.x-w,c.y-h},(Pt){c.x+w,c.y+h},cr);
    } else out->shape = copper_shape_point(c, radius);
    out->net = (CopperNet)p->net_id;
    return PCB_COPPER_OK;
  }

  case COPPER_ITEM_VIA: {
    if (index >= vec_len(board->vias)) return PCB_COPPER_ERR_ARGUMENT;
    const PcbVia *v = &board->vias[index];
    if (!v->plated) return PCB_COPPER_SKIP;
    Pt c;
    Nm radius;
    if ((s = mm_point(v->x_mm, v->y_mm, &c)) != PCB_COPPER_OK) return s;
    if ((s = radius_from_size_mm(v->diameter_mm, &radius)) != PCB_COPPER_OK) return s;
    if ((s = span_from_kicad_layers(v->layer_start, v->layer_end, layers,
                                    &out->layers)) != PCB_COPPER_OK) {
      return s;
    }
    out->shape = copper_shape_point(c, radius);
    out->net = (CopperNet)v->net_id;
    return PCB_COPPER_OK;
  }

  default:
    return PCB_COPPER_ERR_ARGUMENT;
  }
}

/* ------------------------------------------------------------------------ *
 * Board scan
 * ------------------------------------------------------------------------ */

int pcb_copper_check_clearance(const PcbBoard *board, Nm required_nm,
                               PcbCopperVisitor visitor, void *user,
                               PcbCopperStats *stats) {
  if (stats) memset(stats, 0, sizeof(*stats));
  if (!board || board->spec.layer_count == 0 || board->spec.layer_count > 32 ||
      required_nm < 0) {
    return -1;
  }

  const size_t n_tr = vec_len(board->traces);
  const size_t n_pad = vec_len(board->pads);
  const size_t n_via = vec_len(board->vias);
  const size_t count = n_tr + n_pad + n_via;
  if (stats) stats->objects_seen = count;
  if (count == 0) return 0;
  if (count > SIZE_MAX / sizeof(CopperItem)) return -1;

  CopperItem *items = (CopperItem *)calloc(count, sizeof(*items));
  Pt *lo = (Pt *)calloc(count, sizeof(*lo));
  Pt *hi = (Pt *)calloc(count, sizeof(*hi));
  unsigned char *active = (unsigned char *)calloc(count, 1);
  if (!items || !lo || !hi || !active) {
    free(items); free(lo); free(hi); free(active);
    return -1;
  }

  int result = 0;

  for (size_t i = 0; i < count; ++i) {
    CopperItemKind kind;
    size_t idx;
    if (i < n_tr) { kind = COPPER_ITEM_TRACE; idx = i; }
    else if (i < n_tr + n_pad) { kind = COPPER_ITEM_PAD; idx = i - n_tr; }
    else { kind = COPPER_ITEM_VIA; idx = i - n_tr - n_pad; }

    PcbCopperStatus s = pcb_copper_item(board, kind, idx, &items[i]);
    if (s == PCB_COPPER_SKIP) continue;
    if (s != PCB_COPPER_OK) {
      if (!stats) { result = -1; goto cleanup; }
      stats->invalid_objects++;
      continue;
    }
    if (!copper_shape_bbox(&items[i].shape, &lo[i], &hi[i])) {
      if (!stats) { result = -1; goto cleanup; }
      stats->unsupported_objects++;
      continue;
    }
    active[i] = 1;
    if (stats) stats->active_copper_objects++;
  }

  for (size_t i = 0; i < count; ++i) {
    if (!active[i]) continue;
    for (size_t j = i + 1; j < count; ++j) {
      if (!active[j]) continue;

      if (!copper_layer_masks_overlap(items[i].layers, items[j].layers)) {
        if (stats) stats->filtered_by_layer++;
        continue;
      }
      if (items[i].net == items[j].net && items[i].net != COPPER_NET_NONE) {
        if (stats) stats->filtered_same_net++;
        continue;
      }

      /* Broad phase: bounding boxes already include the radii, so a separation
       * strictly greater than the requirement on any axis means PASS (and
       * cannot be touching). Cheap integer rejects. */
      if (lo[i].x - hi[j].x > required_nm || lo[j].x - hi[i].x > required_nm ||
          lo[i].y - hi[j].y > required_nm || lo[j].y - hi[i].y > required_nm) {
        continue;
      }

      if (stats) stats->pairs_considered++;
      const CopperResult r = copper_pair_check(&items[i], &items[j], required_nm);
      if (r == COPPER_RESULT_INVALID_INPUT) { result = -1; goto cleanup; }
      if (r == COPPER_RESULT_UNVERIFIED) {
        if (stats) stats->unsupported_objects++;
        continue;
      }
      if (r != COPPER_RESULT_TOO_CLOSE && r != COPPER_RESULT_SHORT) continue;

      if (stats) stats->violations++;
      if (visitor) {
        PcbCopperFinding f;
        memset(&f, 0, sizeof(f));
        f.a = items[i];
        f.b = items[j];
        f.result = r;
        f.required_nm = required_nm;
        double gap_nm = 0.0;
        if (copper_pair_gap_nm(&items[i], &items[j], &gap_nm)) {
          f.gap_mm = gap_nm / (double)NM_PER_MM;
        }
        if (visitor(&f, user) != 0) { result = -1; goto cleanup; }
      }
    }
  }

cleanup:
  free(items); free(lo); free(hi); free(active);
  return result;
}

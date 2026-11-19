/* jlc_part.h - JLCPCB/LCSC assembly record, layered ON TOP of PartRecord.
 * Availability and price are time-stamped facts, never permanent truths. */
#ifndef COPPER_JLC_PART_H
#define COPPER_JLC_PART_H
#include <stdint.h>
#include <stdbool.h>

typedef enum { JLC_CLASS_UNKNOWN = 0, JLC_CLASS_BASIC, JLC_CLASS_PREFERRED, JLC_CLASS_EXTENDED } JlcClass;

typedef struct {
  const char *lcsc_id;          /* "C25804" */
  const char *part_id;          /* Copper PartRecord stable id (canonical identity) */
  const char *mpn, *manufacturer;
  const char *jlc_package;      /* "0603" as JLC names it */
  const char *footprint_id;     /* KiCad footprint this part is placed with */
  JlcClass part_class;
  int32_t cpl_rot_offset_mdeg;  /* footprint-orientation fix for the CPL file */
  bool cpl_flip_bottom;
  int64_t unit_price_micro_usd; /* integer, never float */
  uint32_t stock;
  bool has_price;
  bool has_stock;
  bool availability_stale; /* retained record; caller must not present stale stock/price as current */
  const char *observed_date;    /* ISO; report must print it */
  const char *source;           /* where the stock/price was read */
} JlcPart;
#endif

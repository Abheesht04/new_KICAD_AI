#ifndef COPPER_JLC_CATALOGUE_H
#define COPPER_JLC_CATALOGUE_H
#include "IR.h"
#include "arena.h"
#include "jlc_part.h"
#include <stddef.h>
#include <stdint.h>

typedef enum {
  JLC_CATALOGUE_OK = 0,
  JLC_CATALOGUE_ERR_ARGUMENT = -1,
  JLC_CATALOGUE_ERR_IO = -2,
  JLC_CATALOGUE_ERR_PARSE = -3,
  JLC_CATALOGUE_ERR_SCHEMA = -4,
  JLC_CATALOGUE_ERR_DUPLICATE = -5,
  JLC_CATALOGUE_ERR_RANGE = -6,
  JLC_CATALOGUE_ERR_STALE = -7,
  JLC_CATALOGUE_ERR_MEMORY = -8
} JlcCatalogueStatus;

typedef struct {
  uint32_t schema_version;
  const char *source;
  JlcPart *parts;
  size_t part_count;
  JlcPart **by_part_id; /* sorted pointer indexes; records retain import order */
  JlcPart **by_lcsc_id;
} JlcCatalogue;

JlcCatalogueStatus jlc_catalogue_load(const char *path, Arena *arena,
                                      const char *today_iso,
                                      uint32_t max_availability_age_days,
                                      JlcCatalogue *out);
const JlcPart *jlc_catalogue_find_part_id(const JlcCatalogue *catalogue,
                                          const char *part_id);
const JlcPart *jlc_catalogue_find_lcsc_id(const JlcCatalogue *catalogue,
                                          const char *lcsc_id);
const char *jlc_catalogue_status_name(JlcCatalogueStatus status);
#endif

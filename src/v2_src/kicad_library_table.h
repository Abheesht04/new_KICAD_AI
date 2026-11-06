#ifndef KICAD_LIBRARY_TABLE_H
#define KICAD_LIBRARY_TABLE_H

#include "IR.h"

#include <stddef.h>
#include <stdint.h>

typedef enum {
  KICAD_LIB_TABLE_SYMBOLS = 0,
  KICAD_LIB_TABLE_FOOTPRINTS
} KicadLibTableKind;

typedef enum {
  KICAD_LIB_OK = 0,
  KICAD_LIB_ERR_ARGUMENT,
  KICAD_LIB_ERR_PARSE,
  KICAD_LIB_ERR_SCHEMA,
  KICAD_LIB_ERR_MEMORY,
  KICAD_LIB_ERR_UNRESOLVED_VARIABLE,
  KICAD_LIB_ERR_RANGE
} KicadLibStatus;

typedef struct {
  const char *nickname;
  const char *type;
  const char *uri;
  const char *options;
  const char *description;

  size_t source_order;
} KicadLibEntry;

typedef struct {
  KicadLibTableKind kind;
  uint32_t version;

  KicadLibEntry *entries;
  size_t entry_count;
} KicadLibTable;

typedef struct {
  const char *name;
  const char *value;
} KicadPathVariable;

typedef struct {
  const char *project_dir;

  const KicadPathVariable *variables;
  size_t variable_count;
} KicadLibPathContext;

/*
 * Parse one KiCad library table from a memory buffer.
 * Entries and their strings are owned by the supplied Arena.
 */
KicadLibStatus kicad_lib_table_parse(Arena *arena, const char *text,
                                     size_t length, KicadLibTableKind kind,
                                     KicadLibTable *out, IRError *error);

const char *kicad_lib_status_name(KicadLibStatus status);

const KicadLibEntry *kicad_lib_find_entry(const KicadLibTable *project_table,
                                          const KicadLibTable *global_table,
                                          const char *nickname);

KicadLibStatus kicad_lib_expand_uri(Arena *arena, const char *uri,
                                    const KicadLibPathContext *context,
                                    const char **out_path);

#endif

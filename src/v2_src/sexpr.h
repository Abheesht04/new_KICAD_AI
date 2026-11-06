// sexpr.h
#ifndef SEXPR_H
#define SEXPR_H

#include "arena.h"
#include <stddef.h>
#include <stdio.h>

typedef enum { SX_ATOM, SX_STRING, SX_LIST } SxKind;

typedef struct SxNode SxNode;
struct SxNode {
  SxKind kind;
  const char *text;    /* SX_ATOM / SX_STRING: arena-owned, NUL-terminated */
  SxNode *first_child; /* SX_LIST only */
  SxNode *last_child;  /* tail pointer -- O(1) append */
  SxNode *next;        /* next sibling within the parent's child list */
};

SxNode *sx_list(Arena *a, const char *head);
SxNode *sx_atom(Arena *a, const char *text);
SxNode *sx_atom_int(Arena *a, long value);
SxNode *sx_atom_double(Arena *a, double value);
SxNode *sx_string(Arena *a, const char *text);

void sx_push(SxNode *list, SxNode *child);

void sx_write(const SxNode *node, FILE *out, int depth);

/* ---- KiCad document header ---- */

typedef struct {
  const char *root_token; /* "kicad_sch" or "kicad_pcb" */
  long version;
  const char *generator;
  const char *generator_version;
  const char *uuid;  /* sch root only; NULL for pcb */
  const char *paper; /* sch only; NULL for pcb */
} KicadDocHeader;

KicadDocHeader kicad_sch_header_default(void);
KicadDocHeader kicad_pcb_header_default(void);

SxNode *kicad_doc_header_build(Arena *a, const KicadDocHeader *header);
// add to sexpr.h
SxNode *sx_parse(Arena *a, const char *src, size_t len, const char **error_msg);
const SxNode *sx_find(const SxNode *list, const char *head);
const SxNode *sx_find_named(const SxNode *list, const char *head,
                            const char *name);

#endif

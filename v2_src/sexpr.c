// sexpr.c
#include "sexpr.h"
#include <string.h>

typedef struct {
  const char *src;
  size_t len;
  size_t pos;
  const char **error_msg;
} SxParser;

static int sxp_peek(SxParser *p) {
  return p->pos < p->len ? (unsigned char)p->src[p->pos] : -1;
}

static void sxp_skip_ws(SxParser *p) {
  while (p->pos < p->len) {
    char c = p->src[p->pos];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      p->pos++;
    } else {
      break;
    }
  }
}

static int is_atom_char(char c) {
  return c != '(' && c != ')' && c != '"' && c != ' ' && c != '\t' &&
         c != '\n' && c != '\r';
}

static SxNode *sx_parse_node(Arena *a, SxParser *p);

static SxNode *node_new(Arena *a, SxKind kind) {
  SxNode *n = arena_alloc(a, sizeof(SxNode));
  n->kind = kind;
  n->text = NULL;
  n->first_child = n->last_child = n->next = NULL;
  return n;
}

static const char *arena_strdup(Arena *a, const char *s) {
  size_t len = strlen(s);
  char *copy = arena_alloc(a, len + 1);
  memcpy(copy, s, len + 1);
  return copy;
}

SxNode *sx_atom(Arena *a, const char *text) {
  SxNode *n = node_new(a, SX_ATOM);
  n->text = arena_strdup(a, text);
  return n;
}

SxNode *sx_string(Arena *a, const char *text) {
  SxNode *n = node_new(a, SX_STRING);
  n->text = arena_strdup(a, text);
  return n;
}

SxNode *sx_atom_int(Arena *a, long value) {
  char buf[32];
  snprintf(buf, sizeof buf, "%ld", value);
  return sx_atom(a, buf);
}

SxNode *sx_atom_double(Arena *a, double value) {
  char buf[64];
  snprintf(buf, sizeof buf, "%.10f", value);

  size_t len = strlen(buf);
  while (len > 0 && buf[len - 1] == '0') {
    buf[--len] = '\0';
  }
  if (len > 0 && buf[len - 1] == '.') {
    buf[len++] = '0';
    buf[len] = '\0';
  }
  return sx_atom(a, buf);
}

SxNode *sx_list(Arena *a, const char *head) {
  SxNode *list = node_new(a, SX_LIST);
  sx_push(list, sx_atom(a, head));
  return list;
}

void sx_push(SxNode *list, SxNode *child) {
  if (list->last_child) {
    list->last_child->next = child;
  } else {
    list->first_child = child;
  }
  list->last_child = child;
}

static void write_indent(FILE *out, int depth) {
  for (int i = 0; i < depth; i++) {
    fputc('\t', out);
  }
}

static void write_quoted(FILE *out, const char *text) {
  fputc('"', out);
  for (const char *p = text; *p; p++) {
    if (*p == '"' || *p == '\\') {
      fputc('\\', out);
      fputc(*p, out);
    } else if (*p == '\n') {
      fputs("\\n", out);
    } else if (*p == '\r') {
      fputs("\\r", out);
    } else {
      fputc(*p, out);
    }
  }
  fputc('"', out);
}

void sx_write(const SxNode *node, FILE *out, int depth) {
  if (!node)
    return;

  switch (node->kind) {
  case SX_ATOM:
    fputs(node->text, out);
    break;

  case SX_STRING:
    write_quoted(out, node->text);
    break;

  case SX_LIST: {
    fputc('(', out);
    int first = 1;
    for (const SxNode *child = node->first_child; child; child = child->next) {
      if (!first) {
        if (child->kind == SX_LIST) {
          fputc('\n', out);
          write_indent(out, depth + 1);
        } else {
          fputc(' ', out);
        }
      }
      sx_write(child, out, depth + 1);
      first = 0;
    }
    fputc(')', out);
    break;
  }
  }
}

KicadDocHeader kicad_sch_header_default(void) {
  return (KicadDocHeader){
      .root_token = "kicad_sch",
      .version = 20250114,
      .generator = "electronics_vendor_v2",
      .generator_version = "0.1",
      .uuid = NULL, /* caller must set a real uuid per document */
      .paper = "A4",
  };
}

KicadDocHeader kicad_pcb_header_default(void) {
  return (KicadDocHeader){
      .root_token = "kicad_pcb",
      .version = 20241229,
      .generator = "electronics_vendor_v2",
      .generator_version = "0.1",
      .uuid = NULL,
      .paper = NULL,
  };
}

SxNode *kicad_doc_header_build(Arena *a, const KicadDocHeader *h) {
  SxNode *root = sx_list(a, h->root_token);

  SxNode *version = sx_list(a, "version");
  sx_push(version, sx_atom_int(a, h->version));
  sx_push(root, version);

  SxNode *generator = sx_list(a, "generator");
  sx_push(generator, sx_string(a, h->generator));
  sx_push(root, generator);

  SxNode *gen_version = sx_list(a, "generator_version");
  sx_push(gen_version, sx_string(a, h->generator_version));
  sx_push(root, gen_version);

  if (h->uuid) {
    SxNode *uuid = sx_list(a, "uuid");
    sx_push(uuid, sx_string(a, h->uuid));
    sx_push(root, uuid);
  }

  if (h->paper) {
    SxNode *paper = sx_list(a, "paper");
    sx_push(paper, sx_string(a, h->paper));
    sx_push(root, paper);
  }

  return root;
}

static SxNode *sx_parse_string(Arena *a, SxParser *p) {
  p->pos++;                                        /* skip opening " */
  char *buf = arena_alloc(a, p->len - p->pos + 1); /* worst case: no escapes */
  size_t out = 0;

  while (p->pos < p->len && p->src[p->pos] != '"') {
    char c = p->src[p->pos];
    if (c == '\\' && p->pos + 1 < p->len) {
      char next = p->src[p->pos + 1];
      if (next == '"') {
        buf[out++] = '"';
        p->pos += 2;
      } else if (next == '\\') {
        buf[out++] = '\\';
        p->pos += 2;
      } else if (next == 'n') {
        buf[out++] = '\n';
        p->pos += 2;
      } else if (next == 'r') {
        buf[out++] = '\r';
        p->pos += 2;
      } else {
        buf[out++] = c;
        p->pos++;
      }
    } else {
      buf[out++] = c;
      p->pos++;
    }
  }

  if (p->pos >= p->len) {
    *p->error_msg = "unterminated string";
    return NULL;
  }
  p->pos++; /* skip closing " */
  buf[out] = '\0';

  SxNode *n = arena_alloc(a, sizeof(SxNode));
  n->kind = SX_STRING;
  n->text = buf;
  n->first_child = n->last_child = n->next = NULL;
  return n;
}

static SxNode *sx_parse_atom(Arena *a, SxParser *p) {
  size_t start = p->pos;
  while (p->pos < p->len && is_atom_char(p->src[p->pos])) {
    p->pos++;
  }
  size_t len = p->pos - start;
  char *buf = arena_alloc(a, len + 1);
  memcpy(buf, p->src + start, len);
  buf[len] = '\0';

  SxNode *n = arena_alloc(a, sizeof(SxNode));
  n->kind = SX_ATOM;
  n->text = buf;
  n->first_child = n->last_child = n->next = NULL;
  return n;
}

static SxNode *sx_parse_list(Arena *a, SxParser *p) {
  p->pos++; /* skip ( */
  SxNode *list = arena_alloc(a, sizeof(SxNode));
  list->kind = SX_LIST;
  list->text = NULL;
  list->first_child = list->last_child = list->next = NULL;

  for (;;) {
    sxp_skip_ws(p);
    int c = sxp_peek(p);

    if (c == ')') {
      p->pos++;
      return list;
    }
    if (c == -1) {
      *p->error_msg = "unterminated list";
      return NULL;
    }

    SxNode *child = sx_parse_node(a, p);
    if (!child)
      return NULL;
    sx_push(list, child);
  }
}

static SxNode *sx_parse_node(Arena *a, SxParser *p) {
  sxp_skip_ws(p);
  int c = sxp_peek(p);
  if (c == '(')
    return sx_parse_list(a, p);
  if (c == '"')
    return sx_parse_string(a, p);
  if (c == -1) {
    *p->error_msg = "unexpected end of input";
    return NULL;
  }
  return sx_parse_atom(a, p);
}

SxNode *sx_parse(Arena *a, const char *src, size_t len,
                 const char **error_msg) {
  const char *dummy;
  SxParser p = {src, len, 0, error_msg ? error_msg : &dummy};
  *p.error_msg = NULL;

  sxp_skip_ws(&p);
  if (sxp_peek(&p) == -1) {
    *p.error_msg = "empty input";
    return NULL;
  }

  SxNode *root = sx_parse_node(a, &p);
  return root;
}

/* First child LIST whose own first child is ATOM(head). */
const SxNode *sx_find(const SxNode *list, const char *head) {
  if (!list || list->kind != SX_LIST)
    return NULL;
  for (const SxNode *child = list->first_child; child; child = child->next) {
    if (child->kind == SX_LIST && child->first_child &&
        child->first_child->kind == SX_ATOM &&
        strcmp(child->first_child->text, head) == 0) {
      return child;
    }
  }
  return NULL;
}

/* Like sx_find, but also requires the SECOND child (atom or string) to
   equal `name` -- this is exactly the shape of (symbol "Device:R" ...). */
const SxNode *sx_find_named(const SxNode *list, const char *head,
                            const char *name) {
  if (!list || list->kind != SX_LIST)
    return NULL;
  for (const SxNode *child = list->first_child; child; child = child->next) {
    if (child->kind != SX_LIST || !child->first_child)
      continue;
    if (child->first_child->kind != SX_ATOM)
      continue;
    if (strcmp(child->first_child->text, head) != 0)
      continue;

    const SxNode *second = child->first_child->next;
    if (second && (second->kind == SX_ATOM || second->kind == SX_STRING) &&
        strcmp(second->text, name) == 0) {
      return child;
    }
  }
  return NULL;
}

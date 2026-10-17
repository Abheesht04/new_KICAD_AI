/*
 * symlib.c   Owner: azure   Status: kernel (new)
 * See symlib.h for the contract. No libm; integer nm only.
 */
#include "symlib.h"

#include "IR.h"

#include <stdlib.h>
#include <string.h>

#define SYM_MAX_EXTENDS_DEPTH 16

/* ---------------------------------------------------------------- helpers */

static SymStatus fail(SymError *err, SymStatus code, const char *msg,
                      const char *symbol) {
  if (err) {
    err->code = code;
    err->line = 0;
    err->column = 0;
    err->message = msg;
    err->symbol = symbol;
  }
  return code;
}

const char *symstatus_string(SymStatus s) {
  switch (s) {
  case SYM_OK:
    return "ok";
  case SYM_ERR_ARG:
    return "invalid argument";
  case SYM_ERR_IO:
    return "i/o error";
  case SYM_ERR_PARSE:
    return "s-expression parse error";
  case SYM_ERR_FORMAT:
    return "not a valid symbol library";
  case SYM_ERR_NUMBER:
    return "bad number";
  case SYM_ERR_DUPLICATE:
    return "duplicate symbol name";
  case SYM_ERR_EXTENDS:
    return "extends error";
  case SYM_ERR_OOM:
    return "out of memory";
  }
  return "unknown";
}

static const char *text_of(const IRNode *n) {
  if (n && (n->kind == IR_NODE_ATOM || n->kind == IR_NODE_STRING))
    return n->text;
  return NULL;
}

/* Is `n` a list whose first child is the atom `head`? */
static bool is_list(const IRNode *n, const char *head) {
  const IRNode *h;
  if (!n || n->kind != IR_NODE_LIST)
    return false;
  h = n->first_child;
  return h && h->kind == IR_NODE_ATOM && strcmp(h->text, head) == 0;
}

static const IRNode *nth(const IRNode *list, size_t i) {
  const IRNode *c = list ? list->first_child : NULL;
  while (c && i--)
    c = c->next;
  return c;
}

/* (yes|no) atom -> bool. */
static bool yes_no(const IRNode *n, bool *out) {
  const char *t = text_of(n);
  if (!t)
    return false;
  if (strcmp(t, "yes") == 0 || strcmp(t, "true") == 0) {
    *out = true;
    return true;
  }
  if (strcmp(t, "no") == 0 || strcmp(t, "false") == 0) {
    *out = false;
    return true;
  }
  return false;
}

static char *arena_strdup(Arena *a, const char *s) {
  size_t n = strlen(s) + 1;
  char *p = (char *)arena_alloc(a, n);
  if (p)
    memcpy(p, s, n);
  return p;
}

/* (key x y) -> Pt in nm. */
static bool read_xy(const IRNode *list, Pt *out) {
  int64_t x, y;
  if (ir_atom_to_nm(nth(list, 1), &x) != IR_STATUS_OK)
    return false;
  if (ir_atom_to_nm(nth(list, 2), &y) != IR_STATUS_OK)
    return false;
  out->x = x;
  out->y = y;
  return true;
}

/* ------------------------------------------------------------- bbox build */

typedef struct {
  bool any;
  Pt lo, hi;
} Box;

static void box_add(Box *b, Pt p) {
  if (!b->any) {
    b->lo = b->hi = p;
    b->any = true;
    return;
  }
  if (p.x < b->lo.x)
    b->lo.x = p.x;
  if (p.y < b->lo.y)
    b->lo.y = p.y;
  if (p.x > b->hi.x)
    b->hi.x = p.x;
  if (p.y > b->hi.y)
    b->hi.y = p.y;
}

/* Adds the named (key x y) child of `g` to the box. Returns false only on a
 * malformed number; a missing child is not an error. */
static bool box_add_child_xy(Box *b, const IRNode *g, const char *key) {
  const IRNode *c = ir_list_find(g, key);
  Pt p;
  if (!c)
    return true;
  if (!read_xy(c, &p))
    return false;
  box_add(b, p);
  return true;
}

static bool graphics_to_box(Box *b, const IRNode *g) {
  if (is_list(g, "rectangle") || is_list(g, "arc"))
    return box_add_child_xy(b, g, "start") && box_add_child_xy(b, g, "mid") &&
           box_add_child_xy(b, g, "end");
  if (is_list(g, "circle")) {
    const IRNode *c = ir_list_find(g, "center");
    const IRNode *r = ir_list_find(g, "radius");
    Pt p;
    int64_t rad;
    if (!c || !r)
      return true;
    if (!read_xy(c, &p) || ir_atom_to_nm(nth(r, 1), &rad) != IR_STATUS_OK)
      return false;
    if (rad < 0)
      return false;
    box_add(b, (Pt){p.x - rad, p.y - rad});
    box_add(b, (Pt){p.x + rad, p.y + rad});
    return true;
  }
  if (is_list(g, "polyline") || is_list(g, "bezier")) {
    const IRNode *pts = ir_list_find(g, "pts");
    const IRNode *c;
    if (!pts)
      return true;
    for (c = pts->first_child; c; c = c->next) {
      Pt p;
      if (!is_list(c, "xy"))
        continue;
      if (!read_xy(c, &p))
        return false;
      box_add(b, p);
    }
    return true;
  }
  return true;
}

/* ---------------------------------------------------------------- parsing */

static bool pin_type_from_text(const char *t, SymPinType *out) {
  static const struct {
    const char *n;
    SymPinType v;
  } tab[] = {
      {"input", SYM_PIN_INPUT},
      {"output", SYM_PIN_OUTPUT},
      {"bidirectional", SYM_PIN_BIDIRECTIONAL},
      {"tri_state", SYM_PIN_TRISTATE},
      {"passive", SYM_PIN_PASSIVE},
      {"free", SYM_PIN_FREE},
      {"unspecified", SYM_PIN_UNSPECIFIED},
      {"power_in", SYM_PIN_POWER_IN},
      {"power_out", SYM_PIN_POWER_OUT},
      {"open_collector", SYM_PIN_OPEN_COLLECTOR},
      {"open_emitter", SYM_PIN_OPEN_EMITTER},
      {"no_connect", SYM_PIN_NO_CONNECT},
  };
  size_t i;
  for (i = 0; i < sizeof tab / sizeof tab[0]; ++i)
    if (strcmp(tab[i].n, t) == 0) {
      *out = tab[i].v;
      return true;
    }
  return false;
}

/* "<parent>_<unit>_<style>" -> unit, style. */
static bool parse_unit_suffix(const char *parent, const char *sub,
                              uint8_t *unit, uint8_t *style) {
  size_t pl = strlen(parent);
  const char *p;
  unsigned long v[2];
  int k;
  if (strncmp(sub, parent, pl) != 0 || sub[pl] != '_')
    return false;
  p = sub + pl + 1;
  for (k = 0; k < 2; ++k) {
    unsigned long acc = 0;
    size_t digits = 0;
    while (*p >= '0' && *p <= '9') {
      acc = acc * 10u + (unsigned long)(*p - '0');
      if (acc > 255u)
        return false;
      ++p;
      ++digits;
    }
    if (digits == 0)
      return false;
    v[k] = acc;
    if (k == 0) {
      if (*p != '_')
        return false;
      ++p;
    }
  }
  if (*p != '\0')
    return false;
  *unit = (uint8_t)v[0];
  *style = (uint8_t)v[1];
  return true;
}

static SymStatus parse_pin(Arena *arena, const IRNode *pin, uint8_t unit,
                           uint8_t style, SymPin *out, SymError *err,
                           const char *symname) {
  const char *type_t = text_of(nth(pin, 1));
  const IRNode *at = ir_list_find(pin, "at");
  const IRNode *len = ir_list_find(pin, "length");
  const IRNode *name = ir_list_find(pin, "name");
  const IRNode *num = ir_list_find(pin, "number");
  const IRNode *c;
  int64_t a;
  const char *s;

  memset(out, 0, sizeof *out);
  if (!type_t || !pin_type_from_text(type_t, &out->type))
    return fail(err, SYM_ERR_FORMAT, "unknown pin electrical type", symname);
  if (!at || !len || !name || !num)
    return fail(err, SYM_ERR_FORMAT, "pin missing at/length/name/number",
                symname);
  if (!read_xy(at, &out->pos))
    return fail(err, SYM_ERR_NUMBER, "bad pin coordinate", symname);
  if (ir_atom_to_i64(nth(at, 3), &a) != IR_STATUS_OK ||
      (a != 0 && a != 90 && a != 180 && a != 270))
    return fail(err, SYM_ERR_NUMBER, "pin angle not in {0,90,180,270}",
                symname);
  out->angle_deg = (int16_t)a;
  if (ir_atom_to_nm(nth(len, 1), &out->length) != IR_STATUS_OK ||
      out->length < 0)
    return fail(err, SYM_ERR_NUMBER, "bad pin length", symname);
  s = text_of(nth(name, 1));
  if (!s)
    return fail(err, SYM_ERR_FORMAT, "pin name missing", symname);
  out->name = arena_strdup(arena, s);
  s = text_of(nth(num, 1));
  if (!s || !*s)
    return fail(err, SYM_ERR_FORMAT, "pin number missing", symname);
  out->number = arena_strdup(arena, s);
  if (!out->name || !out->number)
    return fail(err, SYM_ERR_OOM, "out of memory", symname);
  out->unit = unit;
  out->body_style = style;
  for (c = pin->first_child; c; c = c->next) {
    bool b;
    if (c->kind == IR_NODE_ATOM && strcmp(c->text, "hide") == 0)
      out->hidden = true;
    else if (is_list(c, "hide") && yes_no(nth(c, 1), &b))
      out->hidden = b;
  }
  return SYM_OK;
}

static size_t count_pins_in(const IRNode *sub) {
  size_t n = 0;
  const IRNode *c;
  for (c = sub->first_child; c; c = c->next)
    if (is_list(c, "pin"))
      ++n;
  return n;
}

static SymStatus parse_symbol(Arena *arena, const IRNode *node, SymbolDef *out,
                              SymError *err) {
  const char *name = text_of(nth(node, 1));
  const IRNode *c;
  size_t pin_total = 0, prop_total = 0, pi = 0, qi = 0;
  Box box = {0};
  SymStatus st;

  memset(out, 0, sizeof *out);
  if (!name || !*name)
    return fail(err, SYM_ERR_FORMAT, "symbol without a name", NULL);
  out->name = arena_strdup(arena, name);
  if (!out->name)
    return fail(err, SYM_ERR_OOM, "out of memory", NULL);
  out->in_bom = true;
  out->on_board = true;

  for (c = node->first_child; c; c = c->next) {
    if (is_list(c, "property"))
      ++prop_total;
    else if (is_list(c, "symbol"))
      pin_total += count_pins_in(c);
  }
  if (prop_total) {
    out->props =
        (SymProp *)arena_alloc_array(arena, sizeof *out->props, prop_total);
    if (!out->props)
      return fail(err, SYM_ERR_OOM, "out of memory", out->name);
  }
  if (pin_total) {
    out->pins =
        (SymPin *)arena_alloc_array(arena, sizeof *out->pins, pin_total);
    if (!out->pins)
      return fail(err, SYM_ERR_OOM, "out of memory", out->name);
  }

  for (c = node->first_child; c; c = c->next) {
    bool b;
    if (is_list(c, "extends")) {
      const char *p = text_of(nth(c, 1));
      if (!p)
        return fail(err, SYM_ERR_FORMAT, "extends without a name", out->name);
      out->extends = arena_strdup(arena, p);
    } else if (is_list(c, "property")) {
      const char *k = text_of(nth(c, 1)), *v = text_of(nth(c, 2));
      if (!k || !v)
        return fail(err, SYM_ERR_FORMAT, "malformed property", out->name);
      out->props[qi].name = arena_strdup(arena, k);
      out->props[qi].value = arena_strdup(arena, v);
      if (!out->props[qi].name || !out->props[qi].value)
        return fail(err, SYM_ERR_OOM, "out of memory", out->name);
      ++qi;
    } else if (is_list(c, "in_bom") && yes_no(nth(c, 1), &b)) {
      out->in_bom = b;
      out->set_mask |= SYM_SET_IN_BOM;
    } else if (is_list(c, "on_board") && yes_no(nth(c, 1), &b)) {
      out->on_board = b;
      out->set_mask |= SYM_SET_ON_BOARD;
    } else if (is_list(c, "exclude_from_sim") && yes_no(nth(c, 1), &b)) {
      out->exclude_from_sim = b;
      out->set_mask |= SYM_SET_EXCL_SIM;
    } else if (is_list(c, "power")) {
      out->is_power = true;
      out->set_mask |= SYM_SET_POWER;
    } else if (is_list(c, "pin_numbers")) {
      const IRNode *h = ir_list_find(c, "hide");
      out->set_mask |= SYM_SET_PIN_NUMBERS;
      out->pin_numbers_hidden = false;
      if (h && yes_no(nth(h, 1), &b))
        out->pin_numbers_hidden = b;
      else if (nth(c, 1) && text_of(nth(c, 1)) &&
               strcmp(text_of(nth(c, 1)), "hide") == 0)
        out->pin_numbers_hidden = true;
    } else if (is_list(c, "pin_names")) {
      const IRNode *h = ir_list_find(c, "hide");
      const IRNode *o = ir_list_find(c, "offset");
      out->set_mask |= SYM_SET_PIN_NAMES;
      out->pin_names_hidden = false;
      if (h && yes_no(nth(h, 1), &b))
        out->pin_names_hidden = b;
      else {
        const IRNode *f = nth(c, 1);
        if (f && f->kind == IR_NODE_ATOM && strcmp(f->text, "hide") == 0)
          out->pin_names_hidden = true;
      }
      if (o) {
        if (ir_atom_to_nm(nth(o, 1), &out->pin_name_offset) != IR_STATUS_OK)
          return fail(err, SYM_ERR_NUMBER, "bad pin_names offset", out->name);
        out->set_mask |= SYM_SET_PIN_OFFSET;
      }
    } else if (is_list(c, "symbol")) {
      const char *subname = text_of(nth(c, 1));
      uint8_t unit = 0, style = 0;
      const IRNode *g;
      if (!subname || !parse_unit_suffix(out->name, subname, &unit, &style))
        return fail(err, SYM_ERR_FORMAT,
                    "bad sub-symbol name (want NAME_unit_style)", out->name);
      if (unit > out->unit_count)
        out->unit_count = unit;
      for (g = c->first_child; g; g = g->next) {
        if (is_list(g, "pin")) {
          st = parse_pin(arena, g, unit, style, &out->pins[pi], err, out->name);
          if (st != SYM_OK)
            return st;
          ++pi;
        } else if (!graphics_to_box(&box, g)) {
          return fail(err, SYM_ERR_NUMBER, "bad graphics coordinate",
                      out->name);
        }
      }
    }
  }
  out->prop_count = qi;
  out->pin_count = pi;
  if (out->unit_count == 0 && out->pin_count)
    out->unit_count = 1;
  if (box.any) {
    out->has_body = true;
    out->body_min = box.lo;
    out->body_max = box.hi;
  }
  return SYM_OK;
}

/* --------------------------------------------------------------- extends */

static int cmp_symbol(const void *a, const void *b) {
  return strcmp(((const SymbolDef *)a)->name, ((const SymbolDef *)b)->name);
}

static SymbolDef *find_mut(SymLib *lib, const char *name) {
  size_t lo = 0, hi = lib->symbol_count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    int c = strcmp(name, lib->symbols[mid].name);
    if (c == 0)
      return &lib->symbols[mid];
    if (c < 0)
      hi = mid;
    else
      lo = mid + 1;
  }
  return NULL;
}

static const SymProp *prop_in(const SymProp *p, size_t n, const char *name) {
  size_t i;
  for (i = 0; i < n; ++i)
    if (strcmp(p[i].name, name) == 0)
      return &p[i];
  return NULL;
}

/* state: 0 = unresolved, 1 = in progress, 2 = done. */
static SymStatus resolve_one(Arena *arena, SymLib *lib, uint8_t *state,
                             size_t idx, int depth, SymError *err) {
  SymbolDef *s = &lib->symbols[idx];
  SymbolDef *par;
  size_t pidx, i, extra, k;
  SymProp *merged;

  if (state[idx] == 2)
    return SYM_OK;
  if (state[idx] == 1)
    return fail(err, SYM_ERR_EXTENDS, "extends cycle", s->name);
  if (depth > SYM_MAX_EXTENDS_DEPTH)
    return fail(err, SYM_ERR_EXTENDS, "extends chain too deep", s->name);
  state[idx] = 1;

  if (s->extends) {
    SymStatus st;
    par = find_mut(lib, s->extends);
    if (!par)
      return fail(err, SYM_ERR_EXTENDS,
                  "extends: parent symbol not in this library", s->name);
    pidx = (size_t)(par - lib->symbols);
    st = resolve_one(arena, lib, state, pidx, depth + 1, err);
    if (st != SYM_OK)
      return st;

    /* Pins and units: a child never declares its own pins in practice.
     * If one did, KiCad would still use the parent's graphics; we refuse
     * the ambiguity instead of guessing. */
    if (s->pin_count != 0)
      return fail(err, SYM_ERR_EXTENDS, "derived symbol declares its own pins",
                  s->name);
    s->pins = par->pins;
    s->pin_count = par->pin_count;
    s->unit_count = par->unit_count;
    if (!s->has_body) {
      s->has_body = par->has_body;
      s->body_min = par->body_min;
      s->body_max = par->body_max;
    }
    if (!(s->set_mask & SYM_SET_IN_BOM))
      s->in_bom = par->in_bom;
    if (!(s->set_mask & SYM_SET_ON_BOARD))
      s->on_board = par->on_board;
    if (!(s->set_mask & SYM_SET_EXCL_SIM))
      s->exclude_from_sim = par->exclude_from_sim;
    if (!(s->set_mask & SYM_SET_POWER))
      s->is_power = par->is_power;
    if (!(s->set_mask & SYM_SET_PIN_NUMBERS))
      s->pin_numbers_hidden = par->pin_numbers_hidden;
    if (!(s->set_mask & SYM_SET_PIN_NAMES))
      s->pin_names_hidden = par->pin_names_hidden;
    if (!(s->set_mask & SYM_SET_PIN_OFFSET))
      s->pin_name_offset = par->pin_name_offset;

    /* Properties: child overrides, parent fills the gaps. */
    extra = 0;
    for (i = 0; i < par->prop_count; ++i)
      if (!prop_in(s->props, s->prop_count, par->props[i].name))
        ++extra;
    if (extra) {
      merged = (SymProp *)arena_alloc_array(arena, sizeof *merged,
                                            s->prop_count + extra);
      if (!merged)
        return fail(err, SYM_ERR_OOM, "out of memory", s->name);
      for (i = 0; i < s->prop_count; ++i)
        merged[i] = s->props[i];
      k = s->prop_count;
      for (i = 0; i < par->prop_count; ++i)
        if (!prop_in(s->props, s->prop_count, par->props[i].name))
          merged[k++] = par->props[i];
      s->props = merged;
      s->prop_count = k;
    }
  }
  state[idx] = 2;
  return SYM_OK;
}

/* ------------------------------------------------------------------ load */

SymStatus symlib_load_text(Arena *arena, const char *text, size_t len,
                           const char *source_name, SymLib *out,
                           SymError *err) {
  IRError ie;
  IRNode *root;
  const IRNode *c;
  size_t n = 0, i = 0;
  uint8_t *state;
  SymStatus st;

  if (out)
    memset(out, 0, sizeof *out);
  if (err)
    memset(err, 0, sizeof *err);
  if (!arena || !text || !out)
    return fail(err, SYM_ERR_ARG, "null argument", NULL);

  root = ir_parse(arena, text, len, &ie);
  if (!root) {
    SymStatus code =
        (ie.code == IR_STATUS_OUT_OF_MEMORY) ? SYM_ERR_OOM : SYM_ERR_PARSE;
    fail(err, code, ie.message ? ie.message : "parse error", NULL);
    if (err) {
      err->line = ie.line;
      err->column = ie.column;
    }
    return code;
  }
  if (!is_list(root, "kicad_symbol_lib"))
    return fail(err, SYM_ERR_FORMAT, "root is not (kicad_symbol_lib ...)",
                NULL);

  for (c = root->first_child; c; c = c->next)
    if (is_list(c, "symbol"))
      ++n;

  out->source_path = source_name ? arena_strdup(arena, source_name) : NULL;
  if (n) {
    out->symbols =
        (SymbolDef *)arena_alloc_array(arena, sizeof *out->symbols, n);
    if (!out->symbols) {
      memset(out, 0, sizeof *out);
      return fail(err, SYM_ERR_OOM, "out of memory", NULL);
    }
  }
  for (c = root->first_child; c; c = c->next) {
    if (!is_list(c, "symbol"))
      continue;
    st = parse_symbol(arena, c, &out->symbols[i], err);
    if (st != SYM_OK) {
      memset(out, 0, sizeof *out);
      return st;
    }
    ++i;
  }
  out->symbol_count = n;
  if (n > 1)
    qsort(out->symbols, n, sizeof *out->symbols, cmp_symbol);
  for (i = 1; i < n; ++i)
    if (strcmp(out->symbols[i - 1].name, out->symbols[i].name) == 0) {
      const char *dup = out->symbols[i].name;
      memset(out, 0, sizeof *out);
      return fail(err, SYM_ERR_DUPLICATE, "duplicate symbol name", dup);
    }

  state = n ? (uint8_t *)arena_alloc(arena, n) : NULL;
  if (n && !state) {
    memset(out, 0, sizeof *out);
    return fail(err, SYM_ERR_OOM, "out of memory", NULL);
  }
  if (n)
    memset(state, 0, n);
  for (i = 0; i < n; ++i) {
    st = resolve_one(arena, out, state, i, 0, err);
    if (st != SYM_OK) {
      memset(out, 0, sizeof *out);
      return st;
    }
  }
  return SYM_OK;
}

SymStatus symlib_load_file(Arena *arena, const char *path, SymLib *out,
                           SymError *err) {
  FILE *f;
  long sz;
  char *buf;
  size_t got;
  SymStatus st;

  if (out)
    memset(out, 0, sizeof *out);
  if (!arena || !path || !out)
    return fail(err, SYM_ERR_ARG, "null argument", NULL);
  f = fopen(path, "rb");
  if (!f)
    return fail(err, SYM_ERR_IO, "cannot open file", NULL);
  if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0 ||
      fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return fail(err, SYM_ERR_IO, "cannot size file", NULL);
  }
  buf = (char *)malloc((size_t)sz + 1u);
  if (!buf) {
    fclose(f);
    return fail(err, SYM_ERR_OOM, "out of memory", NULL);
  }
  got = fread(buf, 1, (size_t)sz, f);
  fclose(f);
  if (got != (size_t)sz) {
    free(buf);
    return fail(err, SYM_ERR_IO, "short read", NULL);
  }
  buf[sz] = '\0';
  st = symlib_load_text(arena, buf, (size_t)sz, path, out, err);
  free(buf);
  return st;
}

/* ---------------------------------------------------------------- queries */

const SymbolDef *symlib_find(const SymLib *lib, const char *name) {
  size_t lo = 0, hi;
  if (!lib || !name)
    return NULL;
  hi = lib->symbol_count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    int c = strcmp(name, lib->symbols[mid].name);
    if (c == 0)
      return &lib->symbols[mid];
    if (c < 0)
      hi = mid;
    else
      lo = mid + 1;
  }
  return NULL;
}

const char *symdef_prop(const SymbolDef *s, const char *prop_name) {
  const SymProp *p;
  if (!s || !prop_name)
    return NULL;
  p = prop_in(s->props, s->prop_count, prop_name);
  return p ? p->value : NULL;
}

const SymPin *symdef_pin_by_number(const SymbolDef *s, const char *number,
                                   uint8_t unit) {
  size_t i;
  if (!s || !number)
    return NULL;
  for (i = 0; i < s->pin_count; ++i)
    if (strcmp(s->pins[i].number, number) == 0 &&
        (s->pins[i].unit == 0 || s->pins[i].unit == unit))
      return &s->pins[i];
  return NULL;
}

Pt sympin_body_end(const SymPin *p) {
  Pt e = p->pos;
  switch (p->angle_deg) {
  case 0:
    e.x += p->length;
    break;
  case 90:
    e.y += p->length;
    break;
  case 180:
    e.x -= p->length;
    break;
  case 270:
    e.y -= p->length;
    break;
  default:
    break;
  }
  return e;
}

static char lower_ascii(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

bool symlib_glob_match(const char *pattern, const char *text) {
  const char *star = NULL, *mark = NULL;
  if (!pattern || !text)
    return false;
  while (*text) {
    if (*pattern == '*') {
      star = pattern++;
      mark = text;
    } else if (*pattern == '?' ||
               (*pattern && lower_ascii(*pattern) == lower_ascii(*text))) {
      ++pattern;
      ++text;
    } else if (star) {
      pattern = star + 1;
      text = ++mark;
    } else {
      return false;
    }
  }
  while (*pattern == '*')
    ++pattern;
  return *pattern == '\0';
}

bool symdef_fp_filter_match(const SymbolDef *s, const char *footprint_name) {
  const char *f = symdef_prop(s, "ki_fp_filters");
  char glob[128];
  if (!f || !footprint_name)
    return false;
  while (*f) {
    size_t n = 0;
    while (*f == ' ')
      ++f;
    while (f[n] && f[n] != ' ')
      ++n;
    if (n && n < sizeof glob) {
      memcpy(glob, f, n);
      glob[n] = '\0';
      if (symlib_glob_match(glob, footprint_name))
        return true;
    }
    f += n;
  }
  return false;
}

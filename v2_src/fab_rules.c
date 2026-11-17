#include "fab_rules.h"

#include <stdio.h>
#include <string.h>

static const IRNode *find(const IRNode *n, const char *name) { return ir_list_find(n, name); }
static const char *val_text(const IRNode *n, const char *name) {
  const IRNode *f = find(n, name), *v = f ? ir_child_at(f, 1) : NULL;
  return v && (v->kind == IR_NODE_ATOM || v->kind == IR_NODE_STRING) ? v->text : NULL;
}
static bool val_i64(const IRNode *n, const char *name, int64_t *out) {
  const IRNode *f = find(n, name), *v = f ? ir_child_at(f, 1) : NULL;
  return v && ir_atom_to_i64(v, out) == IR_STATUS_OK;
}
static bool is_rule(const IRNode *n) {
  const IRNode *h = ir_list_head(n);
  return n && n->kind == IR_NODE_LIST && h && h->kind == IR_NODE_ATOM &&
         strcmp(h->text, "rule") == 0;
}
static bool leap_year(int y) {
  return (y % 4 == 0) && ((y % 100 != 0) || (y % 400 == 0));
}
static bool parse_date(const char *s, int64_t *days) {
  static const unsigned month_days[12] = {
      31u, 28u, 31u, 30u, 31u, 30u,
      31u, 31u, 30u, 31u, 30u, 31u};
  if (!s || !days || strlen(s) != 10 || s[4] != '-' || s[7] != '-') return false;
  for (size_t i = 0; i < 10; ++i) {
    if (i == 4 || i == 7) continue;
    if (s[i] < '0' || s[i] > '9') return false;
  }
  int y = (s[0]-'0')*1000 + (s[1]-'0')*100 + (s[2]-'0')*10 + (s[3]-'0');
  unsigned m = (unsigned)((s[5]-'0')*10 + (s[6]-'0'));
  unsigned d = (unsigned)((s[8]-'0')*10 + (s[9]-'0'));
  if (y < 1970 || m < 1 || m > 12) return false;
  unsigned max_day = month_days[m - 1];
  if (m == 2 && leap_year(y)) max_day = 29;
  if (d < 1 || d > max_day) return false;

  /* Howard Hinnant's civil-date conversion; all intermediate fields are bounded. */
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned mp = m > 2 ? m - 3 : m + 9;
  const unsigned doy = (153u * mp + 2u) / 5u + d - 1u;
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  *days = (int64_t)era * 146097 + (int64_t)doe - 719468;
  return true;
}
static bool nonempty(const char *s) { return s && s[0] != '\0'; }
static FabStatus parse_one_rule(const IRNode *node, FabRule *r) {
  int64_t v = 0, n = 0;
  memset(r, 0, sizeof(*r));
  r->id = val_text(node, "id");
  r->kind = val_text(node, "kind");
  r->source_url = val_text(node, "source_url");
  r->retrieved = val_text(node, "retrieved");
  r->note = val_text(node, "note");
  r->edge = val_text(node, "edge");
  r->mask_color = val_text(node, "mask_color");
  if (r->mask_color && strcmp(r->mask_color, "standard") != 0 &&
      strcmp(r->mask_color, "black_white") != 0) return FAB_ERR_RANGE;
  if (!nonempty(r->id) || !nonempty(r->kind) || !val_i64(node, "value_nm", &v) ||
      !nonempty(r->source_url) || !nonempty(r->retrieved)) return FAB_ERR_MISSING_FIELD;
  if (v < 0) return FAB_ERR_RANGE;
  r->value = v;
  if (val_i64(node, "layers_min", &n)) { if (n < 0 || n > UINT32_MAX) return FAB_ERR_RANGE; r->layers_min = (uint32_t)n; }
  if (val_i64(node, "layers_max", &n)) { if (n < 0 || n > UINT32_MAX) return FAB_ERR_RANGE; r->layers_max = (uint32_t)n; }
  if (val_i64(node, "copper_oz_x10", &n)) { if (n < 0 || n > UINT32_MAX) return FAB_ERR_RANGE; r->copper_oz_x10 = (uint32_t)n; }
  const char *severity = val_text(node, "severity");
  if (severity && !strcmp(severity, "recommended")) r->severity = FAB_SEV_RECOMMENDED;
  else if (!severity || !strcmp(severity, "required")) r->severity = FAB_SEV_REQUIRED;
  else return FAB_ERR_RANGE;
  if (r->layers_min && r->layers_max && r->layers_min > r->layers_max) return FAB_ERR_RANGE;
  return FAB_OK;
}
const char *fab_status_name(FabStatus s) {
  switch (s) {
  case FAB_OK: return "OK"; case FAB_ERR_ARGUMENT: return "ARGUMENT";
  case FAB_ERR_IO: return "IO"; case FAB_ERR_PARSE: return "PARSE";
  case FAB_ERR_MISSING_FIELD: return "MISSING_FIELD"; case FAB_ERR_STALE: return "STALE";
  case FAB_ERR_NO_RULE: return "NO_RULE"; case FAB_ERR_MEMORY: return "MEMORY";
  case FAB_ERR_DUPLICATE: return "DUPLICATE"; case FAB_ERR_RANGE: return "RANGE";
  default: return "UNKNOWN";
  }
}
FabStatus fab_rules_load(const char *path, Arena *arena, const char *today_iso,
                         uint32_t max_age_days, FabRuleSet *out) {
  if (!path || !arena || !out || !today_iso) return FAB_ERR_ARGUMENT;
  /* Parse transactionally: a failed reload must not destroy a previously valid set. */
  FabRuleSet parsed;
  memset(&parsed, 0, sizeof(parsed));
  IRError error; ir_error_clear(&error);
  IRNode *root = ir_parse_file(arena, path, &error);
  if (!root) return FAB_ERR_PARSE;
  const IRNode *h = ir_list_head(root);
  if (!root || root->kind != IR_NODE_LIST || !h || strcmp(h->text, "copper_fab_rules") != 0)
    return FAB_ERR_PARSE;
  parsed.ruleset = val_text(root, "ruleset");
  parsed.fabricator = val_text(root, "fabricator");
  const IRNode *rules = find(root, "rules");
  if (!nonempty(parsed.ruleset) || !nonempty(parsed.fabricator) || !rules) return FAB_ERR_MISSING_FIELD;
  size_t count = 0;
  for (const IRNode *it = rules->first_child; it; it = it->next) if (is_rule(it)) ++count;
  if (count == 0) return FAB_ERR_NO_RULE;
  parsed.rules = (FabRule *)arena_alloc_array(arena, sizeof(FabRule), count);
  if (!parsed.rules) return FAB_ERR_MEMORY;
  int64_t today_days;
  if (!parse_date(today_iso, &today_days)) return FAB_ERR_ARGUMENT;
  size_t i = 0;
  for (const IRNode *it = rules->first_child; it; it = it->next) {
    if (!is_rule(it)) continue;
    FabStatus st = parse_one_rule(it, &parsed.rules[i]);
    if (st != FAB_OK) return st;
    for (size_t j = 0; j < i; ++j)
      if (!strcmp(parsed.rules[j].id, parsed.rules[i].id)) return FAB_ERR_DUPLICATE;
    int64_t retrieved_days;
    if (!parse_date(parsed.rules[i].retrieved, &retrieved_days) || retrieved_days > today_days)
      return FAB_ERR_RANGE;
    if ((uint64_t)(today_days - retrieved_days) > max_age_days) return FAB_ERR_STALE;
    ++i;
  }
  parsed.rule_count = i;
  *out = parsed;
  return FAB_OK;
}

FabStatus fab_rules_select_ex(const FabRuleSet *rs, const char *kind,
                              uint32_t layers, uint32_t copper_oz_x10,
                              const char *edge, const char *mask_color,
                              const FabRule **out, size_t capacity,
                              size_t *out_count) {
  if (!rs || !kind || !out || !out_count) return FAB_ERR_ARGUMENT;
  *out_count = 0;
  for (size_t i = 0; i < rs->rule_count; ++i) {
    const FabRule *r = &rs->rules[i];
    if (strcmp(r->kind, kind) != 0) continue;
    if (r->layers_min && layers < r->layers_min) continue;
    if (r->layers_max && layers > r->layers_max) continue;
    if (r->copper_oz_x10 && copper_oz_x10 && r->copper_oz_x10 != copper_oz_x10) continue;
    if (r->edge && (!edge || strcmp(r->edge, edge) != 0)) continue;
    if (r->mask_color) {
      if (!mask_color) return FAB_ERR_RANGE; /* caller must disambiguate */
      if (strcmp(r->mask_color, mask_color) != 0) continue;
    }
    if (*out_count >= capacity) return FAB_ERR_RANGE;
    out[(*out_count)++] = r;
  }
  return *out_count ? FAB_OK : FAB_ERR_NO_RULE;
}
FabStatus fab_rules_select(const FabRuleSet *rs, const char *kind,
                           uint32_t layers, uint32_t copper_oz_x10,
                           const char *edge, const FabRule **out,
                           size_t capacity, size_t *out_count) {
  return fab_rules_select_ex(rs, kind, layers, copper_oz_x10, edge, NULL,
                             out, capacity, out_count);
}

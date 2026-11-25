/* sch_rules.c - KiCad PWR_FLAGs on external-supply nets, and the project's library tables (see sch_rules.h). */
#include "sch_rules.h"

#include "json.h"
#include "sha256.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MM 1000000LL
#define GRID_NM 1270000LL          /* KiCad's 50 mil schematic grid */

const char *sch_rules_status_name(SchRulesStatus s)
{
    switch (s) {
    case SCH_RULES_OK: return "OK";
    case SCH_RULES_INVALID_INPUT: return "INVALID_INPUT";
    case SCH_RULES_ERR_ARGUMENT: return "ERR_ARGUMENT";
    case SCH_RULES_ERR_MEMORY: return "ERR_MEMORY";
    }
    return "UNKNOWN";
}

static SchRulesStatus fail(SchRulesReport *r, SchRulesStatus st, const char *fmt, const char *a)
{
    r->status = st;
    snprintf(r->message, sizeof r->message, fmt, a ? a : "");
    return st;
}

/* ------------------------------------------------------------------------------------------------ text */

typedef struct Buf { Arena *a; char *s; size_t len, cap; bool oom; } Buf;

static void put_n(Buf *b, const char *s, size_t n)
{
    if (b->oom || !s) return;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 4096;
        while (cap < b->len + n + 1) cap *= 2;
        char *p = arena_alloc(b->a, cap);
        if (!p) { b->oom = true; return; }
        if (b->len) memcpy(p, b->s, b->len);
        b->s = p; b->cap = cap;
    }
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = 0;
}

static void put(Buf *b, const char *s) { if (s) put_n(b, s, strlen(s)); }

static void putf(Buf *b, const char *fmt, ...)
{
    char t[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof t) { b->oom = true; return; }
    put_n(b, t, (size_t)n);
}

/* a length in mm from nm, without trailing zeros (KiCad's own form: 25.4, 194.31) */
static void put_mm(Buf *b, int64_t nm)
{
    char t[40];
    size_t n = 0;
    uint64_t v = nm < 0 ? (uint64_t)(-(nm + 1)) + 1u : (uint64_t)nm;
    if (nm < 0) t[n++] = '-';
    uint64_t whole = v / MM, frac = v % MM;
    char w[24];
    int wn = 0;
    do { w[wn++] = (char)('0' + (int)(whole % 10)); whole /= 10; } while (whole && wn < 23);
    while (wn) t[n++] = w[--wn];
    if (frac) {
        char f[6];
        for (int i = 5; i >= 0; i--) { f[i] = (char)('0' + (int)(frac % 10)); frac /= 10; }
        int last = 5;
        while (last > 0 && f[last] == '0') last--;
        t[n++] = '.';
        for (int i = 0; i <= last; i++) t[n++] = f[i];
    }
    t[n] = 0;
    put(b, t);
}

/* an S-expression string body: backslash and quote escaped */
static void put_escaped(Buf *b, const char *s)
{
    for (; s && *s; s++) {
        if (*s == '\\' || *s == '"') put_n(b, "\\", 1);
        put_n(b, s, 1);
    }
}

/* index of the ')' closing the '(' at `open`, skipping strings; SIZE_MAX when unbalanced */
static size_t close_of(const char *t, size_t len, size_t open)
{
    int depth = 0;
    for (size_t i = open; i < len; i++) {
        char c = t[i];
        if (c == '"') {
            for (i++; i < len && t[i] != '"'; i++) if (t[i] == '\\') i++;
            continue;
        }
        if (c == '(') depth++;
        else if (c == ')' && --depth == 0) return i;
    }
    return SIZE_MAX;
}

static const char *find_in(const char *t, size_t len, const char *what, size_t from)
{
    size_t n = strlen(what);
    for (size_t i = from; i + n <= len; i++) if (!memcmp(t + i, what, n)) return t + i;
    return NULL;
}

static size_t count_in(const char *t, size_t len, const char *what)
{
    size_t n = 0;
    for (const char *p = find_in(t, len, what, 0); p; p = find_in(t, len, what, (size_t)(p - t) + 1)) n++;
    return n;
}

/* the raw (still escaped) text of the string that starts right after `at` ("), up to its closing quote */
static bool raw_string(Arena *a, const char *t, size_t len, size_t at, const char **out)
{
    size_t i = at;
    while (i < len && t[i] != '"') { if (t[i] == '\\') i++; i++; }
    if (i >= len) return false;
    char *s = arena_alloc(a, i - at + 1);
    if (!s) return false;
    memcpy(s, t + at, i - at);
    s[i - at] = 0;
    *out = s;
    return true;
}

/* a decimal in mm (at most 6 decimals) at t[i..] to nm; false when it is not one */
static bool mm_at(const char *t, size_t len, size_t i, int64_t *out)
{
    bool neg = false;
    if (i < len && t[i] == '-') { neg = true; i++; }
    int64_t whole = 0, frac = 0;
    int digits = 0, fd = 0;
    while (i < len && t[i] >= '0' && t[i] <= '9') { if (whole > 1000000000LL) return false; whole = whole * 10 + (t[i++] - '0'); digits++; }
    if (i < len && t[i] == '.') {
        i++;
        while (i < len && t[i] >= '0' && t[i] <= '9') { if (fd == 6) return false; frac = frac * 10 + (t[i++] - '0'); fd++; }
    }
    if (!digits && !fd) return false;
    while (fd < 6) { frac *= 10; fd++; }
    *out = (whole * MM + frac) * (neg ? -1 : 1);
    return true;
}

/* deterministic UUID (version-5 layout) from SHA-256 of the sheet UUID and the item's key */
static void put_uuid(Buf *b, const char *root, const char *key)
{
    FsSha256 h;
    unsigned char d[32];
    fs_sha256_init(&h);
    fs_sha256_update(&h, root, strlen(root));
    fs_sha256_update(&h, "/copper-sch-rules/", 18);
    fs_sha256_update(&h, key, strlen(key));
    fs_sha256_final(&h, d);
    d[6] = (unsigned char)((d[6] & 0x0f) | 0x50);
    d[8] = (unsigned char)((d[8] & 0x3f) | 0x80);
    static const char hex[] = "0123456789abcdef";
    char u[37];
    size_t n = 0;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) u[n++] = '-';
        u[n++] = hex[d[i] >> 4];
        u[n++] = hex[d[i] & 15];
    }
    u[n] = 0;
    put(b, u);
}

/* ------------------------------------------------------------------------------------------------ power flags */

static void put_hidden_property(Buf *b, const char *name, const char *raw_value, int64_t x, int64_t y)
{
    put(b, "    (property \""); put(b, name); put(b, "\" \""); put(b, raw_value); put(b, "\"\n      (at ");
    put_mm(b, x); put(b, " "); put_mm(b, y);
    put(b, " 0)\n      (effects\n        (font\n          (size 1.27 1.27))\n        (hide yes)))\n");
}

SchRulesStatus sch_power_flags(Arena *a, const char *sch, size_t sch_len, const char *hints, size_t hints_len,
                               const char *lib, size_t lib_len, const char **out, size_t *out_len, SchRulesReport *r)
{
    if (!r) return SCH_RULES_ERR_ARGUMENT;
    memset(r, 0, sizeof *r);
    if (!a || !sch || !hints || !lib || !out || !out_len) return r->status = SCH_RULES_ERR_ARGUMENT;
    *out = sch; *out_len = sch_len;
    JsonValue *h = NULL;
    JsonError je;
    if (json_parse(a, hints, hints_len, &h, &je) != JSON_OK || !h || h->kind != JSON_OBJECT)
        return fail(r, SCH_RULES_INVALID_INPUT, "layout_hints.json does not parse%s", NULL);
    const JsonValue *flags = json_get(h, "power_flags");
    if (!flags || flags->kind != JSON_ARRAY || !flags->count) return r->status = SCH_RULES_OK;
    r->flags_present = count_in(sch, sch_len, "(lib_id \"power:PWR_FLAG\")");
    if (r->flags_present) return r->status = SCH_RULES_OK;   /* idempotent: never a second set */

    /* the PWR_FLAG symbol of KiCad's power library, copied verbatim */
    const char *ls = find_in(lib, lib_len, "(symbol \"PWR_FLAG\"", 0);
    size_t le = ls ? close_of(lib, lib_len, (size_t)(ls - lib)) : SIZE_MAX;
    if (!ls || le == SIZE_MAX) return fail(r, SCH_RULES_INVALID_INPUT, "the power library has no PWR_FLAG symbol%s", NULL);
    const char *desc = "";
    const char *dp = find_in(lib, le, "(property \"Description\" \"", (size_t)(ls - lib));
    if (dp && !raw_string(a, lib, le, (size_t)(dp - lib) + 25, &desc)) return r->status = SCH_RULES_ERR_MEMORY;

    /* the sheet: its UUID, the project name, lib_symbols, where the items end */
    const char *libsym = find_in(sch, sch_len, "(lib_symbols", 0);
    const char *up = find_in(sch, sch_len, "(uuid \"", 0);
    const char *pp = find_in(sch, sch_len, "(project \"", 0);
    if (!libsym || !up || up > libsym) return fail(r, SCH_RULES_INVALID_INPUT, "the schematic has no sheet UUID or no lib_symbols%s", NULL);
    size_t lclose = close_of(sch, sch_len, (size_t)(libsym - sch));
    if (lclose == SIZE_MAX) return fail(r, SCH_RULES_INVALID_INPUT, "the schematic's brackets do not balance%s", NULL);
    const char *root = NULL, *project = NULL;
    if (!raw_string(a, sch, sch_len, (size_t)(up - sch) + 7, &root)) return r->status = SCH_RULES_ERR_MEMORY;
    if (!pp) return fail(r, SCH_RULES_INVALID_INPUT, "the schematic names no project in its symbol instances%s", NULL);
    if (!raw_string(a, sch, sch_len, (size_t)(pp - sch) + 10, &project)) return r->status = SCH_RULES_ERR_MEMORY;
    const char *sheet_inst = find_in(sch, sch_len, "\n  (sheet_instances", 0);
    size_t insert_items = sheet_inst ? (size_t)(sheet_inst - sch) + 1 : SIZE_MAX;
    if (insert_items == SIZE_MAX) {   /* before the file's last ')' */
        size_t i = sch_len;
        while (i > 0 && sch[i - 1] != ')') i--;
        if (!i) return fail(r, SCH_RULES_INVALID_INPUT, "the schematic does not end with ')'%s", NULL);
        insert_items = i - 1;
    }

    /* the flags go below everything on the sheet, on the 1.27 mm grid */
    int64_t max_y = 0;
    for (const char *p = find_in(sch, sch_len, "(at ", 0); p; p = find_in(sch, sch_len, "(at ", (size_t)(p - sch) + 1)) {
        size_t i = (size_t)(p - sch) + 4;
        while (i < sch_len && sch[i] != ' ' && sch[i] != ')') i++;
        int64_t y;
        if (i < sch_len && sch[i] == ' ' && mm_at(sch, sch_len, i + 1, &y) && y > max_y) max_y = y;
    }
    int64_t y = max_y + 10 * GRID_NM;
    y = ((y + GRID_NM - 1) / GRID_NM) * GRID_NM;

    Buf items = {a, NULL, 0, 0, false};
    for (size_t k = 0; k < flags->count; k++) {
        const JsonValue *f = flags->items[k];
        const JsonValue *nv = json_get(f, "net");
        if (!nv || nv->kind != JSON_STRING || !nv->text_len) return fail(r, SCH_RULES_INVALID_INPUT, "a power flag in layout_hints.json has no net%s", NULL);
        Buf lab = {a, NULL, 0, 0, false};
        put(&lab, "(label \""); put_escaped(&lab, nv->text); put(&lab, "\"");
        if (lab.oom) return r->status = SCH_RULES_ERR_MEMORY;
        if (!find_in(sch, sch_len, lab.s, 0)) return fail(r, SCH_RULES_INVALID_INPUT, "power flag for net %s: the schematic has no such net label", nv->text);
        int64_t x = 2 * 10 * GRID_NM + (int64_t)k * 12 * GRID_NM;   /* 25.4 mm, then every 15.24 mm */
        char ref[16], key[600];
        snprintf(ref, sizeof ref, "#FLG%02zu", k + 1);
        put(&items, "  (symbol\n    (lib_id \"power:PWR_FLAG\")\n    (at "); put_mm(&items, x); put(&items, " "); put_mm(&items, y);
        put(&items, " 0)\n    (unit 1)\n    (exclude_from_sim no)\n    (in_bom yes)\n    (on_board yes)\n    (dnp no)\n    (uuid \"");
        snprintf(key, sizeof key, "pwr_flag:%s", nv->text); put_uuid(&items, root, key);
        put(&items, "\")\n    (property \"Reference\" \""); put(&items, ref); put(&items, "\"\n      (at ");
        put_mm(&items, x); put(&items, " "); put_mm(&items, y - 1905000);
        put(&items, " 0)\n      (effects\n        (font\n          (size 1.27 1.27))\n        (hide yes)))\n");
        put(&items, "    (property \"Value\" \"PWR_FLAG\"\n      (at "); put_mm(&items, x); put(&items, " "); put_mm(&items, y - 5080000);
        put(&items, " 0)\n      (effects\n        (font\n          (size 1.27 1.27))))\n");
        put_hidden_property(&items, "Footprint", "", x, y);
        put_hidden_property(&items, "Datasheet", "~", x, y);
        put_hidden_property(&items, "Description", desc, x, y);
        put(&items, "    (pin \"1\"\n      (uuid \"");
        snprintf(key, sizeof key, "pwr_flag_pin:%s", nv->text); put_uuid(&items, root, key);
        put(&items, "\"))\n    (instances\n      (project \""); put(&items, project); put(&items, "\"\n        (path \"/"); put(&items, root);
        put(&items, "\"\n          (reference \""); put(&items, ref); put(&items, "\")\n          (unit 1)))))\n");
        put(&items, "  "); put(&items, lab.s); put(&items, "\n    (at "); put_mm(&items, x); put(&items, " "); put_mm(&items, y);
        put(&items, " 0)\n    (effects\n      (font\n        (size 1.0 1.0))\n      (justify left bottom))\n    (uuid \"");
        snprintf(key, sizeof key, "pwr_flag_label:%s", nv->text); put_uuid(&items, root, key);
        put(&items, "\"))\n");
    }

    /* the library symbol, renamed to its library id, at the end of lib_symbols */
    Buf o = {a, NULL, 0, 0, false};
    size_t name_at = (size_t)(ls - lib) + 9;   /* after (symbol " (9 characters) */
    put_n(&o, sch, lclose);
    put(&o, "\n    (symbol \"power:");
    put_n(&o, lib + name_at, le + 1 - name_at);
    put_n(&o, sch + lclose, insert_items - lclose);
    put_n(&o, items.s, items.len);
    put_n(&o, sch + insert_items, sch_len - insert_items);
    if (o.oom || items.oom) return r->status = SCH_RULES_ERR_MEMORY;
    *out = o.s;
    *out_len = o.len;
    r->flags_added = flags->count;
    return r->status = SCH_RULES_OK;
}

/* ------------------------------------------------------------------------------------------------ library tables */

static bool nickname_ok(const char *s)
{
    if (!*s) return false;
    for (; *s; s++) {
        char c = *s;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '+' || c == '.'))
            return false;
    }
    return true;
}

static int cmp_str(const void *x, const void *y) { return strcmp(*(const char *const *)x, *(const char *const *)y); }

/* the library nicknames of every `head"NICK:ITEM"` in the text, sorted and unique */
static SchRulesStatus nicknames(Arena *a, const char *t, size_t len, const char *head, const char ***out, size_t *n, SchRulesReport *r)
{
    size_t cap = count_in(t, len, head), k = 0;
    const char **names = arena_alloc_array(a, sizeof *names, cap + 1);
    if (!names) return r->status = SCH_RULES_ERR_MEMORY;
    size_t hl = strlen(head);
    for (const char *p = find_in(t, len, head, 0); p; p = find_in(t, len, head, (size_t)(p - t) + 1)) {
        const char *id = NULL;
        if (!raw_string(a, t, len, (size_t)(p - t) + hl, &id)) return r->status = SCH_RULES_ERR_MEMORY;
        char *colon = strchr(id, ':');
        if (!colon) return fail(r, SCH_RULES_INVALID_INPUT, "library id '%s' has no library name", id);
        *colon = 0;
        if (!nickname_ok(id)) return fail(r, SCH_RULES_INVALID_INPUT, "library name '%s' is not one Copper writes into a table", id);
        names[k++] = id;
    }
    qsort(names, k, sizeof *names, cmp_str);
    size_t u = 0;
    for (size_t i = 0; i < k; i++) if (!u || strcmp(names[u - 1], names[i])) names[u++] = names[i];
    *out = names;
    *n = u;
    return SCH_RULES_OK;
}

static const char *table(Arena *a, const char *kind, const char **names, size_t n, const char *var, const char *ext, size_t *len)
{
    Buf b = {a, NULL, 0, 0, false};
    putf(&b, "(%s\n  (version 7)\n", kind);
    for (size_t i = 0; i < n; i++)
        putf(&b, "  (lib (name \"%s\")(type \"KiCad\")(uri \"${%s}/%s%s\")(options \"\")(descr \"%s\"))\n", names[i], var, names[i], ext, SCH_LIB_TABLE_DESCR);
    put(&b, ")\n");
    if (b.oom) return NULL;
    *len = b.len;
    return b.s;
}

SchRulesStatus sch_lib_tables(Arena *a, const char *sch, size_t sch_len, const char *pcb, size_t pcb_len,
                              const char **sym_table, size_t *sym_len, const char **fp_table, size_t *fp_len, SchRulesReport *r)
{
    if (!r) return SCH_RULES_ERR_ARGUMENT;
    memset(r, 0, sizeof *r);
    if (!a || !sch || !pcb || !sym_table || !sym_len || !fp_table || !fp_len) return r->status = SCH_RULES_ERR_ARGUMENT;
    const char **sn = NULL, **fn = NULL;
    size_t ns = 0, nf = 0;
    SchRulesStatus st = nicknames(a, sch, sch_len, "(lib_id \"", &sn, &ns, r);
    if (st) return st;
    if ((st = nicknames(a, pcb, pcb_len, "(footprint \"", &fn, &nf, r))) return st;
    *sym_table = table(a, "sym_lib_table", sn, ns, "KICAD9_SYMBOL_DIR", ".kicad_sym", sym_len);
    *fp_table = table(a, "fp_lib_table", fn, nf, "KICAD9_FOOTPRINT_DIR", ".pretty", fp_len);
    if (!*sym_table || !*fp_table) return r->status = SCH_RULES_ERR_MEMORY;
    r->sym_libs = ns;
    r->fp_libs = nf;
    return r->status = SCH_RULES_OK;
}

bool sch_lib_table_is_copper(const char *t, size_t len)
{
    if (!t) return false;
    size_t libs = count_in(t, len, "(lib ");
    size_t ours = count_in(t, len, "(descr \"" SCH_LIB_TABLE_DESCR "\")");
    return libs > 0 && libs == ours;
}

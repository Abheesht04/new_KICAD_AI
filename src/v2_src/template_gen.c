/* template_gen.c - deterministic design generation from analysed requirements (see template_gen.h). */
#include "template_gen.h"

#include "filesystem.h"
#include "json.h"
#include "kicad_assets.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MICRO INT64_C(1000000)

typedef struct Value { const char *name, *display, *provenance, *note; } Value;
typedef struct Check { const char *id, *status, *message; } Check;
typedef struct Ignored { const char *field, *reason; } Ignored;

typedef struct List { void *items; size_t count, capacity, size; } List;

typedef struct Ctx {
    Arena *a;
    const char *prompt_norm;      /* NULL: evidence not checked */
    const JsonValue *req, *evidence;
    List values, checks, questions, ignored;
    bool oom;
    TemplateGenStatus status;
    const char *template_name;
} Ctx;

/* ---------------------------------------------------------------------------------------------- small helpers */

static void *push(Ctx *c, List *l)
{
    if (l->count == l->capacity) {
        size_t cap = l->capacity ? l->capacity * 2 : 16;
        void *more = arena_alloc(c->a, cap * l->size);
        if (!more) { c->oom = true; return NULL; }
        if (l->count) memcpy(more, l->items, l->count * l->size);
        l->items = more;
        l->capacity = cap;
    }
    return (char *)l->items + l->size * l->count++;
}

static const char *dup(Ctx *c, const char *s)
{
    size_t n = strlen(s);
    char *d = arena_alloc(c->a, n + 1);
    if (!d) { c->oom = true; return ""; }
    memcpy(d, s, n + 1);
    return d;
}

static const char *fmt(Ctx *c, const char *f, const char *x, const char *y, const char *z)
{
    char buf[640];
    int n = snprintf(buf, sizeof buf, f, x ? x : "", y ? y : "", z ? z : "");
    if (n < 0) buf[0] = 0;
    else if ((size_t)n >= sizeof buf) memcpy(buf + sizeof buf - 4, "...", 4);
    return dup(c, buf);
}

static void value(Ctx *c, const char *name, const char *display, const char *prov, const char *note)
{
    Value *v = push(c, &c->values);
    if (v) { v->name = name; v->display = display; v->provenance = prov; v->note = note; }
}
static void check(Ctx *c, const char *id, const char *status, const char *message)
{
    Check *k = push(c, &c->checks);
    if (k) { k->id = id; k->status = status; k->message = message; }
}
static void question(Ctx *c, const char *q)
{
    const char **p = push(c, &c->questions);
    if (p) *p = q;
}
static void ignore(Ctx *c, const char *field, const char *reason)
{
    Ignored *i = push(c, &c->ignored);
    if (i) { i->field = field; i->reason = reason; }
}

/* Lower-case ASCII, every run of white space as one space, trimmed. */
static const char *norm(Ctx *c, const char *s)
{
    size_t n = strlen(s), j = 0;
    char *d = arena_alloc(c->a, n + 1);
    if (!d) { c->oom = true; return ""; }
    bool space = true;
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') { if (!space) d[j++] = ' '; space = true; continue; }
        d[j++] = (char)(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
        space = false;
    }
    while (j && d[j - 1] == ' ') j--;
    d[j] = 0;
    return d;
}

/* micro-units to "3.267" (rounded to 3 decimals, zeros trimmed) */
static const char *micro_text(Ctx *c, int64_t v, const char *unit)
{
    bool neg = v < 0;
    uint64_t m = neg ? (uint64_t)(-(v + 1)) + 1u : (uint64_t)v;
    m = (m + 500u) / 1000u;                       /* to milli, rounded */
    char frac[8];
    snprintf(frac, sizeof frac, "%03u", (unsigned)(m % 1000u));
    size_t f = strlen(frac);
    while (f && frac[f - 1] == '0') frac[--f] = 0;
    char buf[64];
    snprintf(buf, sizeof buf, "%s%llu%s%s%s%s", neg ? "-" : "", (unsigned long long)(m / 1000u), f ? "." : "", frac,
             unit && unit[0] ? " " : "", unit ? unit : "");
    return dup(c, buf);
}

static const char *ohms_text(Ctx *c, int64_t ohms)
{
    char buf[32];
    if (ohms < 1000) snprintf(buf, sizeof buf, "%lld", (long long)ohms);
    else {
        int64_t div = ohms < 1000000 ? 1000 : 1000000;
        const char *suffix = ohms < 1000000 ? "k" : "M";
        int64_t whole = ohms / div, rest = (ohms % div) * 100 / div;   /* two decimals, truncated */
        if (rest == 0) snprintf(buf, sizeof buf, "%lld%s", (long long)whole, suffix);
        else if (rest % 10 == 0) snprintf(buf, sizeof buf, "%lld.%lld%s", (long long)whole, (long long)(rest / 10), suffix);
        else snprintf(buf, sizeof buf, "%lld.%02lld%s", (long long)whole, (long long)rest, suffix);
    }
    return dup(c, buf);
}

/* ---------------------------------------------------------------------------------------------- evidence */

/* Every decimal number in `s` as micro-units (at most 6 decimals). */
static size_t numbers_in(const char *s, int64_t *out, size_t cap)
{
    size_t n = 0;
    for (const char *p = s; *p && n < cap;) {
        if (*p < '0' || *p > '9') { p++; continue; }
        int64_t whole = 0, frac = 0, scale = MICRO;
        while (*p >= '0' && *p <= '9') { if (whole < INT64_C(1000000000)) whole = whole * 10 + (*p - '0'); p++; }
        if (*p == '.' && p[1] >= '0' && p[1] <= '9') {
            p++;
            while (*p >= '0' && *p <= '9') { if (scale > 1) { scale /= 10; frac += (*p - '0') * scale; } p++; }
        }
        out[n++] = whole * MICRO + frac;
    }
    return n;
}

/* Does the quote's number match `micro` directly or as a k/M/m/u multiple ("10 kOhm" for 10000)? */
static bool number_matches(int64_t q, int64_t micro)
{
    if (q == micro) return true;
    if (q <= INT64_MAX / 1000000 && q * 1000 == micro) return true;
    if (q <= INT64_MAX / 1000000000 && q * 1000000 == micro) return true;
    if (q % 1000 == 0 && q / 1000 == micro) return true;
    if (q % 1000000 == 0 && q / 1000000 == micro) return true;
    return false;
}

typedef enum Kind { K_NUMBER, K_STRING, K_BOOL } Kind;

/* Is the requirement `field` backed by a quote from the prompt? Records why not. */
static bool backed(Ctx *c, const char *field, Kind kind, int64_t micro, const char *text)
{
    if (!c->prompt_norm) return true;
    bool any_quote = false;
    for (size_t i = 0; c->evidence && i < c->evidence->count; i++) {
        const JsonValue *e = c->evidence->items[i];
        const JsonValue *f = json_get(e, "field"), *q = json_get(e, "quote");
        if (!f || f->kind != JSON_STRING || strcmp(f->text, field) != 0 || !q || q->kind != JSON_STRING) continue;
        any_quote = true;
        const char *qn = norm(c, q->text);
        if (strlen(qn) < 2 || !strstr(c->prompt_norm, qn)) continue;
        if (kind == K_NUMBER) {
            int64_t nums[16];
            size_t n = numbers_in(qn, nums, 16);
            for (size_t k = 0; k < n; k++) if (number_matches(nums[k], micro)) return true;
        } else if (kind == K_STRING) {
            const char *tn = norm(c, text);
            if (tn[0] && strstr(qn, tn)) return true;
        } else {
            return true;
        }
    }
    ignore(c, field, any_quote ? "its quote does not occur in the prompt, or does not contain this value"
                               : "no quote from the prompt supports it");
    return false;
}

/* Read an optional requirement. Returns true with the value when present AND backed. */
static bool req_number(Ctx *c, const char *field, int64_t *micro)
{
    const JsonValue *v = json_get(c->req, field);
    if (!v || v->kind == JSON_NULL) return false;
    if (json_to_nm(v, micro) != JSON_NUMBER_OK) { ignore(c, field, "not a plain decimal number"); return false; }
    return backed(c, field, K_NUMBER, *micro, NULL);
}
static bool req_string(Ctx *c, const char *field, const char **text)
{
    const JsonValue *v = json_get(c->req, field);
    if (!v || v->kind != JSON_STRING || !v->text[0]) return false;
    *text = v->text;
    return backed(c, field, K_STRING, 0, v->text);
}
static bool req_bool(Ctx *c, const char *field, bool *b)
{
    const JsonValue *v = json_get(c->req, field);
    if (!v || (v->kind != JSON_TRUE && v->kind != JSON_FALSE)) return false;
    *b = v->kind == JSON_TRUE;
    return backed(c, field, K_BOOL, 0, NULL);
}

static int64_t template_default_micro(const JsonValue *tpl, const char *name, const char **why)
{
    const JsonValue *d = json_get(json_get(tpl, "defaults"), name);
    int64_t v = 0;
    *why = "";
    if (!d) return -1;
    const JsonValue *w = json_get(d, "why");
    if (w && w->kind == JSON_STRING) *why = w->text;
    if (json_to_nm(json_get(d, "value"), &v) != JSON_NUMBER_OK) return -1;
    return v;
}

/* Part values chosen by name (value_from) are kept here. */
typedef struct Named { const char *name, *text; } Named;

/* ---------------------------------------------------------------------------------------------- circuits */

static const int64_t E24[] = {10, 11, 12, 13, 15, 16, 18, 20, 22, 24, 27, 30, 33, 36, 39, 43, 47, 51, 56, 62, 68, 75, 82, 91};

/* Smallest E24 resistance R (ohms, 10 ohm to 91 Mohm) with R * den >= num, i.e. R >= num/den. 0 if none. */
static int64_t e24_at_least(int64_t num, int64_t den)
{
    for (int64_t decade = 10; decade <= 10000000; decade *= 10)
        for (size_t i = 0; i < sizeof E24 / sizeof E24[0]; i++) {
            int64_t r = E24[i] * decade / 10;
            if (r >= 1 && r * den >= num) return r;
        }
    return 0;
}

static bool resistor_from(Ctx *c, const JsonValue *tpl, const char *field, int64_t *ohms, Named *named, size_t *nn)
{
    int64_t micro = 0;
    const char *why = "";
    if (req_number(c, field, &micro)) {
        if (micro % MICRO != 0 || micro < MICRO || micro > INT64_C(100000000) * MICRO) {
            check(c, "INVALID_VALUE", "INVALID_INPUT", fmt(c, "%s must be a whole number of ohms from 1 to 100M", field, NULL, NULL));
            c->status = TGEN_INVALID_INPUT;
            return false;
        }
        *ohms = micro / MICRO;
        value(c, field, ohms_text(c, *ohms), "stated", "from the prompt");
    } else {
        int64_t d = template_default_micro(tpl, field, &why);
        if (d < MICRO) { c->status = TGEN_TEMPLATE_ERROR; return false; }
        *ohms = d / MICRO;
        value(c, field, ohms_text(c, *ohms), "default", why);
    }
    named[*nn].name = field;
    named[(*nn)++].text = ohms_text(c, *ohms);
    return true;
}

static void need(Ctx *c, const char *field, const char *q)
{
    question(c, q);
    (void)field;
    if (c->status == TGEN_GENERATED) c->status = TGEN_NEEDS_INPUT;
}

static void low_side_switch(Ctx *c, const JsonValue *tpl, Named *named, size_t *nn, bool *include_flyback)
{
    int64_t vs = 0, il = 0, vl = 0, max = 0;
    bool has_vs = req_number(c, "supply_voltage_v", &vs), has_il = req_number(c, "load_current_a", &il),
         has_vl = req_number(c, "logic_voltage_v", &vl);
    if (!has_vs) need(c, "supply_voltage_v", "What is the load supply voltage (V)?");
    if (!has_il) need(c, "load_current_a", "What is the maximum load current (A)?");
    if (!has_vl) need(c, "logic_voltage_v", "What is the logic (GPIO) high voltage that drives the switch (V)?");
    const JsonValue *lim = json_get(tpl, "limits");
    if (has_vs && json_to_nm(json_get(lim, "supply_voltage_v_max"), &max) == JSON_NUMBER_OK && (vs <= 0 || vs > max)) {
        check(c, "OUT_OF_SCOPE", "NOT_IMPLEMENTED", fmt(c, "a supply of %s is outside this template's scope (above 0 and up to %s)", micro_text(c, vs, "V"), micro_text(c, max, "V"), NULL));
        c->status = TGEN_NOT_IMPLEMENTED;
    }
    if (has_vl && json_to_nm(json_get(lim, "logic_voltage_v_max"), &max) == JSON_NUMBER_OK && (vl <= 0 || vl > max)) {
        check(c, "OUT_OF_SCOPE", "NOT_IMPLEMENTED", fmt(c, "a logic voltage of %s is outside this template's scope (above 0 and up to %s)", micro_text(c, vl, "V"), micro_text(c, max, "V"), NULL));
        c->status = TGEN_NOT_IMPLEMENTED;
    }
    if (has_il && il <= 0) { check(c, "INVALID_VALUE", "INVALID_INPUT", "the load current must be above 0 A"); c->status = TGEN_INVALID_INPUT; }
    if (has_vs) value(c, "supply_voltage_v", micro_text(c, vs, "V"), "stated", "from the prompt");
    if (has_il) value(c, "load_current_a", micro_text(c, il, "A"), "stated", "from the prompt");
    if (has_vl) value(c, "logic_voltage_v", micro_text(c, vl, "V"), "stated", "from the prompt");

    int64_t rg = 0, rpd = 0;
    if (!resistor_from(c, tpl, "gate_resistor_ohm", &rg, named, nn)) return;
    if (!resistor_from(c, tpl, "pulldown_ohm", &rpd, named, nn)) return;

    bool fly = true;
    const char *why = "";
    if (req_bool(c, "flyback_diode", &fly)) value(c, "flyback_diode", fly ? "yes" : "no", "stated", "from the prompt");
    else {
        int64_t d = template_default_micro(tpl, "flyback_diode", &why);
        const JsonValue *dv = json_get(json_get(json_get(tpl, "defaults"), "flyback_diode"), "value");
        fly = dv ? dv->kind == JSON_TRUE : d != 0;
        value(c, "flyback_diode", fly ? "yes" : "no", "default", why);
    }
    *include_flyback = fly;

    const char *part = NULL;
    if (req_string(c, "switch_part", &part)) {
        char clean[33];
        size_t j = 0;
        for (const char *p = part; *p && j < sizeof clean - 1; p++)
            if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || strchr("_.+-", *p)) clean[j++] = *p;
        clean[j] = 0;
        if (j) {
            named[*nn].name = "switch_part";
            named[(*nn)++].text = dup(c, clean);
            value(c, "switch_part", dup(c, clean), "stated", "the part named in the prompt, placed as the generic SOT-23 N-MOSFET symbol (pins G-S-D)");
        }
    }

    if (!has_vs || !has_il || !has_vl || c->status != TGEN_GENERATED) return;
    /* Gate voltage from the divider, the logic pin's peak current, the pull-down's dissipation. */
    int64_t vg = vl * rpd / (rg + rpd);
    int64_t ipk = vl / rg;                              /* uV / ohm = uA */
    int64_t ppd = (vg / 1000) * (vg / 1000) / rpd;      /* mV^2/ohm = uW */
    value(c, "gate_voltage_on", micro_text(c, vg, "V"), "computed", "logic voltage x pull-down / (gate resistor + pull-down); the MOSFET's gate current is neglected");
    value(c, "logic_peak_current", micro_text(c, ipk * 1000, "mA"), "computed", "logic voltage / gate resistor, at the switching edge only");
    value(c, "pulldown_power", micro_text(c, ppd * 1000, "mW"), "computed", "gate voltage squared / pull-down, while on");
    const char *sw = part ? part : "Q1";
    check(c, "MOSFET_RATINGS", "UNVERIFIED",
          fmt(c, "%s must be rated for Vds above %s with margin and a continuous drain current of at least %s, and be fully on (low Rds(on)) at the gate drive below. Copper has no rating data for this part.",
              sw, micro_text(c, vs, "V"), micro_text(c, il, "A")));
    Check *last = &((Check *)c->checks.items)[c->checks.count - 1];
    last->message = fmt(c, "%s Required gate drive: %s.", last->message, micro_text(c, vg, "V"), NULL);
    if (fly) check(c, "DIODE_RATINGS", "UNVERIFIED",
                   fmt(c, "D1 must block at least %s in reverse and carry the load current (%s) when the switch turns off. Copper has no rating data for it.", micro_text(c, vs, "V"), micro_text(c, il, "A"), NULL));
    else check(c, "NO_FLYBACK", "INFO", "no flyback diode: suitable for resistive loads only");
    check(c, "TRACE_WIDTH", "NOT_IMPLEMENTED",
          fmt(c, "copper width and clearance for %s are not computed by Copper, and the board is not routed: size the load path (J1 to Q1 to D1) when routing", micro_text(c, il, "A"), NULL, NULL));
}

static void led_indicator(Ctx *c, const JsonValue *tpl, Named *named, size_t *nn)
{
    int64_t vs = 0, ima = 0, vf = 0, max = 0;
    const char *why = "";
    bool has_vs = req_number(c, "supply_voltage_v", &vs);
    if (!has_vs) need(c, "supply_voltage_v", "What is the supply voltage that powers the LED (V)?");
    const JsonValue *lim = json_get(tpl, "limits");
    if (has_vs && json_to_nm(json_get(lim, "supply_voltage_v_max"), &max) == JSON_NUMBER_OK && (vs <= 0 || vs > max)) {
        check(c, "OUT_OF_SCOPE", "NOT_IMPLEMENTED", fmt(c, "a supply of %s is outside this template's scope (above 0 and up to %s)", micro_text(c, vs, "V"), micro_text(c, max, "V"), NULL));
        c->status = TGEN_NOT_IMPLEMENTED;
    }
    if (has_vs) value(c, "supply_voltage_v", micro_text(c, vs, "V"), "stated", "from the prompt");
    if (req_number(c, "led_current_ma", &ima)) value(c, "led_current_ma", micro_text(c, ima, "mA"), "stated", "from the prompt");
    else { ima = template_default_micro(tpl, "led_current_ma", &why); value(c, "led_current_ma", micro_text(c, ima, "mA"), "default", why); }
    bool vf_assumed = false;
    if (req_number(c, "led_forward_voltage_v", &vf)) value(c, "led_forward_voltage_v", micro_text(c, vf, "V"), "stated", "from the prompt");
    else { vf = template_default_micro(tpl, "led_forward_voltage_v", &why); vf_assumed = true; value(c, "led_forward_voltage_v", micro_text(c, vf, "V"), "assumed", why); }
    const char *color = NULL;
    if (req_string(c, "led_color", &color)) {
        named[*nn].name = "led_color";
        named[(*nn)++].text = norm(c, color);
    }
    if (ima <= 0 || vf <= 0) { check(c, "INVALID_VALUE", "INVALID_INPUT", "the LED current and forward voltage must be above 0"); c->status = TGEN_INVALID_INPUT; return; }
    if (!has_vs || c->status != TGEN_GENERATED) return;
    if (vs <= vf) {
        check(c, "SUPPLY_TOO_LOW", "INVALID_INPUT", fmt(c, "a supply of %s cannot drive an LED with a forward voltage of %s", micro_text(c, vs, "V"), micro_text(c, vf, "V"), NULL));
        c->status = TGEN_INVALID_INPUT;
        return;
    }
    int64_t ia = ima / 1000;                            /* mA micro-units -> uA */
    if (ia <= 0) { check(c, "INVALID_VALUE", "INVALID_INPUT", "the LED current is below 1 uA"); c->status = TGEN_INVALID_INPUT; return; }
    int64_t r = e24_at_least(vs - vf, ia);              /* uV / uA = ohm; rounded UP so the current stays at or below the target */
    if (!r) { check(c, "INVALID_VALUE", "INVALID_INPUT", "no E24 resistor up to 91M gives that current"); c->status = TGEN_INVALID_INPUT; return; }
    int64_t actual_na = (vs - vf) * 1000 / r;                   /* uV * 1000 / ohm = nA (mA in micro-units) */
    int64_t p_uw = (actual_na * r / 1000) * actual_na / 1000000000;   /* uV x nA = 1e-15 W -> uW */
    value(c, "led_resistor_ohm", ohms_text(c, r), "computed", "(supply - LED forward voltage) / LED current, rounded UP to the E24 series");
    value(c, "led_current_actual", micro_text(c, actual_na, "mA"), "computed", "(supply - forward voltage) / chosen resistor");
    value(c, "resistor_power", micro_text(c, p_uw * 1000, "mW"), "computed", "current squared x resistor");
    named[*nn].name = "led_resistor_ohm";
    named[(*nn)++].text = ohms_text(c, r);
    check(c, "LED_RATINGS", vf_assumed ? "UNVERIFIED" : "INFO",
          vf_assumed ? "the LED forward voltage is a template assumption, not datasheet data: the real current depends on the LED used"
                     : "LED forward voltage taken from the prompt; LED ratings are not checked by Copper");
}

/* ---------------------------------------------------------------------------------------------- intent writer */

typedef struct Lib { const char *name; KicadSymbolLibrary lib; bool ok; } Lib;

static const KicadSymbolLibrary *load_symbols(Ctx *c, Lib *libs, size_t *nlibs, const char *root, const char *lib)
{
    for (size_t i = 0; i < *nlibs; i++) if (!strcmp(libs[i].name, lib)) return libs[i].ok ? &libs[i].lib : NULL;
    Lib *l = &libs[(*nlibs)++];
    memset(l, 0, sizeof *l);
    l->name = lib;
    char file[FS_PATH_MAX], name[FS_PATH_MAX];
    const unsigned char *data = NULL;
    size_t size = 0;
    KicadAssetError e;
    memset(&e, 0, sizeof e);
    int n = snprintf(name, sizeof name, "%s.kicad_sym", lib);
    l->ok = n > 0 && (size_t)n < sizeof name && fs_path_join(file, sizeof file, root, name) == FS_OK &&
            fs_read_file_arena(c->a, file, &data, &size) == FS_OK &&
            kicad_symbol_library_parse(c->a, (const char *)data, size, &l->lib, &e) == KICAD_ASSET_OK;
    return l->ok ? &l->lib : NULL;
}

static bool load_footprint(Ctx *c, const char *root, const char *lib, const char *name, KicadFootprintAsset *fp)
{
    char dir[FS_PATH_MAX], file[FS_PATH_MAX], a[FS_PATH_MAX], b[FS_PATH_MAX];
    const unsigned char *data = NULL;
    size_t size = 0;
    KicadAssetError e;
    memset(&e, 0, sizeof e);
    int n1 = snprintf(a, sizeof a, "%s.pretty", lib), n2 = snprintf(b, sizeof b, "%s.kicad_mod", name);
    return n1 > 0 && (size_t)n1 < sizeof a && n2 > 0 && (size_t)n2 < sizeof b &&
           fs_path_join(dir, sizeof dir, root, a) == FS_OK && fs_path_join(file, sizeof file, dir, b) == FS_OK &&
           fs_read_file_arena(c->a, file, &data, &size) == FS_OK &&
           kicad_footprint_parse(c->a, (const char *)data, size, fp, &e) == KICAD_ASSET_OK;
}

static bool split(Ctx *c, const char *id, const char **lib, const char **name)
{
    const char *colon = strchr(id, ':');
    if (!colon || colon == id || !colon[1]) return false;
    char *l = arena_alloc(c->a, (size_t)(colon - id) + 1);
    if (!l) { c->oom = true; return false; }
    memcpy(l, id, (size_t)(colon - id));
    l[colon - id] = 0;
    *lib = l;
    *name = colon + 1;
    return true;
}

static void template_error(Ctx *c, const char *ref, const char *what)
{
    check(c, "TEMPLATE_LIBRARY_MISMATCH", "ERROR", fmt(c, "%s: %s", ref, what, NULL));
    c->status = TGEN_TEMPLATE_ERROR;
}

static void write_mm(JsonWriter *w, Ctx *c, int64_t micro)
{
    const char *t = micro_text(c, micro, NULL);
    json_write_raw(w, t, strlen(t));
}

static const char *write_intent(Ctx *c, const JsonValue *tpl, const char *project, int64_t bw, int64_t bh,
                                const Named *named, size_t nn, bool include_flyback, const char *symbols_root,
                                const char *footprints_root, size_t *len)
{
    const JsonValue *parts = json_get(tpl, "parts");
    if (!parts || parts->kind != JSON_ARRAY) { template_error(c, "template", "has no parts"); return NULL; }
    Lib libs[32];
    size_t nlibs = 0;
    JsonWriter w;
    json_writer_init(&w, c->a);
    json_write_begin_object(&w);
    json_write_key(&w, "schema"); json_write_string(&w, "copper.design-intent");
    json_write_key(&w, "schema_version"); json_write_int(&w, 1);
    json_write_key(&w, "project");
    json_write_begin_object(&w);
    json_write_key(&w, "name"); json_write_string(&w, project);
    json_write_key(&w, "board");
    json_write_begin_object(&w);
    json_write_key(&w, "width_mm"); write_mm(&w, c, bw);
    json_write_key(&w, "height_mm"); write_mm(&w, c, bh);
    json_write_key(&w, "layers"); json_write_int(&w, 2);
    json_write_end_object(&w);
    json_write_end_object(&w);
    json_write_key(&w, "components");
    json_write_begin_array(&w);
    for (size_t i = 0; i < parts->count && c->status == TGEN_GENERATED; i++) {
        const JsonValue *p = parts->items[i];
        const JsonValue *ref = json_get(p, "ref"), *sym = json_get(p, "symbol"), *fpid = json_get(p, "footprint");
        const JsonValue *inc = json_get(p, "include_if");
        if (inc && inc->kind == JSON_STRING && !strcmp(inc->text, "flyback_diode") && !include_flyback) continue;
        if (!ref || ref->kind != JSON_STRING || !sym || sym->kind != JSON_STRING || !fpid || fpid->kind != JSON_STRING) {
            template_error(c, "template", "a part lacks ref, symbol or footprint");
            break;
        }
        const char *slib, *sname, *flib, *fname;
        if (!split(c, sym->text, &slib, &sname) || !split(c, fpid->text, &flib, &fname)) { template_error(c, ref->text, "bad library ID"); break; }
        if (nlibs == sizeof libs / sizeof libs[0]) { template_error(c, ref->text, "too many symbol libraries"); break; }
        const KicadSymbolLibrary *lib = load_symbols(c, libs, &nlibs, symbols_root, slib);
        const KicadSymbolAsset *asset = lib ? kicad_symbol_find(lib, sname) : NULL;
        if (!asset) { template_error(c, ref->text, fmt(c, "symbol %s is not installed", sym->text, NULL, NULL)); break; }
        if (asset->extends || asset->pins_inherited) { template_error(c, ref->text, fmt(c, "symbol %s is derived; the template must name its base", sym->text, NULL, NULL)); break; }
        KicadFootprintAsset fp;
        if (!load_footprint(c, footprints_root, flib, fname, &fp)) { template_error(c, ref->text, fmt(c, "footprint %s is not installed", fpid->text, NULL, NULL)); break; }

        /* value */
        const char *val = NULL;
        const JsonValue *v = json_get(p, "value"), *vf = json_get(p, "value_from"), *vd = json_get(p, "value_default");
        if (v && v->kind == JSON_STRING) val = v->text;
        if (vf && vf->kind == JSON_STRING)
            for (size_t k = 0; k < nn; k++) if (!strcmp(named[k].name, vf->text)) val = named[k].text;
        if (!val && vd && vd->kind == JSON_STRING) val = vd->text;
        if (!val) { template_error(c, ref->text, "no value"); break; }

        json_write_begin_object(&w);
        json_write_key(&w, "reference"); json_write_string(&w, ref->text);
        json_write_key(&w, "value"); json_write_string(&w, val);
        json_write_key(&w, "symbol_id"); json_write_string(&w, sym->text);
        json_write_key(&w, "footprint_id"); json_write_string(&w, fpid->text);
        const JsonValue *pos = json_get(p, "position");
        int64_t x = 0, y = 0;
        if (pos && pos->kind == JSON_ARRAY && pos->count == 2 && json_to_nm(pos->items[0], &x) == JSON_NUMBER_OK &&
            json_to_nm(pos->items[1], &y) == JSON_NUMBER_OK) {
            json_write_key(&w, "position");
            json_write_begin_object(&w);
            json_write_key(&w, "x_mm"); write_mm(&w, c, x);
            json_write_key(&w, "y_mm"); write_mm(&w, c, y);
            json_write_key(&w, "rotation_deg"); json_write_int(&w, 0);
            json_write_end_object(&w);
        }
        /* pins: every library pin, mapped by name or number from the template; pad = pin number */
        const JsonValue *by_name = json_get(p, "pins_by_name"), *by_num = json_get(p, "pins_by_number");
        const JsonValue *map = by_name ? by_name : by_num;
        if (!map || map->kind != JSON_OBJECT) { template_error(c, ref->text, "no pin map"); break; }
        size_t mapped = 0;
        json_write_key(&w, "pins");
        json_write_begin_array(&w);
        for (size_t k = 0; k < asset->pin_count; k++) {
            const KicadSymbolPin *pin = &asset->pins[k];
            if (pin->body_style > 1) continue;
            bool dup_earlier = false;
            for (size_t m = 0; m < k; m++) dup_earlier |= asset->pins[m].body_style <= 1 && !strcmp(asset->pins[m].number, pin->number);
            if (dup_earlier) continue;
            if (pin->unit > 1) { template_error(c, ref->text, "multi-unit symbol"); break; }
            const char *key = by_name ? (pin->name ? pin->name : "") : pin->number;
            const JsonValue *net = json_get(map, key);
            if (!net) { template_error(c, ref->text, fmt(c, "the template does not map library pin %s (%s)", pin->number, pin->name, NULL)); break; }
            if (!kicad_footprint_has_pad(&fp, pin->number)) { template_error(c, ref->text, fmt(c, "footprint %s has no pad %s for pin %s", fpid->text, pin->number, pin->name)); break; }
            mapped++;
            json_write_begin_object(&w);
            json_write_key(&w, "symbol_pin"); json_write_string(&w, pin->number);
            json_write_key(&w, "pin_name"); json_write_string(&w, pin->name ? pin->name : "");
            json_write_key(&w, "pad"); json_write_string(&w, pin->number);
            json_write_key(&w, "net");
            if (net->kind == JSON_NULL) json_write_null(&w); else json_write_string(&w, net->text);
            json_write_end_object(&w);
        }
        json_write_end_array(&w);
        json_write_end_object(&w);
        if (c->status == TGEN_GENERATED && mapped != map->count)
            template_error(c, ref->text, "the template maps a pin the installed symbol does not have");
    }
    json_write_end_array(&w);
    json_write_end_object(&w);
    if (c->status != TGEN_GENERATED) return NULL;
    return json_writer_finish(&w, len);
}

/* ---------------------------------------------------------------------------------------------- report */

static const char *write_report(Ctx *c, size_t *len)
{
    JsonWriter w;
    json_writer_init(&w, c->a);
    json_write_begin_object(&w);
    json_write_key(&w, "format"); json_write_string(&w, "copper.generation/1");
    json_write_key(&w, "status"); json_write_string(&w, template_gen_status_name(c->status));
    json_write_key(&w, "template"); if (c->template_name) json_write_string(&w, c->template_name); else json_write_null(&w);
    json_write_key(&w, "generated_by"); json_write_string(&w, "copper template generator (deterministic); the language model only analysed the prompt");
    json_write_key(&w, "evidence_checked"); json_write_bool(&w, c->prompt_norm != NULL);
    json_write_key(&w, "values");
    json_write_begin_array(&w);
    for (size_t i = 0; i < c->values.count; i++) {
        const Value *v = &((const Value *)c->values.items)[i];
        json_write_begin_object(&w);
        json_write_key(&w, "name"); json_write_string(&w, v->name);
        json_write_key(&w, "value"); json_write_string(&w, v->display);
        json_write_key(&w, "provenance"); json_write_string(&w, v->provenance);
        json_write_key(&w, "note"); json_write_string(&w, v->note);
        json_write_end_object(&w);
    }
    json_write_end_array(&w);
    json_write_key(&w, "checks");
    json_write_begin_array(&w);
    for (size_t i = 0; i < c->checks.count; i++) {
        const Check *k = &((const Check *)c->checks.items)[i];
        json_write_begin_object(&w);
        json_write_key(&w, "id"); json_write_string(&w, k->id);
        json_write_key(&w, "status"); json_write_string(&w, k->status);
        json_write_key(&w, "message"); json_write_string(&w, k->message);
        json_write_end_object(&w);
    }
    json_write_end_array(&w);
    json_write_key(&w, "questions");
    json_write_begin_array(&w);
    for (size_t i = 0; i < c->questions.count; i++) json_write_string(&w, ((const char **)c->questions.items)[i]);
    json_write_end_array(&w);
    json_write_key(&w, "ignored_requirements");
    json_write_begin_array(&w);
    for (size_t i = 0; i < c->ignored.count; i++) {
        const Ignored *g = &((const Ignored *)c->ignored.items)[i];
        json_write_begin_object(&w);
        json_write_key(&w, "field"); json_write_string(&w, g->field);
        json_write_key(&w, "reason"); json_write_string(&w, g->reason);
        json_write_end_object(&w);
    }
    json_write_end_array(&w);
    json_write_end_object(&w);
    return json_writer_finish(&w, len);
}

/* ---------------------------------------------------------------------------------------------- entry */

static const char *project_name(Ctx *c, const char *template_name)
{
    char buf[64];
    size_t j = 0;
    bool up = true;
    for (const char *p = template_name; *p && j < sizeof buf - 1; p++) {
        if (*p == '_') { up = true; continue; }
        buf[j++] = up && *p >= 'a' && *p <= 'z' ? (char)(*p - 'a' + 'A') : *p;
        up = false;
    }
    buf[j] = 0;
    return dup(c, buf);
}

TemplateGenStatus template_generate(Arena *arena, const char *requirements_json, size_t requirements_len,
                                    const char *prompt, const char *templates_json, size_t templates_len,
                                    const char *symbols_root, const char *footprints_root, TemplateGenOutput *out)
{
    if (!arena || !requirements_json || !templates_json || !symbols_root || !footprints_root || !out) return TGEN_ERR_ARGUMENT;
    memset(out, 0, sizeof *out);
    Ctx c;
    memset(&c, 0, sizeof c);
    c.a = arena;
    c.values.size = sizeof(Value);
    c.checks.size = sizeof(Check);
    c.questions.size = sizeof(const char *);
    c.ignored.size = sizeof(Ignored);
    c.status = TGEN_GENERATED;
    c.prompt_norm = prompt ? norm(&c, prompt) : NULL;
    if (!prompt) check(&c, "EVIDENCE", "UNVERIFIED", "no prompt was given, so the analysis could not be checked against it");

    JsonValue *req = NULL, *tpls = NULL;
    JsonError je;
    memset(&je, 0, sizeof je);
    if (json_parse(arena, requirements_json, requirements_len, &req, &je) != JSON_OK || req->kind != JSON_OBJECT) {
        check(&c, "REQUIREMENTS_JSON", "INVALID_INPUT", "the requirements are not a JSON object");
        c.status = TGEN_INVALID_INPUT;
    } else if (json_parse(arena, templates_json, templates_len, &tpls, &je) != JSON_OK || !json_get(tpls, "templates")) {
        check(&c, "TEMPLATES_JSON", "ERROR", "the template file is not valid");
        c.status = TGEN_TEMPLATE_ERROR;
    }
    const JsonValue *tpl = NULL;
    Named named[16];
    size_t nn = 0;
    bool include_flyback = false;
    int64_t bw = 0, bh = 0;
    if (c.status == TGEN_GENERATED) {
        c.req = req;
        const JsonValue *schema = json_get(req, "schema"), *version = json_get(req, "schema_version"), *circ = json_get(req, "circuit");
        int64_t ver = 0;
        if (!schema || schema->kind != JSON_STRING || strcmp(schema->text, "copper.requirements") != 0 || !version ||
            json_to_i64(version, &ver) != JSON_NUMBER_OK || ver != 1 || !circ || circ->kind != JSON_STRING) {
            check(&c, "REQUIREMENTS_SCHEMA", "INVALID_INPUT", "expected copper.requirements version 1 with a circuit name");
            c.status = TGEN_INVALID_INPUT;
        } else {
            c.evidence = json_get(req, "evidence");
            if (c.evidence && c.evidence->kind != JSON_ARRAY) c.evidence = NULL;
            tpl = json_get(json_get(tpls, "templates"), circ->text);
            if (!tpl) {
                const JsonValue *all = json_get(tpls, "templates");
                char list[256] = "";
                for (size_t i = 0; i < all->count; i++) {
                    if (strlen(list) + strlen(all->keys[i]) + 3 >= sizeof list) break;
                    if (i) strcat(list, ", ");
                    strcat(list, all->keys[i]);
                }
                check(&c, "NO_TEMPLATE", "NOT_IMPLEMENTED", fmt(&c, "Copper has no template for this circuit (\"%s\"); it can generate: %s", circ->text, list, NULL));
                c.status = TGEN_NOT_IMPLEMENTED;
            } else {
                c.template_name = circ->text;
                if (!strcmp(circ->text, "low_side_switch")) low_side_switch(&c, tpl, named, &nn, &include_flyback);
                else if (!strcmp(circ->text, "led_indicator")) led_indicator(&c, tpl, named, &nn);
                else { check(&c, "NO_GENERATOR", "NOT_IMPLEMENTED", "this template has no generator yet"); c.status = TGEN_NOT_IMPLEMENTED; }
                /* Board: the template's size, or a stated one that is at least as large. */
                const JsonValue *tb = json_get(tpl, "board");
                if (json_to_nm(json_get(tb, "width_mm"), &bw) != JSON_NUMBER_OK || json_to_nm(json_get(tb, "height_mm"), &bh) != JSON_NUMBER_OK) {
                    check(&c, "TEMPLATE_BOARD", "ERROR", "the template has no board size");
                    c.status = TGEN_TEMPLATE_ERROR;
                } else {
                    int64_t w = 0, h = 0;
                    bool sw = req_number(&c, "board_width_mm", &w), sh = req_number(&c, "board_height_mm", &h);
                    if ((sw && w < bw) || (sh && h < bh)) {
                        check(&c, "BOARD_TOO_SMALL", "INVALID_INPUT", fmt(&c, "this template needs a board of at least %s x %s", micro_text(&c, bw, "mm"), micro_text(&c, bh, "mm"), NULL));
                        if (c.status == TGEN_GENERATED) c.status = TGEN_INVALID_INPUT;
                    }
                    if (sw && w >= bw) bw = w;
                    if (sh && h >= bh) bh = h;
                    value(&c, "board", fmt(&c, "%s x %s", micro_text(&c, bw, "mm"), micro_text(&c, bh, "mm"), NULL),
                          (sw || sh) ? "stated" : "default", (sw || sh) ? "from the prompt" : "template default");
                }
            }
            /* Requirements this template does not use are reported, not silently dropped. */
            static const char *const fields[] = {"supply_voltage_v", "load_current_a", "logic_voltage_v", "switch_part", "gate_resistor_ohm",
                                                 "pulldown_ohm", "flyback_diode", "led_color", "led_current_ma", "led_forward_voltage_v", NULL};
            static const char *const switch_fields[] = {"supply_voltage_v", "load_current_a", "logic_voltage_v", "switch_part", "gate_resistor_ohm", "pulldown_ohm", "flyback_diode", NULL};
            static const char *const led_fields[] = {"supply_voltage_v", "led_color", "led_current_ma", "led_forward_voltage_v", NULL};
            const char *const *used = tpl && !strcmp(c.template_name ? c.template_name : "", "low_side_switch") ? switch_fields
                                      : tpl && !strcmp(c.template_name ? c.template_name : "", "led_indicator") ? led_fields : NULL;
            for (const char *const *f = fields; used && *f; f++) {
                const JsonValue *v = json_get(req, *f);
                if (!v || v->kind == JSON_NULL) continue;
                bool in = false;
                for (const char *const *u = used; *u; u++) in |= !strcmp(*u, *f);
                if (!in) ignore(&c, *f, "not used by this circuit template");
            }
        }
    }
    if (c.status == TGEN_GENERATED && tpl) {
        out->intent_json = write_intent(&c, tpl, project_name(&c, c.template_name), bw, bh, named, nn, include_flyback,
                                        symbols_root, footprints_root, &out->intent_len);
        if (!out->intent_json && c.status == TGEN_GENERATED) c.status = TGEN_ERR_MEMORY;
    }
    if (c.oom) return TGEN_ERR_MEMORY;
    out->report_json = write_report(&c, &out->report_len);
    if (!out->report_json) return TGEN_ERR_MEMORY;
    return c.status;
}

const char *template_gen_status_name(TemplateGenStatus status)
{
    switch (status) {
    case TGEN_GENERATED: return "GENERATED";
    case TGEN_NEEDS_INPUT: return "NEEDS_INPUT";
    case TGEN_NOT_IMPLEMENTED: return "NOT_IMPLEMENTED";
    case TGEN_INVALID_INPUT: return "INVALID_INPUT";
    case TGEN_TEMPLATE_ERROR: return "TEMPLATE_ERROR";
    case TGEN_ERR_ARGUMENT: return "ERR_ARGUMENT";
    case TGEN_ERR_MEMORY: return "ERR_MEMORY";
    }
    return "UNKNOWN";
}

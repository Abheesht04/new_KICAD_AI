/* compose.c - see compose.h. Joins VALID blocks into a design intent; computes only what a block's formula or source
 * defines, and refuses or asks instead of guessing. Integers only (uV, uA, uW, uohm, pF, ns). */
#include "compose.h"

#include "blocks.h"
#include "filesystem.h"
#include "json.h"
#include "kicad_assets.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define N(a) (sizeof(a) / sizeof((a)[0]))
#define NO_NET ((size_t)-1)

/* ---------------------------------------------------------------------------------------------- model */

typedef struct Inst {
    const char *id, *block;
    const JsonValue *root;      /* the block definition */
    const JsonValue *given;     /* request parameters (object) or NULL */
} Inst;

typedef struct Port {
    size_t inst;
    const char *name, *kind;
    bool has_range;
    int64_t vmin, vmax;
    const char *vparam;
    bool has_current, has_cmax, pullup;
    bool external;              /* Step 3d: "external_supply": power from outside the board */
    int64_t current, cmax;
    const char *follows, *level;
    size_t net;
} Port;

typedef struct Net {
    const char *name;
    size_t *ports;
    size_t count;
    bool has_v;                 /* voltage range known */
    int64_t vmin, vmax;
    int demand_state;           /* 0 not computed, 1 computing, 2 done */
    int64_t demand;
    bool demand_complete;
} Net;

typedef struct Param {
    size_t inst;
    const char *name, *unit;
    bool known;
    int64_t value;
    const char *origin, *source;
} Param;

typedef struct Ctx {
    Arena *a;
    ComposeReport *r;
    const ComposeInputs *in;
    size_t fcap, vcap;
    bool oom;
    Inst *insts; size_t ninst;
    Port *ports; size_t nport;
    Net *nets; size_t nnet;
    Param *params; size_t nparam, pcap;
    const char *evidence_prompt;   /* normalised prompt, or NULL: evidence not checked */
    const JsonValue *evidence;     /* the compose file's "evidence" array, or NULL */
} Ctx;

/* ---------------------------------------------------------------------------------------------- helpers */

static char *dup_text(Arena *a, const char *s)
{
    size_t n = strlen(s);
    char *d = arena_alloc(a, n + 1);
    if (d) memcpy(d, s, n + 1);
    return d;
}

static const char *fmt(Ctx *c, const char *f, const char *a, const char *b, const char *d)
{
    char buf[512];
    snprintf(buf, sizeof buf, f, a ? a : "", b ? b : "", d ? d : "");
    char *s = dup_text(c->a, buf);
    if (!s) c->oom = true;
    return s ? s : "";
}

static const char *num(Ctx *c, int64_t v, const char *unit)
{
    char buf[64];
    snprintf(buf, sizeof buf, "%lld %s", (long long)v, unit);
    char *s = dup_text(c->a, buf);
    if (!s) c->oom = true;
    return s ? s : "";
}

static void add(Ctx *c, ComposeLevel level, const char *code, const char *where, const char *message)
{
    ComposeReport *r = c->r;
    if (r->finding_count == c->fcap) {
        size_t cap = c->fcap ? c->fcap * 2 : 32;
        ComposeFinding *more = arena_alloc_array(c->a, sizeof *more, cap);
        if (!more) { c->oom = true; return; }
        if (r->finding_count) memcpy(more, r->findings, r->finding_count * sizeof *more);
        r->findings = more;
        c->fcap = cap;
    }
    ComposeFinding *f = &r->findings[r->finding_count++];
    f->level = level;
    f->code = code;
    f->where = dup_text(c->a, where ? where : "");
    f->message = dup_text(c->a, message ? message : "");
    if (!f->where || !f->message) c->oom = true;
}

static void value(Ctx *c, const char *where, const char *v, const char *origin, const char *source)
{
    ComposeReport *r = c->r;
    for (size_t i = 0; i < r->value_count; i++)   /* one formula used by two parts is reported once */
        if (!strcmp(r->values[i].where, where) && !strcmp(r->values[i].value, v)) return;
    if (r->value_count == c->vcap) {
        size_t cap = c->vcap ? c->vcap * 2 : 32;
        ComposeValue *more = arena_alloc_array(c->a, sizeof *more, cap);
        if (!more) { c->oom = true; return; }
        if (r->value_count) memcpy(more, r->values, r->value_count * sizeof *more);
        r->values = more;
        c->vcap = cap;
    }
    ComposeValue *x = &r->values[r->value_count++];
    x->where = where;
    x->value = v;
    x->origin = origin;
    x->source = source ? source : "";
}

static const char *str(const JsonValue *v) { return v && v->kind == JSON_STRING ? v->text : NULL; }

static bool identifier(const char *s)
{
    if (!s || !(*s >= 'a' && *s <= 'z')) return false;
    for (const char *p = s + 1; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) return false;
    return true;
}

static bool net_name_ok(const char *s)
{
    size_t n = s ? strlen(s) : 0;
    if (n == 0 || n > 64) return false;
    for (size_t i = 0; i < n; i++) {
        char ch = s[i];
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || strchr("_+./:-", ch))) return false;
    }
    return true;
}

static bool keys_ok(Ctx *c, const JsonValue *obj, const char *where, const char *const *allowed, size_t n)
{
    if (!obj || obj->kind != JSON_OBJECT) { add(c, COMPOSE_INVALID, "TYPE", where, "must be a JSON object"); return false; }
    bool ok = true;
    for (size_t i = 0; i < obj->count; i++) {
        bool known = false;
        for (size_t k = 0; k < n; k++) if (!strcmp(obj->keys[i], allowed[k])) known = true;
        if (!known) { add(c, COMPOSE_INVALID, "UNKNOWN_KEY", where, fmt(c, "unknown key '%s'", obj->keys[i], NULL, NULL)); ok = false; }
    }
    return ok;
}

static bool int_of(const JsonValue *v, int64_t *out) { return v && v->kind == JSON_NUMBER && json_to_i64(v, out) == JSON_NUMBER_OK; }

static const JsonValue *find_named(const JsonValue *arr, const char *key, const char *name)
{
    for (size_t i = 0; arr && arr->kind == JSON_ARRAY && i < arr->count; i++)
        if (str(json_get(arr->items[i], key)) && !strcmp(str(json_get(arr->items[i], key)), name)) return arr->items[i];
    return NULL;
}

static void upper(char *s) { for (; *s; s++) if (*s >= 'a' && *s <= 'z') *s = (char)(*s - 'a' + 'A'); }

/* ---------------------------------------------------------------------------------------------- evidence
 * The same rule as template_gen.c (its helpers are static there; H10 keeps that file unchanged): a quote must occur in
 * the prompt, case and white space ignored, and contain the value directly or as a k/M/m/u multiple. */




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

/* Every decimal number in `s`, scaled by 1e6 (at most 6 decimals). */
static size_t numbers_in(const char *s, int64_t *out, size_t cap)
{
    size_t n = 0;
    for (const char *p = s; *p && n < cap;) {
        if (*p < '0' || *p > '9') { p++; continue; }
        int64_t whole = 0, frac = 0, scale = 1000000;
        while (*p >= '0' && *p <= '9') { if (whole < INT64_C(1000000000)) whole = whole * 10 + (*p - '0'); p++; }
        if (*p == '.' && p[1] >= '0' && p[1] <= '9') {
            p++;
            while (*p >= '0' && *p <= '9') { if (scale > 1) { scale /= 10; frac += (*p - '0') * scale; } p++; }
        }
        out[n++] = whole * 1000000 + frac;
    }
    return n;
}

/* Does a quoted number (scaled by 1e6) equal `v` in its unit, directly or as a k/M/m/u multiple? */
static bool number_matches(int64_t q, int64_t v)
{
    if (q == v) return true;
    if (q <= INT64_MAX / 1000000 && q * 1000 == v) return true;
    if (q <= INT64_MAX / 1000000000 && q * 1000000 == v) return true;
    if (q % 1000 == 0 && q / 1000 == v) return true;
    if (q % 1000000 == 0 && q / 1000000 == v) return true;
    if (q % 1000000000 == 0 && q / 1000000000 == v) return true;
    return false;
}

/* Is the stated value of `field` backed by a quote from the prompt? Always true when no prompt is given. */
static bool backed(Ctx *c, const char *field, int64_t value)
{
    if (!c->evidence_prompt) return true;
    for (size_t i = 0; c->evidence && i < c->evidence->count; i++) {
        const JsonValue *e = c->evidence->items[i];
        if (!str(json_get(e, "field")) || strcmp(str(json_get(e, "field")), field) || !str(json_get(e, "quote"))) continue;
        const char *qn = norm(c, str(json_get(e, "quote")));
        if (strlen(qn) < 2 || !strstr(c->evidence_prompt, qn)) continue;
        int64_t nums[16];
        size_t n = numbers_in(qn, nums, 16);
        for (size_t k = 0; k < n; k++) if (number_matches(nums[k], value)) return true;
    }
    return false;
}

/* ---------------------------------------------------------------------------------------------- parameters */

static Param *param(Ctx *c, size_t inst, const char *name)
{
    for (size_t i = 0; i < c->nparam; i++) if (c->params[i].inst == inst && !strcmp(c->params[i].name, name)) return &c->params[i];
    return NULL;
}

static const char *source_doc(const JsonValue *block, const char *id)
{
    const JsonValue *s = find_named(json_get(block, "sources"), "id", id ? id : "");
    return s ? str(json_get(s, "document")) : NULL;
}

static void init_params(Ctx *c)
{
    for (size_t i = 0; i < c->ninst; i++) {
        const Inst *in = &c->insts[i];
        const JsonValue *decl = json_get(in->root, "parameters");
        for (size_t k = 0; in->given && k < in->given->count; k++)
            if (!find_named(decl, "name", in->given->keys[k]))
                add(c, COMPOSE_INVALID, "UNKNOWN_PARAMETER", in->id, fmt(c, "block '%s' has no parameter '%s'", in->block, in->given->keys[k], NULL));
        for (size_t k = 0; decl && k < decl->count; k++) {
            const JsonValue *d = decl->items[k];
            if (c->nparam == c->pcap) {
                size_t cap = c->pcap ? c->pcap * 2 : 32;
                Param *more = arena_alloc_array(c->a, sizeof *more, cap);
                if (!more) { c->oom = true; return; }
                if (c->nparam) memcpy(more, c->params, c->nparam * sizeof *more);
                c->params = more;
                c->pcap = cap;
            }
            Param *p = &c->params[c->nparam++];
            memset(p, 0, sizeof *p);
            p->inst = i;
            p->name = str(json_get(d, "name"));
            p->unit = str(json_get(d, "unit"));
            const char *where = fmt(c, "%s.%s", in->id, p->name, NULL);
            const JsonValue *g = in->given ? json_get(in->given, p->name) : NULL;
            if (g) {
                if (json_get(d, "from")) {
                    add(c, COMPOSE_INVALID, "CONFLICT", where, "this parameter is derived from a connection; it cannot also be given");
                } else if (!int_of(g, &p->value)) {
                    add(c, COMPOSE_INVALID, "BAD_NUMBER", where, fmt(c, "must be an integer in %s", p->unit, NULL, NULL));
                } else if (!backed(c, where, p->value)) {
                    add(c, COMPOSE_INFO, "IGNORED_VALUE", where,
                        fmt(c, "%s is not backed by a quote from the prompt that contains it: ignored", num(c, p->value, p->unit), NULL, NULL));
                    g = NULL;
                } else {
                    p->known = true;
                    p->origin = "request";
                    p->source = c->evidence_prompt ? "stated in the request (quote checked)" : "stated in the request";
                }
            }
            if (!g && json_get(d, "default")) {
                int_of(json_get(d, "default"), &p->value);
                p->known = true;
                p->origin = "default";
                p->source = source_doc(in->root, str(json_get(d, "source")));
            }
            if (p->known) value(c, where, num(c, p->value, p->unit), p->origin, p->source);
        }
    }
}

static void check_param_range(Ctx *c, const Param *p)
{
    const JsonValue *d = find_named(json_get(c->insts[p->inst].root, "parameters"), "name", p->name);
    int64_t lo = 0, hi = 0;
    const char *where = fmt(c, "%s.%s", c->insts[p->inst].id, p->name, NULL);
    if (int_of(json_get(d, "min"), &lo) && p->value < lo)
        add(c, COMPOSE_FAIL, "PARAMETER_RANGE", where, fmt(c, "%s is below the block's minimum %s", num(c, p->value, p->unit), num(c, lo, p->unit), NULL));
    if (int_of(json_get(d, "max"), &hi) && p->value > hi)
        add(c, COMPOSE_FAIL, "PARAMETER_RANGE", where, fmt(c, "%s is above the block's maximum %s", num(c, p->value, p->unit), num(c, hi, p->unit), NULL));
}

/* ---------------------------------------------------------------------------------------------- ports and nets */

static size_t port_index(Ctx *c, const char *ref)
{
    const char *dot = strchr(ref, '.');
    if (!dot) return NO_NET;
    for (size_t i = 0; i < c->nport; i++) {
        const Inst *in = &c->insts[c->ports[i].inst];
        size_t n = (size_t)(dot - ref);
        if (strlen(in->id) == n && !strncmp(in->id, ref, n) && !strcmp(c->ports[i].name, dot + 1)) return i;
    }
    return NO_NET;
}

static const char *pname(Ctx *c, size_t p) { return fmt(c, "%s.%s", c->insts[c->ports[p].inst].id, c->ports[p].name, NULL); }

static bool kind_is(const Port *p, const char *k) { return p->kind && !strcmp(p->kind, k); }

static void load_ports(Ctx *c)
{
    size_t total = 0;
    for (size_t i = 0; i < c->ninst; i++) {
        const JsonValue *ps = json_get(c->insts[i].root, "ports");
        total += ps ? ps->count : 0;
    }
    c->ports = total ? arena_alloc_array(c->a, sizeof *c->ports, total) : NULL;
    if (total && !c->ports) { c->oom = true; return; }
    for (size_t i = 0; i < c->ninst; i++) {
        const JsonValue *ps = json_get(c->insts[i].root, "ports");
        for (size_t k = 0; ps && k < ps->count; k++) {
            const JsonValue *v = ps->items[k];
            Port *p = &c->ports[c->nport++];
            memset(p, 0, sizeof *p);
            p->inst = i;
            p->name = str(json_get(v, "name"));
            p->kind = str(json_get(v, "kind"));
            p->has_range = int_of(json_get(v, "voltage_min"), &p->vmin) && int_of(json_get(v, "voltage_max"), &p->vmax);
            p->vparam = str(json_get(v, "voltage_parameter"));
            p->has_current = int_of(json_get(v, "current"), &p->current);
            p->has_cmax = int_of(json_get(v, "current_max"), &p->cmax);
            p->follows = str(json_get(v, "current_follows"));
            p->level = str(json_get(v, "level_port"));
            p->pullup = json_get(v, "pullup") && json_get(v, "pullup")->kind == JSON_TRUE;
            p->external = json_get(v, "external_supply") && json_get(v, "external_supply")->kind == JSON_TRUE;
            p->net = NO_NET;
        }
    }
}

static size_t same_inst_port(Ctx *c, size_t p, const char *name)
{
    for (size_t i = 0; name && i < c->nport; i++)
        if (c->ports[i].inst == c->ports[p].inst && !strcmp(c->ports[i].name, name)) return i;
    return NO_NET;
}

static void load_nets(Ctx *c, const JsonValue *arr)
{
    static const char *const keys[] = {"name", "ports"};
    if (!arr || arr->kind != JSON_ARRAY) { add(c, COMPOSE_INVALID, "MISSING_KEY", "nets", "'nets' must be an array"); return; }
    c->nets = arr->count ? arena_alloc_array(c->a, sizeof *c->nets, arr->count) : NULL;
    if (arr->count && !c->nets) { c->oom = true; return; }
    for (size_t i = 0; i < arr->count; i++) {
        char where[32];
        snprintf(where, sizeof where, "nets[%zu]", i);
        const JsonValue *v = arr->items[i];
        if (!keys_ok(c, v, where, keys, N(keys))) continue;
        const JsonValue *ps = json_get(v, "ports");
        if (!ps || ps->kind != JSON_ARRAY) { add(c, COMPOSE_INVALID, "MISSING_KEY", where, "'ports' must be an array of \"block.port\""); continue; }
        if (ps->count < 2) { add(c, COMPOSE_FAIL, "SINGLE_PORT_NET", where, "a net joins at least two ports"); continue; }
        Net *n = &c->nets[c->nnet];
        memset(n, 0, sizeof *n);
        n->ports = arena_alloc_array(c->a, sizeof(size_t), ps->count);
        if (!n->ports) { c->oom = true; return; }
        bool ok = true;
        for (size_t k = 0; k < ps->count; k++) {
            const char *ref = str(ps->items[k]);
            size_t p = ref ? port_index(c, ref) : NO_NET;
            if (p == NO_NET) { add(c, COMPOSE_INVALID, "UNKNOWN_PORT", where, fmt(c, "'%s' is not a port of a declared block", ref ? ref : "?", NULL, NULL)); ok = false; continue; }
            if (c->ports[p].net != NO_NET) { add(c, COMPOSE_INVALID, "DUPLICATE_CONNECTION", where, fmt(c, "port %s is already in another net", ref, NULL, NULL)); ok = false; continue; }
            for (size_t q = 0; q < n->count; q++)
                if (c->ports[n->ports[q]].inst == c->ports[p].inst)
                    add(c, COMPOSE_FAIL, "SAME_BLOCK_SHORT", where, fmt(c, "%s and %s belong to one block; joining them would short two of its ports", pname(c, n->ports[q]), ref, NULL));
            c->ports[p].net = c->nnet;
            n->ports[n->count++] = p;
        }
        const char *name = str(json_get(v, "name"));
        if (ok && name && !net_name_ok(name)) { add(c, COMPOSE_INVALID, "BAD_NET_NAME", where, "a net name is 1-64 letters, digits or _+./:-"); ok = false; }
        if (!ok) {   /* a rejected net releases its ports, so the index can be reused by the next net */
            for (size_t q = 0; q < n->count; q++) c->ports[n->ports[q]].net = NO_NET;
            continue;
        }
        if (!name) {
            bool ground = false;
            for (size_t q = 0; q < n->count; q++) ground |= kind_is(&c->ports[n->ports[q]], "ground");
            char buf[96];
            if (ground) snprintf(buf, sizeof buf, "GND");
            else snprintf(buf, sizeof buf, "%s_%s", c->insts[c->ports[n->ports[0]].inst].id, c->ports[n->ports[0]].name);
            upper(buf);
            name = dup_text(c->a, buf);
            if (!name) { c->oom = true; return; }
        }
        for (size_t q = 0; q < c->nnet; q++)
            if (!strcmp(c->nets[q].name, name)) { add(c, COMPOSE_INVALID, "DUPLICATE_NET_NAME", where, fmt(c, "net name '%s' is used twice", name, NULL, NULL)); ok = false; }
        if (!ok) {
            for (size_t q = 0; q < n->count; q++) c->ports[n->ports[q]].net = NO_NET;
            continue;
        }
        n->name = name;
        c->nnet++;
    }
}

/* The voltage range on each net: from its single power source, else from a signal's level port. */
static void net_voltages(Ctx *c)
{
    for (size_t i = 0; i < c->nnet; i++) {
        Net *n = &c->nets[i];
        for (size_t q = 0; q < n->count; q++) {
            const Port *p = &c->ports[n->ports[q]];
            if (!kind_is(p, "power_out")) continue;
            if (p->has_range) { n->has_v = true; n->vmin = p->vmin; n->vmax = p->vmax; }
            else if (p->vparam) {
                const Param *par = param(c, p->inst, p->vparam);
                if (par && par->known) { n->has_v = true; n->vmin = n->vmax = par->value; }
            }
        }
    }
    for (size_t i = 0; i < c->nnet; i++) {
        Net *n = &c->nets[i];
        for (size_t q = 0; !n->has_v && q < n->count; q++) {
            const Port *p = &c->ports[n->ports[q]];
            size_t lp = same_inst_port(c, n->ports[q], p->level);
            if (lp == NO_NET || c->ports[lp].net == NO_NET || !c->nets[c->ports[lp].net].has_v) continue;
            n->has_v = true;
            n->vmin = c->nets[c->ports[lp].net].vmin;
            n->vmax = c->nets[c->ports[lp].net].vmax;
        }
    }
}

static void check_nets(Ctx *c)
{
    for (size_t i = 0; i < c->nnet; i++) {
        const Net *n = &c->nets[i];
        size_t grounds = 0, sources = 0, scl = 0, sda = 0, pullups = 0, power = 0, tx = 0;
        for (size_t q = 0; q < n->count; q++) {
            const Port *p = &c->ports[n->ports[q]];
            grounds += kind_is(p, "ground");
            sources += kind_is(p, "power_out");
            power += kind_is(p, "power_in") || kind_is(p, "power_out");
            scl += kind_is(p, "i2c_scl");
            sda += kind_is(p, "i2c_sda");
            tx += kind_is(p, "uart_tx");
            pullups += p->pullup;
        }
        if (grounds && grounds != n->count)
            add(c, COMPOSE_FAIL, "KIND_MISMATCH", n->name, "a ground port is joined with ports that are not ground");
        if (sources > 1) add(c, COMPOSE_FAIL, "MULTIPLE_SOURCES", n->name, "two power sources (power_out) drive one net");
        if (scl && sda) add(c, COMPOSE_FAIL, "KIND_MISMATCH", n->name, "SCL and SDA are joined");
        if ((scl || sda) && power) add(c, COMPOSE_FAIL, "KIND_MISMATCH", n->name, "an I2C line is joined to a power net");
        if (tx > 1) add(c, COMPOSE_FAIL, "MULTIPLE_DRIVERS", n->name, "two UART transmit outputs drive one net");
        if ((scl || sda) && pullups > 1) add(c, COMPOSE_FAIL, "DUPLICATE_PULLUP", n->name, "more than one block puts pull-ups on this I2C line (one pair per bus)");
        if ((scl || sda) && pullups == 0) add(c, COMPOSE_FAIL, "NO_PULLUP", n->name, "this I2C line has no pull-up: one block on the bus must provide them");
        for (size_t q = 0; q < n->count; q++) {
            const Port *p = &c->ports[n->ports[q]];
            if (!kind_is(p, "power_in")) continue;
            if (!sources) { add(c, COMPOSE_FAIL, "NO_SOURCE", pname(c, n->ports[q]), fmt(c, "power input on net %s has no power source", n->name, NULL, NULL)); continue; }
            if (!n->has_v) continue;   /* the source's voltage is a question (reported with the parameter) */
            if (n->vmin < p->vmin || n->vmax > p->vmax) {
                char m[400];
                snprintf(m, sizeof m, "net %s carries %lld..%lld uV, but this input accepts %lld..%lld uV (see the block's source)",
                         n->name, (long long)n->vmin, (long long)n->vmax, (long long)p->vmin, (long long)p->vmax);
                add(c, COMPOSE_FAIL, "VOLTAGE_RANGE", pname(c, n->ports[q]), m);
            }
        }
    }
    for (size_t i = 0; i < c->nport; i++) {
        const Port *p = &c->ports[i];
        if (p->net != NO_NET) continue;
        if (kind_is(p, "power_in") || kind_is(p, "ground"))
            add(c, COMPOSE_FAIL, "UNCONNECTED_POWER", pname(c, i), "a power input or ground port must be connected");
        else
            add(c, COMPOSE_INFO, "UNCONNECTED", pname(c, i), "left unconnected");
        /* Step 3d (G-077): a two-terminal part (resistor, capacitor, LED, ...) with a pin on this port is then fitted
           with one pin floating: say so (H3), naming the part's role and pin */
        const JsonValue *parts = json_get(c->insts[p->inst].root, "parts");
        for (size_t k = 0; parts && k < parts->count; k++) {
            const JsonValue *pins = json_get(parts->items[k], "pins");
            if (!pins || pins->kind != JSON_OBJECT || pins->count != 2) continue;
            for (size_t t = 0; t < pins->count; t++) {
                if (!str(pins->items[t]) || strcmp(str(pins->items[t]), p->name)) continue;
                char m[300];
                const char *role = str(json_get(parts->items[k], "role"));
                snprintf(m, sizeof m, "port left unconnected, so part '%s' is fitted with pin '%s' floating; leave the block out, or connect the port",
                         role ? role : "?", pins->keys[t]);
                add(c, COMPOSE_UNVERIFIED, "UNCONNECTED_PORT_PARTS", pname(c, i), m);
            }
        }
    }
}

/* The current drawn from a net: the declared demands of its power inputs; a regulator input adds its output's load. */
static int64_t net_demand(Ctx *c, size_t net, bool *complete)
{
    Net *n = &c->nets[net];
    if (n->demand_state == 2) { *complete = *complete && n->demand_complete; return n->demand; }
    if (n->demand_state == 1) { *complete = false; return 0; }   /* a loop: unknowable */
    n->demand_state = 1;
    int64_t sum = 0;
    bool ok = true;
    for (size_t q = 0; q < n->count; q++) {
        const Port *p = &c->ports[n->ports[q]];
        if (!kind_is(p, "power_in")) continue;
        bool known = false;
        if (p->has_current) { sum += p->current; known = true; }
        size_t out = same_inst_port(c, n->ports[q], p->follows);
        if (out != NO_NET) {
            if (c->ports[out].net != NO_NET) sum += net_demand(c, c->ports[out].net, &ok);
            known = true;
        }
        if (!known) ok = false;
    }
    n->demand = sum;
    n->demand_complete = ok;
    n->demand_state = 2;
    *complete = *complete && ok;
    return sum;
}

static void check_currents(Ctx *c)
{
    for (size_t i = 0; i < c->nnet; i++) {
        const Net *n = &c->nets[i];
        const Port *src = NULL;
        for (size_t q = 0; q < n->count; q++) if (kind_is(&c->ports[n->ports[q]], "power_out")) src = &c->ports[n->ports[q]];
        if (!src) continue;
        bool complete = true;
        int64_t demand = net_demand(c, i, &complete);
        char m[400];
        if (src->has_cmax && demand > src->cmax) {
            snprintf(m, sizeof m, "the loads on %s draw at least %lld uA; the source delivers %lld uA at most", n->name, (long long)demand, (long long)src->cmax);
            add(c, COMPOSE_FAIL, "CURRENT_BUDGET", n->name, m);
        } else if (!complete) {
            snprintf(m, sizeof m, "%lld uA is a lower bound: a load on this net declares no current", (long long)demand);
            add(c, COMPOSE_UNVERIFIED, "CURRENT_BUDGET_INCOMPLETE", n->name, m);
        } else if (!src->has_cmax) {
            snprintf(m, sizeof m, "the loads draw %lld uA; the source's capacity is not known (an external supply)", (long long)demand);
            add(c, COMPOSE_UNVERIFIED, "SOURCE_CAPACITY_UNKNOWN", n->name, m);
        } else {
            snprintf(m, sizeof m, "%lld uA of %lld uA", (long long)demand, (long long)src->cmax);
            value(c, fmt(c, "net %s current", n->name, NULL, NULL), dup_text(c->a, m), "computed", "sum of the blocks' declared currents");
        }
    }
}

/* Parameters derived from connections ("from"), then questions for the rest. */
static void derive_params(Ctx *c)
{
    for (size_t i = 0; i < c->nparam; i++) {
        Param *p = &c->params[i];
        const Inst *in = &c->insts[p->inst];
        const JsonValue *d = find_named(json_get(in->root, "parameters"), "name", p->name);
        const JsonValue *from = json_get(d, "from");
        const char *where = fmt(c, "%s.%s", in->id, p->name, NULL);
        if (!p->known && from) {
            const char *port = str(json_get(from, "port")), *qty = str(json_get(from, "quantity"));
            size_t pi = NO_NET;
            for (size_t k = 0; k < c->nport; k++) if (c->ports[k].inst == p->inst && !strcmp(c->ports[k].name, port)) pi = k;
            size_t net = pi == NO_NET ? NO_NET : c->ports[pi].net;
            if (net == NO_NET) {
                add(c, COMPOSE_FAIL, "UNDERIVABLE", where, fmt(c, "derived from port '%s', which is not connected", port, NULL, NULL));
                continue;
            }
            if (!strcmp(qty, "current")) {
                bool complete = true;
                p->value = net_demand(c, net, &complete);
                p->known = true;
                p->origin = "derived";
                p->source = complete ? "sum of the declared currents on the net" : "lower bound: a load on the net declares no current";
            } else if (c->nets[net].has_v) {
                p->value = !strcmp(qty, "voltage_min") ? c->nets[net].vmin : c->nets[net].vmax;
                p->known = true;
                p->origin = "derived";
                p->source = fmt(c, "%s of net %s", qty, c->nets[net].name, NULL);
            } else {
                add(c, COMPOSE_QUESTION, "NEEDS_INPUT", where, fmt(c, "the voltage of net %s is not known, so this cannot be derived", c->nets[net].name, NULL, NULL));
                continue;
            }
            value(c, where, num(c, p->value, p->unit), p->origin, p->source);
        }
        if (!p->known) {
            if (!from) add(c, COMPOSE_QUESTION, "NEEDS_INPUT", where, fmt(c, "block '%s' needs '%s' (%s): give it in the request", in->block, p->name, p->unit));
            continue;
        }
        check_param_range(c, p);
    }
}

/* ---------------------------------------------------------------------------------------------- formulas and parts */

typedef struct Range { bool ok; int64_t lo, hi; bool prefer_low; const char *unit; } Range;

static bool pval(Ctx *c, size_t inst, const JsonValue *inputs, const char *key, int64_t *out)
{
    const Param *p = param(c, inst, str(json_get(inputs, key)) ? str(json_get(inputs, key)) : "");
    if (!p || !p->known) return false;
    *out = p->value;
    return true;
}

static Range eval_formula(Ctx *c, size_t inst, const char *name)
{
    Range r = {false, 0, 0, false, NULL};
    const Inst *in = &c->insts[inst];
    const JsonValue *f = find_named(json_get(in->root, "formulas"), "name", name);
    const char *fn = str(json_get(f, "function"));
    const JsonValue *inputs = json_get(f, "inputs");
    const char *where = fmt(c, "%s.%s", in->id, name, NULL);
    const char *src = source_doc(in->root, str(json_get(f, "source")));
    char m[400];
    if (!fn) return r;
    if (!strcmp(fn, "led_series_resistor")) {
        int64_t vd, vf, i;
        if (!pval(c, inst, inputs, "v_drive", &vd) || !pval(c, inst, inputs, "v_forward", &vf) || !pval(c, inst, inputs, "current", &i)) return r;
        if (vd <= vf || i <= 0) {
            snprintf(m, sizeof m, "drive voltage %lld uV is not above the forward voltage %lld uV", (long long)vd, (long long)vf);
            add(c, COMPOSE_FAIL, "FORMULA_DOMAIN", where, m);
            return r;
        }
        int64_t target = ((vd - vf) * 1000000 + i - 1) / i;      /* uohm, rounded up */
        r.ok = true; r.lo = target; r.hi = INT64_MAX; r.prefer_low = true; r.unit = "uohm";
        snprintf(m, sizeof m, "at least %lld uohm: (%lld - %lld) uV / %lld uA, rounded up", (long long)target, (long long)vd, (long long)vf, (long long)i);
        value(c, where, dup_text(c->a, m), "computed", src);
    } else if (!strcmp(fn, "i2c_pullup")) {
        int64_t vdd, vol, iol, tr, cb;
        if (!pval(c, inst, inputs, "vdd", &vdd) || !pval(c, inst, inputs, "vol", &vol) || !pval(c, inst, inputs, "iol", &iol) ||
            !pval(c, inst, inputs, "rise_time", &tr) || !pval(c, inst, inputs, "bus_capacitance", &cb)) return r;
        if (vdd <= vol || iol <= 0 || cb <= 0 || tr <= 0) { add(c, COMPOSE_FAIL, "FORMULA_DOMAIN", where, "VDD must exceed VOL; IOL, rise time and bus capacitance must be positive"); return r; }
        int64_t rmin = ((vdd - vol) * 1000000 + iol - 1) / iol;                 /* (VDD - VOL) / IOL, uohm, up */
        int64_t rmax = (tr * 10000000000000LL) / (8473 * cb);                   /* tr / (0.8473 Cb), uohm, down */
        snprintf(m, sizeof m, "%lld..%lld uohm: Rp(min) = (VDD - VOL) / IOL, Rp(max) = tr / (0.8473 Cb)", (long long)rmin, (long long)rmax);
        value(c, where, dup_text(c->a, m), "computed", src);
        if (rmin > rmax) { add(c, COMPOSE_FAIL, "NO_VALID_PULLUP", where, fmt(c, "no pull-up value meets both limits (%s): reduce the bus capacitance or the speed", m, NULL, NULL)); return r; }
        r.ok = true; r.lo = rmin; r.hi = rmax; r.prefer_low = false; r.unit = "uohm";
    } else if (!strcmp(fn, "ldo_dissipation")) {
        int64_t vin, vout, i;
        if (!pval(c, inst, inputs, "vin", &vin) || !pval(c, inst, inputs, "vout", &vout) || !pval(c, inst, inputs, "current", &i)) return r;
        int64_t p = vin > vout ? (vin - vout) * i / 1000000 : 0;
        r.ok = true; r.lo = r.hi = p; r.unit = "uW";
        value(c, where, num(c, p, "uW"), "computed", src);
        add(c, COMPOSE_UNVERIFIED, "NO_THERMAL_CHECK", where,
            fmt(c, "the regulator dissipates %s at most; Copper has no thermal rating for the package and does not check it", num(c, p, "uW"), NULL, NULL));
    } else if (!strcmp(fn, "rc_time_constant")) {
        int64_t rr, cc;
        if (!pval(c, inst, inputs, "resistance", &rr) || !pval(c, inst, inputs, "capacitance", &cc)) return r;
        if (cc && rr > INT64_MAX / cc) { add(c, COMPOSE_FAIL, "FORMULA_DOMAIN", where, "R x C is out of range"); return r; }
        int64_t t = rr * cc / 1000000000;
        r.ok = true; r.lo = r.hi = t; r.unit = "ns";
        value(c, where, num(c, t, "ns"), "computed", src);
    }
    return r;
}

static PartRatingName rating_for(const char *unit)
{
    if (!strcmp(unit, "uohm")) return PART_RATING_RESISTANCE;
    if (!strcmp(unit, "pF")) return PART_RATING_CAPACITANCE;
    if (!strcmp(unit, "nH")) return PART_RATING_INDUCTANCE;
    return PART_RATING_INVALID;
}

static const PartRecord *by_id(Ctx *c, const char *id)
{
    for (size_t i = 0; i < c->in->catalogue_count; i++) {
        const PartRecord *p = part_catalogue_find(c->in->catalogues[i], id);
        if (p) return p;
    }
    return NULL;
}

/* A catalogue part of this kind and package whose nominal value lies in [lo, hi]: the lowest or the highest one. */
static const PartRecord *by_value(Ctx *c, const char *kind, const char *package, Range r)
{
    const PartRecord *best = NULL;
    int64_t best_v = 0;
    PartRatingName rn = rating_for(r.unit);
    for (size_t i = 0; i < c->in->catalogue_count; i++)
        for (size_t k = 0; k < c->in->catalogues[i]->part_count; k++) {
            const PartRecord *p = &c->in->catalogues[i]->parts[k];
            if (strcmp(part_kind_name(p->kind), kind) || !p->package || strcmp(p->package, package)) continue;
            const PartRating *rt = part_record_find_rating(p, rn, PART_BOUND_NOMINAL);
            if (!rt || rt->value < r.lo || rt->value > r.hi) continue;
            if (!best || (r.prefer_low ? rt->value < best_v : rt->value > best_v)) { best = p; best_v = rt->value; }
        }
    return best;
}

/* The catalogue part for each block part (NULL where it cannot be chosen; a finding says why). */
static const PartRecord **choose_parts(Ctx *c, size_t inst)
{
    const Inst *in = &c->insts[inst];
    const JsonValue *parts = json_get(in->root, "parts");
    size_t n = parts ? parts->count : 0;
    const PartRecord **out = n ? arena_alloc_array(c->a, sizeof *out, n) : NULL;
    if (n && !out) { c->oom = true; return NULL; }
    for (size_t k = 0; k < n; k++) {
        const JsonValue *sel = json_get(parts->items[k], "select");
        const char *role = str(json_get(parts->items[k], "role"));
        const char *where = fmt(c, "%s.%s", in->id, role, NULL);
        out[k] = NULL;
        if (str(json_get(sel, "part_id"))) {
            out[k] = by_id(c, str(json_get(sel, "part_id")));
            if (!out[k]) add(c, COMPOSE_INVALID, "UNKNOWN_PART", where, "the block's part is in no catalogue");
            continue;
        }
        const char *kind = str(json_get(sel, "kind")), *package = str(json_get(sel, "package"));
        const JsonValue *val = json_get(sel, "value");
        Range r = {false, 0, 0, false, NULL};
        if (str(json_get(val, "formula"))) r = eval_formula(c, inst, str(json_get(val, "formula")));
        else if (str(json_get(val, "parameter"))) {
            const Param *p = param(c, inst, str(json_get(val, "parameter")));
            if (p && p->known) { r.ok = true; r.lo = r.hi = p->value; r.unit = p->unit; }
        }
        if (!r.ok) continue;   /* a question or a failure was already reported for the inputs */
        out[k] = by_value(c, kind, package, r);
        if (!out[k]) {
            char m[300];
            if (r.hi == INT64_MAX) snprintf(m, sizeof m, "no %s %s part of %lld %s or more is in the catalogues: add one", package, kind, (long long)r.lo, r.unit);
            else snprintf(m, sizeof m, "no %s %s part between %lld and %lld %s is in the catalogues: add one", package, kind, (long long)r.lo, (long long)r.hi, r.unit);
            add(c, COMPOSE_NOT_IMPL, "NO_PART_FOR_VALUE", where, m);
            continue;
        }
        const PartRating *rt = part_record_find_rating(out[k], rating_for(r.unit), PART_BOUND_NOMINAL);
        value(c, where, fmt(c, "%s (%s)", out[k]->mpn ? out[k]->mpn : out[k]->id, num(c, rt ? rt->value : 0, r.unit), NULL), "chosen",
              "the catalogue part closest to the computed value within its limits");
    }
    return out;
}

/* ---------------------------------------------------------------------------------------------- KiCad symbols */

typedef struct SymLib { const char *lib; bool ok; KicadSymbolLibrary parsed; } SymLib;

static const KicadSymbolLibrary *symbol_library(Ctx *c, SymLib *cache, size_t *count, size_t cap, const char *lib)
{
    for (size_t i = 0; i < *count; i++) if (!strcmp(cache[i].lib, lib)) return cache[i].ok ? &cache[i].parsed : NULL;
    if (*count == cap) return NULL;
    SymLib *s = &cache[(*count)++];
    memset(s, 0, sizeof *s);
    s->lib = lib;
    char file[FS_PATH_MAX], name[FS_PATH_MAX];
    int n = snprintf(name, sizeof name, "%s.kicad_sym", lib);
    const unsigned char *data = NULL;
    size_t size = 0;
    KicadAssetError e;
    memset(&e, 0, sizeof e);
    s->ok = n > 0 && (size_t)n < sizeof name && fs_path_join(file, sizeof file, c->in->symbols_root, name) == FS_OK &&
            fs_read_file_arena(c->a, file, &data, &size) == FS_OK &&
            kicad_symbol_library_parse(c->a, (const char *)data, size, &s->parsed, &e) == KICAD_ASSET_OK;
    return s->ok ? &s->parsed : NULL;
}

/* ---------------------------------------------------------------------------------------------- intent */

/* The net of a block-level target ("nc", a port, or an internal net) for instance `inst`; NULL = unconnected. */
static const char *target_net(Ctx *c, size_t inst, const char *target, const size_t *port_pins)
{
    if (!target || !strcmp(target, "nc")) return NULL;
    for (size_t i = 0; i < c->nport; i++) {
        const Port *p = &c->ports[i];
        if (p->inst != inst || strcmp(p->name, target)) continue;
        if (p->net != NO_NET) return c->nets[p->net].name;
        if (port_pins[i] < 2) return NULL;          /* an unconnected port that touches one pin: no net */
        char buf[96];
        snprintf(buf, sizeof buf, "%s_%s", c->insts[inst].id, p->name);
        upper(buf);
        return fmt(c, "%s", buf, NULL, NULL);
    }
    char buf[96];
    snprintf(buf, sizeof buf, "%s_%s", c->insts[inst].id, target);
    upper(buf);
    return fmt(c, "%s", buf, NULL, NULL);
}

/* References in block order, then part order, numbered per prefix across the design (R1, R2, C1...). One helper, so
 * the intent and the layout hints always name a part the same way. */
static const char ***assign_refs(Ctx *c)
{
    const char ***refs = arena_alloc_array(c->a, sizeof *refs, c->ninst ? c->ninst : 1);
    if (!refs) { c->oom = true; return NULL; }
    struct { const char *prefix; size_t n; } counters[32];
    size_t ncounters = 0;
    for (size_t i = 0; i < c->ninst; i++) {
        const JsonValue *parts = json_get(c->insts[i].root, "parts");
        size_t n = parts ? parts->count : 0;
        refs[i] = n ? arena_alloc_array(c->a, sizeof **refs, n) : NULL;
        if (n && !refs[i]) { c->oom = true; return NULL; }
        for (size_t k = 0; k < n; k++) {
            const char *prefix = str(json_get(parts->items[k], "prefix"));
            size_t *ctr = NULL;
            for (size_t q = 0; q < ncounters; q++) if (!strcmp(counters[q].prefix, prefix)) ctr = &counters[q].n;
            if (!ctr && ncounters < N(counters)) { counters[ncounters].prefix = prefix; counters[ncounters].n = 0; ctr = &counters[ncounters++].n; }
            if (!ctr) { add(c, COMPOSE_INVALID, "TOO_MANY_PREFIXES", c->insts[i].id, "more than 32 reference prefixes"); return NULL; }
            char ref[16];
            snprintf(ref, sizeof ref, "%s%zu", prefix, ++*ctr);
            refs[i][k] = fmt(c, "%s", ref, NULL, NULL);
        }
    }
    return refs;
}

/* Layout hints: the blocks' part keep-outs, and each block capacitor between a power port and ground with the block's
 * IC on that net (decoupling: the placer keeps them close). */
static void write_hints(Ctx *c, const char ***refs)
{
    JsonWriter w;
    json_writer_init(&w, c->a);
    json_write_begin_object(&w);
    json_write_key(&w, "format"); json_write_string(&w, "copper.layout-hints/1");
    json_write_key(&w, "keepouts");
    json_write_begin_array(&w);
    for (size_t i = 0; i < c->ninst; i++) {
        const JsonValue *parts = json_get(c->insts[i].root, "parts");
        for (size_t k = 0; parts && k < parts->count; k++) {
            const JsonValue *kos = json_get(parts->items[k], "keepouts");
            for (size_t q = 0; kos && q < kos->count; q++) {
                const JsonValue *ko = kos->items[q];
                int64_t v[4] = {0, 0, 0, 0};
                int_of(json_get(ko, "x1_um"), &v[0]); int_of(json_get(ko, "y1_um"), &v[1]);
                int_of(json_get(ko, "x2_um"), &v[2]); int_of(json_get(ko, "y2_um"), &v[3]);
                json_write_begin_object(&w);
                json_write_key(&w, "reference"); json_write_string(&w, refs[i][k]);
                json_write_key(&w, "rect_um");
                json_write_begin_array(&w);
                for (int e = 0; e < 4; e++) json_write_int(&w, v[e]);
                json_write_end_array(&w);
                const char *why = source_doc(c->insts[i].root, str(json_get(ko, "source")));
                json_write_key(&w, "why"); json_write_string(&w, why ? why : "");
                json_write_end_object(&w);
            }
        }
    }
    json_write_end_array(&w);
    json_write_key(&w, "decoupling");
    json_write_begin_array(&w);
    for (size_t i = 0; i < c->ninst; i++) {
        const JsonValue *parts = json_get(c->insts[i].root, "parts");
        const char *ic = NULL;
        for (size_t k = 0; parts && k < parts->count; k++)
            if (!ic && str(json_get(parts->items[k], "prefix")) && !strcmp(str(json_get(parts->items[k], "prefix")), "U")) ic = refs[i][k];
        for (size_t k = 0; ic && parts && k < parts->count; k++) {
            if (strcmp(str(json_get(parts->items[k], "prefix")), "C")) continue;
            const JsonValue *pins = json_get(parts->items[k], "pins");
            const char *power_net = NULL;
            bool to_ground = false;
            for (size_t t = 0; pins && t < pins->count; t++) {
                size_t p = NO_NET;
                for (size_t q = 0; q < c->nport; q++)
                    if (c->ports[q].inst == i && str(pins->items[t]) && !strcmp(c->ports[q].name, str(pins->items[t]))) p = q;
                if (p == NO_NET) continue;
                if (kind_is(&c->ports[p], "ground")) to_ground = true;
                else if ((kind_is(&c->ports[p], "power_in") || kind_is(&c->ports[p], "power_out")) && c->ports[p].net != NO_NET)
                    power_net = c->nets[c->ports[p].net].name;
            }
            if (!power_net || !to_ground) continue;
            json_write_begin_object(&w);
            json_write_key(&w, "capacitor"); json_write_string(&w, refs[i][k]);
            json_write_key(&w, "net"); json_write_string(&w, power_net);
            json_write_key(&w, "ic"); json_write_string(&w, ic);
            json_write_end_object(&w);
        }
    }
    json_write_end_array(&w);
    /* Step 3d: nets fed from outside the board (a port with "external_supply"): the schematic step puts one
       KiCad PWR_FLAG on each, so ERC knows the net is driven. Nowhere else: an undriven net stays an ERC error. */
    json_write_key(&w, "power_flags");
    json_write_begin_array(&w);
    for (size_t n = 0; n < c->nnet; n++) {
        size_t by = NO_NET;
        for (size_t q = 0; q < c->nets[n].count && by == NO_NET; q++)
            if (c->ports[c->nets[n].ports[q]].external) by = c->nets[n].ports[q];
        if (by == NO_NET || !c->nets[n].name) continue;
        json_write_begin_object(&w);
        json_write_key(&w, "net"); json_write_string(&w, c->nets[n].name);
        json_write_key(&w, "port"); json_write_string(&w, pname(c, by));
        json_write_key(&w, "why"); json_write_string(&w, "this port is an external supply (external_supply in its block): power enters the board here");
        json_write_end_object(&w);
    }
    json_write_end_array(&w);
    json_write_end_object(&w);
    c->r->hints_json = json_writer_finish(&w, &c->r->hints_len);
    if (!c->r->hints_json) c->oom = true;
}

static void write_intent(Ctx *c, const JsonValue *project, const PartRecord ***chosen)
{
    const char ***refs = assign_refs(c);
    if (!refs) return;
    /* physical pins per port, to decide whether an unconnected port still joins two pins */
    size_t *port_pins = c->nport ? arena_alloc_array(c->a, sizeof(size_t), c->nport) : NULL;
    if (c->nport && !port_pins) { c->oom = true; return; }
    for (size_t i = 0; i < c->nport; i++) port_pins[i] = 0;
    for (size_t i = 0; i < c->ninst; i++) {
        const JsonValue *parts = json_get(c->insts[i].root, "parts");
        for (size_t k = 0; parts && k < parts->count; k++) {
            const PartRecord *pr = chosen[i][k];
            const JsonValue *pins = json_get(parts->items[k], "pins");
            for (size_t t = 0; pr && t < pr->pin_count; t++) {
                const char *target = str(json_get(pins, pr->pins[t].terminal));
                for (size_t p = 0; target && p < c->nport; p++)
                    if (c->ports[p].inst == i && !strcmp(c->ports[p].name, target)) port_pins[p]++;
            }
        }
    }
    SymLib cache[64];
    size_t cached = 0;
    JsonWriter w;
    json_writer_init(&w, c->a);
    json_write_begin_object(&w);
    json_write_key(&w, "schema"); json_write_string(&w, "copper.design-intent");
    json_write_key(&w, "schema_version"); json_write_uint(&w, 1);
    json_write_key(&w, "project");
    json_write_begin_object(&w);
    json_write_key(&w, "name"); json_write_string(&w, str(json_get(project, "name")));
    json_write_key(&w, "board");
    json_write_begin_object(&w);
    int64_t bw = 0, bh = 0;
    int_of(json_get(json_get(project, "board"), "width_mm"), &bw);
    int_of(json_get(json_get(project, "board"), "height_mm"), &bh);
    json_write_key(&w, "width_mm"); json_write_int(&w, bw);
    json_write_key(&w, "height_mm"); json_write_int(&w, bh);
    json_write_key(&w, "layers"); json_write_uint(&w, 2);
    json_write_end_object(&w);
    json_write_end_object(&w);
    json_write_key(&w, "components");
    json_write_begin_array(&w);
    for (size_t i = 0; i < c->ninst && !c->oom; i++) {
        const JsonValue *parts = json_get(c->insts[i].root, "parts");
        for (size_t k = 0; parts && k < parts->count; k++) {
            const PartRecord *pr = chosen[i][k];
            const JsonValue *bp = parts->items[k];
            if (!pr || !pr->symbol_id || !pr->footprint_id) {
                add(c, COMPOSE_INVALID, "NO_PART", c->insts[i].id, "a block part has no catalogue part with a symbol and footprint");
                return;
            }
            const char *ref = refs[i][k];
            /* the KiCad symbol; a derived symbol is written as its base, which has the same pins */
            char lib[128];
            const char *colon = strchr(pr->symbol_id, ':');
            size_t ln = colon ? (size_t)(colon - pr->symbol_id) : 0;
            if (!colon || ln >= sizeof lib) { add(c, COMPOSE_INVALID, "BAD_SYMBOL_ID", ref, pr->symbol_id); return; }
            memcpy(lib, pr->symbol_id, ln);
            lib[ln] = 0;
            const KicadSymbolLibrary *sl = symbol_library(c, cache, &cached, N(cache), fmt(c, "%s", lib, NULL, NULL));
            const KicadSymbolAsset *sym = sl ? kicad_symbol_find(sl, colon + 1) : NULL;
            const char *symbol_id = pr->symbol_id;
            for (int depth = 0; sym && sym->extends && depth < 8; depth++) {
                const KicadSymbolAsset *parent = kicad_symbol_find(sl, sym->extends);
                if (!parent) { sym = NULL; break; }
                symbol_id = fmt(c, "%s:%s", lib, parent->name, NULL);
                sym = parent;
            }
            if (!sym) { add(c, COMPOSE_INVALID, "SYMBOL_NOT_FOUND", ref, fmt(c, "%s is not in the symbol libraries", pr->symbol_id, NULL, NULL)); return; }
            if (symbol_id != pr->symbol_id)
                add(c, COMPOSE_INFO, "BASE_SYMBOL", ref, fmt(c, "%s derives from %s in KiCad; the intent uses %s, which has the same pins", pr->symbol_id, symbol_id, symbol_id));
            json_write_begin_object(&w);
            json_write_key(&w, "reference"); json_write_string(&w, ref);
            json_write_key(&w, "value"); json_write_string(&w, pr->mpn ? pr->mpn : pr->id);
            json_write_key(&w, "symbol_id"); json_write_string(&w, symbol_id);
            json_write_key(&w, "footprint_id"); json_write_string(&w, pr->footprint_id);
            json_write_key(&w, "pins");
            json_write_begin_array(&w);
            const JsonValue *pins = json_get(bp, "pins");
            for (size_t s = 0; s < sym->pin_count; s++) {
                const KicadSymbolPin *sp = &sym->pins[s];
                if (sp->body_style > 1) continue;
                bool dup = false;
                for (size_t q = 0; q < s; q++) dup |= sym->pins[q].body_style <= 1 && !strcmp(sym->pins[q].number, sp->number);
                if (dup) continue;
                const char *net = NULL;
                for (size_t t = 0; t < pr->pin_count; t++)
                    if (!strcmp(pr->pins[t].symbol_pin, sp->number)) net = target_net(c, i, str(json_get(pins, pr->pins[t].terminal)), port_pins);
                json_write_begin_object(&w);
                json_write_key(&w, "symbol_pin"); json_write_string(&w, sp->number);
                json_write_key(&w, "pin_name"); json_write_string(&w, sp->name ? sp->name : "");
                json_write_key(&w, "pad"); json_write_string(&w, sp->number);
                json_write_key(&w, "net"); if (net) json_write_string(&w, net); else json_write_null(&w);
                json_write_end_object(&w);
            }
            json_write_end_array(&w);
            json_write_end_object(&w);
            c->r->components++;
        }
    }
    json_write_end_array(&w);
    json_write_end_object(&w);
    c->r->intent_json = json_writer_finish(&w, &c->r->intent_len);
    if (!c->r->intent_json) { c->oom = true; return; }
    write_hints(c, refs);
}

/* ---------------------------------------------------------------------------------------------- entry point */

static ComposeStatus finish(Ctx *c)
{
    ComposeReport *r = c->r;
    if (c->oom) return r->status = COMPOSE_ERR_MEMORY;
    bool inv = false, fail = false, ni = false, q = false;
    for (size_t i = 0; i < r->finding_count; i++) {
        inv |= r->findings[i].level == COMPOSE_INVALID;
        fail |= r->findings[i].level == COMPOSE_FAIL;
        ni |= r->findings[i].level == COMPOSE_NOT_IMPL;
        q |= r->findings[i].level == COMPOSE_QUESTION;
    }
    r->status = inv ? COMPOSE_INVALID_INPUT : fail ? COMPOSE_REFUSED : ni ? COMPOSE_NOT_IMPLEMENTED : q ? COMPOSE_NEEDS_INPUT : COMPOSE_COMPOSED;
    return r->status;
}

ComposeStatus compose_design(Arena *arena, const char *text, size_t length, const ComposeInputs *in, ComposeReport *out)
{
    if (!out) return COMPOSE_ERR_ARGUMENT;
    memset(out, 0, sizeof *out);
    out->status = COMPOSE_ERR_ARGUMENT;
    if (!arena || !text || !in || !in->blocks_dir || !in->symbols_root || (in->catalogue_count && !in->catalogues)) return COMPOSE_ERR_ARGUMENT;
    Ctx c;
    memset(&c, 0, sizeof c);
    c.a = arena;
    c.r = out;
    c.in = in;

    JsonValue *root = NULL;
    JsonError je;
    memset(&je, 0, sizeof je);
    if (json_parse(arena, text, length, &root, &je) != JSON_OK) {
        char m[200];
        snprintf(m, sizeof m, "%s at line %zu, column %zu", je.message ? je.message : "JSON error", je.line, je.column);
        add(&c, COMPOSE_INVALID, "JSON", "", m);
        return finish(&c);
    }
    static const char *const top[] = {"format", "project", "blocks", "nets", "evidence"};
    static const char *const pkeys[] = {"name", "board"}, *const bkeys[] = {"width_mm", "height_mm"};
    static const char *const ikeys[] = {"id", "block", "parameters"};
    static const char *const ekeys[] = {"field", "quote"};
    if (!keys_ok(&c, root, "", top, N(top))) return finish(&c);
    /* evidence: checked only when the caller gives the prompt */
    const JsonValue *ev = json_get(root, "evidence");
    if (ev) {
        if (ev->kind != JSON_ARRAY) add(&c, COMPOSE_INVALID, "TYPE", "evidence", "'evidence' must be an array of {field, quote}");
        else {
            for (size_t i = 0; i < ev->count; i++)
                if (keys_ok(&c, ev->items[i], "evidence", ekeys, N(ekeys)) && (!str(json_get(ev->items[i], "field")) || !str(json_get(ev->items[i], "quote"))))
                    add(&c, COMPOSE_INVALID, "TYPE", "evidence", "each evidence item has a string 'field' and a string 'quote'");
            c.evidence = ev;
        }
    }
    if (in->prompt) c.evidence_prompt = norm(&c, in->prompt);
    if (!str(json_get(root, "format")) || strcmp(str(json_get(root, "format")), "copper.compose/1"))
        add(&c, COMPOSE_INVALID, "FORMAT", "format", "format must be \"copper.compose/1\"");
    const JsonValue *project = json_get(root, "project");
    int64_t bw = 0, bh = 0;
    if (keys_ok(&c, project, "project", pkeys, N(pkeys))) {
        const char *name = str(json_get(project, "name"));
        bool name_ok = name && *name && strlen(name) <= 40;
        for (const char *p = name; name_ok && *p; p++)
            name_ok = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-';
        if (!name_ok) add(&c, COMPOSE_INVALID, "BAD_NAME", "project.name", "1-40 letters, digits, _ or -");
        const JsonValue *board = json_get(project, "board");
        if (!board) {
            add(&c, COMPOSE_QUESTION, "NEEDS_INPUT", "project.board", "the board size is not stated: give its width and height in mm");
        } else if (!keys_ok(&c, board, "project.board", bkeys, N(bkeys)) || !int_of(json_get(board, "width_mm"), &bw) ||
                   !int_of(json_get(board, "height_mm"), &bh) || bw < 20 || bw > 300 || bh < 15 || bh > 300) {
            add(&c, COMPOSE_INVALID, "BAD_BOARD", "project.board", "width_mm 20..300 and height_mm 15..300 (integers, as the compiler accepts)");
        } else if (!backed(&c, "project.board.width_mm", bw) || !backed(&c, "project.board.height_mm", bh)) {
            add(&c, COMPOSE_INFO, "IGNORED_VALUE", "project.board", "the board size is not backed by quotes from the prompt that contain it: ignored");
            add(&c, COMPOSE_QUESTION, "NEEDS_INPUT", "project.board", "the board size is not stated: give its width and height in mm");
        }
    }
    /* blocks: each loaded from the library and checked VALID against the catalogues */
    const JsonValue *blocks = json_get(root, "blocks");
    if (!blocks || blocks->kind != JSON_ARRAY || !blocks->count) { add(&c, COMPOSE_INVALID, "MISSING_KEY", "blocks", "at least one block is required"); return finish(&c); }
    c.insts = arena_alloc_array(arena, sizeof *c.insts, blocks->count);
    if (!c.insts) { c.oom = true; return finish(&c); }
    for (size_t i = 0; i < blocks->count; i++) {
        char where[32];
        snprintf(where, sizeof where, "blocks[%zu]", i);
        const JsonValue *b = blocks->items[i];
        if (!keys_ok(&c, b, where, ikeys, N(ikeys))) continue;
        const char *id = str(json_get(b, "id")), *name = str(json_get(b, "block"));
        if (!identifier(id) || !identifier(name)) { add(&c, COMPOSE_INVALID, "BAD_NAME", where, "'id' and 'block' must match [a-z][a-z0-9_]*"); continue; }
        bool dup = false;
        for (size_t k = 0; k < c.ninst; k++) dup |= !strcmp(c.insts[k].id, id);
        if (dup) { add(&c, COMPOSE_INVALID, "DUPLICATE", where, fmt(&c, "block id '%s' is used twice", id, NULL, NULL)); continue; }
        char file[FS_PATH_MAX], fname[FS_PATH_MAX];
        snprintf(fname, sizeof fname, "%s.block.json", name);
        const unsigned char *data = NULL;
        size_t size = 0;
        if (fs_path_join(file, sizeof file, in->blocks_dir, fname) != FS_OK || fs_read_file_arena(arena, file, &data, &size) != FS_OK) {
            add(&c, COMPOSE_INVALID, "UNKNOWN_BLOCK", where, fmt(&c, "no block '%s' in the block library", name, NULL, NULL));
            continue;
        }
        BlockReport br;
        BlockStatus bs = block_check_text(arena, (const char *)data, size, in->catalogues, in->catalogue_count, &br);
        if (bs != BLOCK_VALID) {
            add(&c, COMPOSE_INVALID, "BLOCK_NOT_VALID", where,
                fmt(&c, "block '%s' is %s against the catalogues: run copper block check", name, block_status_name(bs), NULL));
            continue;
        }
        JsonValue *broot = NULL;
        if (json_parse(arena, (const char *)data, size, &broot, &je) != JSON_OK) { c.oom = true; return finish(&c); }
        const JsonValue *given = json_get(b, "parameters");
        if (given && given->kind != JSON_OBJECT) { add(&c, COMPOSE_INVALID, "TYPE", where, "'parameters' must be an object"); continue; }
        Inst *x = &c.insts[c.ninst++];
        x->id = id;
        x->block = name;
        x->root = broot;
        x->given = given;
    }
    out->blocks = c.ninst;
    if (c.oom) return finish(&c);
    for (size_t i = 0; i < out->finding_count; i++) if (out->findings[i].level == COMPOSE_INVALID) return finish(&c);

    load_ports(&c);
    load_nets(&c, json_get(root, "nets"));
    out->nets = c.nnet;
    init_params(&c);
    for (size_t i = 0; i < out->finding_count; i++) if (out->findings[i].level == COMPOSE_INVALID) return finish(&c);
    net_voltages(&c);
    check_nets(&c);
    check_currents(&c);
    derive_params(&c);
    const PartRecord ***chosen = arena_alloc_array(arena, sizeof *chosen, c.ninst);
    if (!chosen) { c.oom = true; return finish(&c); }
    for (size_t i = 0; i < c.ninst; i++) chosen[i] = choose_parts(&c, i);
    /* formulas not used by a part are still computed and reported (for example a regulator's dissipation) */
    for (size_t i = 0; i < c.ninst; i++) {
        const JsonValue *fs = json_get(c.insts[i].root, "formulas");
        const JsonValue *parts = json_get(c.insts[i].root, "parts");
        for (size_t k = 0; fs && k < fs->count; k++) {
            const char *fname = str(json_get(fs->items[k], "name"));
            bool used = false;
            for (size_t p = 0; parts && p < parts->count; p++)
                used |= str(json_get(json_get(json_get(parts->items[p], "select"), "value"), "formula")) &&
                        !strcmp(str(json_get(json_get(json_get(parts->items[p], "select"), "value"), "formula")), fname);
            if (!used) eval_formula(&c, i, fname);
        }
    }
    if (finish(&c) != COMPOSE_COMPOSED) return out->status;
    write_intent(&c, project, chosen);
    return finish(&c);
}

/* ---------------------------------------------------------------------------------------------- report */

const char *compose_report_json(Arena *arena, const ComposeReport *r, size_t *length)
{
    JsonWriter w;
    json_writer_init(&w, arena);
    json_write_begin_object(&w);
    json_write_key(&w, "format"); json_write_string(&w, "copper.compose-report/1");
    json_write_key(&w, "status"); json_write_string(&w, compose_status_name(r->status));
    json_write_key(&w, "blocks"); json_write_uint(&w, r->blocks);
    json_write_key(&w, "nets"); json_write_uint(&w, r->nets);
    json_write_key(&w, "components"); json_write_uint(&w, r->components);
    json_write_key(&w, "findings");
    json_write_begin_array(&w);
    for (size_t i = 0; i < r->finding_count; i++) {
        json_write_begin_object(&w);
        json_write_key(&w, "level"); json_write_string(&w, compose_level_name(r->findings[i].level));
        json_write_key(&w, "code"); json_write_string(&w, r->findings[i].code);
        json_write_key(&w, "where"); json_write_string(&w, r->findings[i].where);
        json_write_key(&w, "message"); json_write_string(&w, r->findings[i].message);
        json_write_end_object(&w);
    }
    json_write_end_array(&w);
    json_write_key(&w, "values");
    json_write_begin_array(&w);
    for (size_t i = 0; i < r->value_count; i++) {
        json_write_begin_object(&w);
        json_write_key(&w, "where"); json_write_string(&w, r->values[i].where);
        json_write_key(&w, "value"); json_write_string(&w, r->values[i].value);
        json_write_key(&w, "origin"); json_write_string(&w, r->values[i].origin);
        json_write_key(&w, "source"); json_write_string(&w, r->values[i].source);
        json_write_end_object(&w);
    }
    json_write_end_array(&w);
    json_write_end_object(&w);
    return json_writer_finish(&w, length);
}

const char *compose_status_name(ComposeStatus s)
{
    switch (s) {
    case COMPOSE_COMPOSED: return "COMPOSED";
    case COMPOSE_NEEDS_INPUT: return "NEEDS_INPUT";
    case COMPOSE_NOT_IMPLEMENTED: return "NOT_IMPLEMENTED";
    case COMPOSE_REFUSED: return "REFUSED";
    case COMPOSE_INVALID_INPUT: return "INVALID_INPUT";
    case COMPOSE_ERR_ARGUMENT: return "ERR_ARGUMENT";
    case COMPOSE_ERR_MEMORY: return "ERR_MEMORY";
    }
    return "UNKNOWN";
}

const char *compose_level_name(ComposeLevel l)
{
    switch (l) {
    case COMPOSE_FAIL: return "FAIL";
    case COMPOSE_INVALID: return "INVALID";
    case COMPOSE_NOT_IMPL: return "NOT_IMPLEMENTED";
    case COMPOSE_QUESTION: return "NEEDS_INPUT";
    case COMPOSE_UNVERIFIED: return "UNVERIFIED";
    case COMPOSE_INFO: return "INFO";
    }
    return "UNKNOWN";
}

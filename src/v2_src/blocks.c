/* blocks.c - see blocks.h. Loads and checks "copper.block/1" definitions; designs nothing, fills in nothing. */
#include "blocks.h"

#include "filesystem.h"
#include "json.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------------------------- tables */

static const BlockFunction FUNCTIONS[] = {
    {"led_series_resistor", "uohm", "R = (V_drive - V_F) / I_F, rounded up to the next E24 value (Ohm's law)",
     {{"v_drive", "uV"}, {"v_forward", "uV"}, {"current", "uA"}}, 3},
    {"i2c_pullup", "uohm",
     "NXP UM10204: Rp(min) = (VDD - VOL) / IOL, Rp(max) = tr / (0.8473 Cb); the largest E24 value in range",
     {{"vdd", "uV"}, {"vol", "uV"}, {"iol", "uA"}, {"rise_time", "ns"}, {"bus_capacitance", "pF"}}, 5},
    {"ldo_dissipation", "uW", "P = (V_in - V_out) * I_out", {{"vin", "uV"}, {"vout", "uV"}, {"current", "uA"}}, 3},
    {"rc_time_constant", "ns", "tau = R * C", {{"resistance", "uohm"}, {"capacitance", "pF"}}, 2},
};

static const char *const UNITS[] = {"uV", "uA", "uW", "uohm", "pF", "nH", "mC", "ppm", "ns"};
static const char *const PORT_KINDS[] = {"power_in", "power_out", "ground", "gpio", "i2c_scl", "i2c_sda",
                                         "uart_tx", "uart_rx", "analog", "signal"};
/* Part kinds (part_catalogue.c names) and the reference prefixes that go with them. */
static const struct { const char *kind, *prefix, *value_unit; } KINDS[] = {
    {"R", "R", "uohm"}, {"C", "C", "pF"}, {"L", "L", "nH"}, {"D", "D", NULL}, {"Q", "Q", NULL},
    {"M", "Q", NULL},   {"U", "U", NULL}, {"J", "J", NULL}, {"S", "SW", NULL},
};

#define N(a) (sizeof(a) / sizeof((a)[0]))

const BlockFunction *block_functions(size_t *count)
{
    if (count) *count = N(FUNCTIONS);
    return FUNCTIONS;
}

/* ---------------------------------------------------------------------------------------------- context */

/* `unit` holds a parameter's unit or a port's kind; `ref`/`what` hold a parameter's "from" port and quantity, or a
 * port's "level_port", checked once every port is known. */
typedef struct Name { const char *name; const char *unit; size_t uses; const char *ref; const char *what; } Name;
typedef struct Names { Name *items; size_t count, capacity; } Names;

typedef struct Ctx {
    Arena *a;
    BlockReport *r;
    size_t capacity;
    bool oom;
    const PartCatalogue *const *cats;
    size_t ncats;
    Names sources, params, ports, nets, formulas, roles;
} Ctx;

static char *dup_text(Arena *a, const char *s)
{
    size_t n = strlen(s);
    char *d = arena_alloc(a, n + 1);
    if (d) memcpy(d, s, n + 1);
    return d;
}

static void add(Ctx *c, const char *code, const char *path, const char *message)
{
    BlockReport *r = c->r;
    if (r->finding_count == c->capacity) {
        size_t cap = c->capacity ? c->capacity * 2 : 16;
        BlockFinding *more = arena_alloc_array(c->a, sizeof *more, cap);
        if (!more) { c->oom = true; return; }
        if (r->finding_count) memcpy(more, r->findings, r->finding_count * sizeof *more);
        r->findings = more;
        c->capacity = cap;
    }
    BlockFinding *f = &r->findings[r->finding_count];
    f->code = code;
    f->path = dup_text(c->a, path);
    f->message = dup_text(c->a, message);
    if (!f->path || !f->message) { c->oom = true; return; }
    r->finding_count++;
}

static void addf(Ctx *c, const char *code, const char *path, const char *fmt, const char *x, const char *y)
{
    char m[400];
    snprintf(m, sizeof m, fmt, x ? x : "", y ? y : "");
    add(c, code, path, m);
}

static const char *pathf(Ctx *c, const char *fmt, size_t i, const char *key)
{
    char p[200];
    snprintf(p, sizeof p, fmt, i, key ? key : "");
    const char *d = dup_text(c->a, p);
    if (!d) c->oom = true;
    return d ? d : "";
}

static Name *find(Names *n, const char *name)
{
    for (size_t i = 0; i < n->count; i++) if (!strcmp(n->items[i].name, name)) return &n->items[i];
    return NULL;
}

static Name *put(Ctx *c, Names *n, const char *name, const char *unit)
{
    if (n->count == n->capacity) {
        size_t cap = n->capacity ? n->capacity * 2 : 16;
        Name *more = arena_alloc_array(c->a, sizeof *more, cap);
        if (!more) { c->oom = true; return NULL; }
        if (n->count) memcpy(more, n->items, n->count * sizeof *more);
        n->items = more;
        n->capacity = cap;
    }
    Name *x = &n->items[n->count++];
    x->name = name;
    x->unit = unit;
    x->uses = 0;
    x->ref = NULL;
    x->what = NULL;
    return x;
}

static bool in(const char *s, const char *const *set, size_t n)
{
    for (size_t i = 0; s && i < n; i++) if (!strcmp(s, set[i])) return true;
    return false;
}

static bool identifier(const char *s)
{
    if (!s || !(*s >= 'a' && *s <= 'z')) return false;
    for (const char *p = s + 1; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) return false;
    return true;
}

static bool prefix_ok(const char *s)
{
    size_t n = s ? strlen(s) : 0;
    if (n < 1 || n > 3) return false;
    for (size_t i = 0; i < n; i++) if (!(s[i] >= 'A' && s[i] <= 'Z')) return false;
    return true;
}

static const char *str(const JsonValue *v) { return v && v->kind == JSON_STRING ? v->text : NULL; }

/* Every key of `obj` must be in `allowed`; every key in `required` must be present. */
static bool object_keys(Ctx *c, const JsonValue *obj, const char *path, const char *const *allowed, size_t na,
                        const char *const *required, size_t nr)
{
    if (!obj || obj->kind != JSON_OBJECT) {
        add(c, "TYPE", path, "must be a JSON object");
        return false;
    }
    for (size_t i = 0; i < obj->count; i++)
        if (!in(obj->keys[i], allowed, na))
            addf(c, "UNKNOWN_KEY", path, "unknown key '%s': the format has no such field (a misspelt key would be lost)", obj->keys[i], NULL);
    bool ok = true;
    for (size_t i = 0; i < nr; i++)
        if (!json_get(obj, required[i])) { addf(c, "MISSING_KEY", path, "'%s' is required", required[i], NULL); ok = false; }
    return ok;
}

static bool integer(Ctx *c, const JsonValue *v, const char *path, int64_t *out)
{
    if (!v) return false;
    if (v->kind != JSON_NUMBER || json_to_i64(v, out) != JSON_NUMBER_OK) {
        add(c, "BAD_NUMBER", path, "must be an integer in the unit named (no decimals, no exponent)");
        return false;
    }
    return true;
}

static void source_ref(Ctx *c, const JsonValue *v, const char *path)
{
    const char *s = str(v);
    if (!s) { add(c, "NO_SOURCE", path, "a source id is required (PIPELINE_HARD_RULES.md H2)"); return; }
    Name *n = find(&c->sources, s);
    if (!n) addf(c, "UNKNOWN_SOURCE", path, "source '%s' is not in \"sources\"", s, NULL);
    else n->uses++;
}

/* ---------------------------------------------------------------------------------------------- sections */

static void check_sources(Ctx *c, const JsonValue *arr)
{
    static const char *const allowed[] = {"id", "document", "url", "standard", "revision", "page"};
    static const char *const required[] = {"id", "document"};
    if (!arr || arr->kind != JSON_ARRAY || arr->count == 0) { add(c, "MISSING_KEY", "sources", "at least one source is required"); return; }
    for (size_t i = 0; i < arr->count; i++) {
        const JsonValue *s = arr->items[i];
        const char *p = pathf(c, "sources[%zu]%s", i, NULL);
        if (!object_keys(c, s, p, allowed, N(allowed), required, N(required))) continue;
        const char *id = str(json_get(s, "id"));
        if (!identifier(id)) { add(c, "BAD_NAME", p, "source id must match [a-z][a-z0-9_]*"); continue; }
        if (find(&c->sources, id)) { addf(c, "DUPLICATE", p, "source id '%s' is used twice", id, NULL); continue; }
        if (!str(json_get(s, "document")) || !*str(json_get(s, "document"))) add(c, "MISSING_KEY", p, "'document' must be a non-empty string");
        if (!str(json_get(s, "url")) && !str(json_get(s, "standard")))
            add(c, "MISSING_KEY", p, "a source needs a 'url' or a 'standard' saying where it can be found");
        int64_t page = 0;
        if (json_get(s, "page") && integer(c, json_get(s, "page"), p, &page) && page <= 0) add(c, "BAD_RANGE", p, "'page' must be positive");
        put(c, &c->sources, id, NULL);
    }
}

static void check_parameters(Ctx *c, const JsonValue *arr)
{
    static const char *const allowed[] = {"name", "unit", "required", "default", "source", "min", "max", "from"};
    static const char *const required[] = {"name", "unit", "required"};
    static const char *const from_keys[] = {"port", "quantity"};
    static const char *const quantities[] = {"voltage_min", "voltage_max", "current"};
    if (!arr) return;
    if (arr->kind != JSON_ARRAY) { add(c, "TYPE", "parameters", "must be an array"); return; }
    for (size_t i = 0; i < arr->count; i++) {
        const JsonValue *v = arr->items[i];
        const char *p = pathf(c, "parameters[%zu]%s", i, NULL);
        if (!object_keys(c, v, p, allowed, N(allowed), required, N(required))) continue;
        const char *name = str(json_get(v, "name")), *unit = str(json_get(v, "unit"));
        if (!identifier(name)) { add(c, "BAD_NAME", p, "parameter name must match [a-z][a-z0-9_]*"); continue; }
        if (find(&c->params, name)) { addf(c, "DUPLICATE", p, "parameter '%s' is declared twice", name, NULL); continue; }
        if (!in(unit, UNITS, N(UNITS))) addf(c, "BAD_UNIT", p, "unit '%s' is not one of uV uA uW uohm pF nH mC ppm ns", unit, NULL);
        const JsonValue *req = json_get(v, "required");
        bool is_required = req && req->kind == JSON_TRUE;
        if (!req || (req->kind != JSON_TRUE && req->kind != JSON_FALSE)) add(c, "TYPE", p, "'required' must be true or false");
        int64_t lo = 0, hi = 0, def = 0;
        bool has_lo = json_get(v, "min") && integer(c, json_get(v, "min"), p, &lo);
        bool has_hi = json_get(v, "max") && integer(c, json_get(v, "max"), p, &hi);
        bool has_def = json_get(v, "default") && integer(c, json_get(v, "default"), p, &def);
        if (has_lo && has_hi && lo > hi) add(c, "BAD_RANGE", p, "'min' is above 'max'");
        if (is_required) {
            if (json_get(v, "default")) add(c, "CONFLICT", p, "a required parameter must come from the request, so it has no default");
        } else if (!json_get(v, "default")) {
            add(c, "MISSING_KEY", p, "a parameter that is not required needs a 'default' and its 'source'");
        } else {
            source_ref(c, json_get(v, "source"), p);
            if (has_def && ((has_lo && def < lo) || (has_hi && def > hi))) add(c, "BAD_RANGE", p, "'default' is outside 'min'..'max'");
        }
        Name *par = put(c, &c->params, name, unit);
        /* "from": the composer derives the value from what a port is connected to (checked against the ports later) */
        const JsonValue *from = json_get(v, "from");
        if (from && object_keys(c, from, p, from_keys, N(from_keys), from_keys, N(from_keys))) {
            const char *port = str(json_get(from, "port")), *qty = str(json_get(from, "quantity"));
            if (!is_required) add(c, "CONFLICT", p, "a parameter derived with 'from' must be required (the composer fills it in)");
            if (!in(qty, quantities, N(quantities))) {
                addf(c, "BAD_KIND", p, "'from.quantity' '%s' is not voltage_min, voltage_max or current", qty, NULL);
            } else if ((!strcmp(qty, "current") && (!unit || strcmp(unit, "uA"))) || (strcmp(qty, "current") && (!unit || strcmp(unit, "uV")))) {
                addf(c, "UNIT_MISMATCH", p, "a parameter derived from a port's %s must be in %s", qty, strcmp(qty, "current") ? "uV" : "uA");
            }
            if (par && port) { par->ref = port; par->what = qty; }
        }
    }
}

/* After all ports are known: a parameter's "from" port and a port's "level_port" must exist and be of a fitting kind. */
static void check_references(Ctx *c)
{
    for (size_t i = 0; i < c->params.count; i++) {
        const Name *par = &c->params.items[i];
        if (!par->ref) continue;
        char where[160];
        snprintf(where, sizeof where, "parameters.%s.from", par->name);
        const char *p = dup_text(c->a, where);
        if (!p) { c->oom = true; return; }
        const Name *port = find(&c->ports, par->ref);
        if (!port) { addf(c, "UNKNOWN_PORT", p, "'from' names port '%s', which is not declared", par->ref, NULL); continue; }
        if (port->unit && !strcmp(port->unit, "ground")) addf(c, "BAD_KIND", p, "a ground port '%s' carries no voltage or current", par->ref, NULL);
        else if (par->what && !strcmp(par->what, "current") && (!port->unit || strcmp(port->unit, "power_out")))
            addf(c, "CONFLICT", p, "the current drawn from a net is read on a power_out port; '%s' is not one", par->ref, NULL);
    }
    for (size_t i = 0; i < c->ports.count; i++) {
        const Name *port = &c->ports.items[i];
        if (port->what) {
            char where[160];
            snprintf(where, sizeof where, "ports.%s.current_follows", port->name);
            const char *p = dup_text(c->a, where);
            if (!p) { c->oom = true; return; }
            const Name *out = find(&c->ports, port->what);
            if (!out) addf(c, "UNKNOWN_PORT", p, "current_follows '%s' is not declared", port->what, NULL);
            else if (!out->unit || strcmp(out->unit, "power_out")) addf(c, "BAD_KIND", p, "current_follows '%s' must be a power_out port", port->what, NULL);
        }
        if (!port->ref) continue;
        char where[160];
        snprintf(where, sizeof where, "ports.%s.level_port", port->name);
        const char *p = dup_text(c->a, where);
        if (!p) { c->oom = true; return; }
        const Name *level = find(&c->ports, port->ref);
        if (!level) addf(c, "UNKNOWN_PORT", p, "level_port '%s' is not declared", port->ref, NULL);
        else if (!level->unit || (strcmp(level->unit, "power_in") && strcmp(level->unit, "power_out")))
            addf(c, "BAD_KIND", p, "level_port '%s' must be a power_in or power_out port", port->ref, NULL);
    }
}

static void check_ports(Ctx *c, const JsonValue *arr)
{
    static const char *const allowed[] = {"name", "kind", "voltage_min", "voltage_max", "voltage_parameter", "source", "note",
                                          "current", "current_max", "current_source", "level_port", "pullup",
                                          "current_follows", "external_supply"};
    static const char *const required[] = {"name", "kind"};
    static const char *const signal_kinds[] = {"gpio", "signal", "i2c_scl", "i2c_sda", "uart_tx", "uart_rx", "analog"};
    if (!arr || arr->kind != JSON_ARRAY || arr->count == 0) { add(c, "MISSING_KEY", "ports", "at least one port is required"); return; }
    for (size_t i = 0; i < arr->count; i++) {
        const JsonValue *v = arr->items[i];
        const char *p = pathf(c, "ports[%zu]%s", i, NULL);
        if (!object_keys(c, v, p, allowed, N(allowed), required, N(required))) continue;
        const char *name = str(json_get(v, "name")), *kind = str(json_get(v, "kind"));
        if (!identifier(name) || !strcmp(name, "nc")) { add(c, "BAD_NAME", p, "port name must match [a-z][a-z0-9_]* and not be 'nc'"); continue; }
        if (find(&c->ports, name)) { addf(c, "DUPLICATE", p, "port '%s' is declared twice", name, NULL); continue; }
        Name *self = put(c, &c->ports, name, kind);
        if (!in(kind, PORT_KINDS, N(PORT_KINDS))) { addf(c, "BAD_KIND", p, "port kind '%s' is not a known kind", kind, NULL); continue; }
        /* current demand (power_in) or capacity (power_out), each with its own source */
        const JsonValue *cur = json_get(v, "current"), *cmax = json_get(v, "current_max");
        int64_t amps = 0;
        if (cur && strcmp(kind, "power_in")) add(c, "CONFLICT", p, "'current' (the current a block draws) belongs on a power_in port");
        if (cmax && strcmp(kind, "power_out")) add(c, "CONFLICT", p, "'current_max' (what a source can deliver) belongs on a power_out port");
        if (cur && integer(c, cur, p, &amps) && amps < 0) add(c, "BAD_RANGE", p, "'current' must not be negative");
        if (cmax && integer(c, cmax, p, &amps) && amps <= 0) add(c, "BAD_RANGE", p, "'current_max' must be positive");
        if (cur || cmax) source_ref(c, json_get(v, "current_source"), p);
        else if (json_get(v, "current_source")) add(c, "CONFLICT", p, "'current_source' without 'current' or 'current_max'");
        /* a regulator's input carries what its output carries (plus its own 'current', e.g. quiescent) */
        const JsonValue *follows = json_get(v, "current_follows");
        if (follows) {
            if (strcmp(kind, "power_in")) add(c, "CONFLICT", p, "'current_follows' belongs on a power_in port");
            else if (!str(follows)) add(c, "TYPE", p, "'current_follows' must be a port name");
            else if (self) self->what = str(follows);
        }
        /* the supply a signal swings to */
        const JsonValue *level = json_get(v, "level_port");
        if (level) {
            if (!in(kind, signal_kinds, N(signal_kinds))) add(c, "CONFLICT", p, "'level_port' belongs on a signal port");
            else if (!str(level)) add(c, "TYPE", p, "'level_port' must be a port name");
            else if (self) self->ref = str(level);
        }
        /* Step 3d: power that comes from outside the board (KiCad ERC needs a PWR_FLAG on its net) */
        const JsonValue *ext = json_get(v, "external_supply");
        if (ext) {
            if (ext->kind != JSON_TRUE && ext->kind != JSON_FALSE) add(c, "TYPE", p, "'external_supply' must be true or false");
            else if (strcmp(kind, "power_out") && strcmp(kind, "ground")) add(c, "CONFLICT", p, "'external_supply' belongs on a power_out or ground port");
        }
        /* the I2C pull-ups this block contributes to a bus */
        const JsonValue *pull = json_get(v, "pullup");
        if (pull) {
            if (pull->kind != JSON_TRUE && pull->kind != JSON_FALSE) add(c, "TYPE", p, "'pullup' must be true or false");
            else if (strcmp(kind, "i2c_scl") && strcmp(kind, "i2c_sda")) add(c, "CONFLICT", p, "'pullup' belongs on an i2c_scl or i2c_sda port");
        }
        const JsonValue *vmin = json_get(v, "voltage_min"), *vmax = json_get(v, "voltage_max"), *vpar = json_get(v, "voltage_parameter");
        int64_t lo = 0, hi = 0;
        bool has_lo = vmin && integer(c, vmin, p, &lo), has_hi = vmax && integer(c, vmax, p, &hi);
        if (has_lo && has_hi && lo > hi) add(c, "BAD_RANGE", p, "'voltage_min' is above 'voltage_max'");
        if (!strcmp(kind, "ground") && (vmin || vmax || vpar)) add(c, "CONFLICT", p, "a ground port has no voltage");
        if (!strcmp(kind, "power_in") && (!vmin || !vmax))
            add(c, "MISSING_KEY", p, "a power_in port needs 'voltage_min' and 'voltage_max' (uV) and their 'source'");
        if (!strcmp(kind, "power_out") && !vpar && (!vmin || !vmax))
            add(c, "MISSING_KEY", p, "a power_out port needs 'voltage_parameter' or 'voltage_min' and 'voltage_max'");
        if (vmin || vmax) source_ref(c, json_get(v, "source"), p);
        if (vpar) {
            Name *par = str(vpar) ? find(&c->params, str(vpar)) : NULL;
            if (!par) addf(c, "UNKNOWN_PARAMETER", p, "voltage_parameter '%s' is not a declared parameter", str(vpar), NULL);
            else if (!par->unit || strcmp(par->unit, "uV")) add(c, "UNIT_MISMATCH", p, "voltage_parameter must be a parameter in uV");
            else par->uses++;
        }
    }
}

static void check_nets(Ctx *c, const JsonValue *arr)
{
    if (!arr) return;
    if (arr->kind != JSON_ARRAY) { add(c, "TYPE", "nets", "must be an array of names"); return; }
    for (size_t i = 0; i < arr->count; i++) {
        const char *p = pathf(c, "nets[%zu]%s", i, NULL);
        const char *name = str(arr->items[i]);
        if (!identifier(name) || !strcmp(name, "nc")) { add(c, "BAD_NAME", p, "net name must match [a-z][a-z0-9_]* and not be 'nc'"); continue; }
        if (find(&c->nets, name) || find(&c->ports, name)) { addf(c, "DUPLICATE", p, "'%s' is already a net or a port", name, NULL); continue; }
        put(c, &c->nets, name, NULL);
    }
}

static const BlockFunction *function_named(const char *name)
{
    for (size_t i = 0; name && i < N(FUNCTIONS); i++) if (!strcmp(FUNCTIONS[i].name, name)) return &FUNCTIONS[i];
    return NULL;
}

static void check_formulas(Ctx *c, const JsonValue *arr)
{
    static const char *const allowed[] = {"name", "function", "source", "inputs"};
    if (!arr) return;
    if (arr->kind != JSON_ARRAY) { add(c, "TYPE", "formulas", "must be an array"); return; }
    for (size_t i = 0; i < arr->count; i++) {
        const JsonValue *v = arr->items[i];
        const char *p = pathf(c, "formulas[%zu]%s", i, NULL);
        if (!object_keys(c, v, p, allowed, N(allowed), allowed, N(allowed))) continue;
        const char *name = str(json_get(v, "name"));
        if (!identifier(name)) { add(c, "BAD_NAME", p, "formula name must match [a-z][a-z0-9_]*"); continue; }
        if (find(&c->formulas, name)) { addf(c, "DUPLICATE", p, "formula '%s' is declared twice", name, NULL); continue; }
        source_ref(c, json_get(v, "source"), p);
        const BlockFunction *fn = function_named(str(json_get(v, "function")));
        put(c, &c->formulas, name, fn ? fn->output_unit : NULL);
        if (!fn) { addf(c, "UNKNOWN_FUNCTION", p, "function '%s' is not in the formula table (blocks.c)", str(json_get(v, "function")), NULL); continue; }
        const JsonValue *inputs = json_get(v, "inputs");
        if (!inputs || inputs->kind != JSON_OBJECT) { add(c, "TYPE", p, "'inputs' must be an object"); continue; }
        for (size_t k = 0; k < inputs->count; k++) {
            const BlockFunctionInput *want = NULL;
            for (size_t f = 0; f < fn->input_count; f++) if (!strcmp(fn->inputs[f].name, inputs->keys[k])) want = &fn->inputs[f];
            if (!want) { addf(c, "BAD_INPUT", p, "'%s' is not an input of %s", inputs->keys[k], fn->name); continue; }
            Name *par = str(inputs->items[k]) ? find(&c->params, str(inputs->items[k])) : NULL;
            if (!par) { addf(c, "UNKNOWN_PARAMETER", p, "input '%s' names no declared parameter", inputs->keys[k], NULL); continue; }
            par->uses++;
            if (!par->unit || strcmp(par->unit, want->unit))
                addf(c, "UNIT_MISMATCH", p, "input '%s' needs a parameter in %s", want->name, want->unit);
        }
        for (size_t f = 0; f < fn->input_count; f++)
            if (!json_get(inputs, fn->inputs[f].name)) addf(c, "BAD_INPUT", p, "input '%s' of %s is missing", fn->inputs[f].name, fn->name);
    }
}

/* ---------------------------------------------------------------------------------------------- parts */

static const PartRecord *catalogue_part(Ctx *c, const char *id)
{
    for (size_t i = 0; i < c->ncats; i++) {
        const PartRecord *p = part_catalogue_find(c->cats[i], id);
        if (p) return p;
    }
    return NULL;
}

static const PartRecord *catalogue_match(Ctx *c, const char *kind, const char *package)
{
    for (size_t i = 0; i < c->ncats; i++)
        for (size_t k = 0; k < c->cats[i]->part_count; k++) {
            const PartRecord *p = &c->cats[i]->parts[k];
            if (!strcmp(part_kind_name(p->kind), kind) && p->package && package && !strcmp(p->package, package)) return p;
        }
    return NULL;
}

/* The block's pin keys must be exactly the part's pin-map terminals (a terminal may appear on several pins). */
static void check_terminals(Ctx *c, const JsonValue *pins, const PartRecord *part, const char *p)
{
    for (size_t k = 0; k < pins->count; k++) {
        bool found = false;
        for (size_t t = 0; t < part->pin_count; t++) if (!strcmp(part->pins[t].terminal, pins->keys[k])) found = true;
        if (!found) addf(c, "TERMINAL_MISMATCH", p, "part '%s' has no terminal '%s'", part->id, pins->keys[k]);
    }
    for (size_t t = 0; t < part->pin_count; t++)
        if (!json_get(pins, part->pins[t].terminal))
            addf(c, "TERMINAL_MISMATCH", p, "terminal '%s' of part '%s' is not connected (connect it or use \"nc\")",
                 part->pins[t].terminal, part->id);
}

static const char *prefix_for(const char *kind)
{
    for (size_t i = 0; i < N(KINDS); i++) if (!strcmp(KINDS[i].kind, kind)) return KINDS[i].prefix;
    return NULL;
}

static void check_parts(Ctx *c, const JsonValue *arr)
{
    static const char *const allowed[] = {"role", "prefix", "select", "pins", "note", "keepouts"};
    static const char *const ko_keys[] = {"x1_um", "y1_um", "x2_um", "y2_um", "source"};
    static const char *const required[] = {"role", "prefix", "select", "pins"};
    if (!arr || arr->kind != JSON_ARRAY || arr->count == 0) { add(c, "MISSING_KEY", "parts", "at least one part is required"); return; }
    for (size_t i = 0; i < arr->count; i++) {
        const JsonValue *v = arr->items[i];
        const char *p = pathf(c, "parts[%zu]%s", i, NULL);
        if (!object_keys(c, v, p, allowed, N(allowed), required, N(required))) continue;
        const char *role = str(json_get(v, "role")), *prefix = str(json_get(v, "prefix"));
        if (!identifier(role)) add(c, "BAD_NAME", p, "role must match [a-z][a-z0-9_]*");
        else if (find(&c->roles, role)) addf(c, "DUPLICATE", p, "role '%s' is used twice", role, NULL);
        else put(c, &c->roles, role, NULL);
        if (!prefix_ok(prefix)) add(c, "BAD_NAME", p, "prefix must be 1 to 3 capital letters (R, C, D, Q, U, J, SW...)");
        /* keep-outs in the part's own footprint frame, um: no other part and no copper there (Step 2e) */
        const JsonValue *kos = json_get(v, "keepouts");
        if (kos && kos->kind != JSON_ARRAY) add(c, "TYPE", p, "'keepouts' must be an array");
        for (size_t k = 0; kos && kos->kind == JSON_ARRAY && k < kos->count; k++) {
            const char *kp = pathf(c, "parts[%zu].keepouts%s", i, NULL);
            if (!object_keys(c, kos->items[k], kp, ko_keys, N(ko_keys), ko_keys, N(ko_keys))) continue;
            int64_t x1 = 0, y1 = 0, x2 = 0, y2 = 0;
            bool nums = integer(c, json_get(kos->items[k], "x1_um"), kp, &x1) & integer(c, json_get(kos->items[k], "y1_um"), kp, &y1) &
                        integer(c, json_get(kos->items[k], "x2_um"), kp, &x2) & integer(c, json_get(kos->items[k], "y2_um"), kp, &y2);
            if (nums && (x1 >= x2 || y1 >= y2)) add(c, "BAD_RANGE", kp, "a keep-out needs x1 < x2 and y1 < y2");
            if (nums && (x1 < -1000000 || x2 > 1000000 || y1 < -1000000 || y2 > 1000000)) add(c, "BAD_RANGE", kp, "a keep-out lies within 1 m of the part");
            source_ref(c, json_get(kos->items[k], "source"), kp);
        }

        /* the pins: every target is a port, an internal net or "nc" */
        const JsonValue *pins = json_get(v, "pins");
        if (!pins || pins->kind != JSON_OBJECT || pins->count == 0) { add(c, "TYPE", p, "'pins' must be a non-empty object"); continue; }
        for (size_t k = 0; k < pins->count; k++) {
            const char *target = str(pins->items[k]);
            const char *pp = pathf(c, "parts[%zu].pins.%s", i, pins->keys[k]);
            if (!target) { add(c, "TYPE", pp, "must be a port, a net or \"nc\""); continue; }
            if (!strcmp(target, "nc")) continue;
            Name *n = find(&c->ports, target);
            if (!n) n = find(&c->nets, target);
            if (!n) addf(c, "UNKNOWN_NET", pp, "'%s' is neither a port nor a declared net", target, NULL);
            else n->uses++;
        }

        /* the selection: a catalogue part id, or a kind + package with a value */
        const JsonValue *sel = json_get(v, "select");
        static const char *const by_id[] = {"part_id"};
        static const char *const by_kind[] = {"kind", "package", "value"};
        static const char *const by_kind_req[] = {"kind", "package"};
        const char *sp = pathf(c, "parts[%zu].select%s", i, NULL);
        if (sel && sel->kind == JSON_OBJECT && json_get(sel, "part_id")) {
            if (!object_keys(c, sel, sp, by_id, N(by_id), by_id, N(by_id))) continue;
            if (!c->ncats) continue;   /* NOT_RUN without catalogues */
            const PartRecord *part = catalogue_part(c, str(json_get(sel, "part_id")));
            if (!part) { addf(c, "UNKNOWN_PART", sp, "part '%s' is in no given catalogue", str(json_get(sel, "part_id")), NULL); continue; }
            const char *want = prefix_for(part_kind_name(part->kind));
            if (prefix_ok(prefix) && want && strcmp(prefix, want))
                addf(c, "KIND_MISMATCH", sp, "a part of kind %s takes prefix %s", part_kind_name(part->kind), want);
            check_terminals(c, pins, part, sp);
            continue;
        }
        if (!object_keys(c, sel, sp, by_kind, N(by_kind), by_kind_req, N(by_kind_req))) continue;
        const char *kind = str(json_get(sel, "kind")), *package = str(json_get(sel, "package"));
        const char *want = kind ? prefix_for(kind) : NULL;
        if (!want) { addf(c, "BAD_KIND", sp, "part kind '%s' is not one of R C L D Q M U J S", kind, NULL); continue; }
        if (prefix_ok(prefix) && strcmp(prefix, want)) addf(c, "KIND_MISMATCH", sp, "a part of kind %s takes prefix %s", kind, want);
        const char *value_unit = NULL;
        for (size_t k = 0; k < N(KINDS); k++) if (!strcmp(KINDS[k].kind, kind)) value_unit = KINDS[k].value_unit;
        const JsonValue *value = json_get(sel, "value");
        if (value) {
            static const char *const vkeys[] = {"formula", "parameter"};
            if (!object_keys(c, value, sp, vkeys, N(vkeys), NULL, 0)) continue;
            const char *unit = NULL;
            if (json_get(value, "formula")) {
                Name *f = str(json_get(value, "formula")) ? find(&c->formulas, str(json_get(value, "formula"))) : NULL;
                if (!f) { addf(c, "UNKNOWN_FORMULA", sp, "formula '%s' is not declared", str(json_get(value, "formula")), NULL); continue; }
                f->uses++;
                unit = f->unit;
            } else if (json_get(value, "parameter")) {
                Name *par = str(json_get(value, "parameter")) ? find(&c->params, str(json_get(value, "parameter"))) : NULL;
                if (!par) { addf(c, "UNKNOWN_PARAMETER", sp, "parameter '%s' is not declared", str(json_get(value, "parameter")), NULL); continue; }
                par->uses++;
                unit = par->unit;
            } else {
                add(c, "MISSING_KEY", sp, "'value' needs a 'formula' or a 'parameter'");
                continue;
            }
            if (!value_unit) addf(c, "CONFLICT", sp, "a part of kind %s is chosen by part_id, not by value", kind, NULL);
            else if (!unit || strcmp(unit, value_unit))
                addf(c, "UNIT_MISMATCH", sp, "a part of kind %s is valued in %s", kind, value_unit);
        } else if (value_unit) {
            addf(c, "MISSING_KEY", sp, "a part of kind %s chosen by kind and package needs a 'value'", kind, NULL);
        }
        if (!c->ncats) continue;
        const PartRecord *match = catalogue_match(c, kind, package);
        if (!match) { addf(c, "NO_CATALOGUE_MATCH", sp, "no catalogue part of kind %s in package '%s'", kind, package); continue; }
        check_terminals(c, pins, match, sp);
    }
}

static void check_notes(Ctx *c, const JsonValue *arr)
{
    static const char *const allowed[] = {"text", "source"};
    if (!arr) return;
    if (arr->kind != JSON_ARRAY) { add(c, "TYPE", "notes", "must be an array"); return; }
    for (size_t i = 0; i < arr->count; i++) {
        const char *p = pathf(c, "notes[%zu]%s", i, NULL);
        if (!object_keys(c, arr->items[i], p, allowed, N(allowed), allowed, N(allowed))) continue;
        source_ref(c, json_get(arr->items[i], "source"), p);
    }
}

/* ---------------------------------------------------------------------------------------------- entry points */

static BlockStatus run(Ctx *c, const char *text, size_t length)
{
    BlockReport *r = c->r;
    JsonValue *root = NULL;
    JsonError je;
    memset(&je, 0, sizeof je);
    if (json_parse(c->a, text, length, &root, &je) != JSON_OK) {
        char m[200];
        snprintf(m, sizeof m, "%s at line %zu, column %zu", je.message ? je.message : json_status_name(je.code), je.line, je.column);
        add(c, "JSON", "", m);
    } else {
        static const char *const allowed[] = {"format", "name", "summary", "sources", "parameters", "ports", "nets",
                                              "parts", "formulas", "notes"};
        static const char *const required[] = {"format", "name", "summary", "sources", "ports", "parts"};
        object_keys(c, root, "", allowed, N(allowed), required, N(required));
        if (root->kind == JSON_OBJECT) {
            const char *format = str(json_get(root, "format"));
            if (!format || strcmp(format, "copper.block/1")) add(c, "FORMAT", "format", "format must be \"copper.block/1\"");
            const char *name = str(json_get(root, "name"));
            if (!identifier(name)) add(c, "BAD_NAME", "name", "block name must match [a-z][a-z0-9_]*");
            else r->name = name;
            if (!str(json_get(root, "summary")) || !*str(json_get(root, "summary"))) add(c, "MISSING_KEY", "summary", "a non-empty summary is required");
            check_sources(c, json_get(root, "sources"));
            check_parameters(c, json_get(root, "parameters"));
            check_ports(c, json_get(root, "ports"));
            check_references(c);
            check_nets(c, json_get(root, "nets"));
            check_formulas(c, json_get(root, "formulas"));
            check_parts(c, json_get(root, "parts"));
            check_notes(c, json_get(root, "notes"));
            for (size_t i = 0; i < c->ports.count; i++)
                if (!c->ports.items[i].uses)
                    addf(c, "UNUSED_PORT", "ports", "port '%s' is connected to no pin", c->ports.items[i].name, NULL);
            for (size_t i = 0; i < c->nets.count; i++)
                if (c->nets.items[i].uses < 2)
                    addf(c, "SINGLE_ENDED_NET", "nets", "net '%s' joins fewer than two pins", c->nets.items[i].name, NULL);
            r->parameters = c->params.count;
            r->ports = c->ports.count;
            r->nets = c->nets.count;
            r->parts = c->roles.count;
            r->formulas = c->formulas.count;
        }
    }
    if (c->oom) return r->verdict = BLOCK_ERR_MEMORY;
    r->verdict = r->finding_count ? BLOCK_INVALID : (c->ncats ? BLOCK_VALID : BLOCK_UNVERIFIED);
    return r->verdict;
}

static BlockStatus start(Ctx *c, Arena *arena, const PartCatalogue *const *cats, size_t ncats, BlockReport *out)
{
    if (!out) return BLOCK_ERR_ARGUMENT;
    memset(out, 0, sizeof *out);
    out->verdict = BLOCK_ERR_ARGUMENT;
    if (!arena || (ncats && !cats)) return BLOCK_ERR_ARGUMENT;
    for (size_t i = 0; i < ncats; i++) if (!cats[i]) return BLOCK_ERR_ARGUMENT;
    memset(c, 0, sizeof *c);
    c->a = arena;
    c->r = out;
    c->cats = cats;
    c->ncats = ncats;
    out->catalogue_checked = ncats > 0;
    return BLOCK_VALID;
}

BlockStatus block_check_text(Arena *arena, const char *json, size_t length, const PartCatalogue *const *catalogues,
                             size_t catalogue_count, BlockReport *out)
{
    Ctx c;
    if (start(&c, arena, catalogues, catalogue_count, out) != BLOCK_VALID) return BLOCK_ERR_ARGUMENT;
    if (!json) return out->verdict = BLOCK_ERR_ARGUMENT;
    out->source = "text";
    return run(&c, json, length);
}

BlockStatus block_check_file(Arena *arena, const char *path, const PartCatalogue *const *catalogues,
                             size_t catalogue_count, BlockReport *out)
{
    Ctx c;
    if (start(&c, arena, catalogues, catalogue_count, out) != BLOCK_VALID) return BLOCK_ERR_ARGUMENT;
    if (!path) return out->verdict = BLOCK_ERR_ARGUMENT;
    out->source = path;
    const unsigned char *data = NULL;
    size_t size = 0;
    if (fs_read_file_arena(arena, path, &data, &size) != FS_OK) {
        add(&c, "IO", "", "the block file cannot be read");
        return out->verdict = c.oom ? BLOCK_ERR_MEMORY : BLOCK_INVALID;
    }
    return run(&c, (const char *)data, size);
}

const char *block_report_json(Arena *arena, const BlockReport *r, size_t *length)
{
    JsonWriter w;
    json_writer_init(&w, arena);
    json_write_begin_object(&w);
    json_write_key(&w, "format"); json_write_string(&w, "copper.block-check/1");
    json_write_key(&w, "source"); json_write_string(&w, r->source);
    json_write_key(&w, "block"); json_write_string(&w, r->name);
    json_write_key(&w, "verdict"); json_write_string(&w, block_status_name(r->verdict));
    json_write_key(&w, "catalogue_checked"); json_write_bool(&w, r->catalogue_checked);
    json_write_key(&w, "parameters"); json_write_uint(&w, r->parameters);
    json_write_key(&w, "ports"); json_write_uint(&w, r->ports);
    json_write_key(&w, "nets"); json_write_uint(&w, r->nets);
    json_write_key(&w, "parts"); json_write_uint(&w, r->parts);
    json_write_key(&w, "formulas"); json_write_uint(&w, r->formulas);
    json_write_key(&w, "findings");
    json_write_begin_array(&w);
    for (size_t i = 0; i < r->finding_count; i++) {
        json_write_begin_object(&w);
        json_write_key(&w, "code"); json_write_string(&w, r->findings[i].code);
        json_write_key(&w, "path"); json_write_string(&w, r->findings[i].path);
        json_write_key(&w, "message"); json_write_string(&w, r->findings[i].message);
        json_write_end_object(&w);
    }
    json_write_end_array(&w);
    json_write_end_object(&w);
    return json_writer_finish(&w, length);
}

const char *block_status_name(BlockStatus status)
{
    switch (status) {
    case BLOCK_VALID: return "VALID";
    case BLOCK_UNVERIFIED: return "UNVERIFIED";
    case BLOCK_INVALID: return "INVALID";
    case BLOCK_ERR_ARGUMENT: return "ERR_ARGUMENT";
    case BLOCK_ERR_MEMORY: return "ERR_MEMORY";
    }
    return "UNKNOWN";
}

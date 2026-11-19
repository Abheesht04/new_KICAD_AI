/* intent_check.c - grounding gate for model-written design intents (see intent_check.h). */
#include "intent_check.h"

#include "filesystem.h"
#include "json.h"
#include "kicad_assets.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MARGIN_NM INT64_C(2500000) /* a part centre at least 2.5 mm inside every board edge (compiler v1) */

typedef struct Ctx {
    Arena *a;
    IntentCheckReport *r;
    const char *symbols_root, *footprints_root;
    bool oom;
} Ctx;

static char *dup_text(Arena *a, const char *s)
{
    size_t n = strlen(s);
    char *d = arena_alloc(a, n + 1);
    if (d) memcpy(d, s, n + 1);
    return d;
}

static void add(Ctx *c, const char *code, const char *ref, const char *item, const char *fmt, const char *x,
                const char *y)
{
    IntentCheckReport *r = c->r;
    if (r->count >= INTENT_CHECK_MAX_FINDINGS) { r->truncated = true; return; }
    if (r->count == r->capacity) {
        size_t cap = r->capacity ? r->capacity * 2 : 16;
        IntentFinding *more = arena_alloc_array(c->a, sizeof *more, cap);
        if (!more) { c->oom = true; return; }
        if (r->count) memcpy(more, r->findings, r->count * sizeof *more);
        r->findings = more;
        r->capacity = cap;
    }
    char buf[512];
    int n = snprintf(buf, sizeof buf, fmt, x ? x : "", y ? y : "");
    if (n < 0) n = 0;
    if ((size_t)n >= sizeof buf) memcpy(buf + sizeof buf - 4, "...", 4);   /* a cut message says so */
    IntentFinding *f = &r->findings[r->count++];
    f->code = code;
    f->reference = ref ? dup_text(c->a, ref) : NULL;
    f->item = item ? dup_text(c->a, item) : NULL;
    f->message = dup_text(c->a, buf);
    if ((ref && !f->reference) || (item && !f->item) || !f->message) c->oom = true;
}

static bool is_string(const JsonValue *v) { return v && v->kind == JSON_STRING; }

/* Every key of `obj` must be in `allowed` (NULL-terminated); unknown keys are findings. */
static void only_keys(Ctx *c, const JsonValue *obj, const char *const *allowed, const char *ref, const char *where)
{
    for (size_t i = 0; i < obj->count; i++) {
        bool ok = false;
        for (const char *const *k = allowed; *k; k++) ok |= !strcmp(obj->keys[i], *k);
        if (!ok) add(c, "UNKNOWN_FIELD", ref, obj->keys[i],
                     "\"%s\" is not part of %s in copper.design-intent v1: nothing outside the schema is accepted "
                     "(no invented part numbers, ratings or claims)", obj->keys[i], where);
    }
}

static bool all_chars(const char *s, const char *allowed_punct, bool letters_digits)
{
    if (!*s) return false;
    for (const char *p = s; *p; p++) {
        char ch = *p;
        bool ok = (letters_digits && ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')))
                  || strchr(allowed_punct, ch);
        if (!ok) return false;
    }
    return true;
}

static bool valid_reference(const char *s)
{
    size_t i = 0;
    while (s[i] >= 'A' && s[i] <= 'Z') i++;
    if (i == 0 || i > 4 || !s[i]) return false;
    size_t j = i;
    while (s[j] >= '0' && s[j] <= '9') j++;
    return j > i && j - i <= 4 && !s[j];
}

/* "Lib:Name" -> two Arena strings. Path-safe: no separators, no "..". */
static bool split_id(Ctx *c, const char *id, char **lib, char **name)
{
    const char *colon = strchr(id, ':');
    if (!colon || colon == id || !colon[1] || strchr(colon + 1, ':')) return false;
    size_t ln = (size_t)(colon - id);
    *lib = arena_alloc(c->a, ln + 1);
    *name = dup_text(c->a, colon + 1);
    if (!*lib || !*name) { c->oom = true; return false; }
    memcpy(*lib, id, ln);
    (*lib)[ln] = 0;
    if (strstr(*lib, "..") || strstr(*name, "..")) return false;
    return all_chars(*lib, "_.-", true) && all_chars(*name, "_.+-", true);
}

/* ---------------------------------------------------------------------------------------------- library cache */

typedef struct SymLib { char *lib; bool loaded, ok; KicadSymbolLibrary parsed; } SymLib;
typedef struct Cache { SymLib *libs; size_t count, capacity; } Cache;

static const KicadSymbolLibrary *symbol_library(Ctx *c, Cache *cache, const char *lib)
{
    for (size_t i = 0; i < cache->count; i++)
        if (!strcmp(cache->libs[i].lib, lib)) return cache->libs[i].ok ? &cache->libs[i].parsed : NULL;
    if (cache->count == cache->capacity) {
        size_t cap = cache->capacity ? cache->capacity * 2 : 8;
        SymLib *more = arena_alloc_array(c->a, sizeof *more, cap);
        if (!more) { c->oom = true; return NULL; }
        if (cache->count) memcpy(more, cache->libs, cache->count * sizeof *more);
        cache->libs = more;
        cache->capacity = cap;
    }
    SymLib *s = &cache->libs[cache->count++];
    memset(s, 0, sizeof *s);
    s->lib = dup_text(c->a, lib);
    char file[FS_PATH_MAX], name[FS_PATH_MAX];
    int n = snprintf(name, sizeof name, "%s.kicad_sym", lib);
    const unsigned char *data = NULL;
    size_t size = 0;
    KicadAssetError e;
    memset(&e, 0, sizeof e);
    if (n > 0 && (size_t)n < sizeof name && fs_path_join(file, sizeof file, c->symbols_root, name) == FS_OK &&
        fs_read_file_arena(c->a, file, &data, &size) == FS_OK &&
        kicad_symbol_library_parse(c->a, (const char *)data, size, &s->parsed, &e) == KICAD_ASSET_OK)
        s->ok = true;
    s->loaded = true;
    return s->ok ? &s->parsed : NULL;
}

static bool load_footprint(Ctx *c, const char *lib, const char *fp, KicadFootprintAsset *out)
{
    char dir[FS_PATH_MAX], file[FS_PATH_MAX], a[FS_PATH_MAX], b[FS_PATH_MAX];
    int n1 = snprintf(a, sizeof a, "%s.pretty", lib), n2 = snprintf(b, sizeof b, "%s.kicad_mod", fp);
    const unsigned char *data = NULL;
    size_t size = 0;
    KicadAssetError e;
    memset(&e, 0, sizeof e);
    return n1 > 0 && (size_t)n1 < sizeof a && n2 > 0 && (size_t)n2 < sizeof b &&
           fs_path_join(dir, sizeof dir, c->footprints_root, a) == FS_OK &&
           fs_path_join(file, sizeof file, dir, b) == FS_OK && fs_read_file_arena(c->a, file, &data, &size) == FS_OK &&
           kicad_footprint_parse(c->a, (const char *)data, size, out, &e) == KICAD_ASSET_OK;
}

/* ---------------------------------------------------------------------------------------------- nets */

typedef struct Net { const char *name; size_t pins; } Net;
typedef struct Nets { Net *items; size_t count, capacity; } Nets;

static void count_net(Ctx *c, Nets *nets, const char *name)
{
    for (size_t i = 0; i < nets->count; i++)
        if (!strcmp(nets->items[i].name, name)) { nets->items[i].pins++; return; }
    if (nets->count == nets->capacity) {
        size_t cap = nets->capacity ? nets->capacity * 2 : 16;
        Net *more = arena_alloc_array(c->a, sizeof *more, cap);
        if (!more) { c->oom = true; return; }
        if (nets->count) memcpy(more, nets->items, nets->count * sizeof *more);
        nets->items = more;
        nets->capacity = cap;
    }
    nets->items[nets->count].name = name;
    nets->items[nets->count++].pins = 1;
}

/* ---------------------------------------------------------------------------------------------- component */

/* KiCad writes an unnamed pin as "~"; parsers and context tools may report it as "". Both mean "unnamed". */
static bool pin_names_equal(const char *claimed, const char *library)
{
    bool cu = !claimed || !claimed[0] || !strcmp(claimed, "~"), lu = !library || !library[0] || !strcmp(library, "~");
    if (cu || lu) return cu && lu;
    return strcmp(claimed, library) == 0;
}

static const KicadSymbolPin *find_pin(const KicadSymbolAsset *sym, const char *number)
{
    for (size_t i = 0; i < sym->pin_count; i++)
        if (sym->pins[i].body_style <= 1 && !strcmp(sym->pins[i].number, number)) return &sym->pins[i];
    return NULL;
}

static void check_component(Ctx *c, Cache *cache, Nets *nets, const JsonValue *comp, size_t index,
                            int64_t board_w, int64_t board_h, const char **refs)
{
    static const char *const keys[] = {"reference", "value", "symbol_id", "footprint_id", "position", "pins", NULL};
    char where[32];
    snprintf(where, sizeof where, "components[%zu]", index);
    if (comp->kind != JSON_OBJECT) { add(c, "SCHEMA", NULL, where, "%s must be an object", where, NULL); return; }
    const JsonValue *jref = json_get(comp, "reference");
    const char *ref = is_string(jref) ? jref->text : where;
    only_keys(c, comp, keys, ref, "a component");
    if (!is_string(jref) || !valid_reference(jref->text)) add(c, "BAD_REFERENCE", ref, NULL, "reference must look like R1, D1 or J12", NULL, NULL);
    else {
        for (size_t i = 0; i < index; i++)
            if (refs[i] && !strcmp(refs[i], jref->text)) add(c, "DUPLICATE_REFERENCE", ref, NULL, "reference %s is used twice", ref, NULL);
        refs[index] = jref->text;
    }
    const JsonValue *value = json_get(comp, "value");
    if (!is_string(value) || !value->text[0] || value->text_len > 64)
        add(c, "BAD_VALUE", ref, NULL, "value must be a non-empty string of at most 64 characters", NULL, NULL);

    /* Position: inside the board, exact nm. */
    const JsonValue *pos = json_get(comp, "position");
    if (pos) {
        static const char *const pkeys[] = {"x_mm", "y_mm", "rotation_deg", NULL};
        if (pos->kind != JSON_OBJECT) add(c, "SCHEMA", ref, "position", "position must be an object", NULL, NULL);
        else {
            only_keys(c, pos, pkeys, ref, "a position");
            int64_t x = 0, y = 0;
            const JsonValue *jx = json_get(pos, "x_mm"), *jy = json_get(pos, "y_mm");
            bool have = jx && jy && json_to_nm(jx, &x) == JSON_NUMBER_OK && json_to_nm(jy, &y) == JSON_NUMBER_OK;
            if ((jx || jy) && !have) add(c, "SCHEMA", ref, "position", "x_mm and y_mm must both be plain decimal numbers", NULL, NULL);
            else if (have && board_w > 0 && board_h > 0 &&
                     (x < MARGIN_NM || y < MARGIN_NM || x > board_w - MARGIN_NM || y > board_h - MARGIN_NM))
                add(c, "POSITION_OUTSIDE_BOARD", ref, "position",
                    "the part centre must be at least 2.5 mm inside the %s board", "declared", NULL);
        }
    }

    /* Symbol: exists, not derived, single unit. */
    const KicadSymbolAsset *sym = NULL;
    const JsonValue *sid = json_get(comp, "symbol_id");
    char *slib = NULL, *sname = NULL;
    if (!is_string(sid) || !split_id(c, sid->text, &slib, &sname))
        add(c, "BAD_LIBRARY_ID", ref, "symbol_id", "symbol_id must be a KiCad ID like Device:R (got \"%s\")", is_string(sid) ? sid->text : "", NULL);
    else {
        const KicadSymbolLibrary *lib = symbol_library(c, cache, slib);
        if (!lib) add(c, "SYMBOL_LIBRARY_NOT_FOUND", ref, sid->text, "no installed symbol library %s.kicad_sym: the part cannot be verified", slib, NULL);
        else if (!(sym = kicad_symbol_find(lib, sname)))
            add(c, "SYMBOL_NOT_FOUND", ref, sid->text, "symbol %s does not exist in the installed library %s", sname, slib);
        else if (sym->extends || sym->pins_inherited) {
            add(c, "DERIVED_SYMBOL", ref, sid->text, "%s is a derived symbol (extends %s); compiler v1 does not accept it", sid->text, sym->extends);
            sym = NULL;
        } else {
            for (size_t i = 0; i < sym->pin_count; i++)
                if (sym->pins[i].unit > 1) { add(c, "MULTI_UNIT_SYMBOL", ref, sid->text, "%s has several units; compiler v1 does not accept it", sid->text, NULL); sym = NULL; break; }
        }
    }

    /* Footprint: exists. */
    KicadFootprintAsset fp;
    bool have_fp = false;
    const JsonValue *fid = json_get(comp, "footprint_id");
    char *flib = NULL, *fname = NULL;
    if (!is_string(fid) || !split_id(c, fid->text, &flib, &fname))
        add(c, "BAD_LIBRARY_ID", ref, "footprint_id", "footprint_id must be a KiCad ID like Resistor_SMD:R_0603_1608Metric (got \"%s\")", is_string(fid) ? fid->text : "", NULL);
    else if (!(have_fp = load_footprint(c, flib, fname, &fp)))
        add(c, "FOOTPRINT_NOT_FOUND", ref, fid->text, "no installed footprint %s.pretty/%s.kicad_mod: the part cannot be verified", flib, fname);

    /* Pins: each mapped exactly once, real numbers, real names, real pads. */
    const JsonValue *pins = json_get(comp, "pins");
    if (!pins || pins->kind != JSON_ARRAY || !pins->count) { add(c, "SCHEMA", ref, "pins", "pins must be a non-empty array", NULL, NULL); return; }
    static const char *const pinkeys[] = {"symbol_pin", "pin_name", "pad", "net", NULL};
    const char **seen_pins = arena_alloc_array(c->a, sizeof *seen_pins, pins->count);
    const char **seen_pads = arena_alloc_array(c->a, sizeof *seen_pads, pins->count);
    if (!seen_pins || !seen_pads) { c->oom = true; return; }
    for (size_t j = 0; j < pins->count; j++) {
        const JsonValue *p = pins->items[j];
        seen_pins[j] = seen_pads[j] = NULL;
        c->r->pins++;
        if (p->kind != JSON_OBJECT) { add(c, "SCHEMA", ref, "pins", "each pin must be an object", NULL, NULL); continue; }
        only_keys(c, p, pinkeys, ref, "a pin");
        const JsonValue *sp = json_get(p, "symbol_pin"), *pn = json_get(p, "pin_name"), *pad = json_get(p, "pad"), *net = json_get(p, "net");
        const char *pin_no = is_string(sp) ? sp->text : "?";
        if (!is_string(sp) || !sp->text[0]) { add(c, "SCHEMA", ref, "symbol_pin", "symbol_pin must be the pin NUMBER as a string", NULL, NULL); }
        else {
            for (size_t k = 0; k < j; k++)
                if (seen_pins[k] && !strcmp(seen_pins[k], sp->text)) add(c, "DUPLICATE_SYMBOL_PIN", ref, sp->text, "symbol pin %s is mapped twice", sp->text, NULL);
            seen_pins[j] = sp->text;
            if (sym) {
                const KicadSymbolPin *lp = find_pin(sym, sp->text);
                if (!lp) add(c, "UNKNOWN_SYMBOL_PIN", ref, sp->text, "%s has no pin numbered %s", sid->text, sp->text);
                else if (!is_string(pn))
                    add(c, "PIN_NAME_MISSING", ref, sp->text, "pin_name is required: the library names pin %s \"%s\" (copy it from the context step)", sp->text, lp->name ? lp->name : "");
                else if (!pin_names_equal(pn->text, lp->name)) {
                    char got[160];
                    snprintf(got, sizeof got, "%s\", but the library names it \"%s", pn->text, lp->name ? lp->name : "");
                    add(c, "PIN_NAME_MISMATCH", ref, sp->text, "pin %s was described as \"%s\": the intent's idea of this pin is wrong", sp->text, got);
                } else if (!net || net->kind == JSON_NULL) {
                    if (lp->electrical_type && !strcmp(lp->electrical_type, "power_in"))
                        add(c, "UNCONNECTED_POWER_PIN", ref, sp->text, "power input pin %s (%s) is left unconnected", sp->text, lp->name);
                }
            }
        }
        if (!is_string(pad) || !pad->text[0]) add(c, "SCHEMA", ref, pin_no, "pad must be the footprint pad number as a string", NULL, NULL);
        else {
            for (size_t k = 0; k < j; k++)
                if (seen_pads[k] && !strcmp(seen_pads[k], pad->text)) add(c, "DUPLICATE_PAD", ref, pad->text, "footprint pad %s is used twice", pad->text, NULL);
            seen_pads[j] = pad->text;
            if (have_fp && !kicad_footprint_has_pad(&fp, pad->text))
                add(c, "UNKNOWN_PAD", ref, pad->text, "footprint %s has no pad %s", fid->text, pad->text);
        }
        if (net && net->kind != JSON_NULL) {
            if (!is_string(net) || net->text_len > 64 || !all_chars(net->text, "_+./:-", true))
                add(c, "BAD_NET_NAME", ref, pin_no, "net must be null or a name of letters, digits and _+./:- (got \"%s\")", is_string(net) ? net->text : "", NULL);
            else count_net(c, nets, net->text);
        } else if (!net) add(c, "SCHEMA", ref, pin_no, "net is required (a name, or null for an intentional no-connect)", NULL, NULL);
    }
    /* Completeness against the library: every symbol pin and every numbered pad must be mapped. */
    if (sym) {
        for (size_t i = 0; i < sym->pin_count; i++) {
            if (sym->pins[i].body_style > 1) continue;
            bool dup_earlier = false;
            for (size_t k = 0; k < i; k++) dup_earlier |= sym->pins[k].body_style <= 1 && !strcmp(sym->pins[k].number, sym->pins[i].number);
            if (dup_earlier) continue;
            bool mapped = false;
            for (size_t j = 0; j < pins->count; j++) mapped |= seen_pins[j] && !strcmp(seen_pins[j], sym->pins[i].number);
            if (!mapped) add(c, "MISSING_SYMBOL_PIN", ref, sym->pins[i].number, "symbol pin %s (%s) is not mapped", sym->pins[i].number, sym->pins[i].name);
        }
    }
    if (have_fp) {
        for (size_t i = 0; i < fp.pad_count; i++) {
            const char *num = fp.pads[i].number;
            if (!num || !num[0]) continue;   /* mechanical, unnumbered */
            bool dup_earlier = false;
            for (size_t k = 0; k < i; k++) dup_earlier |= fp.pads[k].number && !strcmp(fp.pads[k].number, num);
            if (dup_earlier) continue;
            bool mapped = false;
            for (size_t j = 0; j < pins->count; j++) mapped |= seen_pads[j] && !strcmp(seen_pads[j], num);
            if (!mapped) add(c, "MISSING_PAD", ref, num, "footprint pad %s is not mapped to any symbol pin", num, NULL);
        }
    }
}

/* ---------------------------------------------------------------------------------------------- entry */

IntentCheckStatus intent_check(Arena *arena, const char *text, size_t length, const char *symbols_root,
                               const char *footprints_root, IntentCheckReport *out)
{
    if (!arena || !text || !symbols_root || !footprints_root || !out) return INTENT_CHECK_ERR_ARGUMENT;
    memset(out, 0, sizeof *out);
    Ctx c = {arena, out, symbols_root, footprints_root, false};
    JsonValue *root = NULL;
    JsonError je;
    memset(&je, 0, sizeof je);
    if (json_parse(arena, text, length, &root, &je) != JSON_OK) {
        char at[64];
        snprintf(at, sizeof at, "line %zu, column %zu", je.line, je.column);
        add(&c, "INVALID_JSON", NULL, NULL, "the design intent is not valid JSON (%s): %s", at, je.message ? je.message : "");
        return c.oom ? INTENT_CHECK_ERR_MEMORY : INTENT_CHECK_REJECTED;
    }
    static const char *const rootkeys[] = {"schema", "schema_version", "project", "components", NULL};
    if (root->kind != JSON_OBJECT) { add(&c, "SCHEMA", NULL, NULL, "the design intent must be a JSON object", NULL, NULL); return INTENT_CHECK_REJECTED; }
    only_keys(&c, root, rootkeys, NULL, "the design intent");
    const JsonValue *schema = json_get(root, "schema"), *version = json_get(root, "schema_version");
    int64_t v = 0;
    if (!is_string(schema) || strcmp(schema->text, "copper.design-intent") != 0 || !version ||
        json_to_i64(version, &v) != JSON_NUMBER_OK || v != 1)
        add(&c, "SCHEMA", NULL, "schema", "schema must be \"copper.design-intent\" with schema_version 1", NULL, NULL);

    int64_t bw = 0, bh = 0;
    const JsonValue *project = json_get(root, "project");
    if (!project || project->kind != JSON_OBJECT) add(&c, "SCHEMA", NULL, "project", "project must be an object", NULL, NULL);
    else {
        static const char *const pkeys[] = {"name", "board", NULL}, *const bkeys[] = {"width_mm", "height_mm", "layers", NULL};
        only_keys(&c, project, pkeys, NULL, "project");
        const JsonValue *name = json_get(project, "name"), *board = json_get(project, "board");
        if (!is_string(name) || !all_chars(name->text, "_-", true) || name->text_len > 64)
            add(&c, "SCHEMA", NULL, "project.name", "project.name must be letters, digits, _ or - (at most 64)", NULL, NULL);
        if (!board || board->kind != JSON_OBJECT) add(&c, "SCHEMA", NULL, "project.board", "project.board must be an object", NULL, NULL);
        else {
            only_keys(&c, board, bkeys, NULL, "project.board");
            int64_t layers = 0;
            const JsonValue *jl = json_get(board, "layers");
            if (!json_get(board, "width_mm") || !json_get(board, "height_mm") ||
                json_to_nm(json_get(board, "width_mm"), &bw) != JSON_NUMBER_OK || json_to_nm(json_get(board, "height_mm"), &bh) != JSON_NUMBER_OK ||
                bw <= 0 || bh <= 0)
                add(&c, "SCHEMA", NULL, "project.board", "width_mm and height_mm must be positive plain decimals", NULL, NULL);
            if (!jl || json_to_i64(jl, &layers) != JSON_NUMBER_OK || layers != 2)
                add(&c, "SCHEMA", NULL, "project.board.layers", "layers must be 2 (compiler v1)", NULL, NULL);
        }
    }

    const JsonValue *comps = json_get(root, "components");
    if (!comps || comps->kind != JSON_ARRAY || !comps->count || comps->count > 64)
        add(&c, "SCHEMA", NULL, "components", "components must be an array of 1 to 64 parts", NULL, NULL);
    else {
        Cache cache = {NULL, 0, 0};
        Nets nets = {NULL, 0, 0};
        const char **refs = arena_alloc_array(arena, sizeof *refs, comps->count);
        if (!refs) return INTENT_CHECK_ERR_MEMORY;
        for (size_t i = 0; i < comps->count; i++) refs[i] = NULL;
        out->components = comps->count;
        for (size_t i = 0; i < comps->count && !c.oom; i++) check_component(&c, &cache, &nets, comps->items[i], i, bw, bh, refs);
        out->nets = nets.count;
        for (size_t i = 0; i < nets.count; i++)
            if (nets.items[i].pins < 2)
                add(&c, "SINGLE_ENDED_NET", NULL, nets.items[i].name, "net %s connects only one pin: a net needs at least two (use null for a no-connect)", nets.items[i].name, NULL);
    }
    if (c.oom) return INTENT_CHECK_ERR_MEMORY;
    return out->count ? INTENT_CHECK_REJECTED : INTENT_CHECK_GROUNDED;
}

const char *intent_check_report_json(Arena *arena, const IntentCheckReport *r, size_t *length)
{
    JsonWriter w;
    json_writer_init(&w, arena);
    json_write_begin_object(&w);
    json_write_key(&w, "format"); json_write_string(&w, "copper.intent-check/1");
    json_write_key(&w, "verdict"); json_write_string(&w, r->count ? "REJECTED" : "GROUNDED");
    json_write_key(&w, "components"); json_write_uint(&w, r->components);
    json_write_key(&w, "pins"); json_write_uint(&w, r->pins);
    json_write_key(&w, "nets"); json_write_uint(&w, r->nets);
    json_write_key(&w, "truncated"); json_write_bool(&w, r->truncated);
    json_write_key(&w, "findings");
    json_write_begin_array(&w);
    for (size_t i = 0; i < r->count; i++) {
        const IntentFinding *f = &r->findings[i];
        json_write_begin_object(&w);
        json_write_key(&w, "code"); json_write_string(&w, f->code);
        json_write_key(&w, "reference"); if (f->reference) json_write_string(&w, f->reference); else json_write_null(&w);
        json_write_key(&w, "item"); if (f->item) json_write_string(&w, f->item); else json_write_null(&w);
        json_write_key(&w, "message"); json_write_string(&w, f->message);
        json_write_end_object(&w);
    }
    json_write_end_array(&w);
    json_write_end_object(&w);
    return json_writer_finish(&w, length);
}

const char *intent_check_status_name(IntentCheckStatus status)
{
    switch (status) {
    case INTENT_CHECK_GROUNDED: return "GROUNDED";
    case INTENT_CHECK_REJECTED: return "REJECTED";
    case INTENT_CHECK_ERR_ARGUMENT: return "ERR_ARGUMENT";
    case INTENT_CHECK_ERR_MEMORY: return "ERR_MEMORY";
    }
    return "UNKNOWN";
}

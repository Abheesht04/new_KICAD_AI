/* parts_check.c - see parts_check.h. Reads only; never fills in or changes a value. */
#include "parts_check.h"

#include "filesystem.h"
#include "json.h"
#include "kicad_assets.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct Ctx {
    Arena *a;
    PartsCheckReport *r;
    size_t capacity;
    bool oom;
} Ctx;

static char *dup_text(Arena *a, const char *s)
{
    size_t n = strlen(s);
    char *d = arena_alloc(a, n + 1);
    if (d) memcpy(d, s, n + 1);
    return d;
}

static void add(Ctx *c, PartsFindingLevel level, const char *code, size_t part, const char *message)
{
    PartsCheckReport *r = c->r;
    if (r->finding_count == c->capacity) {
        size_t cap = c->capacity ? c->capacity * 2 : 32;
        PartsFinding *more = arena_alloc_array(c->a, sizeof *more, cap);
        if (!more) { c->oom = true; return; }
        if (r->finding_count) memcpy(more, r->findings, r->finding_count * sizeof *more);
        r->findings = more;
        c->capacity = cap;
    }
    PartsFinding *f = &r->findings[r->finding_count];
    f->level = level;
    f->code = code;
    f->part_index = part;
    f->part_id = part == SIZE_MAX ? NULL : r->parts[part].id;
    f->message = dup_text(c->a, message);
    if (!f->message) { c->oom = true; return; }
    r->finding_count++;
    if (level == PARTS_FINDING_FAIL) {
        r->fail_count++;
        if (part != SIZE_MAX) r->parts[part].fails++;
    } else {
        r->unverified_count++;
        if (part != SIZE_MAX && level == PARTS_FINDING_UNVERIFIED) r->parts[part].unverified++;
    }
}

/* Append ", name" (or "name" first) to a fixed buffer; truncation only shortens a message. */
static void list_add(char *buf, size_t cap, const char *name)
{
    size_t n = strlen(buf);
    if (n + 3 >= cap) return;
    snprintf(buf + n, cap - n, "%s%s", n ? ", " : "", name);
}

static const char *const KNOWN_FIELDS[] = {
    "id", "kind", "manufacturer", "mpn", "symbol", "footprint", "model", "package", "description",
    "datasheet", "supplier", "identity_source", "ratings", "pins", "aliases"
};

static void check_fields(Ctx *c, size_t i, const PartRecord *p)
{
    const IRNode *rec = p->source_record;
    if (!rec || rec->kind != IR_NODE_LIST) return;
    char unknown[200] = "";
    for (size_t k = 1; k < ir_list_length(rec); k++) {
        const IRNode *child = ir_child_at(rec, k);
        const IRNode *head = child && child->kind == IR_NODE_LIST ? ir_list_head(child) : NULL;
        if (!head || head->kind != IR_NODE_ATOM) continue;
        bool known = false;
        for (size_t f = 0; f < sizeof KNOWN_FIELDS / sizeof KNOWN_FIELDS[0]; f++)
            if (!strcmp(head->text, KNOWN_FIELDS[f])) known = true;
        if (!known) list_add(unknown, sizeof unknown, head->text);
    }
    if (unknown[0]) {
        char m[320];
        snprintf(m, sizeof m, "unknown field(s) %s: the catalogue parser ignores them, so a misspelt field is lost", unknown);
        add(c, PARTS_FINDING_FAIL, "UNKNOWN_FIELD", i, m);
    }
}

static void check_identity(Ctx *c, size_t i, const PartRecord *p)
{
    char missing[160] = "";
    if (!p->manufacturer || !*p->manufacturer) list_add(missing, sizeof missing, "manufacturer");
    if (!p->mpn || !*p->mpn) list_add(missing, sizeof missing, "mpn");
    if (!p->datasheet || !*p->datasheet) list_add(missing, sizeof missing, "datasheet");
    if (!p->symbol_id || !*p->symbol_id) list_add(missing, sizeof missing, "symbol");
    if (!p->footprint_id || !*p->footprint_id) list_add(missing, sizeof missing, "footprint");
    if (missing[0]) {
        char m[240];
        snprintf(m, sizeof m, "missing %s: a library part must name a real, buyable part and its KiCad assets", missing);
        add(c, PARTS_FINDING_FAIL, "MISSING_IDENTITY", i, m);
    }
    if (!p->identity_source.url && !p->identity_source.document) {
        add(c, PARTS_FINDING_FAIL, "NO_IDENTITY_SOURCE", i,
            "no (identity_source ...): nothing says which document the manufacturer and MPN come from");
    } else if (!p->identity_source.revision || !*p->identity_source.revision) {
        add(c, PARTS_FINDING_FAIL, "CITATION_INCOMPLETE", i,
            "the identity source has no (revision ...): name the datasheet revision or the date it was retrieved");
    }
}

static void check_ratings(Ctx *c, size_t i, const PartRecord *p)
{
    PartsCheckPart *s = &c->r->parts[i];
    s->ratings = p->rating_count;
    s->identity_verified = p->identity_source.status == PART_SOURCE_VERIFIED;
    char unverified[200] = "";
    if ((p->identity_source.url || p->identity_source.document) && !s->identity_verified)
        list_add(unverified, sizeof unverified, "identity");
    for (size_t k = 0; k < p->rating_count; k++) {
        const PartRating *rt = &p->ratings[k];
        char what[64];
        snprintf(what, sizeof what, "%s %s", part_rating_name(rt->name), part_rating_bound_name(rt->bound));
        if (rt->source.status == PART_SOURCE_VERIFIED) s->ratings_verified++;
        else list_add(unverified, sizeof unverified, part_rating_name(rt->name));
        if (!rt->source.revision || !*rt->source.revision || rt->source.page == 0) {
            char m[200];
            snprintf(m, sizeof m, "the source of rating '%s' has no %s", what,
                     (!rt->source.revision || !*rt->source.revision) ? (rt->source.page ? "(revision ...)" : "(revision ...) and no (page ...)") : "(page ...)");
            add(c, PARTS_FINDING_FAIL, "CITATION_INCOMPLETE", i, m);
        }
    }
    if (unverified[0]) {
        char m[320];
        snprintf(m, sizeof m, "not verified by a person: %s (transcribed data; a person must check it against the datasheet)", unverified);
        add(c, PARTS_FINDING_UNVERIFIED, "SOURCE_UNVERIFIED", i, m);
    }

    /* Ratings this kind of part needs before a design can be checked against it. */
    PartRatingName need[2] = {PART_RATING_INVALID, PART_RATING_INVALID};
    switch (p->kind) {
    case PART_KIND_RESISTOR: need[0] = PART_RATING_RESISTANCE; need[1] = PART_RATING_POWER; break;
    case PART_KIND_CAPACITOR: need[0] = PART_RATING_CAPACITANCE; need[1] = PART_RATING_VOLTAGE; break;
    case PART_KIND_INDUCTOR: need[0] = PART_RATING_INDUCTANCE; need[1] = PART_RATING_CURRENT; break;
    case PART_KIND_DIODE:
    case PART_KIND_TRANSISTOR:
    case PART_KIND_MOSFET: need[0] = PART_RATING_VOLTAGE; need[1] = PART_RATING_CURRENT; break;
    default: break;
    }
    char absent[100] = "";
    for (size_t n = 0; n < 2; n++) {
        if (need[n] == PART_RATING_INVALID) continue;
        bool found = false;
        for (size_t k = 0; k < p->rating_count; k++)
            if (p->ratings[k].name == need[n]) found = true;
        if (!found) list_add(absent, sizeof absent, part_rating_name(need[n]));
    }
    if (absent[0]) {
        char m[200];
        snprintf(m, sizeof m, "no %s rating: a design using this part cannot be checked against it", absent);
        add(c, PARTS_FINDING_UNVERIFIED, "REQUIRED_RATING_MISSING", i, m);
    }
}

static void check_duplicates(Ctx *c, size_t i, const PartCatalogue *cat)
{
    const PartRecord *p = &cat->parts[i];
    if (!p->mpn || !p->manufacturer) return;
    for (size_t j = 0; j < i; j++) {
        const PartRecord *q = &cat->parts[j];
        if (q->mpn && q->manufacturer && !strcmp(p->mpn, q->mpn) && !strcmp(p->manufacturer, q->manufacturer)) {
            char m[240];
            snprintf(m, sizeof m, "%s %s is already part '%s'", p->manufacturer, p->mpn, q->id);
            add(c, PARTS_FINDING_FAIL, "DUPLICATE_MPN", i, m);
            return;
        }
    }
}

/* ---------------------------------------------------------------------------------------------- KiCad assets */

typedef struct SymLib { const char *lib; bool ok; KicadSymbolLibrary parsed; } SymLib;

static bool split_id(Ctx *c, const char *id, char **lib, char **name)
{
    const char *colon = id ? strchr(id, ':') : NULL;
    if (!colon || colon == id || !colon[1] || strchr(colon + 1, ':')) return false;
    size_t ln = (size_t)(colon - id);
    *lib = arena_alloc(c->a, ln + 1);
    *name = dup_text(c->a, colon + 1);
    if (!*lib || !*name) { c->oom = true; return false; }
    memcpy(*lib, id, ln);
    (*lib)[ln] = 0;
    /* path-safe: no separators, no ".." */
    return !strstr(*lib, "..") && !strstr(*name, "..") && !strpbrk(*lib, "/\\") && !strpbrk(*name, "/\\");
}

static const KicadSymbolLibrary *symbol_library(Ctx *c, SymLib *cache, size_t *count, size_t cap,
                                                const char *root, const char *lib)
{
    for (size_t i = 0; i < *count; i++)
        if (!strcmp(cache[i].lib, lib)) return cache[i].ok ? &cache[i].parsed : NULL;
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
    s->ok = n > 0 && (size_t)n < sizeof name && fs_path_join(file, sizeof file, root, name) == FS_OK &&
            fs_read_file_arena(c->a, file, &data, &size) == FS_OK &&
            kicad_symbol_library_parse(c->a, (const char *)data, size, &s->parsed, &e) == KICAD_ASSET_OK;
    return s->ok ? &s->parsed : NULL;
}

static bool load_footprint(Ctx *c, const char *root, const char *lib, const char *fp, KicadFootprintAsset *out)
{
    char dir[FS_PATH_MAX], file[FS_PATH_MAX], a[FS_PATH_MAX], b[FS_PATH_MAX];
    int n1 = snprintf(a, sizeof a, "%s.pretty", lib), n2 = snprintf(b, sizeof b, "%s.kicad_mod", fp);
    const unsigned char *data = NULL;
    size_t size = 0;
    KicadAssetError e;
    memset(&e, 0, sizeof e);
    return n1 > 0 && (size_t)n1 < sizeof a && n2 > 0 && (size_t)n2 < sizeof b &&
           fs_path_join(dir, sizeof dir, root, a) == FS_OK && fs_path_join(file, sizeof file, dir, b) == FS_OK &&
           fs_read_file_arena(c->a, file, &data, &size) == FS_OK &&
           kicad_footprint_parse(c->a, (const char *)data, size, out, &e) == KICAD_ASSET_OK;
}

static void check_kicad(Ctx *c, size_t i, const PartRecord *p, const char *sroot, const char *froot,
                        SymLib *cache, size_t *cached, size_t cap)
{
    PartsCheckPart *s = &c->r->parts[i];
    char m[300];
    char *slib = NULL, *sname = NULL, *flib = NULL, *fname = NULL;
    s->kicad = "FAIL";
    if (!p->symbol_id || !p->footprint_id) return;   /* already a MISSING_IDENTITY finding */
    if (!split_id(c, p->symbol_id, &slib, &sname) || !split_id(c, p->footprint_id, &flib, &fname)) {
        snprintf(m, sizeof m, "'%s' / '%s' is not a Nickname:Entry KiCad id", p->symbol_id, p->footprint_id);
        add(c, PARTS_FINDING_FAIL, "KICAD_ASSET", i, m);
        return;
    }
    const KicadSymbolLibrary *lib = symbol_library(c, cache, cached, cap, sroot, slib);
    if (!lib) {
        snprintf(m, sizeof m, "symbol library '%s.kicad_sym' was not found or does not parse under the symbol root", slib);
        add(c, PARTS_FINDING_FAIL, "KICAD_ASSET", i, m);
        return;
    }
    KicadFootprintAsset fp;
    memset(&fp, 0, sizeof fp);
    if (!load_footprint(c, froot, flib, fname, &fp)) {
        snprintf(m, sizeof m, "footprint '%s' was not found or does not parse under the footprint root", p->footprint_id);
        add(c, PARTS_FINDING_FAIL, "KICAD_ASSET", i, m);
        return;
    }
    KicadAssetError e;
    memset(&e, 0, sizeof e);
    KicadAssetStatus st = kicad_part_assets_validate(p, lib, &fp, &e);
    if (st != KICAD_ASSET_OK) {
        if (e.item_index != SIZE_MAX && e.item_index < p->pin_count)
            snprintf(m, sizeof m, "%s (pin map entry '%s': symbol pin %s, pad %s)", e.message ? e.message : kicad_asset_status_name(st),
                     p->pins[e.item_index].terminal, p->pins[e.item_index].symbol_pin, p->pins[e.item_index].footprint_pad);
        else
            snprintf(m, sizeof m, "%s (%s, %s)", e.message ? e.message : kicad_asset_status_name(st), p->symbol_id, p->footprint_id);
        add(c, PARTS_FINDING_FAIL, "KICAD_ASSET", i, m);
        return;
    }
    s->kicad = "PASS";
}

/* ---------------------------------------------------------------------------------------------- entry points */

static PartsCheckStatus finish(Ctx *c)
{
    PartsCheckReport *r = c->r;
    if (c->oom) return PARTS_CHECK_ERR_MEMORY;
    r->verdict = r->fail_count ? PARTS_CHECK_FAIL : (r->unverified_count ? PARTS_CHECK_UNVERIFIED : PARTS_CHECK_PASS);
    return r->verdict;
}

static PartsCheckStatus check_catalogue(Ctx *c, const PartCatalogue *cat, const char *sroot, const char *froot)
{
    PartsCheckReport *r = c->r;
    r->part_count = cat->part_count;
    r->parts = cat->part_count ? arena_alloc_array(c->a, sizeof *r->parts, cat->part_count) : NULL;
    if (cat->part_count && !r->parts) return PARTS_CHECK_ERR_MEMORY;
    for (size_t i = 0; i < cat->part_count; i++) {
        PartsCheckPart *s = &r->parts[i];
        memset(s, 0, sizeof *s);
        s->id = cat->parts[i].id;
        s->kind = part_kind_name(cat->parts[i].kind);
        s->mpn = cat->parts[i].mpn;
        s->kicad = "NOT_RUN";
    }
    r->kicad_checked = sroot != NULL;
    SymLib *cache = NULL;
    size_t cached = 0;
    if (sroot && cat->part_count) {
        cache = arena_alloc_array(c->a, sizeof *cache, cat->part_count);
        if (!cache) return PARTS_CHECK_ERR_MEMORY;
    }
    for (size_t i = 0; i < cat->part_count && !c->oom; i++) {
        const PartRecord *p = &cat->parts[i];
        check_fields(c, i, p);
        check_identity(c, i, p);
        check_ratings(c, i, p);
        check_duplicates(c, i, cat);
        if (sroot) check_kicad(c, i, p, sroot, froot, cache, &cached, cat->part_count);
    }
    if (!sroot)
        add(c, PARTS_FINDING_NOT_RUN, "KICAD_NOT_RUN", SIZE_MAX,
            "no KiCad symbol and footprint roots were given: no symbol, footprint, pin or pad was checked");
    return finish(c);
}

static PartsCheckStatus start(Arena *arena, const char *sroot, const char *froot, PartsCheckReport *out, Ctx *c)
{
    if (!out) return PARTS_CHECK_ERR_ARGUMENT;
    memset(out, 0, sizeof *out);
    out->verdict = PARTS_CHECK_ERR_ARGUMENT;
    if (!arena || (!sroot) != (!froot)) return PARTS_CHECK_ERR_ARGUMENT;
    memset(c, 0, sizeof *c);
    c->a = arena;
    c->r = out;
    return PARTS_CHECK_PASS;
}

static PartsCheckStatus invalid(PartsCheckReport *r, const char *what, const PartCatalogueError *e)
{
    if (e && e->item_index != SIZE_MAX)
        snprintf(r->invalid_message, sizeof r->invalid_message, "%s: %s (part %zu, field %s)",
                 part_catalogue_status_name(e->code), e->message ? e->message : "catalogue does not parse",
                 e->item_index + 1, e->field ? e->field : "-");
    else if (e)
        snprintf(r->invalid_message, sizeof r->invalid_message, "%s: %s (field %s)",
                 part_catalogue_status_name(e->code), e->message ? e->message : "catalogue does not parse",
                 e->field ? e->field : "-");
    else
        snprintf(r->invalid_message, sizeof r->invalid_message, "%s", what);
    r->verdict = PARTS_CHECK_INVALID_INPUT;
    return r->verdict;
}

PartsCheckStatus parts_check_text(Arena *arena, const char *text, size_t length, const char *symbols_root,
                                  const char *footprints_root, PartsCheckReport *out)
{
    Ctx c;
    PartsCheckStatus st = start(arena, symbols_root, footprints_root, out, &c);
    if (st != PARTS_CHECK_PASS) return st;
    if (!text) return out->verdict = PARTS_CHECK_ERR_ARGUMENT;
    out->source = "text";
    PartCatalogue cat;
    PartCatalogueError e;
    memset(&e, 0, sizeof e);
    if (part_catalogue_parse(arena, text, length, &cat, &e) != PART_CATALOGUE_OK) return invalid(out, NULL, &e);
    st = check_catalogue(&c, &cat, symbols_root, footprints_root);
    if (st == PARTS_CHECK_ERR_MEMORY) out->verdict = st;
    return st;
}

PartsCheckStatus parts_check_file(Arena *arena, const char *path, const char *symbols_root,
                                  const char *footprints_root, PartsCheckReport *out)
{
    Ctx c;
    PartsCheckStatus st = start(arena, symbols_root, footprints_root, out, &c);
    if (st != PARTS_CHECK_PASS) return st;
    if (!path) return out->verdict = PARTS_CHECK_ERR_ARGUMENT;
    out->source = path;
    const unsigned char *data = NULL;
    size_t size = 0;
    if (fs_read_file_arena(arena, path, &data, &size) != FS_OK) return invalid(out, "the catalogue file cannot be read", NULL);
    PartCatalogue cat;
    PartCatalogueError e;
    memset(&e, 0, sizeof e);
    if (part_catalogue_parse(arena, (const char *)data, size, &cat, &e) != PART_CATALOGUE_OK) return invalid(out, NULL, &e);
    st = check_catalogue(&c, &cat, symbols_root, footprints_root);
    if (st == PARTS_CHECK_ERR_MEMORY) out->verdict = st;
    return st;
}

const char *parts_check_report_json(Arena *arena, const PartsCheckReport *r, size_t *length)
{
    JsonWriter w;
    json_writer_init(&w, arena);
    json_write_begin_object(&w);
    json_write_key(&w, "format"); json_write_string(&w, "copper.parts-check/1");
    json_write_key(&w, "source"); json_write_string(&w, r->source);
    json_write_key(&w, "verdict"); json_write_string(&w, parts_check_status_name(r->verdict));
    if (r->verdict == PARTS_CHECK_INVALID_INPUT) { json_write_key(&w, "message"); json_write_string(&w, r->invalid_message); }
    json_write_key(&w, "kicad_checked"); json_write_bool(&w, r->kicad_checked);
    json_write_key(&w, "fail_count"); json_write_uint(&w, r->fail_count);
    json_write_key(&w, "unverified_count"); json_write_uint(&w, r->unverified_count);
    json_write_key(&w, "parts");
    json_write_begin_array(&w);
    for (size_t i = 0; i < r->part_count; i++) {
        const PartsCheckPart *p = &r->parts[i];
        json_write_begin_object(&w);
        json_write_key(&w, "id"); json_write_string(&w, p->id);
        json_write_key(&w, "kind"); json_write_string(&w, p->kind);
        json_write_key(&w, "mpn"); json_write_string(&w, p->mpn);
        json_write_key(&w, "kicad"); json_write_string(&w, p->kicad);
        json_write_key(&w, "identity_verified"); json_write_bool(&w, p->identity_verified);
        json_write_key(&w, "ratings"); json_write_uint(&w, p->ratings);
        json_write_key(&w, "ratings_verified"); json_write_uint(&w, p->ratings_verified);
        json_write_key(&w, "fails"); json_write_uint(&w, p->fails);
        json_write_key(&w, "unverified"); json_write_uint(&w, p->unverified);
        json_write_end_object(&w);
    }
    json_write_end_array(&w);
    json_write_key(&w, "findings");
    json_write_begin_array(&w);
    for (size_t i = 0; i < r->finding_count; i++) {
        const PartsFinding *f = &r->findings[i];
        json_write_begin_object(&w);
        json_write_key(&w, "level"); json_write_string(&w, parts_finding_level_name(f->level));
        json_write_key(&w, "code"); json_write_string(&w, f->code);
        json_write_key(&w, "part"); json_write_string(&w, f->part_id);
        json_write_key(&w, "message"); json_write_string(&w, f->message);
        json_write_end_object(&w);
    }
    json_write_end_array(&w);
    json_write_end_object(&w);
    return json_writer_finish(&w, length);
}

const char *parts_check_status_name(PartsCheckStatus status)
{
    switch (status) {
    case PARTS_CHECK_PASS: return "PASS";
    case PARTS_CHECK_UNVERIFIED: return "UNVERIFIED";
    case PARTS_CHECK_FAIL: return "FAIL";
    case PARTS_CHECK_INVALID_INPUT: return "INVALID_INPUT";
    case PARTS_CHECK_ERR_ARGUMENT: return "ERR_ARGUMENT";
    case PARTS_CHECK_ERR_MEMORY: return "ERR_MEMORY";
    }
    return "UNKNOWN";
}

const char *parts_finding_level_name(PartsFindingLevel level)
{
    switch (level) {
    case PARTS_FINDING_FAIL: return "FAIL";
    case PARTS_FINDING_UNVERIFIED: return "UNVERIFIED";
    case PARTS_FINDING_NOT_RUN: return "NOT_RUN";
    }
    return "UNKNOWN";
}

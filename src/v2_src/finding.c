/* finding.c - Findings and the JSON report (see finding.h). */
#include "finding.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const STATUS[] = {"FAIL", "INVALID_INPUT", "UNVERIFIED", "NOT_IMPLEMENTED", "INFO"};
static const char *const SEVERITY[] = {"error", "warning", "info"};
static const char *const VERDICT[] = {"PASS", "FAIL", "INVALID_INPUT", "UNVERIFIED"};

const char *finding_status_name(FindingStatus s) { return (unsigned)s < 5 ? STATUS[s] : "UNKNOWN"; }
const char *finding_severity_name(FindingSeverity s) { return (unsigned)s < 3 ? SEVERITY[s] : "unknown"; }
const char *finding_verdict_name(FindingVerdict v) { return (unsigned)v < 4 ? VERDICT[v] : "UNKNOWN"; }

/* Excellon issue codes as names (excellon.h offers only sentences). */
static const char *const EXC_NAMES[] = {
    "none", "missing_header", "missing_units", "missing_end", "unknown_command", "unsupported_format",
    "unsupported_slot", "bad_number", "coord_range", "rounded", "duplicate_tool", "undefined_tool",
    "hit_before_tool", "bad_diameter", "missing_header_end"};

static char *dup(Arena *a, const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s);
    char *d = arena_alloc(a, n + 1);
    if (d) memcpy(d, s, n + 1);
    return d;
}

static char *lower(Arena *a, const char *prefix, const char *name)
{
    size_t np = strlen(prefix), nn = strlen(name);
    char *d = arena_alloc(a, np + nn + 1);
    if (!d) return NULL;
    memcpy(d, prefix, np);
    for (size_t i = 0; i < nn; i++) d[np + i] = (char)(name[i] >= 'A' && name[i] <= 'Z' ? name[i] - 'A' + 'a' : name[i]);
    d[np + nn] = 0;
    return d;
}

void finding_list_init(FindingList *l, Arena *arena)
{
    memset(l, 0, sizeof *l);
    l->arena = arena;
    if (!arena) l->failed = true;
}

static bool grow(Arena *a, void **ptr, size_t *cap, size_t need, size_t elem)
{
    if (need <= *cap) return true;
    size_t n = *cap ? *cap * 2 : 32;
    while (n < need) n *= 2;
    void *p = arena_alloc_array(a, elem, n);
    if (!p) return false;
    if (*ptr && *cap) memcpy(p, *ptr, *cap * elem);
    *ptr = p;
    *cap = n;
    return true;
}

bool finding_add(FindingList *l, const Finding *f)
{
    if (l->failed || !f || !f->source || !f->rule || !f->message) { l->failed = true; return false; }
    if (!grow(l->arena, (void **)&l->items, &l->cap, l->count + 1, sizeof *l->items)) { l->failed = true; return false; }
    Finding *d = &l->items[l->count];
    *d = *f;
    d->source = dup(l->arena, f->source);
    d->rule = dup(l->arena, f->rule);
    d->file = dup(l->arena, f->file);
    d->items[0] = dup(l->arena, f->items[0]);
    d->items[1] = dup(l->arena, f->items[1]);
    d->message = dup(l->arena, f->message);
    d->quote = dup(l->arena, f->quote);
    if (!d->source || !d->rule || !d->message || (f->file && !d->file) || (f->items[0] && !d->items[0]) ||
        (f->items[1] && !d->items[1]) || (f->quote && !d->quote)) { l->failed = true; return false; }
    l->count++;
    return true;
}

static bool add_source(FindingList *l, const char *source, const char *file, FindingVerdict v, size_t passed, size_t findings)
{
    if (l->failed) return false;
    if (!grow(l->arena, (void **)&l->sources, &l->source_cap, l->source_count + 1, sizeof *l->sources)) { l->failed = true; return false; }
    FindingSourceSummary *s = &l->sources[l->source_count++];
    s->source = dup(l->arena, source);
    s->file = dup(l->arena, file);
    s->verdict = v; s->checks_passed = passed; s->findings = findings;
    return true;
}

static FindingVerdict verdict_of(const Finding *f, size_t n)
{
    bool fail = false, invalid = false, unverified = false;
    for (size_t i = 0; i < n; i++) {
        fail |= f[i].status == FINDING_FAIL;
        invalid |= f[i].status == FINDING_INVALID_INPUT;
        unverified |= f[i].status == FINDING_UNVERIFIED || f[i].status == FINDING_NOT_IMPLEMENTED;
    }
    return fail ? FINDING_VERDICT_FAIL : invalid ? FINDING_VERDICT_INVALID_INPUT : unverified ? FINDING_VERDICT_UNVERIFIED : FINDING_VERDICT_PASS;
}

/* ------------------------------------------------------------------------------------------------ mappings */

static const char *item_name(FindingList *l, const Board *b, DfmItemRef r)
{
    char buf[512];
    switch (r.kind) {
    case DFM_ITEM_TRACK: snprintf(buf, sizeof buf, "track %zu", r.index); break;
    case DFM_ITEM_VIA: snprintf(buf, sizeof buf, "via %zu", r.index); break;
    case DFM_ITEM_EDGE: snprintf(buf, sizeof buf, "edge %zu", r.index); break;
    case DFM_ITEM_BOARD: snprintf(buf, sizeof buf, "board"); break;
    case DFM_ITEM_PAD:
        if (b && r.index < b->pad_count) {
            const BoardPad *p = &b->pads[r.index];
            const char *ref = b->footprints[p->footprint].reference;
            snprintf(buf, sizeof buf, "pad %s.%s", ref && *ref ? ref : "?", p->number && *p->number ? p->number : "-");
        } else {
            snprintf(buf, sizeof buf, "pad %zu", r.index);
        }
        break;
    default: return NULL;
    }
    return dup(l->arena, buf);
}

static const char *rule_message(DfmRule rule)
{
    switch (rule) {
    case DFM_RULE_TRACK_WIDTH: return "track narrower than the fabricator's minimum";
    case DFM_RULE_CLEARANCE: return "copper of two nets closer than the fabricator's minimum";
    case DFM_RULE_ANNULAR_RING: return "copper ring around a hole narrower than the fabricator's minimum";
    case DFM_RULE_DRILL: return "hole or via smaller than the fabricator's minimum";
    case DFM_RULE_HOLE_TO_HOLE: return "holes closer than the fabricator's minimum";
    case DFM_RULE_HOLE_TO_TRACK: return "hole closer to a track than the fabricator's minimum";
    case DFM_RULE_COPPER_TO_EDGE: return "copper closer to the board edge than the fabricator's minimum";
    case DFM_RULE_HOLE_TO_EDGE: return "hole closer to the board edge than the fabricator's minimum";
    case DFM_RULE_BOARD_SIZE: return "board outside the fabricator's size limits";
    default: return "outside the fabricator's limits";
    }
}

bool finding_from_dfm(FindingList *l, const char *label, const Board *b, const DfmProfile *p, const DfmReport *r)
{
    if (!r || !p) { l->failed = true; return false; }
    size_t before = l->count, passed = 0;
    for (size_t i = 0; i < r->violation_count; i++) {
        const DfmViolation *v = &r->violations[i];
        char rule[160];
        snprintf(rule, sizeof rule, "%s/%s", dfm_rule_name(v->rule), dfm_limit_key(v->limit));
        Finding f;
        memset(&f, 0, sizeof f);
        f.source = "dfm"; f.rule = rule; f.status = FINDING_FAIL; f.severity = FINDING_SEV_ERROR; f.file = label;
        f.has_point = true; f.at = v->at; f.layers = v->layers;
        f.has_limit = true; f.limit_nm = v->limit_nm;
        f.has_measured = v->measured_nm >= 0; f.measured_nm = v->measured_nm;
        f.items[0] = item_name(l, b, v->a);
        f.items[1] = v->b.kind == DFM_ITEM_NONE ? NULL : item_name(l, b, v->b);
        f.message = rule_message(v->rule);
        f.quote = p->limits[v->limit].quote;
        if (!finding_add(l, &f)) return false;
    }
    for (int k = 0; k < DFM_RULE_COUNT; k++) {
        const DfmRuleResult *x = &r->rules[k];
        if (x->verdict == DFM_PASS) { passed++; continue; }
        if (x->verdict == DFM_FAIL && !x->unverified) continue;          /* its violations are listed */
        char rule[96], msg[512];
        snprintf(rule, sizeof rule, "%s", dfm_rule_name(x->rule));
        snprintf(msg, sizeof msg, "%zu %s not decided: %s", x->unverified, x->unverified == 1 ? "item or pair" : "items or pairs",
                 x->note ? x->note : "no exact check");
        Finding f;
        memset(&f, 0, sizeof f);
        f.source = "dfm"; f.rule = rule; f.file = label; f.message = msg;
        f.status = x->verdict == DFM_NOT_IMPLEMENTED ? FINDING_NOT_IMPLEMENTED : FINDING_UNVERIFIED;
        f.severity = FINDING_SEV_WARNING;
        if (!finding_add(l, &f)) return false;
    }
    return add_source(l, "dfm", label, verdict_of(l->items + before, l->count - before), passed, l->count - before);
}

static FindingStatus status_of(int error, int warning) { return error ? FINDING_INVALID_INPUT : warning ? FINDING_UNVERIFIED : FINDING_INFO; }

bool finding_from_excellon(FindingList *l, const char *label, const ExcFile *x)
{
    if (!x) { l->failed = true; return false; }
    size_t before = l->count;
    for (size_t i = 0; i < x->issue_count; i++) {
        const ExcIssue *e = &x->issues[i];
        const char *name = (unsigned)e->code < sizeof EXC_NAMES / sizeof *EXC_NAMES ? EXC_NAMES[e->code] : "unknown";
        char rule[96], msg[256];
        snprintf(rule, sizeof rule, "excellon/%s", name);
        if (e->tool) snprintf(msg, sizeof msg, "%s (tool T%u)", exc_issue_text(e->code), (unsigned)e->tool);
        else snprintf(msg, sizeof msg, "%s", exc_issue_text(e->code));
        Finding f;
        memset(&f, 0, sizeof f);
        f.source = "excellon"; f.rule = rule; f.file = label; f.line = e->line; f.message = msg;
        f.status = status_of(e->severity == EXC_SEV_ERROR, e->severity == EXC_SEV_WARNING);
        f.severity = e->severity == EXC_SEV_ERROR ? FINDING_SEV_ERROR : e->severity == EXC_SEV_WARNING ? FINDING_SEV_WARNING : FINDING_SEV_INFO;
        if (!finding_add(l, &f)) return false;
    }
    if (x->issues_dropped) {
        char msg[128];
        snprintf(msg, sizeof msg, "%zu more issues were not recorded (the reader keeps a fixed number)", x->issues_dropped);
        Finding f;
        memset(&f, 0, sizeof f);
        f.source = "excellon"; f.rule = "excellon/issues_dropped"; f.file = label; f.message = msg;
        f.status = FINDING_UNVERIFIED; f.severity = FINDING_SEV_WARNING;
        if (!finding_add(l, &f)) return false;
    }
    return add_source(l, "excellon", label, verdict_of(l->items + before, l->count - before), 0, l->count - before);
}

bool finding_from_gerber(FindingList *l, const char *label, const GerberFile *g)
{
    if (!g) { l->failed = true; return false; }
    size_t before = l->count;
    for (size_t i = 0; i < g->issue_count; i++) {
        const GerberIssue *e = &g->issues[i];
        char *rule = lower(l->arena, "gerber/", gerber_issue_name(e->code));
        Finding f;
        memset(&f, 0, sizeof f);
        f.source = "gerber"; f.rule = rule ? rule : "gerber/unknown"; f.file = label; f.line = e->line;
        f.message = e->detail ? e->detail : gerber_issue_name(e->code);
        f.status = status_of(e->severity == GERBER_ERROR, 1);
        f.severity = e->severity == GERBER_ERROR ? FINDING_SEV_ERROR : FINDING_SEV_WARNING;
        if (!finding_add(l, &f)) return false;
    }
    return add_source(l, "gerber", label, verdict_of(l->items + before, l->count - before), 0, l->count - before);
}

bool finding_from_board(FindingList *l, const char *label, const Board *b)
{
    if (!b) { l->failed = true; return false; }
    size_t before = l->count;
    for (size_t i = 0; i < b->issue_count; i++) {
        const BoardIssue *e = &b->issues[i];
        char *rule = lower(l->arena, "board/", board_issue_name(e->code));
        char msg[512];
        snprintf(msg, sizeof msg, "%s (top-level item %zu)", e->context ? e->context : board_issue_name(e->code), e->item);
        Finding f;
        memset(&f, 0, sizeof f);
        f.source = "board"; f.rule = rule ? rule : "board/unknown"; f.file = label; f.message = msg;
        f.status = FINDING_UNVERIFIED; f.severity = FINDING_SEV_WARNING;
        if (!finding_add(l, &f)) return false;
    }
    return add_source(l, "board", label, verdict_of(l->items + before, l->count - before), 0, l->count - before);
}

/* ------------------------------------------------------------------------------------------------ order */

static int scmp(const char *a, const char *b)
{
    if (a == b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    return strcmp(a, b);
}

static int cmp_finding(const void *pa, const void *pb)
{
    const Finding *a = pa, *b = pb;
    int c;
#define NUM(x, y) do { if ((x) != (y)) return (x) < (y) ? -1 : 1; } while (0)
    if ((c = scmp(a->source, b->source))) return c;
    if ((c = scmp(a->file, b->file))) return c;
    if ((c = scmp(a->rule, b->rule))) return c;
    NUM(a->line, b->line);
    NUM(a->has_point, b->has_point);
    NUM(a->at.y, b->at.y);
    NUM(a->at.x, b->at.x);
    if ((c = scmp(a->items[0], b->items[0]))) return c;
    if ((c = scmp(a->items[1], b->items[1]))) return c;
    if ((c = scmp(a->message, b->message))) return c;
    NUM(a->status, b->status);
    NUM(a->measured_nm, b->measured_nm);
#undef NUM
    return 0;
}

static int cmp_source(const void *pa, const void *pb)
{
    const FindingSourceSummary *a = pa, *b = pb;
    int c = scmp(a->source, b->source);
    return c ? c : scmp(a->file, b->file);
}

void finding_sort(FindingList *l)
{
    if (l->count > 1) qsort(l->items, l->count, sizeof *l->items, cmp_finding);
    if (l->source_count > 1) qsort(l->sources, l->source_count, sizeof *l->sources, cmp_source);
}

FindingVerdict finding_verdict(const FindingList *l)
{
    if (l->failed) return FINDING_VERDICT_INVALID_INPUT;
    return verdict_of(l->items, l->count);
}

/* ------------------------------------------------------------------------------------------------ report */

const char *finding_report_json(FindingList *l, Arena *arena, size_t *len)
{
    JsonWriter w;
    finding_sort(l);
    json_writer_init(&w, arena);
    json_write_begin_object(&w);
    json_write_key(&w, "format"); json_write_string(&w, "copper.findings/1");
    json_write_key(&w, "verdict"); json_write_string(&w, finding_verdict_name(finding_verdict(l)));
    json_write_key(&w, "complete"); json_write_bool(&w, !l->failed);
    json_write_key(&w, "counts");
    json_write_begin_object(&w);
    for (int s = 0; s < 5; s++) {
        size_t n = 0;
        for (size_t i = 0; i < l->count; i++) n += l->items[i].status == (FindingStatus)s;
        json_write_key(&w, STATUS[s]); json_write_uint(&w, n);
    }
    json_write_end_object(&w);
    json_write_key(&w, "sources");
    json_write_begin_array(&w);
    for (size_t i = 0; i < l->source_count; i++) {
        const FindingSourceSummary *s = &l->sources[i];
        json_write_begin_object(&w);
        json_write_key(&w, "source"); json_write_string(&w, s->source);
        json_write_key(&w, "file"); json_write_string(&w, s->file);
        json_write_key(&w, "verdict"); json_write_string(&w, finding_verdict_name(s->verdict));
        json_write_key(&w, "findings"); json_write_uint(&w, s->findings);
        if (!strcmp(s->source, "dfm")) { json_write_key(&w, "rules_passed"); json_write_uint(&w, s->checks_passed); }
        json_write_end_object(&w);
    }
    json_write_end_array(&w);
    json_write_key(&w, "findings");
    json_write_begin_array(&w);
    for (size_t i = 0; i < l->count; i++) {
        const Finding *f = &l->items[i];
        json_write_begin_object(&w);
        json_write_key(&w, "source"); json_write_string(&w, f->source);
        json_write_key(&w, "rule"); json_write_string(&w, f->rule);
        json_write_key(&w, "status"); json_write_string(&w, finding_status_name(f->status));
        json_write_key(&w, "severity"); json_write_string(&w, finding_severity_name(f->severity));
        json_write_key(&w, "file"); json_write_string(&w, f->file);
        if (f->line) { json_write_key(&w, "line"); json_write_uint(&w, f->line); }
        if (f->has_point) {
            json_write_key(&w, "at_nm");
            json_write_begin_array(&w); json_write_int(&w, f->at.x); json_write_int(&w, f->at.y); json_write_end_array(&w);
        }
        if (f->layers) { json_write_key(&w, "layers"); json_write_uint(&w, f->layers); }
        if (f->has_limit) { json_write_key(&w, "limit_nm"); json_write_int(&w, f->limit_nm); }
        if (f->has_measured) { json_write_key(&w, "measured_nm"); json_write_int(&w, f->measured_nm); }
        if (f->items[0]) {
            json_write_key(&w, "items");
            json_write_begin_array(&w);
            json_write_string(&w, f->items[0]);
            if (f->items[1]) json_write_string(&w, f->items[1]);
            json_write_end_array(&w);
        }
        json_write_key(&w, "message"); json_write_string(&w, f->message);
        if (f->quote) { json_write_key(&w, "fabricator_says"); json_write_string(&w, f->quote); }
        json_write_end_object(&w);
    }
    json_write_end_array(&w);
    json_write_end_object(&w);
    return json_writer_finish(&w, len);
}

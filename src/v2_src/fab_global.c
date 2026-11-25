/* fab_global.c - the strictest of several fab profiles (see fab_global.h). Integer nanometres only. */
#include "fab_global.h"

#include <string.h>

static const char *const MS_KEYS[FAB_MASK_SILK_COUNT] = {
    "solder_mask_web_min_mm", "silk_line_width_min_mm", "silk_text_height_min_mm", "pad_to_silk_min_mm",
};

const char *fab_mask_silk_key(FabMaskSilkLimit l) { return (unsigned)l < FAB_MASK_SILK_COUNT ? MS_KEYS[l] : "unknown_limit"; }

const char *fab_global_status_name(FabGlobalStatus s)
{
    switch (s) {
    case FAB_GLOBAL_OK: return "OK";
    case FAB_GLOBAL_INVALID_ARGUMENT: return "INVALID_ARGUMENT";
    case FAB_GLOBAL_INVALID_PROFILE: return "INVALID_PROFILE";
    case FAB_GLOBAL_OUT_OF_MEMORY: return "OUT_OF_MEMORY";
    }
    return "UNKNOWN";
}

static FabGlobalStatus gerr(FabGlobalError *e, FabGlobalStatus s, size_t profile, const char *key, const char *message)
{
    if (e) { memset(e, 0, sizeof *e); e->code = s; e->profile = profile; e->key = key; e->message = message; }
    return s;
}

static const char *str(const JsonValue *o, const char *key)
{
    const JsonValue *v = json_get(o, key);
    return v && v->kind == JSON_STRING && v->text_len ? v->text : NULL;
}

/* Maximum board sizes are the only limits where smaller is stricter. */
static bool is_maximum(DfmLimit l)
{
    return l == DFM_BOARD_MAX_X || l == DFM_BOARD_MAX_Y || l == DFM_BOARD_MAX_THIN_X || l == DFM_BOARD_MAX_THIN_Y;
}

/* ------------------------------------------------------------------------------------------------ reading */

FabGlobalStatus fab_mask_silk_from(const JsonValue *root, FabMaskSilk *out, FabGlobalError *e)
{
    if (!root || !out) return gerr(e, FAB_GLOBAL_INVALID_ARGUMENT, 0, NULL, "NULL argument");
    memset(out, 0, sizeof *out);
    if (e) memset(e, 0, sizeof *e);
    if (root->kind != JSON_OBJECT) return gerr(e, FAB_GLOBAL_INVALID_PROFILE, 0, NULL, "a profile is a JSON object");
    const JsonValue *s = json_get(root, "mask_silk_limits");
    if (!s) return FAB_GLOBAL_OK;
    if (s->kind != JSON_OBJECT) return gerr(e, FAB_GLOBAL_INVALID_PROFILE, 0, "mask_silk_limits", "mask_silk_limits is an object");
    for (size_t i = 0; i < s->count; i++) {                     /* a misspelt key must not drop a rule */
        bool known = !strcmp(s->keys[i], "retrieved");
        for (int k = 0; k < FAB_MASK_SILK_COUNT; k++) known |= !strcmp(s->keys[i], MS_KEYS[k]);
        if (!known) return gerr(e, FAB_GLOBAL_INVALID_PROFILE, 0, s->keys[i], "unknown mask/silk limit");
    }
    out->retrieved = str(s, "retrieved");
    if (!out->retrieved) return gerr(e, FAB_GLOBAL_INVALID_PROFILE, 0, "retrieved", "mask_silk_limits records when its source was read");
    for (int k = 0; k < FAB_MASK_SILK_COUNT; k++) {
        const JsonValue *l = json_get(s, MS_KEYS[k]);
        DfmLimitValue *lv = &out->limits[k];
        if (!l || l->kind != JSON_OBJECT) return gerr(e, FAB_GLOBAL_INVALID_PROFILE, 0, MS_KEYS[k], "every limit must be present as {\"value\", \"row\", \"quote\"} (value may be null)");
        const JsonValue *v = json_get(l, "value");
        lv->quote = str(l, "quote");
        lv->row = str(l, "row");
        lv->note = str(l, "note");
        if (!lv->quote) return gerr(e, FAB_GLOBAL_INVALID_PROFILE, 0, MS_KEYS[k], "every limit quotes its source (\"quote\")");
        if (!v) return gerr(e, FAB_GLOBAL_INVALID_PROFILE, 0, MS_KEYS[k], "\"value\" is required (null when not published)");
        if (v->kind == JSON_NULL) continue;
        if (json_to_nm(v, &lv->nm) != JSON_NUMBER_OK || lv->nm < 0 || lv->nm > G2_COORD_MAX_NM)
            return gerr(e, FAB_GLOBAL_INVALID_PROFILE, 0, MS_KEYS[k], "a value is a plain decimal in mm, >= 0, at most 6 decimals");
        if (!lv->row) return gerr(e, FAB_GLOBAL_INVALID_PROFILE, 0, MS_KEYS[k], "a published value names its row (\"row\")");
        lv->present = true;
    }
    out->present = true;
    return FAB_GLOBAL_OK;
}

FabGlobalStatus fab_global_load(Arena *arena, const char *const *paths, size_t count, FabGlobalInput *inputs,
                                FabGlobalError *e)
{
    if (!arena || !paths || !inputs || count == 0) return gerr(e, FAB_GLOBAL_INVALID_ARGUMENT, 0, NULL, "no profiles given");
    for (size_t i = 0; i < count; i++) {
        memset(&inputs[i], 0, sizeof inputs[i]);
        if (!paths[i]) return gerr(e, FAB_GLOBAL_INVALID_ARGUMENT, i, NULL, "NULL path");
        JsonValue *root;
        JsonError je = {0};
        JsonStatus js = json_parse_file(arena, paths[i], &root, &je);
        if (js == JSON_OUT_OF_MEMORY) return gerr(e, FAB_GLOBAL_OUT_OF_MEMORY, i, NULL, "out of memory");
        if (js != JSON_OK) return gerr(e, FAB_GLOBAL_INVALID_PROFILE, i, NULL, je.message ? je.message : "the profile could not be read as JSON");
        /* dfm.c owns the limits object: reuse its parser on the same text so both agree exactly */
        DfmError de = {0};
        DfmStatus ds = dfm_profile_load(arena, paths[i], &inputs[i].profile, &de);
        if (ds == DFM_OUT_OF_MEMORY) return gerr(e, FAB_GLOBAL_OUT_OF_MEMORY, i, NULL, "out of memory");
        if (ds != DFM_OK) return gerr(e, FAB_GLOBAL_INVALID_PROFILE, i, de.key, de.message ? de.message : "invalid profile");
        FabGlobalStatus s = fab_mask_silk_from(root, &inputs[i].mask_silk, e);
        if (s != FAB_GLOBAL_OK) { if (e) e->profile = i; return s; }
    }
    if (e) memset(e, 0, sizeof *e);
    return FAB_GLOBAL_OK;
}

/* ------------------------------------------------------------------------------------------------ strings */

typedef struct Buf { Arena *arena; char *p; size_t len, cap; bool failed; } Buf;

static void put(Buf *b, const char *s)
{
    if (b->failed || !s) return;
    size_t n = strlen(s);
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 128;
        while (cap < b->len + n + 1) cap *= 2;
        char *p = arena_alloc(b->arena, cap);
        if (!p) { b->failed = true; return; }
        if (b->len) memcpy(p, b->p, b->len);
        b->p = p; b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

static const char *done(Buf *b) { return b->failed ? NULL : (b->p ? b->p : ""); }

/* "<fabricator>: <row>" */
static const char *join2(Arena *a, const char *x, const char *sep, const char *y)
{
    Buf b = {a, NULL, 0, 0, false};
    put(&b, x); put(&b, sep); put(&b, y);
    return done(&b);
}

/* mm text of a non-negative nm value: integer part, then up to 6 decimals without trailing zeros */
static void put_mm(Buf *b, G2Nm nm)
{
    char t[40];
    int64_t whole = nm / 1000000, frac = nm % 1000000;
    char w[24]; int wn = 0;
    do { w[wn++] = (char)('0' + (int)(whole % 10)); whole /= 10; } while (whole > 0 && wn < 23);
    size_t n = 0;
    while (wn > 0) t[n++] = w[--wn];
    if (frac) {
        t[n++] = '.';
        char f[6];
        for (int i = 5; i >= 0; i--) { f[i] = (char)('0' + (int)(frac % 10)); frac /= 10; }
        int last = 5;
        while (last > 0 && f[last] == '0') last--;
        for (int i = 0; i <= last; i++) t[n++] = f[i];
    }
    t[n] = '\0';
    put(b, t);
}

/* ------------------------------------------------------------------------------------------------ combining */

/* Picks the strictest present value among n limits (stride over inputs); fills the result and its provenance. */
static FabGlobalStatus pick(Arena *a, const FabGlobalInput *in, size_t n, const DfmLimitValue *const *vals,
                            bool maximum, DfmLimitValue *out, int *from, size_t *missing)
{
    memset(out, 0, sizeof *out);
    *from = -1; *missing = 0;
    for (size_t i = 0; i < n; i++) {
        const DfmLimitValue *v = vals[i];
        if (!v || !v->present) { (*missing)++; continue; }
        bool better = *from < 0 || (maximum ? v->nm < out->nm : v->nm > out->nm);   /* ties keep the earlier */
        if (better) { *out = *v; *from = (int)i; }
    }
    Buf names = {a, NULL, 0, 0, false};
    bool first = true;
    for (size_t i = 0; i < n; i++) {
        if (vals[i] && vals[i]->present) continue;
        put(&names, first ? "" : ", "); put(&names, in[i].profile.fabricator); first = false;
    }
    if (names.failed) return FAB_GLOBAL_OUT_OF_MEMORY;
    if (*from < 0) {
        out->present = false;
        out->row = NULL;
        out->quote = join2(a, "not published by any of: ", "", done(&names));
        out->note = NULL;
        return out->quote ? FAB_GLOBAL_OK : FAB_GLOBAL_OUT_OF_MEMORY;
    }
    const char *fab = in[*from].profile.fabricator;
    out->row = join2(a, fab, ": ", vals[*from]->row);
    out->quote = vals[*from]->quote;
    Buf note = {a, NULL, 0, 0, false};
    put(&note, maximum ? "Strictest (smallest) published maximum, from " : "Strictest (largest) published minimum, from ");
    put(&note, fab); put(&note, ".");
    if (vals[*from]->note) { put(&note, " "); put(&note, fab); put(&note, " note: "); put(&note, vals[*from]->note); }
    if (*missing) { put(&note, " Not published by: "); put(&note, done(&names)); put(&note, " (UNVERIFIED for them)."); }
    out->note = done(&note);
    return out->row && out->note ? FAB_GLOBAL_OK : FAB_GLOBAL_OUT_OF_MEMORY;
}

FabGlobalStatus fab_global_build(Arena *arena, const FabGlobalInput *inputs, size_t count, FabGlobal *out,
                                 FabGlobalError *e)
{
    if (!arena || !inputs || !out || count == 0) return gerr(e, FAB_GLOBAL_INVALID_ARGUMENT, 0, NULL, "no profiles given");
    memset(out, 0, sizeof *out);
    out->count = count;
    const DfmLimitValue **vals = arena_alloc_array(arena, sizeof *vals, count);
    if (!vals) return gerr(e, FAB_GLOBAL_OUT_OF_MEMORY, 0, NULL, "out of memory");
    for (size_t i = 0; i < count; i++)
        if (!inputs[i].profile.fabricator) return gerr(e, FAB_GLOBAL_INVALID_ARGUMENT, i, "fabricator", "every input names its fabricator");
    for (int k = 0; k < DFM_LIMIT_COUNT; k++) {
        for (size_t i = 0; i < count; i++) vals[i] = &inputs[i].profile.limits[k];
        if (pick(arena, inputs, count, vals, is_maximum((DfmLimit)k), &out->limits[k], &out->from[k], &out->missing[k]))
            return gerr(e, FAB_GLOBAL_OUT_OF_MEMORY, 0, NULL, "out of memory");
    }
    for (int k = 0; k < FAB_MASK_SILK_COUNT; k++) {
        for (size_t i = 0; i < count; i++) vals[i] = inputs[i].mask_silk.present ? &inputs[i].mask_silk.limits[k] : NULL;
        if (pick(arena, inputs, count, vals, false, &out->mask_silk[k], &out->mask_silk_from[k], &out->mask_silk_missing[k]))
            return gerr(e, FAB_GLOBAL_OUT_OF_MEMORY, 0, NULL, "out of memory");
    }
    if (e) memset(e, 0, sizeof *e);
    return FAB_GLOBAL_OK;
}

/* ------------------------------------------------------------------------------------------------ writing */

static void write_limit(JsonWriter *w, Arena *a, const char *key, const DfmLimitValue *v)
{
    json_write_key(w, key);
    json_write_begin_object(w);
    json_write_key(w, "value");
    if (v->present) {
        Buf b = {a, NULL, 0, 0, false};
        put_mm(&b, v->nm);
        const char *t = done(&b);
        if (t) json_write_raw(w, t, strlen(t)); else w->failed = true;
    } else {
        json_write_null(w);
    }
    json_write_key(w, "row"); json_write_string(w, v->row);
    json_write_key(w, "quote"); json_write_string(w, v->quote);
    if (v->note) { json_write_key(w, "note"); json_write_string(w, v->note); }
    json_write_end_object(w);
}

FabGlobalStatus fab_global_write(Arena *arena, const FabGlobalInput *inputs, const FabGlobal *g, const char *name,
                                 const char **text, size_t *len)
{
    if (!arena || !inputs || !g || !name || !text || !len || g->count == 0) return FAB_GLOBAL_INVALID_ARGUMENT;
    /* the earliest retrieval date of any input: the global profile is no fresher than its oldest source */
    const char *oldest = NULL;
    Buf fabs = {arena, NULL, 0, 0, false};
    for (size_t i = 0; i < g->count; i++) {
        const char *r = inputs[i].profile.retrieved;
        if (r && (!oldest || strcmp(r, oldest) < 0)) oldest = r;
        put(&fabs, i ? ", " : ""); put(&fabs, inputs[i].profile.fabricator);
    }
    const char *fab_list = done(&fabs);
    if (!fab_list) return FAB_GLOBAL_OUT_OF_MEMORY;

    JsonWriter w;
    json_writer_init(&w, arena);
    json_write_begin_object(&w);
    json_write_key(&w, "format"); json_write_string(&w, "copper.fab_profile/1");
    json_write_key(&w, "name"); json_write_string(&w, name);
    json_write_key(&w, "fabricator"); json_write_string(&w, join2(arena, "strictest of: ", "", fab_list));
    json_write_key(&w, "process"); json_write_string(&w, "Derived by copper-core fab-global (src/v2_src/fab_global.c): each limit is the strictest value the listed profiles publish. Do not edit; regenerate from the inputs.");
    json_write_key(&w, "source");
    json_write_begin_object(&w);
    json_write_key(&w, "url"); json_write_string(&w, "derived:fab_global");
    json_write_key(&w, "title"); json_write_string(&w, join2(arena, "Strictest of the fab profiles of ", "", fab_list));
    json_write_key(&w, "retrieved"); json_write_string(&w, oldest ? oldest : "unknown");
    json_write_end_object(&w);
    json_write_key(&w, "derived_from");
    json_write_begin_array(&w);
    for (size_t i = 0; i < g->count; i++) {
        const DfmProfile *p = &inputs[i].profile;
        json_write_begin_object(&w);
        json_write_key(&w, "name"); json_write_string(&w, p->name);
        json_write_key(&w, "fabricator"); json_write_string(&w, p->fabricator);
        json_write_key(&w, "url"); json_write_string(&w, p->source_url);
        json_write_key(&w, "retrieved"); json_write_string(&w, p->retrieved);
        json_write_key(&w, "mask_silk_retrieved"); json_write_string(&w, inputs[i].mask_silk.present ? inputs[i].mask_silk.retrieved : NULL);
        json_write_end_object(&w);
    }
    json_write_end_array(&w);
    json_write_key(&w, "limits");
    json_write_begin_object(&w);
    for (int k = 0; k < DFM_LIMIT_COUNT; k++) write_limit(&w, arena, dfm_limit_key((DfmLimit)k), &g->limits[k]);
    json_write_end_object(&w);
    json_write_key(&w, "mask_silk_limits");
    json_write_begin_object(&w);
    const char *ms_oldest = NULL;
    for (size_t i = 0; i < g->count; i++) {
        const char *r = inputs[i].mask_silk.present ? inputs[i].mask_silk.retrieved : NULL;
        if (r && (!ms_oldest || strcmp(r, ms_oldest) < 0)) ms_oldest = r;
    }
    json_write_key(&w, "retrieved"); json_write_string(&w, ms_oldest ? ms_oldest : "unknown");
    for (int k = 0; k < FAB_MASK_SILK_COUNT; k++) write_limit(&w, arena, MS_KEYS[k], &g->mask_silk[k]);
    json_write_end_object(&w);
    json_write_end_object(&w);
    if (w.failed) return FAB_GLOBAL_OUT_OF_MEMORY;
    *text = json_writer_finish(&w, len);
    return *text ? FAB_GLOBAL_OK : FAB_GLOBAL_OUT_OF_MEMORY;
}

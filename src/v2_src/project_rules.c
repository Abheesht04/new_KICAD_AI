/* project_rules.c - KiCad project rules from a fab profile and Copper's routing rules (see project_rules.h). */
#include "project_rules.h"

#include <string.h>

#define UDEG_90 INT64_C(90000000)
#define UDEG_360 INT64_C(360000000)

const char *pr_status_name(PrStatus s)
{
    switch (s) {
    case PR_OK: return "OK";
    case PR_INVALID_ARGUMENT: return "INVALID_ARGUMENT";
    case PR_INVALID_RULES: return "INVALID_RULES";
    case PR_INVALID_PROJECT: return "INVALID_PROJECT";
    case PR_OUT_OF_MEMORY: return "OUT_OF_MEMORY";
    }
    return "UNKNOWN";
}

static PrStatus kerr(PrError *e, PrStatus s, const char *key, const char *message)
{
    if (e) { memset(e, 0, sizeof *e); e->code = s; e->key = key; e->message = message; }
    return s;
}

/* ------------------------------------------------------------------------------------------------ text */

typedef struct Buf { Arena *arena; char *p; size_t len, cap; bool failed; } Buf;

static void put_n(Buf *b, const char *s, size_t n)
{
    if (b->failed || !s) return;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 256;
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

static void put(Buf *b, const char *s) { if (s) put_n(b, s, strlen(s)); }

/* a non-negative length in mm: integer part, then up to 6 decimals without trailing zeros */
static void put_mm(Buf *b, G2Nm nm)
{
    char t[40];
    size_t n = 0;
    if (nm < 0) { t[n++] = '-'; nm = -nm; }
    int64_t whole = nm / 1000000, frac = nm % 1000000;
    char w[24]; int wn = 0;
    do { w[wn++] = (char)('0' + (int)(whole % 10)); whole /= 10; } while (whole > 0 && wn < 23);
    while (wn > 0) t[n++] = w[--wn];
    if (frac) {
        char f[6];
        for (int i = 5; i >= 0; i--) { f[i] = (char)('0' + (int)(frac % 10)); frac /= 10; }
        int last = 5;
        while (last > 0 && f[last] == '0') last--;
        t[n++] = '.';
        for (int i = 0; i <= last; i++) t[n++] = f[i];
    }
    t[n] = '\0';
    put(b, t);
}

static const char *mm_text(Arena *a, G2Nm nm)
{
    Buf b = {a, NULL, 0, 0, false};
    put_mm(&b, nm);
    return b.failed ? NULL : b.p;
}

/* ------------------------------------------------------------------------------------------------ routing rules */

static PrStatus rule_value(const JsonValue *rules, const char *key, G2Nm *out, PrError *e)
{
    const JsonValue *v = json_get(json_get(rules, key), "value");
    if (!v || json_to_nm(v, out) != JSON_NUMBER_OK || *out <= 0)
        return kerr(e, PR_INVALID_RULES, key, "routing rules need a positive {\"value\": mm} for this key");
    return PR_OK;
}

PrStatus pr_design_rules_from(const JsonValue *root, PrDesignRules *out, PrError *e)
{
    if (!root || !out) return kerr(e, PR_INVALID_ARGUMENT, NULL, "NULL argument");
    memset(out, 0, sizeof *out);
    const JsonValue *f = json_get(root, "format");
    if (!f || f->kind != JSON_STRING || strcmp(f->text, "copper.routing_rules/1"))
        return kerr(e, PR_INVALID_RULES, "format", "format must be \"copper.routing_rules/1\"");
    const JsonValue *n = json_get(root, "name");
    out->name = n && n->kind == JSON_STRING ? n->text : NULL;
    const JsonValue *rules = json_get(root, "rules");
    PrStatus s;
    if ((s = rule_value(rules, "track_mm", &out->track, e))) return s;
    if ((s = rule_value(rules, "clearance_mm", &out->clearance, e))) return s;
    if ((s = rule_value(rules, "via_size_mm", &out->via_diameter, e))) return s;
    if ((s = rule_value(rules, "via_drill_mm", &out->via_drill, e))) return s;
    if (json_get(rules, "fine_pitch_clearance_mm")) {   /* Step 3c: optional */
        if ((s = rule_value(rules, "fine_pitch_clearance_mm", &out->fine_clearance, e))) return s;
        out->has_fine = true;
    }
    if (e) memset(e, 0, sizeof *e);
    return PR_OK;
}

/* Copper's rules may never be below the fab's limit (copper design route refuses the same). */
static PrStatus check_rules(const PrDesignRules *r, const DfmProfile *f, PrError *e)
{
    const DfmLimitValue *L = f->limits;
    if (r->via_drill >= r->via_diameter) return kerr(e, PR_INVALID_RULES, "via_drill_mm", "the via drill must be smaller than the via");
#define BELOW(lim, val) (L[lim].present && (val) < L[lim].nm)
    if (BELOW(DFM_TRACK_WIDTH_MIN, r->track)) return kerr(e, PR_INVALID_RULES, "track_mm", "below the fab's minimum track width");
    if (BELOW(DFM_CLEARANCE_DEFAULT_MIN, r->clearance) || BELOW(DFM_TRACK_SPACING_MIN, r->clearance))
        return kerr(e, PR_INVALID_RULES, "clearance_mm", "below the fab's minimum copper clearance");
    if (BELOW(DFM_VIA_HOLE_MIN, r->via_drill) || BELOW(DFM_DRILL_MIN, r->via_drill))
        return kerr(e, PR_INVALID_RULES, "via_drill_mm", "below the fab's minimum via hole");
    if (BELOW(DFM_VIA_DIAMETER_MIN, r->via_diameter)) return kerr(e, PR_INVALID_RULES, "via_size_mm", "below the fab's minimum via diameter");
    if (BELOW(DFM_VIA_DIAMETER_OVER_HOLE_MIN, r->via_diameter - r->via_drill))
        return kerr(e, PR_INVALID_RULES, "via_size_mm", "the via ring is below the fab's minimum");
    if (r->has_fine && (r->fine_clearance <= 0 || BELOW(DFM_CLEARANCE_DEFAULT_MIN, r->fine_clearance) || BELOW(DFM_TRACK_SPACING_MIN, r->fine_clearance) ||
                        BELOW(DFM_PAD_TO_TRACK_MIN, r->fine_clearance) || BELOW(DFM_SMD_PAD_TO_PAD_MIN, r->fine_clearance)))
        return kerr(e, PR_INVALID_RULES, "fine_pitch_clearance_mm", "below one of the fab's copper limits (copper design route refuses it too)");
#undef BELOW
    return PR_OK;
}

/* ------------------------------------------------------------------------------------------------ settings */

typedef struct Map { const char *kicad_key; int fab; bool mask_silk; bool half; } Map;

static const Map MAP[] = {
    {"min_clearance", DFM_CLEARANCE_DEFAULT_MIN, false, false},
    {"min_track_width", DFM_TRACK_WIDTH_MIN, false, false},
    {"min_through_hole_diameter", DFM_DRILL_MIN, false, false},
    {"min_hole_to_hole", DFM_VIA_HOLE_TO_HOLE_MIN, false, false},
    {"min_hole_clearance", DFM_VIA_HOLE_TO_TRACK_MIN, false, false},
    {"min_copper_edge_clearance", DFM_COPPER_TO_EDGE_MIN, false, false},
    {"min_via_diameter", DFM_VIA_DIAMETER_MIN, false, false},
    {"min_via_annular_width", DFM_VIA_DIAMETER_OVER_HOLE_MIN, false, true},
    {"min_text_height", FAB_SILK_TEXT_HEIGHT_MIN, true, false},
    {"min_text_thickness", FAB_SILK_LINE_WIDTH_MIN, true, false},
};
#define MAP_COUNT (sizeof MAP / sizeof MAP[0])

static PrStatus build_settings(Arena *a, const DfmProfile *fab, const FabMaskSilk *ms, PrResult *out)
{
    out->setting_count = MAP_COUNT + 1;
    out->settings = arena_alloc_array(a, sizeof *out->settings, out->setting_count);
    if (!out->settings) return PR_OUT_OF_MEMORY;
    for (size_t i = 0; i < MAP_COUNT; i++) {
        PrSetting *s = &out->settings[i];
        memset(s, 0, sizeof *s);
        s->kicad_key = MAP[i].kicad_key;
        const char *key = MAP[i].mask_silk ? fab_mask_silk_key((FabMaskSilkLimit)MAP[i].fab) : dfm_limit_key((DfmLimit)MAP[i].fab);
        Buf from = {a, NULL, 0, 0, false};
        put(&from, "fab:"); put(&from, key);
        s->from = from.p;
        const DfmLimitValue *v = MAP[i].mask_silk ? (ms && ms->present ? &ms->limits[MAP[i].fab] : NULL) : &fab->limits[MAP[i].fab];
        if (!v || !v->present) {
            s->note = "the fab publishes no value: not written, so the KiCad default stands and KiCad's DRC result for it is not fab evidence";
            continue;
        }
        s->written = true;
        s->nm = MAP[i].half ? (v->nm + 1) / 2 : v->nm;
        if (MAP[i].half) s->note = "half the fab's via diameter over hole, rounded up";
        if (from.failed) return PR_OUT_OF_MEMORY;
    }
    PrSetting *silk = &out->settings[MAP_COUNT];
    memset(silk, 0, sizeof *silk);
    silk->kicad_key = "min_silk_clearance";
    silk->from = "fab:pad_to_silk_min_mm";
    silk->note = "not written: KiCad's min_silk_clearance is silkscreen to silkscreen, the fab's limit is pad to silkscreen";
    return PR_OK;
}

/* ------------------------------------------------------------------------------------------------ fine pitch */

/* half extents of a pad's box on the board; *approx when the box is larger than the pad's own box */
static void pad_half(const BoardPad *p, G2Nm *hx, G2Nm *hy, bool *approx)
{
    int64_t a = p->angle_udeg % UDEG_360;
    if (a < 0) a += UDEG_360;
    if (a % UDEG_90 == 0) {
        bool swap = (a / UDEG_90) % 2 == 1;
        *hx = (swap ? p->size_y : p->size_x) / 2 + (swap ? p->size_y : p->size_x) % 2;
        *hy = (swap ? p->size_x : p->size_y) / 2 + (swap ? p->size_x : p->size_y) % 2;
        return;
    }
    G2Nm h = (p->size_x + p->size_y + 1) / 2;   /* contains the turned pad: |cos| + |sin| <= ... sx/2 + sy/2 each way */
    *hx = h; *hy = h; *approx = true;
}

static bool quotable(const char *ref)
{
    if (!ref || !*ref) return false;
    for (const char *c = ref; *c; c++) {
        char x = *c;
        bool ok = (x >= 'A' && x <= 'Z') || (x >= 'a' && x <= 'z') || (x >= '0' && x <= '9') || x == '_' || x == '-' || x == '+' || x == '.';
        if (!ok) return false;
    }
    return true;
}

static PrStatus find_fine_pitch(Arena *a, const Board *b, const DfmProfile *fab, const PrDesignRules *r, PrResult *out)
{
    out->fine = arena_alloc_array(a, sizeof *out->fine, b->footprint_count ? b->footprint_count : 1);
    if (!out->fine) return PR_OUT_OF_MEMORY;
    out->fine_count = 0;
    const DfmLimitValue *pad = &fab->limits[DFM_SMD_PAD_TO_PAD_MIN], *def = &fab->limits[DFM_CLEARANCE_DEFAULT_MIN];
    for (size_t f = 0; f < b->footprint_count; f++) {
        const BoardFootprint *fp = &b->footprints[f];
        bool have = false, approx_best = false;
        G2Nm best = 0;
        const char *pa = NULL, *pb = NULL;
        for (size_t i = fp->first_pad; i < fp->first_pad + fp->pad_count; i++) {
            const BoardPad *p = &b->pads[i];
            if (p->type == BOARD_PAD_NP_THRU_HOLE) continue;
            for (size_t j = i + 1; j < fp->first_pad + fp->pad_count; j++) {
                const BoardPad *q = &b->pads[j];
                if (q->type == BOARD_PAD_NP_THRU_HOLE) continue;
                if (p->net >= 0 && p->net == q->net) continue;                       /* same net: no clearance */
                if (*p->number && !strcmp(p->number, q->number)) continue;           /* one pin, several pads */
                if (!(p->layers & q->layers)) continue;                              /* never on the same layer */
                bool approx = false;
                G2Nm phx, phy, qhx, qhy;
                pad_half(p, &phx, &phy, &approx);
                pad_half(q, &qhx, &qhy, &approx);
                G2Nm dx = p->at.x - q->at.x, dy = p->at.y - q->at.y;
                if (dx < 0) dx = -dx;
                if (dy < 0) dy = -dy;
                G2Nm gx = dx - phx - qhx, gy = dy - phy - qhy;
                G2Nm gap;
                if (gx > 0 && gy > 0) { gap = gx > gy ? gx : gy; approx = true; }   /* diagonal: a lower bound */
                else gap = gx > gy ? gx : gy;
                if (p->shape == BOARD_SHAPE_CIRCLE || p->shape == BOARD_SHAPE_OVAL || q->shape == BOARD_SHAPE_CIRCLE ||
                    q->shape == BOARD_SHAPE_OVAL || p->shape >= BOARD_SHAPE_TRAPEZOID || q->shape >= BOARD_SHAPE_TRAPEZOID)
                    approx = true;
                if (gap < 0) gap = 0;
                if (!have || gap < best) { have = true; best = gap; pa = p->number; pb = q->number; approx_best = approx; }
            }
        }
        if (!have || best >= r->clearance) continue;
        PrFinePitch *k = &out->fine[out->fine_count++];
        memset(k, 0, sizeof *k);
        k->reference = fp->reference;
        k->pad_a = pa; k->pad_b = pb;
        k->min_gap = best;
        k->approximate = approx_best;
        if (pad->present) { k->floor_present = true; k->floor = pad->nm; k->floor_from = "fab:smd_pad_to_pad_min_mm"; }
        else if (def->present) { k->floor_present = true; k->floor = def->nm; k->floor_from = "fab:clearance_default_min_mm"; }
        k->rule_written = k->floor_present && quotable(fp->reference);
        if (!k->rule_written) k->verdict = DFM_UNVERIFIED;
        else k->verdict = best >= k->floor ? DFM_PASS : DFM_FAIL;
    }
    out->verdict = DFM_PASS;
    for (size_t i = 0; i < out->fine_count; i++) {
        if (out->fine[i].verdict == DFM_FAIL) out->verdict = DFM_FAIL;
        else if (out->fine[i].verdict == DFM_UNVERIFIED && out->verdict != DFM_FAIL) out->verdict = DFM_UNVERIFIED;
    }
    return PR_OK;
}

/* ------------------------------------------------------------------------------------------------ .kicad_pro */

static void copy_value(JsonWriter *w, const JsonValue *v)
{
    switch (v->kind) {
    case JSON_NULL: json_write_null(w); break;
    case JSON_FALSE: json_write_bool(w, false); break;
    case JSON_TRUE: json_write_bool(w, true); break;
    case JSON_NUMBER: json_write_raw(w, v->text, v->text_len); break;
    case JSON_STRING: json_write_string(w, v->text); break;
    case JSON_ARRAY:
        json_write_begin_array(w);
        for (size_t i = 0; i < v->count; i++) copy_value(w, v->items[i]);
        json_write_end_array(w);
        break;
    case JSON_OBJECT:
        json_write_begin_object(w);
        for (size_t i = 0; i < v->count; i++) { json_write_key(w, v->keys[i]); copy_value(w, v->items[i]); }
        json_write_end_object(w);
        break;
    }
}

typedef struct Num { const char *key; G2Nm nm; } Num;

static void write_mm(JsonWriter *w, Arena *a, G2Nm nm)
{
    const char *t = mm_text(a, nm);
    if (t) json_write_raw(w, t, strlen(t)); else w->failed = true;
}

/* an object with `nums` replaced or appended; NULL obj makes a new one */
static void write_with_nums(JsonWriter *w, Arena *a, const JsonValue *obj, const Num *nums, size_t n)
{
    json_write_begin_object(w);
    if (obj && obj->kind == JSON_OBJECT) {
        for (size_t i = 0; i < obj->count; i++) {
            json_write_key(w, obj->keys[i]);
            size_t k = 0;
            while (k < n && strcmp(nums[k].key, obj->keys[i])) k++;
            if (k < n) write_mm(w, a, nums[k].nm); else copy_value(w, obj->items[i]);
        }
    }
    for (size_t k = 0; k < n; k++) {
        if (obj && obj->kind == JSON_OBJECT && json_get(obj, nums[k].key)) continue;
        json_write_key(w, nums[k].key); write_mm(w, a, nums[k].nm);
    }
    json_write_end_object(w);
}

typedef struct ProCtx {
    Arena *arena;
    const PrResult *res;
    const PrDesignRules *rules;
    Num rule_nums[MAP_COUNT];
    size_t rule_num_count;
    Num class_nums[4];
} ProCtx;

static void write_design_settings(JsonWriter *w, ProCtx *c, const JsonValue *ds)
{
    json_write_begin_object(w);
    bool seen = false;
    if (ds && ds->kind == JSON_OBJECT) {
        for (size_t i = 0; i < ds->count; i++) {
            json_write_key(w, ds->keys[i]);
            if (!strcmp(ds->keys[i], "rules")) { write_with_nums(w, c->arena, ds->items[i], c->rule_nums, c->rule_num_count); seen = true; }
            else copy_value(w, ds->items[i]);
        }
    }
    if (!seen) { json_write_key(w, "rules"); write_with_nums(w, c->arena, NULL, c->rule_nums, c->rule_num_count); }
    json_write_end_object(w);
}

static void write_board(JsonWriter *w, ProCtx *c, const JsonValue *board)
{
    json_write_begin_object(w);
    bool seen = false;
    if (board && board->kind == JSON_OBJECT) {
        for (size_t i = 0; i < board->count; i++) {
            json_write_key(w, board->keys[i]);
            if (!strcmp(board->keys[i], "design_settings")) { write_design_settings(w, c, board->items[i]); seen = true; }
            else copy_value(w, board->items[i]);
        }
    }
    if (!seen) { json_write_key(w, "design_settings"); write_design_settings(w, c, NULL); }
    json_write_end_object(w);
}

static bool is_default(const JsonValue *cls)
{
    const JsonValue *n = json_get(cls, "name");
    return n && n->kind == JSON_STRING && !strcmp(n->text, "Default");
}

static void write_new_default(JsonWriter *w, ProCtx *c)
{
    json_write_begin_object(w);
    json_write_key(w, "name"); json_write_string(w, "Default");
    for (size_t k = 0; k < 4; k++) { json_write_key(w, c->class_nums[k].key); write_mm(w, c->arena, c->class_nums[k].nm); }
    json_write_key(w, "priority"); json_write_int(w, INT64_C(2147483647));
    json_write_end_object(w);
}

static void write_net_settings(JsonWriter *w, ProCtx *c, const JsonValue *ns)
{
    json_write_begin_object(w);
    bool seen_classes = false, seen_meta = false;
    if (ns && ns->kind == JSON_OBJECT) {
        for (size_t i = 0; i < ns->count; i++) {
            const JsonValue *v = ns->items[i];
            json_write_key(w, ns->keys[i]);
            if (!strcmp(ns->keys[i], "meta")) seen_meta = true;
            if (strcmp(ns->keys[i], "classes") || v->kind != JSON_ARRAY) { copy_value(w, v); continue; }
            seen_classes = true;
            bool found = false;
            json_write_begin_array(w);
            for (size_t j = 0; j < v->count; j++) {
                if (!found && is_default(v->items[j])) { write_with_nums(w, c->arena, v->items[j], c->class_nums, 4); found = true; }
                else copy_value(w, v->items[j]);
            }
            if (!found) write_new_default(w, c);
            json_write_end_array(w);
        }
    }
    if (!seen_classes) { json_write_key(w, "classes"); json_write_begin_array(w); write_new_default(w, c); json_write_end_array(w); }
    if (!seen_meta) {   /* the net_settings version of a KiCad 9.0 project file */
        json_write_key(w, "meta"); json_write_begin_object(w); json_write_key(w, "version"); json_write_int(w, 4); json_write_end_object(w);
    }
    json_write_end_object(w);
}

static PrStatus write_pro(Arena *a, const JsonValue *existing, const char *filename, ProCtx *c, PrResult *out)
{
    JsonWriter w;
    json_writer_init(&w, a);
    json_write_begin_object(&w);
    bool seen_board = false, seen_ns = false, seen_meta = false;
    if (existing) {
        for (size_t i = 0; i < existing->count; i++) {
            const char *k = existing->keys[i];
            json_write_key(&w, k);
            if (!strcmp(k, "board")) { write_board(&w, c, existing->items[i]); seen_board = true; }
            else if (!strcmp(k, "net_settings")) { write_net_settings(&w, c, existing->items[i]); seen_ns = true; }
            else { if (!strcmp(k, "meta")) seen_meta = true; copy_value(&w, existing->items[i]); }
        }
    }
    if (!seen_board) { json_write_key(&w, "board"); write_board(&w, c, NULL); }
    if (!seen_meta) {   /* a new file: the project version of a KiCad 9.0 project file */
        json_write_key(&w, "meta"); json_write_begin_object(&w);
        json_write_key(&w, "filename"); json_write_string(&w, filename);
        json_write_key(&w, "version"); json_write_int(&w, 3);
        json_write_end_object(&w);
    }
    if (!seen_ns) { json_write_key(&w, "net_settings"); write_net_settings(&w, c, NULL); }
    json_write_end_object(&w);
    if (w.failed) return PR_OUT_OF_MEMORY;
    out->pro_text = json_writer_finish(&w, &out->pro_len);
    return out->pro_text ? PR_OK : PR_OUT_OF_MEMORY;
}

/* ------------------------------------------------------------------------------------------------ .kicad_dru */

static PrStatus write_dru(Arena *a, const DfmProfile *fab, const PrDesignRules *r, PrResult *out)
{
    Buf b = {a, NULL, 0, 0, false};
    put(&b, PR_DRU_HEADER "\n");
    put(&b, "# Target fab profile: "); put(&b, fab->name); put(&b, " ("); put(&b, fab->fabricator); put(&b, "). Routing rules: ");
    put(&b, r->name ? r->name : "(unnamed)"); put(&b, ".\n");
    put(&b, "# Fine-pitch footprints (LAYOUT_RULES.md R5): clearance between a footprint's own pads is the fab's limit,\n");
    put(&b, "# not the Default net class clearance (");
    put_mm(&b, r->clearance);
    put(&b, " mm). Never below the fab's limit.\n(version 1)\n");
    for (size_t i = 0; i < out->fine_count; i++) {
        const PrFinePitch *k = &out->fine[i];
        if (!k->rule_written) continue;
        put(&b, "\n# "); put(&b, k->reference); put(&b, ": own pads "); put(&b, k->pad_a); put(&b, " and "); put(&b, k->pad_b);
        put(&b, " are "); put_mm(&b, k->min_gap); put(&b, k->approximate ? " mm apart or more" : " mm apart");
        put(&b, "; "); put(&b, fab->fabricator); put(&b, " "); put(&b, k->floor_from + 4); put(&b, " "); put_mm(&b, k->floor);
        put(&b, " mm: "); put(&b, dfm_verdict_name(k->verdict)); put(&b, "\n");
        put(&b, "(rule \"copper_fine_pitch_"); put(&b, k->reference); put(&b, "\"\n");
        put(&b, "  (constraint clearance (min "); put_mm(&b, k->floor); put(&b, "mm))\n");
        put(&b, "  (condition \"A.memberOfFootprint('"); put(&b, k->reference); put(&b, "') && B.memberOfFootprint('");
        put(&b, k->reference); put(&b, "')\"))\n");
        if (r->has_fine) {   /* Step 3c: the router's escape stubs into this footprint */
            put(&b, "# the router's escape stubs: a track entering "); put(&b, k->reference);
            put(&b, "'s courtyard keeps the routing rules' fine_pitch_clearance_mm to its pads\n");
            put(&b, "(rule \"copper_fine_pitch_"); put(&b, k->reference); put(&b, "_escape\"\n");
            put(&b, "  (constraint clearance (min "); put_mm(&b, r->fine_clearance); put(&b, "mm))\n");
            put(&b, "  (condition \"A.memberOfFootprint('"); put(&b, k->reference); put(&b, "') && B.Type == 'Track' && B.intersectsCourtyard('");
            put(&b, k->reference); put(&b, "')\"))\n");
        }
    }
    if (b.failed) return PR_OUT_OF_MEMORY;
    out->dru_text = b.p; out->dru_len = b.len;
    return PR_OK;
}

/* ------------------------------------------------------------------------------------------------ report */

static void write_limit_mm(JsonWriter *w, Arena *a, const DfmLimitValue *v)
{
    if (v && v->present) write_mm(w, a, v->nm); else json_write_null(w);
}

/* a fab copper limit KiCad has no board minimum for: covered by the Default clearance, or not checked by KiCad */
static void coverage(JsonWriter *w, Arena *a, const char *limit, const DfmLimitValue *v, G2Nm by, const char *by_name, const char *otherwise)
{
    json_write_begin_object(w);
    json_write_key(w, "limit"); json_write_string(w, limit);
    json_write_key(w, "value_mm"); write_limit_mm(w, a, v);
    json_write_key(w, "status");
    if (!v || !v->present) json_write_string(w, "NOT_PUBLISHED");
    else if (by_name && by >= v->nm) json_write_string(w, "COVERED");
    else json_write_string(w, "NOT_CHECKED_BY_KICAD");
    json_write_key(w, "note");
    if (v && v->present && by_name && by >= v->nm) {
        Buf b = {a, NULL, 0, 0, false};
        put(&b, "KiCad checks "); put(&b, by_name); put(&b, " ("); put_mm(&b, by); put(&b, " mm), which is at least this limit");
        json_write_string(w, b.failed ? NULL : b.p);
    } else json_write_string(w, otherwise);
    json_write_end_object(w);
}

static PrStatus write_report(Arena *a, const DfmProfile *fab, const FabMaskSilk *ms, const PrDesignRules *r, PrResult *out)
{
    JsonWriter w;
    json_writer_init(&w, a);
    json_write_begin_object(&w);
    json_write_key(&w, "format"); json_write_string(&w, "copper.kicad-rules/1");
    json_write_key(&w, "verdict"); json_write_string(&w, dfm_verdict_name(out->verdict));
    json_write_key(&w, "fab");
    json_write_begin_object(&w);
    json_write_key(&w, "name"); json_write_string(&w, fab->name);
    json_write_key(&w, "fabricator"); json_write_string(&w, fab->fabricator);
    json_write_key(&w, "url"); json_write_string(&w, fab->source_url);
    json_write_key(&w, "retrieved"); json_write_string(&w, fab->retrieved);
    json_write_end_object(&w);
    json_write_key(&w, "routing_rules"); json_write_string(&w, r->name);
    json_write_key(&w, "board_minimums");
    json_write_begin_array(&w);
    for (size_t i = 0; i < out->setting_count; i++) {
        const PrSetting *s = &out->settings[i];
        json_write_begin_object(&w);
        json_write_key(&w, "kicad_key"); json_write_string(&w, s->kicad_key);
        json_write_key(&w, "written"); json_write_bool(&w, s->written);
        json_write_key(&w, "value_mm"); if (s->written) write_mm(&w, a, s->nm); else json_write_null(&w);
        json_write_key(&w, "from"); json_write_string(&w, s->from);
        json_write_key(&w, "note"); json_write_string(&w, s->note);
        json_write_end_object(&w);
    }
    json_write_end_array(&w);
    json_write_key(&w, "default_net_class");
    json_write_begin_object(&w);
    json_write_key(&w, "clearance_mm"); write_mm(&w, a, r->clearance);
    json_write_key(&w, "track_width_mm"); write_mm(&w, a, r->track);
    json_write_key(&w, "via_diameter_mm"); write_mm(&w, a, r->via_diameter);
    json_write_key(&w, "via_drill_mm"); write_mm(&w, a, r->via_drill);
    json_write_key(&w, "fine_pitch_escape_clearance_mm"); if (r->has_fine) write_mm(&w, a, r->fine_clearance); else json_write_null(&w);
    json_write_key(&w, "from"); json_write_string(&w, r->name);
    json_write_end_object(&w);
    const DfmLimitValue *L = fab->limits;
    G2Nm hole_to_hole = L[DFM_VIA_HOLE_TO_HOLE_MIN].present ? L[DFM_VIA_HOLE_TO_HOLE_MIN].nm : 0;
    json_write_key(&w, "other_fab_limits");
    json_write_begin_array(&w);
    coverage(&w, a, "pad_to_track_min_mm", &L[DFM_PAD_TO_TRACK_MIN], r->clearance, "the Default clearance", "KiCad has no separate pad-to-track minimum");
    coverage(&w, a, "pth_to_track_min_mm", &L[DFM_PTH_TO_TRACK_MIN], r->clearance, "the Default clearance", "KiCad has no separate PTH-to-track minimum");
    coverage(&w, a, "smd_pad_to_pad_min_mm", &L[DFM_SMD_PAD_TO_PAD_MIN], r->clearance, "the Default clearance, and the fine-pitch rules at the fab's limit", "KiCad has no separate pad-to-pad minimum");
    coverage(&w, a, "pad_hole_to_hole_min_mm", &L[DFM_PAD_HOLE_TO_HOLE_MIN], hole_to_hole, L[DFM_VIA_HOLE_TO_HOLE_MIN].present ? "min_hole_to_hole" : NULL,
             "KiCad's min_hole_to_hole is set from the via limit, which is smaller: pad holes are not checked at this limit");
    coverage(&w, a, "hole_to_edge_min_mm", &L[DFM_HOLE_TO_EDGE_MIN], 0, NULL, "not written");
    coverage(&w, a, "solder_mask_web_min_mm", ms && ms->present ? &ms->limits[FAB_SOLDER_MASK_WEB_MIN] : NULL, 0, NULL,
             "not written: the solder mask web is a .kicad_pcb setup value, which this step does not change");
    coverage(&w, a, "pad_to_silk_min_mm", ms && ms->present ? &ms->limits[FAB_PAD_TO_SILK_MIN] : NULL, 0, NULL,
             "KiCad has no pad-to-silkscreen minimum; silk_over_copper only reports silk clipped by the mask");
    json_write_end_array(&w);
    json_write_key(&w, "fine_pitch");
    json_write_begin_array(&w);
    for (size_t i = 0; i < out->fine_count; i++) {
        const PrFinePitch *k = &out->fine[i];
        json_write_begin_object(&w);
        json_write_key(&w, "reference"); json_write_string(&w, k->reference);
        json_write_key(&w, "pads"); json_write_begin_array(&w); json_write_string(&w, k->pad_a); json_write_string(&w, k->pad_b); json_write_end_array(&w);
        json_write_key(&w, "min_gap_mm"); write_mm(&w, a, k->min_gap);
        json_write_key(&w, "approximate"); json_write_bool(&w, k->approximate);
        json_write_key(&w, "rule_clearance_mm"); if (k->floor_present) write_mm(&w, a, k->floor); else json_write_null(&w);
        json_write_key(&w, "from"); json_write_string(&w, k->floor_from);
        json_write_key(&w, "rule_written"); json_write_bool(&w, k->rule_written);
        json_write_key(&w, "verdict"); json_write_string(&w, dfm_verdict_name(k->verdict));
        json_write_end_object(&w);
    }
    json_write_end_array(&w);
    json_write_key(&w, "count"); json_write_uint(&w, out->fine_count);
    json_write_end_object(&w);
    if (w.failed) return PR_OUT_OF_MEMORY;
    out->report_text = json_writer_finish(&w, &out->report_len);
    return out->report_text ? PR_OK : PR_OUT_OF_MEMORY;
}

/* ------------------------------------------------------------------------------------------------ build */

PrStatus pr_build(Arena *arena, const Board *board, const DfmProfile *fab, const FabMaskSilk *mask_silk,
                  const PrDesignRules *rules, const JsonValue *existing_pro, const char *pro_filename,
                  PrResult *out, PrError *e)
{
    if (!arena || !board || !fab || !rules || !out || !pro_filename) return kerr(e, PR_INVALID_ARGUMENT, NULL, "NULL argument");
    memset(out, 0, sizeof *out);
    if (existing_pro && existing_pro->kind != JSON_OBJECT) return kerr(e, PR_INVALID_PROJECT, NULL, "the .kicad_pro is not a JSON object");
    PrStatus s = check_rules(rules, fab, e);
    if (s) return s;
    if ((s = build_settings(arena, fab, mask_silk, out))) return kerr(e, s, NULL, "out of memory");
    if ((s = find_fine_pitch(arena, board, fab, rules, out))) return kerr(e, s, NULL, "out of memory");

    ProCtx c;
    memset(&c, 0, sizeof c);
    c.arena = arena; c.res = out; c.rules = rules;
    for (size_t i = 0; i < MAP_COUNT; i++)
        if (out->settings[i].written) c.rule_nums[c.rule_num_count++] = (Num){out->settings[i].kicad_key, out->settings[i].nm};
    c.class_nums[0] = (Num){"clearance", rules->clearance};
    c.class_nums[1] = (Num){"track_width", rules->track};
    c.class_nums[2] = (Num){"via_diameter", rules->via_diameter};
    c.class_nums[3] = (Num){"via_drill", rules->via_drill};
    if ((s = write_pro(arena, existing_pro, pro_filename, &c, out))) return kerr(e, s, NULL, "out of memory");
    if ((s = write_dru(arena, fab, rules, out))) return kerr(e, s, NULL, "out of memory");
    if ((s = write_report(arena, fab, mask_silk, rules, out))) return kerr(e, s, NULL, "out of memory");
    if (e) memset(e, 0, sizeof *e);
    return PR_OK;
}

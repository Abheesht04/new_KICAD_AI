/* dfm.c - V2 DFM checks (see dfm.h). Integer nm, no floating point, no printf/exit. */
#include "dfm.h"

#include "nmath.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------------------------------ names */

static const char *const STATUS_NAMES[] = {"OK", "INVALID_ARGUMENT", "IO_ERROR", "PARSE_ERROR", "INVALID_PROFILE",
                                           "OUT_OF_MEMORY", "GEOMETRY_ERROR"};
static const char *const LIMIT_KEYS[DFM_LIMIT_COUNT] = {
    "track_width_min_mm", "track_spacing_min_mm", "clearance_default_min_mm", "pad_to_track_min_mm",
    "pth_to_track_min_mm", "smd_pad_to_pad_min_mm", "pth_annular_ring_min_mm", "npth_annular_ring_min_mm",
    "via_diameter_over_hole_min_mm", "via_hole_min_mm", "via_diameter_min_mm", "drill_min_mm", "npth_drill_min_mm",
    "plated_slot_width_min_mm", "npth_slot_width_min_mm", "via_hole_to_hole_min_mm", "pad_hole_to_hole_min_mm",
    "via_hole_to_track_min_mm", "npth_to_track_min_mm", "copper_to_edge_min_mm", "hole_to_edge_min_mm",
    "board_min_x_mm", "board_min_y_mm", "board_min_size_thickness_min_mm", "board_max_x_mm", "board_max_y_mm",
    "board_max_thickness_min_mm", "board_max_thin_x_mm", "board_max_thin_y_mm"};
static const char *const RULE_NAMES[DFM_RULE_COUNT] = {
    "track_width", "clearance", "zone_clearance", "annular_ring", "drill", "hole_to_hole", "hole_to_track",
    "copper_to_edge", "hole_to_edge", "board_size"};
static const char *const VERDICT_NAMES[] = {"PASS", "FAIL", "UNVERIFIED", "NOT_IMPLEMENTED"};

const char *dfm_status_name(DfmStatus s) { return (unsigned)s < sizeof STATUS_NAMES / sizeof *STATUS_NAMES ? STATUS_NAMES[s] : "UNKNOWN_STATUS"; }
const char *dfm_limit_key(DfmLimit l) { return (unsigned)l < DFM_LIMIT_COUNT ? LIMIT_KEYS[l] : "unknown_limit"; }
const char *dfm_rule_name(DfmRule r) { return (unsigned)r < DFM_RULE_COUNT ? RULE_NAMES[r] : "unknown_rule"; }
const char *dfm_verdict_name(DfmVerdict v) { return (unsigned)v < 4 ? VERDICT_NAMES[v] : "UNKNOWN_VERDICT"; }

/* ------------------------------------------------------------------------------------------------ profile */

static DfmStatus perr(DfmError *e, DfmStatus s, const char *key, const char *message)
{
    if (e) { memset(e, 0, sizeof *e); e->code = s; e->key = key; e->message = message; }
    return s;
}

static const char *str(const JsonValue *o, const char *key)
{
    const JsonValue *v = json_get(o, key);
    return v && v->kind == JSON_STRING && v->text_len ? v->text : NULL;
}

static DfmStatus profile_from(const JsonValue *root, DfmProfile *out, DfmError *e)
{
    memset(out, 0, sizeof *out);
    if (!root || root->kind != JSON_OBJECT) return perr(e, DFM_INVALID_PROFILE, NULL, "a profile is a JSON object");
    const char *format = str(root, "format");
    if (!format || strcmp(format, "copper.fab_profile/1")) return perr(e, DFM_INVALID_PROFILE, "format", "format must be \"copper.fab_profile/1\"");
    out->name = str(root, "name");
    out->fabricator = str(root, "fabricator");
    out->process = str(root, "process");
    if (!out->name) return perr(e, DFM_INVALID_PROFILE, "name", "a profile needs a name");
    if (!out->fabricator) return perr(e, DFM_INVALID_PROFILE, "fabricator", "a profile names its fabricator");
    const JsonValue *src = json_get(root, "source");
    out->source_url = str(src, "url");
    out->source_title = str(src, "title");
    out->retrieved = str(src, "retrieved");
    if (!out->source_url) return perr(e, DFM_INVALID_PROFILE, "source.url", "every profile cites where its limits come from");
    if (!out->retrieved) return perr(e, DFM_INVALID_PROFILE, "source.retrieved", "every profile records when its source was read");
    const JsonValue *limits = json_get(root, "limits");
    if (!limits || limits->kind != JSON_OBJECT) return perr(e, DFM_INVALID_PROFILE, "limits", "a profile needs a limits object");
    for (size_t i = 0; i < limits->count; i++) {              /* a misspelt key must not silently drop a rule */
        bool known = false;
        for (int k = 0; k < DFM_LIMIT_COUNT; k++) known |= !strcmp(limits->keys[i], LIMIT_KEYS[k]);
        if (!known) return perr(e, DFM_INVALID_PROFILE, limits->keys[i], "unknown limit");
    }
    for (int k = 0; k < DFM_LIMIT_COUNT; k++) {
        const JsonValue *l = json_get(limits, LIMIT_KEYS[k]);
        DfmLimitValue *lv = &out->limits[k];
        if (!l || l->kind != JSON_OBJECT) return perr(e, DFM_INVALID_PROFILE, LIMIT_KEYS[k], "every limit must be present as {\"value\", \"row\", \"quote\"} (value may be null)");
        const JsonValue *v = json_get(l, "value");
        lv->quote = str(l, "quote");
        lv->row = str(l, "row");
        lv->note = str(l, "note");
        if (!lv->quote) return perr(e, DFM_INVALID_PROFILE, LIMIT_KEYS[k], "every limit quotes its source (\"quote\")");
        if (!v) return perr(e, DFM_INVALID_PROFILE, LIMIT_KEYS[k], "\"value\" is required (null when not published)");
        if (v->kind == JSON_NULL) continue;
        if (json_to_nm(v, &lv->nm) != JSON_NUMBER_OK || lv->nm < 0 || lv->nm > G2_COORD_MAX_NM)
            return perr(e, DFM_INVALID_PROFILE, LIMIT_KEYS[k], "a value is a plain decimal in mm, >= 0, at most 6 decimals");
        if (!lv->row) return perr(e, DFM_INVALID_PROFILE, LIMIT_KEYS[k], "a published value names its row (\"row\")");
        lv->present = true;
    }
    if (e) memset(e, 0, sizeof *e);
    return DFM_OK;
}

static DfmStatus from_json_error(const JsonError *je, JsonStatus s, DfmError *e)
{
    DfmStatus d = s == JSON_IO_ERROR ? DFM_IO_ERROR : s == JSON_OUT_OF_MEMORY ? DFM_OUT_OF_MEMORY
                : s == JSON_INVALID_ARGUMENT ? DFM_INVALID_ARGUMENT : DFM_PARSE_ERROR;
    if (e) { memset(e, 0, sizeof *e); e->code = d; e->line = je->line; e->column = je->column; e->offset = je->offset; e->message = je->message; }
    return d;
}

DfmStatus dfm_profile_load(Arena *arena, const char *path, DfmProfile *out, DfmError *error)
{
    JsonValue *root;
    JsonError je = {0};
    if (!arena || !path || !out) return perr(error, DFM_INVALID_ARGUMENT, NULL, "NULL argument");
    JsonStatus s = json_parse_file(arena, path, &root, &je);
    return s ? from_json_error(&je, s, error) : profile_from(root, out, error);
}

DfmStatus dfm_profile_parse(Arena *arena, const char *text, size_t len, DfmProfile *out, DfmError *error)
{
    JsonValue *root;
    JsonError je = {0};
    if (!arena || !text || !out) return perr(error, DFM_INVALID_ARGUMENT, NULL, "NULL argument");
    JsonStatus s = json_parse(arena, text, len, &root, &je);
    return s ? from_json_error(&je, s, error) : profile_from(root, out, error);
}

/* ------------------------------------------------------------------------------------------------ shapes */

typedef enum CuKind { CU_TRACK = 0, CU_VIA, CU_SMD, CU_PTH, CU_NPTH } CuKind;

typedef struct Item {
    G2Shape shape;       /* exact, or (exact == false) a shape containing the item */
    bool exact;
    uint64_t layers;
    int32_t net;
    int kind;            /* CuKind for copper; HoleKind for holes */
    DfmItemRef ref;
    G2Point at;
    size_t footprint;    /* pads: their footprint; SIZE_MAX otherwise */
    const char *number;  /* pads: their number */
} Item;

typedef enum HoleKind { HOLE_VIA = 0, HOLE_PTH, HOLE_NPTH } HoleKind;

static G2Nm half_up(G2Nm v) { return v / 2 + (v % 2 != 0); }

static G2Nm hypot_up(G2Nm w, G2Nm h)
{
    uint64_t q = (uint64_t)w * (uint64_t)w + (uint64_t)h * (uint64_t)h;   /* w, h <= 2e9: < 2^64 */
    uint64_t r = nm_isqrt_u64(q);
    if (r * r < q) r++;
    return (G2Nm)r;
}

static G2Shape circle(G2Point c, G2Nm r)
{
    G2Shape s;
    memset(&s, 0, sizeof s);
    s.kind = G2_SHAPE_CIRCLE;
    s.as.circle.center = c;
    s.as.circle.radius = r;
    return s;
}

static G2Shape capsule(G2Point a, G2Point b, G2Nm r)
{
    G2Shape s;
    memset(&s, 0, sizeof s);
    s.kind = G2_SHAPE_CAPSULE;
    s.as.capsule.a = a; s.as.capsule.b = b; s.as.capsule.radius = r;
    return s;
}

static G2Shape rrect(G2Aabb core, G2Nm r)
{
    G2Shape s;
    memset(&s, 0, sizeof s);
    s.kind = G2_SHAPE_ROUNDED_RECT;
    s.as.rounded_rect.core = core;
    s.as.rounded_rect.radius = r;
    return s;
}

static G2Shape segment(G2Point a, G2Point b)
{
    G2Shape s;
    memset(&s, 0, sizeof s);
    s.kind = G2_SHAPE_SEGMENT;
    s.as.segment.a = a; s.as.segment.b = b;
    return s;
}

/* A w x h obround or rectangle centred on c, long axis along x when horizontal. */
static G2Shape oval(G2Point c, G2Nm w, G2Nm h)
{
    if (w == h) return circle(c, half_up(w));
    if (w > h) {
        G2Nm l = half_up(w - h);
        return capsule((G2Point){c.x - l, c.y}, (G2Point){c.x + l, c.y}, half_up(h));
    }
    G2Nm l = half_up(h - w);
    return capsule((G2Point){c.x, c.y - l}, (G2Point){c.x, c.y + l}, half_up(w));
}

static bool axis_aligned(int64_t udeg, bool *swap)
{
    int64_t a = udeg % 360000000;
    if (a < 0) a += 360000000;
    *swap = (a / 90000000) % 2 == 1;
    return a % 90000000 == 0;
}

/* The copper of a pad. Returns false for pads without a usable shape (custom, trapezoid, other). */
static bool pad_shape(const BoardPad *p, G2Shape *out, bool *exact)
{
    bool swap;
    if (p->shape == BOARD_SHAPE_CUSTOM || p->shape == BOARD_SHAPE_TRAPEZOID || p->shape == BOARD_SHAPE_OTHER) return false;
    if (!axis_aligned(p->angle_udeg, &swap) && p->shape != BOARD_SHAPE_CIRCLE) {
        *out = circle(p->at, half_up(hypot_up(p->size_x, p->size_y)));   /* contains the pad at any angle */
        *exact = false;
        return true;
    }
    G2Nm w = swap ? p->size_y : p->size_x, h = swap ? p->size_x : p->size_y;
    *exact = true;
    switch (p->shape) {
    case BOARD_SHAPE_CIRCLE: *out = circle(p->at, half_up(p->size_x)); return true;
    case BOARD_SHAPE_OVAL: *out = oval(p->at, w, h); return true;
    default: {
        G2Nm r = 0;
        if (p->shape == BOARD_SHAPE_ROUNDRECT) {   /* KiCad: radius = rratio x min(w, h); floored = more copper */
            G2Nm m = w < h ? w : h;
            r = (m / 1000000) * p->roundrect_rratio_ppm + (m % 1000000) * p->roundrect_rratio_ppm / 1000000;
            if (r > m / 2) r = m / 2;
        }
        G2Nm hx = half_up(w) - r, hy = half_up(h) - r;
        G2Aabb core = {{p->at.x - hx, p->at.y - hy}, {p->at.x + hx, p->at.y + hy}};
        *out = rrect(core, r);
        return true;
    }
    }
}

/* The drill of a pad or via. */
static G2Shape hole_shape(G2Point at, G2Nm dx, G2Nm dy, int64_t udeg, bool *exact)
{
    bool swap;
    if (dx == dy) { *exact = true; return circle(at, half_up(dx)); }
    if (!axis_aligned(udeg, &swap)) { *exact = false; return circle(at, half_up(dx > dy ? dx : dy)); }
    *exact = true;
    return swap ? oval(at, dy, dx) : oval(at, dx, dy);
}

static uint64_t via_span(uint64_t layers, BoardViaType type)
{
    uint64_t cu = layers & BOARD_LAYERS_CU;
    if (type == BOARD_VIA_THROUGH || !cu) return BOARD_LAYERS_CU;
    int lo = 0, hi = 31;
    while (lo < 32 && !((cu >> lo) & 1u)) lo++;
    while (hi > 0 && !((cu >> hi) & 1u)) hi--;
    uint64_t m = 0;
    for (int b = lo; b <= hi; b++) m |= UINT64_C(1) << b;
    return m;
}

/* ------------------------------------------------------------------------------------------------ report */

typedef struct Ctx {
    Arena *arena;
    const Board *b;
    const DfmProfile *p;
    DfmReport *r;
    size_t cap;
} Ctx;

static bool lim(const Ctx *c, DfmLimit l, G2Nm *out)
{
    if (!c->p->limits[l].present) return false;
    *out = c->p->limits[l].nm;
    return true;
}

static DfmStatus add_violation(Ctx *c, DfmRule rule, DfmLimit limit, G2Nm measured, DfmItemRef a, DfmItemRef b, G2Point at, uint64_t layers)
{
    DfmReport *r = c->r;
    if (r->violation_count == c->cap) {
        size_t cap = c->cap ? c->cap * 2 : 32;
        DfmViolation *v = arena_alloc_array(c->arena, sizeof *v, cap);
        if (!v) return DFM_OUT_OF_MEMORY;
        if (r->violation_count) memcpy(v, r->violations, r->violation_count * sizeof *v);
        r->violations = v;
        c->cap = cap;
    }
    DfmViolation *v = &r->violations[r->violation_count++];
    v->rule = rule; v->limit = limit; v->limit_nm = c->p->limits[limit].nm; v->measured_nm = measured;
    v->a = a; v->b = b; v->at = at; v->layers = layers;
    r->rules[rule].violations++;
    return DFM_OK;
}

static DfmItemRef ref(DfmItemKind k, size_t i) { DfmItemRef x = {k, i}; return x; }
static const DfmItemRef NONE = {DFM_ITEM_NONE, 0};

/* ------------------------------------------------------------------------------------------------ item lists */

static DfmStatus copper_items(Ctx *c, Item **out, size_t *count, size_t *custom)
{
    const Board *b = c->b;
    size_t cap = b->track_count + b->via_count + b->pad_count, n = 0;
    Item *it = arena_alloc_array(c->arena, sizeof *it, cap ? cap : 1);
    if (!it) return DFM_OUT_OF_MEMORY;
    *custom = 0;
    for (size_t i = 0; i < b->track_count; i++) {
        const BoardTrack *t = &b->tracks[i];
        Item *x = &it[n++];
        memset(x, 0, sizeof *x);
        G2Nm r = half_up(t->width);
        if (t->is_arc) {
            G2Aabb box;
            if (board_arc_bounds(t->start, t->mid, t->end, &box)) return DFM_GEOMETRY_ERROR;
            x->shape = rrect(box, r);
            x->exact = false;
        } else {
            x->shape = capsule(t->start, t->end, r);
            x->exact = true;
        }
        x->layers = t->layer & BOARD_LAYERS_CU; x->net = t->net; x->kind = CU_TRACK; x->ref = ref(DFM_ITEM_TRACK, i);
        x->at = t->start; x->footprint = SIZE_MAX;
    }
    for (size_t i = 0; i < b->via_count; i++) {
        const BoardVia *v = &b->vias[i];
        Item *x = &it[n++];
        memset(x, 0, sizeof *x);
        x->shape = circle(v->at, half_up(v->size)); x->exact = true;
        x->layers = via_span(v->layers, v->type); x->net = v->net; x->kind = CU_VIA; x->ref = ref(DFM_ITEM_VIA, i);
        x->at = v->at; x->footprint = SIZE_MAX;
    }
    for (size_t i = 0; i < b->pad_count; i++) {
        const BoardPad *p = &b->pads[i];
        uint64_t cu = p->layers & BOARD_LAYERS_CU;
        if (!cu) continue;
        if (p->type == BOARD_PAD_NP_THRU_HOLE && !(p->has_drill && (p->size_x > p->drill_x || p->size_y > p->drill_y))) continue;
        Item *x = &it[n];
        memset(x, 0, sizeof *x);
        if (!pad_shape(p, &x->shape, &x->exact)) { (*custom)++; continue; }
        n++;
        x->layers = p->type == BOARD_PAD_THRU_HOLE ? BOARD_LAYERS_CU : cu;   /* a plated hole's copper is on every layer */
        x->net = p->net;
        x->kind = p->type == BOARD_PAD_THRU_HOLE ? CU_PTH : p->type == BOARD_PAD_NP_THRU_HOLE ? CU_NPTH : CU_SMD;
        x->ref = ref(DFM_ITEM_PAD, i); x->at = p->at; x->footprint = p->footprint; x->number = p->number;
    }
    *out = it;
    *count = n;
    return DFM_OK;
}

static DfmStatus hole_items(Ctx *c, Item **out, size_t *count)
{
    const Board *b = c->b;
    size_t n = 0;
    Item *it = arena_alloc_array(c->arena, sizeof *it, b->via_count + b->pad_count + 1);
    if (!it) return DFM_OUT_OF_MEMORY;
    for (size_t i = 0; i < b->via_count; i++) {
        const BoardVia *v = &b->vias[i];
        if (v->drill <= 0) continue;
        Item *x = &it[n++];
        memset(x, 0, sizeof *x);
        x->shape = circle(v->at, half_up(v->drill)); x->exact = true;
        x->layers = via_span(v->layers, v->type); x->net = v->net; x->kind = HOLE_VIA; x->ref = ref(DFM_ITEM_VIA, i);
        x->at = v->at; x->footprint = SIZE_MAX;
    }
    for (size_t i = 0; i < b->pad_count; i++) {
        const BoardPad *p = &b->pads[i];
        if (!p->has_drill) continue;
        Item *x = &it[n++];
        memset(x, 0, sizeof *x);
        x->shape = hole_shape(p->at, p->drill_x, p->drill_y, p->angle_udeg, &x->exact);
        x->layers = BOARD_LAYERS_CU; x->net = p->type == BOARD_PAD_NP_THRU_HOLE ? -1 : p->net;
        x->kind = p->type == BOARD_PAD_NP_THRU_HOLE ? HOLE_NPTH : HOLE_PTH;
        x->ref = ref(DFM_ITEM_PAD, i); x->at = p->at; x->footprint = p->footprint; x->number = p->number;
    }
    *out = it;
    *count = n;
    return DFM_OK;
}

/* ------------------------------------------------------------------------------------------------ pair checks */

typedef bool (*PairLimit)(const Ctx *c, const Item *a, const Item *b, DfmLimit *which, G2Nm *nm, bool *skip);

/* Connected by a net: KiCad's net 0 ("") is "no net", so two unconnected items still need their clearance. */
static bool same_net(const Ctx *c, int32_t a, int32_t b)
{
    return a >= 0 && a == b && c->b->nets[a].name && c->b->nets[a].name[0];
}

static bool same_pad(const Item *a, const Item *b)
{
    return a->footprint != SIZE_MAX && a->footprint == b->footprint && a->number && b->number && a->number[0] &&
           !strcmp(a->number, b->number);
}

static bool copper_pair_limit(const Ctx *c, const Item *a, const Item *b, DfmLimit *which, G2Nm *nm, bool *skip)
{
    *skip = !(a->layers & b->layers) || same_net(c, a->net, b->net) || same_pad(a, b);
    if (*skip) return true;
    int x = a->kind < b->kind ? a->kind : b->kind, y = a->kind < b->kind ? b->kind : a->kind;
    if (x == CU_TRACK && (y == CU_TRACK || y == CU_VIA)) *which = DFM_TRACK_SPACING_MIN;
    else if (x == CU_TRACK && y == CU_SMD) *which = DFM_PAD_TO_TRACK_MIN;
    else if (x == CU_TRACK && y == CU_PTH) *which = DFM_PTH_TO_TRACK_MIN;
    else if (x == CU_SMD && y == CU_SMD) *which = DFM_SMD_PAD_TO_PAD_MIN;
    else *which = DFM_CLEARANCE_DEFAULT_MIN;
    return lim(c, *which, nm);
}

/* Holes against tracks: via holes against tracks of other nets, non-plated holes against every track. */
static bool hole_track_limit(const Ctx *c, const Item *hole, const Item *cu, DfmLimit *which, G2Nm *nm, bool *skip)
{
    *skip = cu->kind != CU_TRACK || hole->kind == HOLE_PTH || !(hole->layers & cu->layers) ||
            (hole->kind == HOLE_VIA && same_net(c, hole->net, cu->net));
    if (*skip) return true;
    *which = hole->kind == HOLE_VIA ? DFM_VIA_HOLE_TO_TRACK_MIN : DFM_NPTH_TO_TRACK_MIN;
    return lim(c, *which, nm);
}

static bool hole_hole_limit(const Ctx *c, const Item *a, const Item *b, DfmLimit *which, G2Nm *nm, bool *skip)
{
    *skip = false;
    *which = a->kind == HOLE_VIA && b->kind == HOLE_VIA ? DFM_VIA_HOLE_TO_HOLE_MIN : DFM_PAD_HOLE_TO_HOLE_MIN;
    return lim(c, *which, nm);
}

static G2Nm max_present(const Ctx *c, const DfmLimit *ls, size_t n)
{
    G2Nm m = 0, v;
    for (size_t i = 0; i < n; i++) if (lim(c, ls[i], &v) && v > m) m = v;
    return m;
}

/* For every a in `as`, every b in `bs` near it (BVH over bs), decide the pair. `triangular`: as == bs, each
 * unordered pair once. */
static DfmStatus pairs(Ctx *c, DfmRule rule, const Item *as, size_t na, const Item *bs, size_t nb, bool triangular,
                       G2Nm reach, PairLimit limit_of)
{
    DfmRuleResult *res = &c->r->rules[rule];
    if (!na || !nb) return DFM_OK;
    G2Shape *shapes = arena_alloc_array(c->arena, sizeof *shapes, nb);
    size_t *ids = arena_alloc_array(c->arena, sizeof *ids, nb);
    if (!shapes || !ids) return DFM_OUT_OF_MEMORY;
    for (size_t i = 0; i < nb; i++) shapes[i] = bs[i].shape;
    G2Bvh tree;
    if (g2_bvh_build_shapes(c->arena, shapes, nb, 4, &tree) != G2_OK) return DFM_GEOMETRY_ERROR;
    for (size_t i = 0; i < na; i++) {
        G2Aabb box;
        if (g2_shape_aabb(as[i].shape, &box) != G2_OK) return DFM_GEOMETRY_ERROR;
        size_t found = 0;
        G2Status gs = g2_bvh_query_shape(&tree, rrect(box, reach), ids, nb, &found);
        if (gs != G2_OK) return DFM_GEOMETRY_ERROR;
        for (size_t k = 0; k < found; k++) {
            size_t j = ids[k];
            if (triangular && j <= i) continue;
            DfmLimit which = DFM_TRACK_SPACING_MIN;
            G2Nm t = 0;
            bool skip = false;
            if (!limit_of(c, &as[i], &bs[j], &which, &t, &skip)) { res->unverified++; continue; }
            if (skip) continue;
            G2Order order;
            gs = g2_compare_shape_distance(as[i].shape, bs[j].shape, t, &order);
            if (gs != G2_OK) { res->unverified++; continue; }
            if (order != G2_ORDER_LESS) { res->checked++; continue; }
            if (!as[i].exact || !bs[j].exact) { res->unverified++; continue; }
            res->checked++;
            DfmStatus s = add_violation(c, rule, which, -1, as[i].ref, bs[j].ref, as[i].at, as[i].layers & bs[j].layers);
            if (s) return s;
        }
    }
    return DFM_OK;
}

/* Items against Edge.Cuts: lines exactly; arcs and circles by their bounding box (pass when far, else UNVERIFIED). */
static DfmStatus to_edges(Ctx *c, DfmRule rule, DfmLimit limit, const Item *items, size_t n)
{
    DfmRuleResult *res = &c->r->rules[rule];
    G2Nm t;
    if (!lim(c, limit, &t)) {
        res->unverified = n * c->b->edge_count;
        res->note = "the profile has no value for this limit (not published by the fabricator)";
        return DFM_OK;
    }
    for (size_t e = 0; e < c->b->edge_count; e++) {
        const BoardEdge *g = &c->b->edges[e];
        G2Shape edge;
        bool exact = g->kind == BOARD_EDGE_LINE;
        if (exact) edge = segment(g->start, g->end);
        else {
            G2Aabb box;
            if (g->kind == BOARD_EDGE_ARC) {
                if (board_arc_bounds(g->start, g->mid, g->end, &box)) return DFM_GEOMETRY_ERROR;
            } else {
                G2Nm r = hypot_up(g->end.x - g->start.x, g->end.y - g->start.y) + 1;
                box.min.x = g->start.x - r; box.min.y = g->start.y - r; box.max.x = g->start.x + r; box.max.y = g->start.y + r;
            }
            edge = rrect(box, 0);
        }
        for (size_t i = 0; i < n; i++) {
            G2Order order;
            if (g2_compare_shape_distance(items[i].shape, edge, t, &order) != G2_OK) { res->unverified++; continue; }
            if (order != G2_ORDER_LESS) { res->checked++; continue; }
            if (!exact || !items[i].exact) { res->unverified++; continue; }
            res->checked++;
            DfmStatus s = add_violation(c, rule, limit, -1, items[i].ref, ref(DFM_ITEM_EDGE, e), items[i].at, items[i].layers);
            if (s) return s;
        }
    }
    if (res->unverified) res->note = "Edge.Cuts arcs and circles are checked by their bounding box only; items inside that box are UNVERIFIED";
    return DFM_OK;
}

/* ------------------------------------------------------------------------------------------------ single checks */

static DfmStatus below(Ctx *c, DfmRule rule, DfmLimit limit, G2Nm measured, DfmItemRef a, G2Point at)
{
    G2Nm t;
    DfmRuleResult *res = &c->r->rules[rule];
    if (!lim(c, limit, &t)) { res->unverified++; return DFM_OK; }
    res->checked++;
    return measured < t ? add_violation(c, rule, limit, measured, a, NONE, at, 0) : DFM_OK;
}

static DfmStatus singles(Ctx *c)
{
    const Board *b = c->b;
    DfmStatus s;
    for (size_t i = 0; i < b->track_count; i++)
        if ((s = below(c, DFM_RULE_TRACK_WIDTH, DFM_TRACK_WIDTH_MIN, b->tracks[i].width, ref(DFM_ITEM_TRACK, i), b->tracks[i].start))) return s;
    for (size_t i = 0; i < b->via_count; i++) {
        const BoardVia *v = &b->vias[i];
        DfmItemRef r = ref(DFM_ITEM_VIA, i);
        if ((s = below(c, DFM_RULE_DRILL, DFM_VIA_HOLE_MIN, v->drill, r, v->at)) ||
            (s = below(c, DFM_RULE_DRILL, DFM_VIA_DIAMETER_MIN, v->size, r, v->at)) ||
            (s = below(c, DFM_RULE_ANNULAR_RING, DFM_VIA_DIAMETER_OVER_HOLE_MIN, v->size - v->drill, r, v->at)))
            return s;
    }
    for (size_t i = 0; i < b->pad_count; i++) {
        const BoardPad *p = &b->pads[i];
        DfmItemRef r = ref(DFM_ITEM_PAD, i);
        if (!p->has_drill) continue;
        bool npth = p->type == BOARD_PAD_NP_THRU_HOLE, slot = p->drill_x != p->drill_y;
        G2Nm width = p->drill_x < p->drill_y ? p->drill_x : p->drill_y;
        DfmLimit dl = npth ? (slot ? DFM_NPTH_SLOT_WIDTH_MIN : DFM_NPTH_DRILL_MIN) : (slot ? DFM_PLATED_SLOT_WIDTH_MIN : DFM_DRILL_MIN);
        if ((s = below(c, DFM_RULE_DRILL, dl, width, r, p->at))) return s;
        if (p->type != BOARD_PAD_THRU_HOLE && !(npth && (p->layers & BOARD_LAYERS_CU) && (p->size_x > p->drill_x || p->size_y > p->drill_y)))
            continue;
        if (p->shape == BOARD_SHAPE_CUSTOM || p->shape == BOARD_SHAPE_TRAPEZOID || p->shape == BOARD_SHAPE_OTHER) {
            c->r->rules[DFM_RULE_ANNULAR_RING].unverified++;
            continue;
        }
        /* Copper around the hole along each pad axis (pad size and drill share the pad's frame). The ring is half the
         * difference; compare twice the limit with the whole difference so no half nanometre is rounded away. */
        G2Nm sy = p->shape == BOARD_SHAPE_CIRCLE ? p->size_x : p->size_y;
        G2Nm d = p->size_x - p->drill_x < sy - p->drill_y ? p->size_x - p->drill_x : sy - p->drill_y;
        DfmLimit rl = npth ? DFM_NPTH_ANNULAR_RING_MIN : DFM_PTH_ANNULAR_RING_MIN;
        G2Nm t;
        DfmRuleResult *res = &c->r->rules[DFM_RULE_ANNULAR_RING];
        if (!lim(c, rl, &t)) { res->unverified++; continue; }
        res->checked++;
        if (d < 2 * t && (s = add_violation(c, DFM_RULE_ANNULAR_RING, rl, d / 2, r, NONE, p->at, 0))) return s;
    }
    return DFM_OK;
}

static DfmStatus board_size(Ctx *c)
{
    DfmRuleResult *res = &c->r->rules[DFM_RULE_BOARD_SIZE];
    const BoardOutline *o = &c->b->outline;
    if (!o->has_bounds) { res->unverified++; res->note = "no Edge.Cuts outline"; return DFM_OK; }
    G2Nm w = o->bounds.max.x - o->bounds.min.x, h = o->bounds.max.y - o->bounds.min.y;
    G2Nm lo = w < h ? w : h, hi = w < h ? h : w, th = c->b->thickness, v1, v2, tmin;
    G2Point at = o->bounds.min;
    DfmItemRef board = ref(DFM_ITEM_BOARD, 0);
    /* Minimum: valid only from the profile's thickness on; a thinner board needs the fabricator's manual review. */
    if (!lim(c, DFM_BOARD_MIN_X, &v1) || !lim(c, DFM_BOARD_MIN_Y, &v2)) res->unverified++;
    else if (lim(c, DFM_BOARD_MIN_SIZE_THICKNESS_MIN, &tmin) && (th <= 0 || th < tmin)) {
        res->unverified++;
        res->note = th <= 0 ? "board thickness unknown: the size limits depend on it" : "board thinner than the size limits' minimum thickness: the fabricator reviews it manually";
    } else {
        G2Nm a = v1 < v2 ? v1 : v2, bb = v1 < v2 ? v2 : v1;
        res->checked++;
        DfmStatus s;
        if (lo < a && (s = add_violation(c, DFM_RULE_BOARD_SIZE, v1 <= v2 ? DFM_BOARD_MIN_X : DFM_BOARD_MIN_Y, lo, board, NONE, at, 0))) return s;
        if (hi < bb && (s = add_violation(c, DFM_RULE_BOARD_SIZE, v1 <= v2 ? DFM_BOARD_MIN_Y : DFM_BOARD_MIN_X, hi, board, NONE, at, 0))) return s;
    }
    /* Maximum: the full size from the profile's thickness on, the thin-board size below it (or when unknown). */
    bool thick = lim(c, DFM_BOARD_MAX_THICKNESS_MIN, &tmin) && th > 0 && th >= tmin;
    DfmLimit lx = thick ? DFM_BOARD_MAX_X : DFM_BOARD_MAX_THIN_X, ly = thick ? DFM_BOARD_MAX_Y : DFM_BOARD_MAX_THIN_Y;
    if (!lim(c, lx, &v1) || !lim(c, ly, &v2)) { res->unverified++; return DFM_OK; }
    res->checked++;
    G2Nm a = v1 < v2 ? v1 : v2, bb = v1 < v2 ? v2 : v1;
    DfmStatus s;
    if (lo > a && (s = add_violation(c, DFM_RULE_BOARD_SIZE, v1 < v2 ? lx : ly, lo, board, NONE, at, 0))) return s;
    if (hi > bb && (s = add_violation(c, DFM_RULE_BOARD_SIZE, v1 < v2 ? ly : lx, hi, board, NONE, at, 0))) return s;
    if (!thick && !res->note) res->note = "board thickness unknown or below the full-size thickness: the thin-board maximum applies";
    return DFM_OK;
}

/* ------------------------------------------------------------------------------------------------ check */

static int cmp_violation(const void *pa, const void *pb)
{
    const DfmViolation *a = pa, *b = pb;
#define CMP(x, y) do { if ((x) != (y)) return (x) < (y) ? -1 : 1; } while (0)
    CMP(a->rule, b->rule); CMP(a->limit, b->limit); CMP(a->at.y, b->at.y); CMP(a->at.x, b->at.x);
    CMP(a->a.kind, b->a.kind); CMP(a->a.index, b->a.index); CMP(a->b.kind, b->b.kind); CMP(a->b.index, b->b.index);
#undef CMP
    return 0;
}

DfmStatus dfm_check(Arena *arena, const Board *board, const DfmProfile *profile, DfmReport *out, DfmError *error)
{
    if (!arena || !board || !profile || !out) return perr(error, DFM_INVALID_ARGUMENT, NULL, "NULL argument");
    memset(out, 0, sizeof *out);
    for (int r = 0; r < DFM_RULE_COUNT; r++) out->rules[r].rule = (DfmRule)r;
    Ctx c = {arena, board, profile, out, 0};
    DfmStatus s;
    Item *cu, *holes;
    size_t ncu, nholes, custom;
    if ((s = copper_items(&c, &cu, &ncu, &custom)) || (s = hole_items(&c, &holes, &nholes)))
        return perr(error, s, NULL, s == DFM_OUT_OF_MEMORY ? "out of memory" : "board geometry outside the exact range");
    if ((s = singles(&c))) return perr(error, s, NULL, "out of memory");

    static const DfmLimit CU_LIMITS[] = {DFM_TRACK_SPACING_MIN, DFM_CLEARANCE_DEFAULT_MIN, DFM_PAD_TO_TRACK_MIN,
                                         DFM_PTH_TO_TRACK_MIN, DFM_SMD_PAD_TO_PAD_MIN};
    static const DfmLimit HT_LIMITS[] = {DFM_VIA_HOLE_TO_TRACK_MIN, DFM_NPTH_TO_TRACK_MIN};
    static const DfmLimit HH_LIMITS[] = {DFM_VIA_HOLE_TO_HOLE_MIN, DFM_PAD_HOLE_TO_HOLE_MIN};
    if ((s = pairs(&c, DFM_RULE_CLEARANCE, cu, ncu, cu, ncu, true, max_present(&c, CU_LIMITS, 5), copper_pair_limit)) ||
        (s = pairs(&c, DFM_RULE_HOLE_TO_TRACK, holes, nholes, cu, ncu, false, max_present(&c, HT_LIMITS, 2), hole_track_limit)) ||
        (s = pairs(&c, DFM_RULE_HOLE_TO_HOLE, holes, nholes, holes, nholes, true, max_present(&c, HH_LIMITS, 2), hole_hole_limit)) ||
        (s = to_edges(&c, DFM_RULE_COPPER_TO_EDGE, DFM_COPPER_TO_EDGE_MIN, cu, ncu)) ||
        (s = to_edges(&c, DFM_RULE_HOLE_TO_EDGE, DFM_HOLE_TO_EDGE_MIN, holes, nholes)) ||
        (s = board_size(&c)))
        return perr(error, s, NULL, s == DFM_OUT_OF_MEMORY ? "out of memory" : "board geometry outside the exact range");
    if (custom) {
        out->rules[DFM_RULE_CLEARANCE].unverified += custom;
        out->rules[DFM_RULE_CLEARANCE].note = "custom and trapezoid pads have no exact shape here: their clearances are UNVERIFIED";
    }
    if (!board->edge_count) {
        out->rules[DFM_RULE_COPPER_TO_EDGE].unverified++;
        out->rules[DFM_RULE_COPPER_TO_EDGE].note = "no Edge.Cuts outline";
        if (profile->limits[DFM_HOLE_TO_EDGE_MIN].present) out->rules[DFM_RULE_HOLE_TO_EDGE].unverified++;
    }
    if (!profile->limits[DFM_HOLE_TO_EDGE_MIN].present) {
        DfmRuleResult *h = &out->rules[DFM_RULE_HOLE_TO_EDGE];
        if (!h->unverified) h->unverified = 1;
        h->note = "the profile has no value for this limit (not published by the fabricator)";
    }
    size_t fills = 0;
    for (size_t i = 0; i < board->zone_count; i++) fills += !board->zones[i].keepout && board->zones[i].filled_polygon_count;
    DfmRuleResult *z = &out->rules[DFM_RULE_ZONE_CLEARANCE];
    z->verdict = fills ? DFM_NOT_IMPLEMENTED : DFM_PASS;
    z->unverified = fills;
    z->note = fills ? "zone fills are not in the board model yet: filled zones are not checked" : "no filled zones";

    out->verdict = DFM_PASS;
    for (int r = 0; r < DFM_RULE_COUNT; r++) {
        DfmRuleResult *x = &out->rules[r];
        if (r != DFM_RULE_ZONE_CLEARANCE)
            x->verdict = x->violations ? DFM_FAIL : x->unverified ? DFM_UNVERIFIED : DFM_PASS;
        if (x->verdict == DFM_FAIL) out->verdict = DFM_FAIL;
        else if (x->verdict != DFM_PASS && out->verdict == DFM_PASS) out->verdict = DFM_UNVERIFIED;
    }
    if (out->violation_count) qsort(out->violations, out->violation_count, sizeof *out->violations, cmp_violation);
    if (error) memset(error, 0, sizeof *error);
    return DFM_OK;
}

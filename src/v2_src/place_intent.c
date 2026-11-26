/* place_intent.c - see place_intent.h. Exact integer nm; builds text, never prints. */
#include "place_intent.h"

#include "IR.h"
#include "filesystem.h"
#include "json.h"
#include "kicad_assets.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------------------------- text buffer */

typedef struct Buf { Arena *a; char *s; size_t len, cap; bool oom; } Buf;

static void put(Buf *b, const char *f, ...)
{
    if (b->oom) return;
    char tmp[1024];
    va_list ap;
    va_start(ap, f);
    int n = vsnprintf(tmp, sizeof tmp, f, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof tmp) { b->oom = true; return; }
    if (b->len + (size_t)n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 4096;
        while (cap < b->len + (size_t)n + 1) cap *= 2;
        char *more = arena_alloc(b->a, cap);
        if (!more) { b->oom = true; return; }
        if (b->len) memcpy(more, b->s, b->len);
        b->s = more;
        b->cap = cap;
    }
    memcpy(b->s + b->len, tmp, (size_t)n + 1);
    b->len += (size_t)n;
}

static void put_raw(Buf *b, const char *s, size_t n)
{
    if (b->oom) return;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 4096;
        while (cap < b->len + n + 1) cap *= 2;
        char *more = arena_alloc(b->a, cap);
        if (!more) { b->oom = true; return; }
        if (b->len) memcpy(more, b->s, b->len);
        b->s = more;
        b->cap = cap;
    }
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = 0;
}

/* nm -> "12.345" mm (exact, trailing zeros dropped) */
static const char *mm_text(char *out, size_t cap, int64_t nm)
{
    uint64_t m = nm < 0 ? (uint64_t)(-(nm + 1)) + 1u : (uint64_t)nm;
    char frac[8];
    snprintf(frac, sizeof frac, "%06llu", (unsigned long long)(m % 1000000u));
    size_t f = strlen(frac);
    while (f && frac[f - 1] == '0') frac[--f] = 0;
    snprintf(out, cap, "%s%llu%s%s", nm < 0 ? "-" : "", (unsigned long long)(m / 1000000u), f ? "." : "", frac);
    return out;
}

static void put_mm(Buf *b, int64_t nm) { char t[40]; put(b, "%s", mm_text(t, sizeof t, nm)); }

static void put_quoted(Buf *b, const char *s)
{
    put(b, "\"");
    for (; *s; s++) put(b, (*s == '"' || *s == '\\') ? "\\%c" : "%c", *s);
    put(b, "\"");
}

static void fail(PlaceReport *r, PlaceStatus st, const char *f, const char *x, const char *y)
{
    r->status = st;
    snprintf(r->message, sizeof r->message, f, x ? x : "", y ? y : "");
}

/* ---------------------------------------------------------------------------------------------- geometry */

typedef struct Rect { int64_t x1, y1, x2, y2; bool set; } Rect;

static void grow(Rect *r, int64_t x, int64_t y)
{
    if (!r->set) { r->x1 = r->x2 = x; r->y1 = r->y2 = y; r->set = true; return; }
    if (x < r->x1) r->x1 = x;
    if (x > r->x2) r->x2 = x;
    if (y < r->y1) r->y1 = y;
    if (y > r->y2) r->y2 = y;
}

/* KiCad rotation by a multiple of 90 degrees: (x, y) -> (x cos r + y sin r, -x sin r + y cos r) */
static void rot_point(int64_t x, int64_t y, int rot, int64_t *ox, int64_t *oy)
{
    switch (((rot % 360) + 360) % 360) {
    case 90: *ox = y; *oy = -x; break;
    case 180: *ox = -x; *oy = -y; break;
    case 270: *ox = -y; *oy = x; break;
    default: *ox = x; *oy = y; break;
    }
}

static Rect rot_rect(Rect r, int rot)
{
    Rect o = {0, 0, 0, 0, false};
    int64_t x, y;
    rot_point(r.x1, r.y1, rot, &x, &y); grow(&o, x, y);
    rot_point(r.x2, r.y2, rot, &x, &y); grow(&o, x, y);
    return o;
}

static bool nm_of(const IRNode *n, int64_t *out) { return n && ir_atom_to_nm(n, out) == IR_STATUS_OK; }

static bool point_of(const IRNode *list, const char *head, int64_t *x, int64_t *y)
{
    const IRNode *p = ir_list_find(list, head);
    return p && nm_of(ir_child_at(p, 1), x) && nm_of(ir_child_at(p, 2), y);
}

static const char *layer_of(const IRNode *n)
{
    const IRNode *l = ir_list_find(n, "layer");
    const IRNode *v = l ? ir_child_at(l, 1) : NULL;
    return v && (v->kind == IR_NODE_STRING || v->kind == IR_NODE_ATOM) ? v->text : NULL;
}

/* The bounding box of a footprint's graphics on one layer (lines, rectangles, polygons, arcs, circles), with half the
 * stroke width added when `stroke` is set. */
static Rect layer_box(const IRNode *fp, const char *layer, bool stroke)
{
    Rect r = {0, 0, 0, 0, false};
    for (const IRNode *g = fp ? fp->first_child : NULL; g; g = g->next) {
        const IRNode *h = g->kind == IR_NODE_LIST ? ir_list_head(g) : NULL;
        if (!h || h->kind != IR_NODE_ATOM) continue;
        bool circle = !strcmp(h->text, "fp_circle");
        if (!circle && strcmp(h->text, "fp_line") && strcmp(h->text, "fp_rect") && strcmp(h->text, "fp_poly") && strcmp(h->text, "fp_arc")) continue;
        const char *l = layer_of(g);
        if (!l || strcmp(l, layer)) continue;
        int64_t w = 0;
        const IRNode *st = ir_list_find(g, "stroke");
        if (stroke && st) nm_of(ir_child_at(ir_list_find(st, "width"), 1), &w);
        w /= 2;
        int64_t x, y, ex, ey;
        if (circle && point_of(g, "center", &x, &y) && point_of(g, "end", &ex, &ey)) {
            int64_t rr = (ex > x ? ex - x : x - ex) + (ey > y ? ey - y : y - ey) + w;   /* >= the radius */
            grow(&r, x - rr, y - rr); grow(&r, x + rr, y + rr);
            continue;
        }
        static const char *const keys[] = {"start", "end", "mid", "center"};
        for (size_t k = 0; k < 4; k++)
            if (point_of(g, keys[k], &x, &y)) { grow(&r, x - w, y - w); grow(&r, x + w, y + w); }
        const IRNode *pts = ir_list_find(g, "pts");
        for (const IRNode *p = pts ? pts->first_child : NULL; p; p = p->next) {
            const IRNode *ph = p->kind == IR_NODE_LIST ? ir_list_head(p) : NULL;
            if (ph && ph->kind == IR_NODE_ATOM && !strcmp(ph->text, "xy") && nm_of(ir_child_at(p, 1), &x) && nm_of(ir_child_at(p, 2), &y)) {
                grow(&r, x - w, y - w); grow(&r, x + w, y + w);
            }
        }
    }
    return r;
}

typedef struct Fp {
    Rect body, silk;
    const KicadFootprintPad *pads;
    size_t pad_count;
    /* Step 3e: the library's Reference label (position and angle in the footprint frame, font), when shown */
    bool label;
    int64_t lx, ly, langle;      /* nm; the angle as written (degrees x 1e6) */
    int64_t lsx, lsy, lthick;    /* font size and stroke, nm */
    bool ljustified;             /* any justify other than the default centre */
} Fp;

/* The Reference property of a footprint: its (at), font and whether it is hidden. */
static void read_label(const IRNode *tree, Fp *out)
{
    out->label = false;
    for (const IRNode *g = tree ? tree->first_child : NULL; g; g = g->next) {
        const IRNode *h = g->kind == IR_NODE_LIST ? ir_list_head(g) : NULL;
        if (!h || h->kind != IR_NODE_ATOM || strcmp(h->text, "property")) continue;
        const IRNode *name = ir_child_at(g, 1);
        if (!name || !name->text || strcmp(name->text, "Reference")) continue;
        const IRNode *at = ir_list_find(g, "at"), *eff = ir_list_find(g, "effects");
        const IRNode *font = eff ? ir_list_find(eff, "font") : NULL, *size = font ? ir_list_find(font, "size") : NULL;
        const IRNode *th = font ? ir_list_find(font, "thickness") : NULL;
        const IRNode *hide = ir_list_find(g, "hide");
        if (!hide && eff) hide = ir_list_find(eff, "hide");
        const IRNode *hv = hide ? ir_child_at(hide, 1) : NULL;
        if (hide && (!hv || (hv->text && !strcmp(hv->text, "yes")))) return;   /* hidden: no label on the board */
        if (!at || !nm_of(ir_child_at(at, 1), &out->lx) || !nm_of(ir_child_at(at, 2), &out->ly)) return;
        if (!ir_child_at(at, 3) || !nm_of(ir_child_at(at, 3), &out->langle)) out->langle = 0;
        if (!size || !nm_of(ir_child_at(size, 1), &out->lsy) || !nm_of(ir_child_at(size, 2), &out->lsx)) return;
        if (!th || !nm_of(ir_child_at(th, 1), &out->lthick)) out->lthick = 0;
        const IRNode *j = eff ? ir_list_find(eff, "justify") : NULL;
        out->ljustified = j != NULL;
        out->label = true;
        return;
    }
}

static bool load_fp(Arena *a, const char *root, const char *id, Fp *out)
{
    char lib[256], name[256], dir[FS_PATH_MAX], file[FS_PATH_MAX], d1[FS_PATH_MAX], d2[FS_PATH_MAX];
    const char *colon = id ? strchr(id, ':') : NULL;
    if (!colon || (size_t)(colon - id) >= sizeof lib || strlen(colon + 1) >= sizeof name) return false;
    memcpy(lib, id, (size_t)(colon - id));
    lib[colon - id] = 0;
    snprintf(name, sizeof name, "%s", colon + 1);
    if (strstr(lib, "..") || strstr(name, "..") || strpbrk(lib, "/\\") || strpbrk(name, "/\\")) return false;
    snprintf(d1, sizeof d1, "%s.pretty", lib);
    snprintf(d2, sizeof d2, "%s.kicad_mod", name);
    const unsigned char *data = NULL;
    size_t size = 0;
    if (fs_path_join(dir, sizeof dir, root, d1) != FS_OK || fs_path_join(file, sizeof file, dir, d2) != FS_OK ||
        fs_read_file_arena(a, file, &data, &size) != FS_OK) return false;
    KicadFootprintAsset fa;
    KicadAssetError e;
    memset(&e, 0, sizeof e);
    if (kicad_footprint_parse(a, (const char *)data, size, &fa, &e) != KICAD_ASSET_OK) return false;
    IRError ie;
    memset(&ie, 0, sizeof ie);
    IRNode *tree = ir_parse(a, (const char *)data, size, &ie);
    if (!tree) return false;
    out->pads = fa.pads;
    out->pad_count = fa.pad_count;
    out->body = layer_box(tree, "F.CrtYd", false);
    out->silk = layer_box(tree, "F.SilkS", true);
    read_label(tree, out);
    if (!out->body.set) {                       /* no courtyard: the pads' envelope plus 0.25 mm */
        for (size_t i = 0; i < fa.pad_count; i++) {
            grow(&out->body, fa.pads[i].x_nm - fa.pads[i].size_x_nm / 2 - 250000, fa.pads[i].y_nm - fa.pads[i].size_y_nm / 2 - 250000);
            grow(&out->body, fa.pads[i].x_nm + fa.pads[i].size_x_nm / 2 + 250000, fa.pads[i].y_nm + fa.pads[i].size_y_nm / 2 + 250000);
        }
    }
    return out->body.set;
}

/* Step 3e: the three silk rules, all or none (has = false when none). */
static bool silk_rules(const JsonValue *ru, int64_t *char_w, int64_t *text_h, int64_t *gap, bool *has);

/* Step 3e: a part's Reference label box in the footprint frame, and the move (dx, dy) that takes it off the part's own
 * drawn silkscreen grown by `gap`: the smallest of up, down, left and right (ties in that order). False: no label. */
static bool label_box(const Fp *fp, const char *ref, int64_t char_w, int64_t text_h, int64_t gap, Rect *box, int64_t *dx, int64_t *dy)
{
    *dx = *dy = 0;
    if (!fp->label || !ref) return false;
    int64_t n = (int64_t)strlen(ref);
    int64_t w = n * fp->lsx * char_w / 1000000 + fp->lthick;   /* <= 10 chars x 1e6 x 1.2e6: well inside int64 */
    int64_t h = fp->lsy * text_h / 1000000 + fp->lthick;
    int64_t a = ((fp->langle % 360000000) + 360000000) % 360000000;
    int64_t hx = w / 2 + w % 2, hy = h / 2 + h % 2;
    if (a == 90000000 || a == 270000000) { int64_t s = hx; hx = hy; hy = s; }
    else if (a % 180000000) { int64_t m = hx > hy ? hx : hy; hx = hy = m; }   /* not a multiple of 90: the square */
    if (fp->ljustified) { int64_t m = hx > hy ? hx : hy; hx = hy = 2 * m; }   /* anchored at an end: both sides */
    Rect lb = {fp->lx - hx, fp->ly - hy, fp->lx + hx, fp->ly + hy, true};
    if (fp->silk.set) {
        Rect g = {fp->silk.x1 - gap, fp->silk.y1 - gap, fp->silk.x2 + gap, fp->silk.y2 + gap, true};
        if (lb.x1 < g.x2 && g.x1 < lb.x2 && lb.y1 < g.y2 && g.y1 < lb.y2) {
            int64_t up = g.y1 - lb.y2, down = g.y2 - lb.y1, left = g.x1 - lb.x2, right = g.x2 - lb.x1;
            int64_t best = -up;
            *dy = up;
            if (down < best) { best = down; *dx = 0; *dy = down; }
            if (-left < best) { best = -left; *dx = left; *dy = 0; }
            if (right < best) { *dx = right; *dy = 0; }
        }
    }
    lb.x1 += *dx; lb.x2 += *dx; lb.y1 += *dy; lb.y2 += *dy;
    *box = lb;
    return true;
}

/* A pad's rectangle in the footprint frame (rotation 0). */
static Rect pad_rect(const KicadFootprintPad *p)
{
    int64_t hx = p->size_x_nm / 2 + p->size_x_nm % 2, hy = p->size_y_nm / 2 + p->size_y_nm % 2;
    int64_t r = ((p->rotation_mdeg % 360000) + 360000) % 360000;
    if (r == 90000 || r == 270000) { int64_t t = hx; hx = hy; hy = t; }
    else if (r % 180000) { int64_t m = hx > hy ? hx : hy; hx = hy = m; }   /* not a multiple of 90: the bounding square */
    Rect o = {p->x_nm - hx, p->y_nm - hy, p->x_nm + hx, p->y_nm + hy, true};
    return o;
}

/* ---------------------------------------------------------------------------------------------- intent helpers */

static const char *jstr(const JsonValue *v) { return v && v->kind == JSON_STRING ? v->text : NULL; }

static bool rule_nm(const JsonValue *rules, const char *key, int64_t *out)
{
    const JsonValue *v = json_get(json_get(json_get(rules, "rules"), key), "value");
    return v && json_to_nm(v, out) == JSON_NUMBER_OK;
}

static const char *rule_text(const JsonValue *rules, const char *key)
{
    const JsonValue *v = json_get(json_get(json_get(rules, "rules"), key), "value");
    return v && v->kind == JSON_NUMBER ? v->text : NULL;
}

static long net_index(const char **nets, size_t n, const char *name)
{
    for (size_t i = 0; name && i < n; i++) if (!strcmp(nets[i], name)) return (long)i;
    return -1;
}

/* The net of a footprint pad, from the component's pins (NULL: unconnected). */
static const char *pad_net(const JsonValue *pins, const char *number)
{
    for (size_t k = 0; pins && number && k < pins->count; k++) {
        const char *pad = jstr(json_get(pins->items[k], "pad"));
        if (pad && !strcmp(pad, number)) return jstr(json_get(pins->items[k], "net"));
    }
    return NULL;
}

/* Step 3c: fine-pitch when two of the footprint's own pads, on different nets (or unconnected), are closer than
   `gap`. Pad boxes at rotation 0 (exact for pads at multiples of 90 degrees; a lower bound otherwise). Only numbered,
   plated pads count (the footprint parser keeps no pad layers; unnumbered pads are paste apertures or mechanical). */
static bool fine_pitch(const Fp *fp, const JsonValue *pins, int64_t gap)
{
    for (size_t i = 0; i < fp->pad_count; i++)
        for (size_t j = i + 1; j < fp->pad_count; j++) {
            const KicadFootprintPad *p = &fp->pads[i], *q = &fp->pads[j];
            /* unnumbered pads (paste apertures, mechanical) and non-plated holes carry no net: never fine-pitch */
            if (!p->number || !*p->number || !q->number || !*q->number) continue;
            if ((p->type && !strcmp(p->type, "np_thru_hole")) || (q->type && !strcmp(q->type, "np_thru_hole"))) continue;
            if (!strcmp(p->number, q->number)) continue;
            const char *np = pad_net(pins, p->number), *nq = pad_net(pins, q->number);
            if (np && nq && !strcmp(np, nq)) continue;
            Rect a = pad_rect(p), c = pad_rect(q);
            int64_t gx = a.x1 > c.x2 ? a.x1 - c.x2 : c.x1 - a.x2;
            int64_t gy = a.y1 > c.y2 ? a.y1 - c.y2 : c.y1 - a.y2;
            if ((gx > gy ? gx : gy) < gap) return true;
        }
    return false;
}

static bool silk_rules(const JsonValue *ru, int64_t *char_w, int64_t *text_h, int64_t *gap, bool *has)
{
    const JsonValue *rr = json_get(ru, "rules");
    bool a = json_get(rr, "silk_char_width_ratio") != NULL, b = json_get(rr, "silk_text_height_ratio") != NULL,
         c = json_get(rr, "silk_to_pad_mm") != NULL;
    *has = a;
    if (a != b || a != c) return false;
    if (!a) return true;
    return rule_nm(ru, "silk_char_width_ratio", char_w) && rule_nm(ru, "silk_text_height_ratio", text_h) &&
           rule_nm(ru, "silk_to_pad_mm", gap) && *char_w > 0 && *text_h > 0 && *gap >= 0;
}

/* ---------------------------------------------------------------------------------------------- problem */

PlaceStatus place_problem(Arena *a, const char *intent, size_t intent_len, const char *hints, size_t hints_len,
                          const char *rules, size_t rules_len, const char *footprints_root,
                          const char **problem, size_t *problem_len, PlaceReport *rep)
{
    if (!rep) return PLACE_ERR_ARGUMENT;
    memset(rep, 0, sizeof *rep);
    if (!a || !intent || !hints || !rules || !footprints_root || !problem || !problem_len) return rep->status = PLACE_ERR_ARGUMENT;
    JsonValue *in = NULL, *hi = NULL, *ru = NULL;
    JsonError je;
    if (json_parse(a, intent, intent_len, &in, &je) != JSON_OK) { fail(rep, PLACE_INVALID_INPUT, "intent.json does not parse", NULL, NULL); return rep->status; }
    if (json_parse(a, hints, hints_len, &hi, &je) != JSON_OK) { fail(rep, PLACE_INVALID_INPUT, "layout_hints.json does not parse", NULL, NULL); return rep->status; }
    if (json_parse(a, rules, rules_len, &ru, &je) != JSON_OK) { fail(rep, PLACE_INVALID_INPUT, "the placement rules do not parse", NULL, NULL); return rep->status; }
    int64_t bw = 0, bh = 0, edge = 0, gap = 0, step = 0, margin = 0;
    const JsonValue *board = json_get(json_get(in, "project"), "board");
    if (!board || json_to_nm(json_get(board, "width_mm"), &bw) != JSON_NUMBER_OK || json_to_nm(json_get(board, "height_mm"), &bh) != JSON_NUMBER_OK) {
        fail(rep, PLACE_INVALID_INPUT, "intent.json has no board size", NULL, NULL);
        return rep->status;
    }
    const char *w_area = rule_text(ru, "area_weight_per_mm2"), *w_dec = rule_text(ru, "decoupling_weight_per_mm"), *w_ko = rule_text(ru, "keepout_edge_weight_per_mm");
    if (!rule_nm(ru, "edge_mm", &edge) || !rule_nm(ru, "gap_mm", &gap) || !rule_nm(ru, "step_mm", &step) || !rule_nm(ru, "body_margin_mm", &margin) ||
        !w_area || !w_dec || !w_ko) {
        fail(rep, PLACE_INVALID_INPUT, "the placement rules need edge_mm, gap_mm, step_mm, body_margin_mm and the three weights", NULL, NULL);
        return rep->status;
    }
    /* Step 3c: optional escape room for fine-pitch parts, both keys or neither */
    int64_t fine_gap = 0, escape = 0;
    const JsonValue *rr = json_get(ru, "rules");
    bool has_gap = json_get(rr, "fine_pitch_gap_mm") != NULL, has_escape = json_get(rr, "fine_pitch_escape_mm") != NULL;
    if (has_gap != has_escape || (has_gap && (!rule_nm(ru, "fine_pitch_gap_mm", &fine_gap) || !rule_nm(ru, "fine_pitch_escape_mm", &escape) ||
                                              fine_gap <= 0 || escape < 0))) {
        fail(rep, PLACE_INVALID_INPUT, "the placement rules need both fine_pitch_gap_mm (> 0) and fine_pitch_escape_mm (>= 0), or neither", NULL, NULL);
        return rep->status;
    }
    /* Step 3e: Reference labels in the silkscreen envelope, all three keys or none */
    int64_t char_w = 0, text_h = 0, silk_pad = 0;
    bool has_cw = false;
    if (!silk_rules(ru, &char_w, &text_h, &silk_pad, &has_cw)) {
        fail(rep, PLACE_INVALID_INPUT, "the placement rules need silk_char_width_ratio, silk_text_height_ratio and silk_to_pad_mm together, or none", NULL, NULL);
        return rep->status;
    }
    const JsonValue *comps = json_get(in, "components");
    if (!comps || comps->kind != JSON_ARRAY || !comps->count) { fail(rep, PLACE_INVALID_INPUT, "intent.json has no components", NULL, NULL); return rep->status; }
    /* nets, in order of first use */
    size_t cap = 0;
    for (size_t i = 0; i < comps->count; i++) { const JsonValue *p = json_get(comps->items[i], "pins"); cap += p ? p->count : 0; }
    const char **nets = arena_alloc_array(a, sizeof *nets, cap ? cap : 1);
    if (!nets) return rep->status = PLACE_ERR_MEMORY;
    size_t nn = 0;
    for (size_t i = 0; i < comps->count; i++) {
        const JsonValue *pins = json_get(comps->items[i], "pins");
        for (size_t k = 0; pins && k < pins->count; k++) {
            const char *n = jstr(json_get(pins->items[k], "net"));
            if (n && net_index(nets, nn, n) < 0) nets[nn++] = n;
        }
    }
    Buf b = {a, NULL, 0, 0, false};
    put(&b, "(placement_problem (version 1)\n  (board "); put_mm(&b, bw); put(&b, " "); put_mm(&b, bh);
    put(&b, ") (edge "); put_mm(&b, edge); put(&b, ") (gap "); put_mm(&b, gap); put(&b, ") (step "); put_mm(&b, step); put(&b, ")\n");
    put(&b, "  (weights (placement_area_mm2 %s) (decoupling_mm %s) (keepout_edge_mm %s))\n  (nets", w_area, w_dec, w_ko);
    for (size_t i = 0; i < nn; i++) { put(&b, " "); put_quoted(&b, nets[i]); }
    put(&b, ")\n");
    /* no part body within body_margin of the edge (flag 8 = no bodies) */
    Rect strips[4] = {{0, 0, bw, margin, true}, {0, bh - margin, bw, bh, true}, {0, 0, margin, bh, true}, {bw - margin, 0, bw, bh, true}};
    for (int s = 0; s < 4; s++) {
        put(&b, "  (keepout "); put_mm(&b, strips[s].x1); put(&b, " "); put_mm(&b, strips[s].y1); put(&b, " ");
        put_mm(&b, strips[s].x2); put(&b, " "); put_mm(&b, strips[s].y2); put(&b, " 8)\n");
    }
    const JsonValue *kos = json_get(hi, "keepouts");
    for (size_t i = 0; i < comps->count; i++) {
        const JsonValue *c = comps->items[i];
        const char *ref = jstr(json_get(c, "reference")), *fid = jstr(json_get(c, "footprint_id"));
        Fp fp;
        memset(&fp, 0, sizeof fp);
        if (!ref || !load_fp(a, footprints_root, fid, &fp)) {
            fail(rep, PLACE_INVALID_INPUT, "%s: footprint %s is missing or does not parse", ref, fid);
            return rep->status;
        }
        const JsonValue *pins = json_get(c, "pins");
        if (fine_gap > 0 && fine_pitch(&fp, pins, fine_gap)) {
            fp.body.x1 -= escape; fp.body.y1 -= escape; fp.body.x2 += escape; fp.body.y2 += escape;
            rep->fine_pitch++;
        }
        if (has_cw) {   /* Step 3e: the Reference label's box joins the silkscreen envelope, which then keeps silk_to_pad */
            Rect lb;
            int64_t mdx, mdy;
            if (label_box(&fp, ref, char_w, text_h, silk_pad, &lb, &mdx, &mdy)) {   /* ratios are mm values x 1e6 */
                if (mdx || mdy) rep->labels_moved++;
                if (!fp.silk.set) fp.silk = lb;
                else { grow(&fp.silk, lb.x1, lb.y1); grow(&fp.silk, lb.x2, lb.y2); }
            }
            if (fp.silk.set) { fp.silk.x1 -= silk_pad; fp.silk.y1 -= silk_pad; fp.silk.x2 += silk_pad; fp.silk.y2 += silk_pad; }
        }
        put(&b, "  (part "); put_quoted(&b, ref); put(&b, "\n");
        for (int rot = 0; rot < 360; rot += 90) {
            Rect body = rot_rect(fp.body, rot);
            put(&b, "    (shape %d (body ", rot); put_mm(&b, body.x1); put(&b, " "); put_mm(&b, body.y1); put(&b, " "); put_mm(&b, body.x2); put(&b, " "); put_mm(&b, body.y2); put(&b, ")");
            if (fp.silk.set) {
                Rect s = rot_rect(fp.silk, rot);
                put(&b, " (silk "); put_mm(&b, s.x1); put(&b, " "); put_mm(&b, s.y1); put(&b, " "); put_mm(&b, s.x2); put(&b, " "); put_mm(&b, s.y2); put(&b, ")");
            }
            for (size_t p = 0; p < fp.pad_count; p++) {
                const char *net = NULL;
                for (size_t k = 0; pins && k < pins->count; k++)
                    if (jstr(json_get(pins->items[k], "pad")) && fp.pads[p].number && !strcmp(jstr(json_get(pins->items[k], "pad")), fp.pads[p].number))
                        net = jstr(json_get(pins->items[k], "net"));
                Rect pr = rot_rect(pad_rect(&fp.pads[p]), rot);
                int64_t cx, cy;
                rot_point(fp.pads[p].x_nm, fp.pads[p].y_nm, rot, &cx, &cy);
                put(&b, "\n      (pad %ld ", net_index(nets, nn, net)); put_mm(&b, cx); put(&b, " "); put_mm(&b, cy); put(&b, " ");
                put_mm(&b, pr.x1); put(&b, " "); put_mm(&b, pr.y1); put(&b, " "); put_mm(&b, pr.x2); put(&b, " "); put_mm(&b, pr.y2); put(&b, ")");
            }
            for (size_t k = 0; kos && k < kos->count; k++) {
                const JsonValue *ko = kos->items[k], *rect = json_get(ko, "rect_um");
                int64_t v[4];
                if (!jstr(json_get(ko, "reference")) || strcmp(jstr(json_get(ko, "reference")), ref)) continue;
                bool ok = rect && rect->kind == JSON_ARRAY && rect->count == 4;
                for (int e = 0; ok && e < 4; e++) ok = json_to_i64(rect->items[e], &v[e]) == JSON_NUMBER_OK;
                if (!ok) { fail(rep, PLACE_INVALID_INPUT, "%s: a keep-out in layout_hints.json is malformed", ref, NULL); return rep->status; }
                Rect kr = {v[0] * 1000, v[1] * 1000, v[2] * 1000, v[3] * 1000, true};
                kr = rot_rect(kr, rot);
                put(&b, "\n      (keepout "); put_mm(&b, kr.x1); put(&b, " "); put_mm(&b, kr.y1); put(&b, " "); put_mm(&b, kr.x2); put(&b, " "); put_mm(&b, kr.y2); put(&b, " 12)");
                if (rot == 0) rep->keepouts++;
            }
            put(&b, ")\n");
        }
        put(&b, "  )\n");
        rep->parts++;
    }
    const JsonValue *dec = json_get(hi, "decoupling");
    for (size_t k = 0; dec && k < dec->count; k++) {
        const char *cap_ref = jstr(json_get(dec->items[k], "capacitor")), *net = jstr(json_get(dec->items[k], "net")), *ic = jstr(json_get(dec->items[k], "ic"));
        long ni = net_index(nets, nn, net);
        if (!cap_ref || !ic || ni < 0) continue;
        put(&b, "  (decoupling "); put_quoted(&b, cap_ref); put(&b, " %ld ", ni); put_quoted(&b, ic); put(&b, ")\n");
        rep->decoupling++;
    }
    put(&b, ")\n");
    if (b.oom) return rep->status = PLACE_ERR_MEMORY;
    *problem = b.s;
    *problem_len = b.len;
    return rep->status = PLACE_OK;
}

/* ---------------------------------------------------------------------------------------------- apply */

static void copy_value(JsonWriter *w, const JsonValue *v)
{
    switch (v->kind) {
    case JSON_NULL: json_write_null(w); break;
    case JSON_TRUE: json_write_bool(w, true); break;
    case JSON_FALSE: json_write_bool(w, false); break;
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

static void write_mm(JsonWriter *w, int64_t nm) { char t[40]; mm_text(t, sizeof t, nm); json_write_raw(w, t, strlen(t)); }

typedef struct Placed { int64_t x, y; int rot; bool ok; } Placed;

static Placed placed_of(const JsonValue *placement, const char *ref)
{
    Placed p = {0, 0, 0, false};
    const JsonValue *v = json_get(placement, ref);
    int64_t r = 0;
    if (v && v->kind == JSON_ARRAY && v->count == 3 && json_to_nm(v->items[0], &p.x) == JSON_NUMBER_OK &&
        json_to_nm(v->items[1], &p.y) == JSON_NUMBER_OK && json_to_i64(v->items[2], &r) == JSON_NUMBER_OK && r % 90 == 0) {
        p.rot = (int)(((r % 360) + 360) % 360);
        p.ok = true;
    }
    return p;
}

PlaceStatus place_apply(Arena *a, const char *intent, size_t intent_len, const char *hints, size_t hints_len,
                        const char *placer_json, size_t placer_len,
                        const char **intent_out, size_t *intent_out_len, const char **routing_out, size_t *routing_out_len,
                        PlaceReport *rep)
{
    if (!rep) return PLACE_ERR_ARGUMENT;
    memset(rep, 0, sizeof *rep);
    if (!a || !intent || !hints || !placer_json || !intent_out || !intent_out_len || !routing_out || !routing_out_len) return rep->status = PLACE_ERR_ARGUMENT;
    JsonValue *in = NULL, *hi = NULL, *pl = NULL;
    JsonError je;
    if (json_parse(a, intent, intent_len, &in, &je) != JSON_OK || json_parse(a, hints, hints_len, &hi, &je) != JSON_OK ||
        json_parse(a, placer_json, placer_len, &pl, &je) != JSON_OK) {
        fail(rep, PLACE_INVALID_INPUT, "intent, layout hints or the placer's output does not parse", NULL, NULL);
        return rep->status;
    }
    /* the lowest-cost legal annealed placement, else the constructive one if it placed everything */
    const JsonValue *best = NULL, *ann = json_get(pl, "annealed");
    int64_t best_cost = 0;
    char seedbuf[32] = "";
    for (size_t i = 0; ann && ann->kind == JSON_ARRAY && i < ann->count; i++) {
        const JsonValue *x = ann->items[i];
        int64_t cost = 0;
        if (!jstr(json_get(x, "status")) || strcmp(jstr(json_get(x, "status")), "OK") || json_to_i64(json_get(x, "cost_nm"), &cost) != JSON_NUMBER_OK) continue;
        if (!best || cost < best_cost) { best = json_get(x, "placement"); best_cost = cost; snprintf(seedbuf, sizeof seedbuf, "seed %s", json_get(x, "seed") ? json_get(x, "seed")->text : "?"); }
    }
    const JsonValue *con = json_get(pl, "constructive");
    if (!best) {
        const JsonValue *fails = json_get(con, "failures");
        if (fails && fails->kind == JSON_ARRAY && fails->count) {
            char list[300] = "";
            for (size_t i = 0; i < fails->count && strlen(list) < 250; i++) {
                strncat(list, i ? ", " : "", sizeof list - strlen(list) - 1);
                strncat(list, jstr(fails->items[i]) ? jstr(fails->items[i]) : "?", sizeof list - strlen(list) - 1);
            }
            fail(rep, PLACE_NO_LEGAL_POSITION, "no legal position on the board for: %s (a larger board, or fewer parts)", list, NULL);
            return rep->status;
        }
        best = json_get(con, "placement");
        snprintf(seedbuf, sizeof seedbuf, "constructive");
    }
    char *seed = arena_alloc(a, strlen(seedbuf) + 1);
    if (!seed) return rep->status = PLACE_ERR_MEMORY;
    memcpy(seed, seedbuf, strlen(seedbuf) + 1);
    rep->seed = seed;
    const JsonValue *comps = json_get(in, "components");
    for (size_t i = 0; comps && i < comps->count; i++) {
        const char *ref = jstr(json_get(comps->items[i], "reference"));
        if (!ref || !placed_of(best, ref).ok) { fail(rep, PLACE_INVALID_INPUT, "the placer gave no position for %s", ref ? ref : "?", NULL); return rep->status; }
    }
    /* the intent, with a position for every part */
    JsonWriter w;
    json_writer_init(&w, a);
    json_write_begin_object(&w);
    for (size_t i = 0; i < in->count; i++) {
        json_write_key(&w, in->keys[i]);
        if (strcmp(in->keys[i], "components")) { copy_value(&w, in->items[i]); continue; }
        json_write_begin_array(&w);
        for (size_t k = 0; k < comps->count; k++) {
            const JsonValue *c = comps->items[k];
            Placed p = placed_of(best, jstr(json_get(c, "reference")));
            json_write_begin_object(&w);
            for (size_t q = 0; q < c->count; q++) {
                if (!strcmp(c->keys[q], "position")) continue;
                json_write_key(&w, c->keys[q]);
                copy_value(&w, c->items[q]);
                if (!strcmp(c->keys[q], "footprint_id")) {
                    json_write_key(&w, "position");
                    json_write_begin_object(&w);
                    json_write_key(&w, "x_mm"); write_mm(&w, p.x);
                    json_write_key(&w, "y_mm"); write_mm(&w, p.y);
                    json_write_key(&w, "rotation_deg"); json_write_int(&w, p.rot);
                    json_write_end_object(&w);
                }
            }
            json_write_end_object(&w);
            rep->parts++;
        }
        json_write_end_array(&w);
    }
    json_write_end_object(&w);
    *intent_out = json_writer_finish(&w, intent_out_len);
    if (!*intent_out) return rep->status = PLACE_ERR_MEMORY;
    /* routing.json: every part keep-out in board coordinates, no tracks or vias on either layer */
    JsonWriter r;
    json_writer_init(&r, a);
    json_write_begin_object(&r);
    json_write_key(&r, "format"); json_write_string(&r, "copper.routing/1");
    json_write_key(&r, "template"); json_write_string(&r, "composed");
    json_write_key(&r, "why"); json_write_string(&r, "composed design: no per-net track widths are given (no sourced current-to-width data), so every net uses the routing rules' default width: UNVERIFIED");
    json_write_key(&r, "keepouts");
    json_write_begin_array(&r);
    const JsonValue *kos = json_get(hi, "keepouts");
    for (size_t k = 0; kos && k < kos->count; k++) {
        const JsonValue *ko = kos->items[k], *rect = json_get(ko, "rect_um");
        const char *ref = jstr(json_get(ko, "reference"));
        Placed p = placed_of(best, ref ? ref : "");
        int64_t v[4];
        bool ok = p.ok && rect && rect->kind == JSON_ARRAY && rect->count == 4;
        for (int e = 0; ok && e < 4; e++) ok = json_to_i64(rect->items[e], &v[e]) == JSON_NUMBER_OK;
        if (!ok) { fail(rep, PLACE_INVALID_INPUT, "a keep-out in layout_hints.json is malformed or its part is not placed", NULL, NULL); return rep->status; }
        Rect kr = {v[0] * 1000, v[1] * 1000, v[2] * 1000, v[3] * 1000, true};
        kr = rot_rect(kr, p.rot);
        json_write_begin_object(&r);
        json_write_key(&r, "ref"); json_write_string(&r, ref);
        json_write_key(&r, "rect_mm");
        json_write_begin_array(&r);
        write_mm(&r, kr.x1 + p.x); write_mm(&r, kr.y1 + p.y); write_mm(&r, kr.x2 + p.x); write_mm(&r, kr.y2 + p.y);
        json_write_end_array(&r);
        json_write_key(&r, "layers");
        json_write_begin_array(&r); json_write_string(&r, "F.Cu"); json_write_string(&r, "B.Cu"); json_write_end_array(&r);
        json_write_key(&r, "why"); json_write_string(&r, jstr(json_get(ko, "why")) ? jstr(json_get(ko, "why")) : "");
        json_write_end_object(&r);
        rep->keepouts++;
    }
    json_write_end_array(&r);
    json_write_end_object(&r);
    *routing_out = json_writer_finish(&r, routing_out_len);
    if (!*routing_out) return rep->status = PLACE_ERR_MEMORY;
    return rep->status = PLACE_OK;
}

const char *place_status_name(PlaceStatus s)
{
    switch (s) {
    case PLACE_OK: return "PLACED";
    case PLACE_INVALID_INPUT: return "INVALID_INPUT";
    case PLACE_NO_LEGAL_POSITION: return "NO_LEGAL_POSITION";
    case PLACE_ERR_ARGUMENT: return "ERR_ARGUMENT";
    case PLACE_ERR_MEMORY: return "ERR_MEMORY";
    }
    return "UNKNOWN";
}

/* ---------------------------------------------------------------------------------------------- labels (Step 3e) */

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

static const char *find_in(const char *t, size_t from, size_t to, const char *what)
{
    size_t n = strlen(what);
    for (size_t i = from; i + n <= to; i++) if (!memcmp(t + i, what, n)) return t + i;
    return NULL;
}

/* a decimal in mm at t[i..] (at most 6 decimals) to nm; *end is set after it */
static bool dec_nm(const char *t, size_t len, size_t i, int64_t *out, size_t *end)
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
    *out = (whole * 1000000 + frac) * (neg ? -1 : 1);
    *end = i;
    return true;
}

PlaceStatus place_labels(Arena *a, const char *pcb, size_t len, const char *rules, size_t rules_len,
                         const char *footprints_root, const char **out, size_t *out_len, PlaceReport *rep)
{
    if (!rep) return PLACE_ERR_ARGUMENT;
    memset(rep, 0, sizeof *rep);
    if (!a || !pcb || !rules || !footprints_root || !out || !out_len) return rep->status = PLACE_ERR_ARGUMENT;
    *out = pcb; *out_len = len;
    JsonValue *ru = NULL;
    JsonError je;
    if (json_parse(a, rules, rules_len, &ru, &je) != JSON_OK) { fail(rep, PLACE_INVALID_INPUT, "the placement rules do not parse", NULL, NULL); return rep->status; }
    int64_t char_w = 0, text_h = 0, gap = 0;
    bool has = false;
    if (!silk_rules(ru, &char_w, &text_h, &gap, &has)) {
        fail(rep, PLACE_INVALID_INPUT, "the placement rules need silk_char_width_ratio, silk_text_height_ratio and silk_to_pad_mm together, or none", NULL, NULL);
        return rep->status;
    }
    if (!has) return rep->status = PLACE_OK;
    Buf o = {a, NULL, 0, 0, false};
    size_t copied = 0;
    static const char FP[] = "(footprint \"";
    for (const char *f = find_in(pcb, 0, len, FP); f; ) {
        size_t start = (size_t)(f - pcb), end = close_of(pcb, len, start);
        if (end == SIZE_MAX) { fail(rep, PLACE_INVALID_INPUT, "the board's brackets do not balance", NULL, NULL); return rep->status; }
        /* the library id, and the Reference property's text and (at X Y [A]) */
        size_t id0 = start + sizeof FP - 1, id1 = id0;
        while (id1 < end && pcb[id1] != '"') id1++;
        char id[300];
        if (id1 - id0 >= sizeof id) { fail(rep, PLACE_INVALID_INPUT, "a footprint id is too long", NULL, NULL); return rep->status; }
        memcpy(id, pcb + id0, id1 - id0); id[id1 - id0] = 0;
        const char *prop = find_in(pcb, start, end, "(property \"Reference\" \"");
        size_t pend = prop ? close_of(pcb, len, (size_t)(prop - pcb)) : SIZE_MAX;
        const char *at = prop && pend != SIZE_MAX ? find_in(pcb, (size_t)(prop - pcb), pend, "(at ") : NULL;
        if (at) {
            size_t r0 = (size_t)(prop - pcb) + 23, r1 = r0;
            while (r1 < pend && pcb[r1] != '"') r1++;
            char ref[64];
            size_t rl = r1 - r0 < sizeof ref - 1 ? r1 - r0 : sizeof ref - 1;
            memcpy(ref, pcb + r0, rl); ref[rl] = 0;
            int64_t x = 0, y = 0;
            size_t xs = (size_t)(at - pcb) + 4, xe = 0, ys = 0, ye = 0;
            bool ok = dec_nm(pcb, pend, xs, &x, &xe) && xe < pend && pcb[xe] == ' ' && dec_nm(pcb, pend, ys = xe + 1, &y, &ye);
            Fp fp;
            memset(&fp, 0, sizeof fp);
            if (!load_fp(a, footprints_root, id, &fp)) { fail(rep, PLACE_INVALID_INPUT, "footprint %s is missing from the library%s", id, ""); return rep->status; }
            Rect lb;
            int64_t dx = 0, dy = 0;
            if (ok && fp.label && x == fp.lx && y == fp.ly && label_box(&fp, ref, char_w, text_h, gap, &lb, &dx, &dy) && (dx || dy)) {
                put_raw(&o, pcb + copied, xs - copied);
                put_mm(&o, x + dx); put_raw(&o, " ", 1); put_mm(&o, y + dy);
                copied = ye;
                rep->labels_moved++;
            }
        }
        f = find_in(pcb, end + 1, len, FP);
    }
    put_raw(&o, pcb + copied, len - copied);
    if (o.oom) return rep->status = PLACE_ERR_MEMORY;
    *out = o.s;
    *out_len = o.len;
    return rep->status = PLACE_OK;
}

/* board.c - the KiCad board model in exact integer nanometres (see board.h). No floating point, no printf/exit. */
#include "board.h"

#include "IR.h"
#include "nmath.h"

#include <stdio.h>
#include <string.h>

#if defined(__SIZEOF_INT128__)
__extension__ typedef __int128 BoardI128;
#define BOARD_HAVE_I128 1
#else
#define BOARD_HAVE_I128 0
#endif

#define UDEG_90 INT64_C(90000000)
#define UDEG_360 INT64_C(360000000)

static const char *const STATUS_NAMES[] = {
    "OK", "INVALID_ARGUMENT", "IO_ERROR", "PARSE_ERROR", "NOT_A_BOARD", "INVALID_CONTENT", "RANGE_ERROR",
    "OUT_OF_MEMORY"};
static const char *const ISSUE_NAMES[] = {
    "INEXACT_ROTATION", "ZONE_ARC", "EDGE_UNSUPPORTED", "ARC_DEGENERATE", "OUTLINE_OPEN", "NO_OUTLINE",
    "ARC_BOUNDS_UNAVAILABLE"};

const char *board_status_name(BoardStatus s)
{
    return (unsigned)s < sizeof STATUS_NAMES / sizeof *STATUS_NAMES ? STATUS_NAMES[s] : "UNKNOWN_STATUS";
}

const char *board_issue_name(BoardIssueCode c)
{
    return (unsigned)c < sizeof ISSUE_NAMES / sizeof *ISSUE_NAMES ? ISSUE_NAMES[c] : "UNKNOWN_ISSUE";
}

/* ------------------------------------------------------------------------------------------------ rotation */

#if BOARD_HAVE_I128
/* sin and cos of r micro-degrees, 0 <= r <= 45e6, in Q62, by Taylor series on the exact Q62 angle. */
static void sincos_q62(int64_t r, BoardI128 *s, BoardI128 *c)
{
    const BoardI128 one = (BoardI128)1 << 62;
    const BoardI128 pi_q62 = ((BoardI128)UINT64_C(0xc90fdaa22168c235));   /* round(pi * 2^62) */
    BoardI128 x = (pi_q62 * r + 90000000) / 180000000;                     /* radians, Q62, rounded */
    BoardI128 x2 = (x * x) >> 62;
    BoardI128 term = x, sum_s = 0, sum_c = 0;
    for (int k = 1; k < 30 && term; k += 2) {                              /* x - x^3/3! + x^5/5! ... */
        sum_s += term;
        term = -((term * x2) >> 62) / ((k + 1) * (k + 2));
    }
    term = one;
    for (int k = 0; k < 30 && term; k += 2) {                              /* 1 - x^2/2! + x^4/4! ... */
        sum_c += term;
        term = -((term * x2) >> 62) / ((k + 1) * (k + 2));
    }
    *s = sum_s;
    *c = sum_c;
}

static BoardI128 round_q62(BoardI128 v)
{
    const BoardI128 half = (BoardI128)1 << 61;
    return v >= 0 ? (v + half) >> 62 : -((-v + half) >> 62);
}
#endif

BoardStatus board_rotate(G2Nm x, G2Nm y, int64_t udeg, G2Nm *ox, G2Nm *oy, bool *exact)
{
    int64_t a = udeg % UDEG_360;
    if (a < 0) a += UDEG_360;
    if (a % UDEG_90 == 0) {
        *exact = true;
        switch (a / UDEG_90) {
        case 0: *ox = x;  *oy = y;  break;
        case 1: *ox = y;  *oy = -x; break;   /* cos 0, sin 1 */
        case 2: *ox = -x; *oy = -y; break;
        default: *ox = -y; *oy = x; break;   /* cos 0, sin -1 */
        }
        return BOARD_OK;
    }
    *exact = false;
#if BOARD_HAVE_I128
    int64_t q = a / UDEG_90, r = a % UDEG_90;
    BoardI128 s, c, sa, ca;
    if (r <= UDEG_90 / 2) sincos_q62(r, &s, &c);
    else { sincos_q62(UDEG_90 - r, &c, &s); }
    switch (q) {                               /* sin/cos of a = q*90 + r */
    case 0: sa = s;  ca = c;  break;
    case 1: sa = c;  ca = -s; break;
    case 2: sa = -s; ca = -c; break;
    default: sa = -c; ca = s; break;
    }
    BoardI128 rx = round_q62((BoardI128)x * ca + (BoardI128)y * sa);
    BoardI128 ry = round_q62(-(BoardI128)x * sa + (BoardI128)y * ca);
    if (rx > INT64_MAX || rx < -INT64_MAX || ry > INT64_MAX || ry < -INT64_MAX) return BOARD_RANGE_ERROR;
    *ox = (G2Nm)rx;
    *oy = (G2Nm)ry;
    return BOARD_OK;
#else
    (void)x; (void)y; (void)ox; (void)oy;
    return BOARD_RANGE_ERROR;
#endif
}

/* ------------------------------------------------------------------------------------------------ helpers */

typedef struct Loader {
    Arena *arena;
    Board *b;
    BoardError *err;
    size_t item;
    size_t cap_nets, cap_fps, cap_pads, cap_tracks, cap_vias, cap_zones, cap_edges, cap_issues;
    int32_t *net_by_number;      /* KiCad net number -> index */
    size_t net_by_number_len;
    int32_t *hash;               /* open addressing over net names -> index, -1 empty */
    size_t hash_cap;
} Loader;

static BoardStatus fail(Loader *L, BoardStatus s, const char *context, const char *message)
{
    if (L->err) {
        L->err->code = s; L->err->line = L->err->column = L->err->offset = 0;
        L->err->item = L->item; L->err->context = context; L->err->message = message;
    }
    return s;
}

static bool grow(Arena *a, void **ptr, size_t *cap, size_t need, size_t elem)
{
    if (need <= *cap) return true;
    size_t n = *cap ? *cap * 2 : 16;
    while (n < need) n *= 2;
    void *p = arena_alloc_array(a, elem, n);
    if (!p) return false;
    if (*ptr && *cap) memcpy(p, *ptr, *cap * elem);
    *ptr = p;
    *cap = n;
    return true;
}

#define PUSH(L, arr, count, capf, outp)                                                                       \
    (grow((L)->arena, (void **)&(L)->b->arr, &(L)->capf, (L)->b->count + 1, sizeof *(L)->b->arr)              \
         ? ((outp) = &(L)->b->arr[(L)->b->count++], memset((outp), 0, sizeof *(outp)), true)                  \
         : false)

static const char *ctx(Loader *L, const char *fmt, const char *a, const char *b)
{
    size_t n = strlen(fmt) + (a ? strlen(a) : 0) + (b ? strlen(b) : 0) + 1;
    char *s = arena_alloc(L->arena, n);
    if (!s) return fmt;
    snprintf(s, n, fmt, a ? a : "", b ? b : "");
    return s;
}

static bool issue(Loader *L, BoardIssueCode code, const char *context)
{
    BoardIssue *i;
    if (!PUSH(L, issues, issue_count, cap_issues, i)) return false;
    i->code = code; i->item = L->item; i->context = context;
    return true;
}

static bool is_atom(const IRNode *n, const char *t) { return n && n->kind == IR_NODE_ATOM && n->text && !strcmp(n->text, t); }
static bool is_form(const IRNode *n, const char *h) { return n && n->kind == IR_NODE_LIST && is_atom(n->first_child, h); }
static const IRNode *arg(const IRNode *form, size_t i) { return ir_child_at(form, i); }
static const IRNode *find(const IRNode *form, const char *head) { return ir_list_find(form, head); }

static const char *text_of(const IRNode *n)
{
    return n && (n->kind == IR_NODE_STRING || n->kind == IR_NODE_ATOM) && n->text ? n->text : NULL;
}

static BoardStatus nm_of(const IRNode *n, G2Nm *out)
{
    int64_t v;
    if (!n) return BOARD_INVALID_CONTENT;
    IRStatus s = ir_atom_to_nm(n, &v);
    if (s == IR_STATUS_RANGE_ERROR) return BOARD_RANGE_ERROR;
    if (s != IR_STATUS_OK) return BOARD_INVALID_CONTENT;
    if (v > G2_COORD_MAX_NM || v < -G2_COORD_MAX_NM) return BOARD_RANGE_ERROR;
    *out = v;
    return BOARD_OK;
}

/* (HEAD X Y ...) -> point; missing -> INVALID_CONTENT. */
static BoardStatus point_of(const IRNode *parent, const char *head, G2Point *p)
{
    const IRNode *f = find(parent, head);
    BoardStatus s;
    if (!f) return BOARD_INVALID_CONTENT;
    if ((s = nm_of(arg(f, 1), &p->x)) || (s = nm_of(arg(f, 2), &p->y))) return s;
    return BOARD_OK;
}

/* Optional angle as the third value of (at X Y [A]), in micro-degrees. */
static BoardStatus angle_of(const IRNode *at, int64_t *udeg)
{
    const IRNode *a = arg(at, 3);
    *udeg = 0;
    if (!a || a->kind != IR_NODE_ATOM) return BOARD_OK;
    if (ir_atom_to_nm(a, udeg) != IR_STATUS_OK) return BOARD_INVALID_CONTENT;   /* exact decimal x 1e6 */
    return BOARD_OK;
}

uint64_t board_layer_bits(const char *name)
{
    static const struct { const char *name; uint64_t bits; } FIXED[] = {
        {"F.Cu", BOARD_LAYER_F_CU}, {"B.Cu", BOARD_LAYER_B_CU}, {"*.Cu", BOARD_LAYERS_CU},
        {"F&B.Cu", BOARD_LAYER_F_CU | BOARD_LAYER_B_CU},
        {"F.Mask", BOARD_LAYER_F_MASK}, {"B.Mask", BOARD_LAYER_B_MASK}, {"*.Mask", BOARD_LAYER_F_MASK | BOARD_LAYER_B_MASK},
        {"F&B.Mask", BOARD_LAYER_F_MASK | BOARD_LAYER_B_MASK},
        {"F.Paste", BOARD_LAYER_F_PASTE}, {"B.Paste", BOARD_LAYER_B_PASTE}, {"*.Paste", BOARD_LAYER_F_PASTE | BOARD_LAYER_B_PASTE},
        {"F.SilkS", BOARD_LAYER_F_SILK}, {"B.SilkS", BOARD_LAYER_B_SILK}, {"*.SilkS", BOARD_LAYER_F_SILK | BOARD_LAYER_B_SILK},
        {"F.Silkscreen", BOARD_LAYER_F_SILK}, {"B.Silkscreen", BOARD_LAYER_B_SILK},
        {"Edge.Cuts", BOARD_LAYER_EDGE_CUTS},
        {"F.CrtYd", BOARD_LAYER_F_CRTYD}, {"B.CrtYd", BOARD_LAYER_B_CRTYD},
        {"F.Courtyard", BOARD_LAYER_F_CRTYD}, {"B.Courtyard", BOARD_LAYER_B_CRTYD},
        {"F.Fab", BOARD_LAYER_F_FAB}, {"B.Fab", BOARD_LAYER_B_FAB}};
    if (!name || !*name) return 0;
    for (size_t i = 0; i < sizeof FIXED / sizeof *FIXED; i++)
        if (!strcmp(name, FIXED[i].name)) return FIXED[i].bits;
    if (name[0] == 'I' && name[1] == 'n') {                       /* In1.Cu .. In30.Cu */
        int n = 0;
        const char *p = name + 2;
        while (*p >= '0' && *p <= '9' && n <= 30) n = n * 10 + (*p++ - '0');
        if (p != name + 2 && !strcmp(p, ".Cu") && n >= 1 && n <= 30) return BOARD_LAYER_IN_CU(n);
    }
    return BOARD_LAYER_OTHER;
}

/* (layer "X") or (layers "X" "Y" ...) under `parent`. */
static uint64_t layers_of(const IRNode *parent)
{
    uint64_t bits = 0;
    const IRNode *f = find(parent, "layer");
    if (f) bits |= board_layer_bits(text_of(arg(f, 1)));
    f = find(parent, "layers");
    if (f)
        for (const IRNode *c = arg(f, 1); c; c = c->next) bits |= board_layer_bits(text_of(c));
    return bits;
}

/* ------------------------------------------------------------------------------------------------ nets */

static uint64_t fnv(const char *s)
{
    uint64_t h = UINT64_C(1469598103934665603);
    while (*s) { h ^= (unsigned char)*s++; h *= UINT64_C(1099511628211); }
    return h;
}

static bool hash_insert(Loader *L, int32_t index);

static bool hash_rebuild(Loader *L, size_t cap)
{
    int32_t *t = arena_alloc_array(L->arena, sizeof *t, cap);
    if (!t) return false;
    for (size_t i = 0; i < cap; i++) t[i] = -1;
    L->hash = t;
    L->hash_cap = cap;
    for (size_t i = 0; i < L->b->net_count; i++)
        if (!hash_insert(L, (int32_t)i)) return false;
    return true;
}

static bool hash_insert(Loader *L, int32_t index)
{
    if (!L->hash || (L->b->net_count + 1) * 2 > L->hash_cap) {
        size_t cap = L->hash_cap ? L->hash_cap * 2 : 64;
        while ((L->b->net_count + 1) * 2 > cap) cap *= 2;
        return hash_rebuild(L, cap);   /* re-inserts every net, `index` included when already counted */
    }
    size_t i = (size_t)(fnv(L->b->nets[index].name) & (L->hash_cap - 1));
    while (L->hash[i] >= 0) {
        if (L->hash[i] == index) return true;
        i = (i + 1) & (L->hash_cap - 1);
    }
    L->hash[i] = index;
    return true;
}

static int32_t net_by_name(const Loader *L, const char *name)
{
    if (!L->hash) return -1;
    size_t i = (size_t)(fnv(name) & (L->hash_cap - 1));
    while (L->hash[i] >= 0) {
        if (!strcmp(L->b->nets[L->hash[i]].name, name)) return L->hash[i];
        i = (i + 1) & (L->hash_cap - 1);
    }
    return -1;
}

static int32_t add_net(Loader *L, int32_t number, const char *name)
{
    BoardNet *n;
    if (!PUSH(L, nets, net_count, cap_nets, n)) return -2;
    n->number = number;
    n->name = name;
    int32_t index = (int32_t)(L->b->net_count - 1);
    if (!hash_insert(L, index)) return -2;
    if (number >= 0) {
        if ((size_t)number >= L->net_by_number_len) {
            size_t len = L->net_by_number_len ? L->net_by_number_len : 64;
            while (len <= (size_t)number) len *= 2;
            int32_t *t = arena_alloc_array(L->arena, sizeof *t, len);
            if (!t) return -2;
            for (size_t i = 0; i < len; i++) t[i] = i < L->net_by_number_len ? L->net_by_number[i] : -1;
            L->net_by_number = t;
            L->net_by_number_len = len;
        }
        L->net_by_number[number] = index;
    }
    return index;
}

/* An item's (net N ["name"]) or (net "name"): its index, -1 for none, -2 out of memory, -3 malformed. */
static int32_t net_ref(Loader *L, const IRNode *parent)
{
    const IRNode *f = find(parent, "net");
    if (!f) return -1;
    const IRNode *a = arg(f, 1), *b = arg(f, 2);
    if (!a) return -3;
    if (a->kind == IR_NODE_ATOM) {
        int64_t number;
        if (ir_atom_to_i64(a, &number) != IR_STATUS_OK || number < 0 || number > INT32_MAX) return -3;
        if ((size_t)number < L->net_by_number_len && L->net_by_number[number] >= 0) return L->net_by_number[number];
        const char *name = text_of(b);
        if (name) {
            int32_t i = net_by_name(L, name);
            if (i >= 0) return i;
        }
        return add_net(L, (int32_t)number, name ? name : "");
    }
    const char *name = text_of(a);
    if (!name) return -3;
    int32_t i = net_by_name(L, name);
    return i >= 0 ? i : add_net(L, -1, name);
}

/* ------------------------------------------------------------------------------------------------ edges */

static BoardStatus add_edge(Loader *L, BoardEdgeKind kind, G2Point s, G2Point m, G2Point e, bool fp, bool exact)
{
    BoardEdge *ed;
    if (!PUSH(L, edges, edge_count, cap_edges, ed)) return fail(L, BOARD_OUT_OF_MEMORY, NULL, "out of memory");
    ed->kind = kind; ed->start = s; ed->mid = m; ed->end = e; ed->from_footprint = fp; ed->exact = exact;
    ed->loop = SIZE_MAX;
    return BOARD_OK;
}

typedef struct Frame { G2Point at; int64_t udeg; bool identity; } Frame;

static BoardStatus place(const Frame *f, G2Point local, G2Point *out, bool *exact)
{
    G2Nm x, y;
    bool ex = true;
    if (f->identity) { *out = local; return BOARD_OK; }
    BoardStatus s = board_rotate(local.x, local.y, f->udeg, &x, &y, &ex);
    if (s) return s;
    out->x = x + f->at.x;
    out->y = y + f->at.y;
    if (out->x > G2_COORD_MAX_NM || out->x < -G2_COORD_MAX_NM || out->y > G2_COORD_MAX_NM || out->y < -G2_COORD_MAX_NM)
        return BOARD_RANGE_ERROR;
    if (!ex) *exact = false;
    return BOARD_OK;
}

/* A gr_* / fp_* graphic on Edge.Cuts (`kind` is the head without its prefix), in frame f. */
static BoardStatus edge_item(Loader *L, const IRNode *g, const char *kind, const Frame *f, bool fp, const char *where)
{
    G2Point a = {0, 0}, b = {0, 0}, m = {0, 0}, pa, pb, pm;
    bool exact = true;
    BoardStatus s;
    if (!strcmp(kind, "line") || !strcmp(kind, "rect")) {
        if ((s = point_of(g, "start", &a)) || (s = point_of(g, "end", &b)))
            return fail(L, s, ctx(L, "%s%s: start/end", where, kind), "an Edge.Cuts item needs (start X Y) (end X Y)");
        if (!strcmp(kind, "line")) {
            if ((s = place(f, a, &pa, &exact)) || (s = place(f, b, &pb, &exact))) return fail(L, s, where, "coordinate out of range");
            return add_edge(L, BOARD_EDGE_LINE, pa, pa, pb, fp, exact);
        }
        G2Point c[4] = {{a.x, a.y}, {b.x, a.y}, {b.x, b.y}, {a.x, b.y}}, w[4];
        for (int i = 0; i < 4; i++)
            if ((s = place(f, c[i], &w[i], &exact))) return fail(L, s, where, "coordinate out of range");
        for (int i = 0; i < 4; i++)
            if ((s = add_edge(L, BOARD_EDGE_LINE, w[i], w[i], w[(i + 1) % 4], fp, exact))) return s;
        return BOARD_OK;
    }
    if (!strcmp(kind, "arc")) {
        if ((s = point_of(g, "start", &a)) || (s = point_of(g, "mid", &m)) || (s = point_of(g, "end", &b)))
            return fail(L, s, ctx(L, "%s%s: start/mid/end", where, kind), "an Edge.Cuts arc needs (start) (mid) (end)");
        if ((s = place(f, a, &pa, &exact)) || (s = place(f, m, &pm, &exact)) || (s = place(f, b, &pb, &exact)))
            return fail(L, s, where, "coordinate out of range");
        return add_edge(L, BOARD_EDGE_ARC, pa, pm, pb, fp, exact);
    }
    if (!strcmp(kind, "circle")) {
        if ((s = point_of(g, "center", &a)) || (s = point_of(g, "end", &b)))
            return fail(L, s, ctx(L, "%s%s: center/end", where, kind), "an Edge.Cuts circle needs (center) (end)");
        if ((s = place(f, a, &pa, &exact)) || (s = place(f, b, &pb, &exact))) return fail(L, s, where, "coordinate out of range");
        return add_edge(L, BOARD_EDGE_CIRCLE, pa, pa, pb, fp, exact);
    }
    if (!strcmp(kind, "poly")) {
        const IRNode *pts = find(g, "pts");
        G2Point first = {0, 0}, prev = {0, 0};
        size_t n = 0;
        for (const IRNode *c = pts ? arg(pts, 1) : NULL; c; c = c->next) {
            G2Point p, w;
            if (!is_form(c, "xy")) { if (!issue(L, BOARD_ISSUE_EDGE_UNSUPPORTED, ctx(L, "%sgr_poly with a non-xy point%s", where, NULL))) return fail(L, BOARD_OUT_OF_MEMORY, NULL, "out of memory"); continue; }
            if ((s = nm_of(arg(c, 1), &p.x)) || (s = nm_of(arg(c, 2), &p.y)) || (s = place(f, p, &w, &exact)))
                return fail(L, s, where, "polygon point");
            if (n++) { if ((s = add_edge(L, BOARD_EDGE_LINE, prev, prev, w, fp, exact))) return s; }
            else first = w;
            prev = w;
        }
        if (n > 2) return add_edge(L, BOARD_EDGE_LINE, prev, prev, first, fp, exact);
        return BOARD_OK;
    }
    if (!issue(L, BOARD_ISSUE_EDGE_UNSUPPORTED, ctx(L, "%s%s on Edge.Cuts", where, kind))) return fail(L, BOARD_OUT_OF_MEMORY, NULL, "out of memory");
    return BOARD_OK;
}

static bool on_edge_cuts(const IRNode *g)
{
    const IRNode *f = find(g, "layer");
    return f && board_layer_bits(text_of(arg(f, 1))) == BOARD_LAYER_EDGE_CUTS;
}

/* ------------------------------------------------------------------------------------------------ items */

static BoardStatus read_pad(Loader *L, const IRNode *p, size_t fp_index, const Frame *f, const char *ref)
{
    BoardPad *pad;
    BoardStatus s;
    const char *number = text_of(arg(p, 1)), *type = text_of(arg(p, 2)), *shape = text_of(arg(p, 3));
    if (!PUSH(L, pads, pad_count, cap_pads, pad)) return fail(L, BOARD_OUT_OF_MEMORY, NULL, "out of memory");
    const char *where = ctx(L, "footprint %s pad %s", ref, number ? number : "?");
    if (!number || !type || !shape) return fail(L, BOARD_INVALID_CONTENT, where, "(pad NUMBER TYPE SHAPE ...)");
    pad->number = number;
    pad->footprint = fp_index;
    pad->type = !strcmp(type, "smd") ? BOARD_PAD_SMD : !strcmp(type, "thru_hole") ? BOARD_PAD_THRU_HOLE
              : !strcmp(type, "np_thru_hole") ? BOARD_PAD_NP_THRU_HOLE : !strcmp(type, "connect") ? BOARD_PAD_CONNECT
              : (BoardPadType)-1;
    if ((int)pad->type < 0) return fail(L, BOARD_INVALID_CONTENT, where, "pad type must be smd, thru_hole, np_thru_hole or connect");
    pad->shape = !strcmp(shape, "rect") ? BOARD_SHAPE_RECT : !strcmp(shape, "roundrect") ? BOARD_SHAPE_ROUNDRECT
               : !strcmp(shape, "circle") ? BOARD_SHAPE_CIRCLE : !strcmp(shape, "oval") ? BOARD_SHAPE_OVAL
               : !strcmp(shape, "trapezoid") ? BOARD_SHAPE_TRAPEZOID : !strcmp(shape, "custom") ? BOARD_SHAPE_CUSTOM
               : BOARD_SHAPE_OTHER;
    const IRNode *at = find(p, "at"), *size = find(p, "size"), *drill = find(p, "drill"), *rr = find(p, "roundrect_rratio");
    if (!at || nm_of(arg(at, 1), &pad->local.x) || nm_of(arg(at, 2), &pad->local.y))
        return fail(L, BOARD_INVALID_CONTENT, ctx(L, "%s%s: at", where, NULL), "a pad needs (at X Y [ANGLE])");
    if (angle_of(at, &pad->angle_udeg)) return fail(L, BOARD_INVALID_CONTENT, where, "pad angle");
    if (!size || (s = nm_of(arg(size, 1), &pad->size_x)) || (s = nm_of(arg(size, 2), &pad->size_y)))
        return fail(L, BOARD_INVALID_CONTENT, ctx(L, "%s%s: size", where, NULL), "a pad needs (size W H)");
    if (drill) {
        const IRNode *c = arg(drill, 1);
        if (is_atom(c, "oval")) { pad->drill_oval = true; c = c->next; }
        if (c && c->kind == IR_NODE_ATOM) {
            if (nm_of(c, &pad->drill_x)) return fail(L, BOARD_INVALID_CONTENT, where, "drill size");
            pad->drill_y = pad->drill_x;
            if (c->next && c->next->kind == IR_NODE_ATOM && nm_of(c->next, &pad->drill_y))
                return fail(L, BOARD_INVALID_CONTENT, where, "drill size");
            pad->has_drill = pad->drill_x > 0;
        }
    }
    if (rr) {
        int64_t v;
        if (ir_atom_to_nm(arg(rr, 1), &v) != IR_STATUS_OK) return fail(L, BOARD_INVALID_CONTENT, where, "roundrect_rratio");
        pad->roundrect_rratio_ppm = v;
    }
    pad->layers = layers_of(p);
    int32_t net = net_ref(L, p);
    if (net == -2) return fail(L, BOARD_OUT_OF_MEMORY, NULL, "out of memory");
    if (net == -3) return fail(L, BOARD_INVALID_CONTENT, where, "(net N [\"name\"]) or (net \"name\")");
    pad->net = net;
    pad->exact = true;
    if ((s = place(f, pad->local, &pad->at, &pad->exact))) return fail(L, s, where, "pad position out of range");
    return BOARD_OK;
}

static BoardStatus read_zone(Loader *L, const IRNode *z, bool in_fp)
{
    BoardZone *zone;
    if (!PUSH(L, zones, zone_count, cap_zones, zone)) return fail(L, BOARD_OUT_OF_MEMORY, NULL, "out of memory");
    zone->in_footprint = in_fp;
    zone->layers = layers_of(z);
    zone->keepout = find(z, "keepout") != NULL;
    int32_t net = net_ref(L, z);
    if (net == -2) return fail(L, BOARD_OUT_OF_MEMORY, NULL, "out of memory");
    if (net == -3) return fail(L, BOARD_INVALID_CONTENT, "zone", "(net ...)");
    zone->net = net;
    if (zone->net < 0) {                          /* KiCad also names it: (net_name "GND") */
        const IRNode *nn = find(z, "net_name");
        const char *name = nn ? text_of(arg(nn, 1)) : NULL;
        if (name && *name) zone->net = net_by_name(L, name);
    }
    for (const IRNode *c = arg(z, 1); c; c = c->next) if (is_form(c, "filled_polygon")) zone->filled_polygon_count++;
    const IRNode *poly = find(z, "polygon"), *pts = poly ? find(poly, "pts") : NULL;
    size_t n = 0;
    for (const IRNode *c = pts ? arg(pts, 1) : NULL; c; c = c->next) n++;
    G2Point *v = n ? arena_alloc_array(L->arena, sizeof *v, n) : NULL;
    if (n && !v) return fail(L, BOARD_OUT_OF_MEMORY, NULL, "out of memory");
    bool arc_noted = false;
    for (const IRNode *c = pts ? arg(pts, 1) : NULL; c; c = c->next) {
        if (is_form(c, "xy")) {
            if (nm_of(arg(c, 1), &v[zone->outline_count].x) || nm_of(arg(c, 2), &v[zone->outline_count].y))
                return fail(L, BOARD_INVALID_CONTENT, "zone polygon", "(xy X Y)");
            zone->outline_count++;
        } else if (is_form(c, "arc")) {
            G2Point a, e;
            if (point_of(c, "start", &a) || point_of(c, "end", &e)) return fail(L, BOARD_INVALID_CONTENT, "zone polygon arc", "(arc (start) (mid) (end))");
            v[zone->outline_count++] = a;
            if (!arc_noted && !issue(L, BOARD_ISSUE_ZONE_ARC, "zone outline")) return fail(L, BOARD_OUT_OF_MEMORY, NULL, "out of memory");
            arc_noted = true;
            (void)e;
        }
    }
    zone->outline = v;
    return BOARD_OK;
}

static BoardStatus read_footprint(Loader *L, const IRNode *fpn)
{
    BoardFootprint *fp;
    if (!PUSH(L, footprints, footprint_count, cap_fps, fp)) return fail(L, BOARD_OUT_OF_MEMORY, NULL, "out of memory");
    size_t index = L->b->footprint_count - 1;
    fp->lib_id = text_of(arg(fpn, 1));
    if (!fp->lib_id) fp->lib_id = "";
    fp->reference = fp->value = "";
    for (const IRNode *c = arg(fpn, 1); c; c = c->next) {
        if (is_form(c, "property")) {
            const char *k = text_of(arg(c, 1)), *v = text_of(arg(c, 2));
            if (k && v && !strcmp(k, "Reference")) fp->reference = v;
            else if (k && v && !strcmp(k, "Value")) fp->value = v;
        } else if (is_form(c, "fp_text")) {          /* KiCad 6 and earlier */
            const char *k = text_of(arg(c, 1)), *v = text_of(arg(c, 2));
            if (k && v && !strcmp(k, "reference")) fp->reference = v;
            else if (k && v && !strcmp(k, "value")) fp->value = v;
        }
    }
    const IRNode *at = find(fpn, "at");
    const char *where = ctx(L, "footprint %s%s", fp->reference, NULL);
    if (!at || nm_of(arg(at, 1), &fp->at.x) || nm_of(arg(at, 2), &fp->at.y))
        return fail(L, BOARD_INVALID_CONTENT, where, "a footprint needs (at X Y [ANGLE])");
    if (angle_of(at, &fp->angle_udeg)) return fail(L, BOARD_INVALID_CONTENT, where, "footprint angle");
    const IRNode *layer = find(fpn, "layer");
    fp->back = layer && board_layer_bits(text_of(arg(layer, 1))) == BOARD_LAYER_B_CU;
    Frame f = {fp->at, fp->angle_udeg, false};
    bool exact_rotation = fp->angle_udeg % UDEG_90 == 0;
    if (!exact_rotation && !issue(L, BOARD_ISSUE_INEXACT_ROTATION, where)) return fail(L, BOARD_OUT_OF_MEMORY, NULL, "out of memory");
    fp->first_pad = L->b->pad_count;
    const char *ref = fp->reference;
    for (const IRNode *c = arg(fpn, 1); c; c = c->next) {
        BoardStatus s;
        if (is_form(c, "pad")) {
            if ((s = read_pad(L, c, index, &f, ref))) return s;
        } else if (is_form(c, "zone")) {
            if ((s = read_zone(L, c, true))) return s;
        } else if (c->kind == IR_NODE_LIST && c->first_child && c->first_child->text &&
                   !strncmp(c->first_child->text, "fp_", 3) && on_edge_cuts(c)) {
            if ((s = edge_item(L, c, c->first_child->text + 3, &f, true, ctx(L, "footprint %s: %s ", ref, c->first_child->text)))) return s;
        }
    }
    /* Re-fetch: reading zones or nets may not move footprints, but `fp` is not used past this point anyway. */
    L->b->footprints[index].pad_count = L->b->pad_count - L->b->footprints[index].first_pad;
    return BOARD_OK;
}

static BoardStatus read_track(Loader *L, const IRNode *t, bool arc)
{
    BoardTrack *tr;
    if (!PUSH(L, tracks, track_count, cap_tracks, tr)) return fail(L, BOARD_OUT_OF_MEMORY, NULL, "out of memory");
    tr->is_arc = arc;
    const IRNode *w = find(t, "width");
    BoardStatus s;
    if ((s = point_of(t, "start", &tr->start)) || (s = point_of(t, "end", &tr->end)) ||
        (s = arc ? point_of(t, "mid", &tr->mid) : BOARD_OK) || (s = w ? nm_of(arg(w, 1), &tr->width) : BOARD_INVALID_CONTENT))
        return fail(L, s, arc ? "arc track" : "segment", "a track needs (start) [(mid)] (end) (width), within +-1 m");
    if (!arc) tr->mid = tr->start;
    tr->layer = layers_of(t);
    int32_t net = net_ref(L, t);
    if (net == -2) return fail(L, BOARD_OUT_OF_MEMORY, NULL, "out of memory");
    if (net == -3) return fail(L, BOARD_INVALID_CONTENT, "track", "(net ...)");
    tr->net = net;
    return BOARD_OK;
}

static BoardStatus read_via(Loader *L, const IRNode *v)
{
    BoardVia *via;
    if (!PUSH(L, vias, via_count, cap_vias, via)) return fail(L, BOARD_OUT_OF_MEMORY, NULL, "out of memory");
    const IRNode *size = find(v, "size"), *drill = find(v, "drill");
    BoardStatus s;
    if ((s = point_of(v, "at", &via->at)) || (s = size ? nm_of(arg(size, 1), &via->size) : BOARD_INVALID_CONTENT) ||
        (s = drill ? nm_of(arg(drill, 1), &via->drill) : BOARD_INVALID_CONTENT))
        return fail(L, s, "via", "a via needs (at X Y) (size D) (drill D), within +-1 m");
    via->type = is_atom(arg(v, 1), "blind") ? BOARD_VIA_BLIND : is_atom(arg(v, 1), "micro") ? BOARD_VIA_MICRO : BOARD_VIA_THROUGH;
    via->layers = layers_of(v);
    int32_t net = net_ref(L, v);
    if (net == -2) return fail(L, BOARD_OUT_OF_MEMORY, NULL, "out of memory");
    if (net == -3) return fail(L, BOARD_INVALID_CONTENT, "via", "(net ...)");
    via->net = net;
    return BOARD_OK;
}

/* ------------------------------------------------------------------------------------------------ outline */

static void box_add(BoardOutline *o, G2Point p, G2Nm pad)
{
    if (!o->has_bounds) {
        o->bounds.min.x = p.x - pad; o->bounds.min.y = p.y - pad; o->bounds.max.x = p.x + pad; o->bounds.max.y = p.y + pad;
        o->has_bounds = true;
        return;
    }
    if (p.x - pad < o->bounds.min.x) o->bounds.min.x = p.x - pad;
    if (p.y - pad < o->bounds.min.y) o->bounds.min.y = p.y - pad;
    if (p.x + pad > o->bounds.max.x) o->bounds.max.x = p.x + pad;
    if (p.y + pad > o->bounds.max.y) o->bounds.max.y = p.y + pad;
}

#if BOARD_HAVE_I128
static BoardI128 i128_isqrt(BoardI128 v)   /* floor(sqrt(v)), v >= 0 */
{
    BoardI128 r = 0, bit = (BoardI128)1 << 124;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return r;
}

static BoardI128 cross(BoardI128 ax, BoardI128 ay, BoardI128 bx, BoardI128 by) { return ax * by - ay * bx; }

/* Circle through the arc's three points: centre rounded to the nearest nm and radius rounded up, so the box
 * centre +- (radius + 1) contains the true circle. false when the points are collinear. */
static bool arc_circle(G2Point s, G2Point m, G2Point e, G2Point *c, G2Nm *r)
{
    BoardI128 bx = (BoardI128)m.x - s.x, by = (BoardI128)m.y - s.y, cx = (BoardI128)e.x - s.x, cy = (BoardI128)e.y - s.y;
    BoardI128 d = 2 * (bx * cy - by * cx);
    if (!d) return false;
    BoardI128 b2 = bx * bx + by * by, c2 = cx * cx + cy * cy;
    BoardI128 ux = cy * b2 - by * c2, uy = bx * c2 - cx * b2;
    if (d < 0) { d = -d; ux = -ux; uy = -uy; }
    BoardI128 rx = ux >= 0 ? (2 * ux + d) / (2 * d) : -((-2 * ux + d) / (2 * d));
    BoardI128 ry = uy >= 0 ? (2 * uy + d) / (2 * d) : -((-2 * uy + d) / (2 * d));
    c->x = (G2Nm)(rx + s.x);
    c->y = (G2Nm)(ry + s.y);
    BoardI128 worst = 0;
    const G2Point pts[3] = {s, m, e};
    for (int i = 0; i < 3; i++) {
        BoardI128 dx = (BoardI128)pts[i].x - c->x, dy = (BoardI128)pts[i].y - c->y, q = dx * dx + dy * dy;
        if (q > worst) worst = q;
    }
    BoardI128 root = i128_isqrt(worst);
    if (root * root < worst) root++;
    *r = (G2Nm)root;
    return true;
}

/* Does the arc s -> m -> e (centre c) sweep through direction (dx, dy) from the centre? */
static bool arc_sweeps(G2Point s, G2Point m, G2Point e, G2Point c, int dx, int dy)
{
    BoardI128 turn = cross((BoardI128)m.x - s.x, (BoardI128)m.y - s.y, (BoardI128)e.x - m.x, (BoardI128)e.y - m.y);
    G2Point a = turn > 0 ? s : e, b = turn > 0 ? e : s;     /* travel a -> b with positive turn */
    BoardI128 ax = (BoardI128)a.x - c.x, ay = (BoardI128)a.y - c.y, bxv = (BoardI128)b.x - c.x, byv = (BoardI128)b.y - c.y;
    /* minor arc when the mid point and the centre lie on opposite sides of the chord */
    BoardI128 side_m = cross((BoardI128)e.x - s.x, (BoardI128)e.y - s.y, (BoardI128)m.x - s.x, (BoardI128)m.y - s.y);
    BoardI128 side_c = cross((BoardI128)e.x - s.x, (BoardI128)e.y - s.y, (BoardI128)c.x - s.x, (BoardI128)c.y - s.y);
    bool minor = (side_m > 0) != (side_c > 0) || side_c == 0;
    BoardI128 c1 = cross(ax, ay, dx, dy), c2 = cross(dx, dy, bxv, byv);
    if (minor) return c1 >= 0 && c2 >= 0;
    return !(cross(bxv, byv, dx, dy) > 0 && cross(dx, dy, ax, ay) > 0);
}
#endif

/* End points used by an odd number of edges, paired nearest-first (ties by order): the widest pair is max_gap. */
static BoardStatus outline_gaps(Loader *L)
{
    Board *b = L->b;
    BoardOutline *o = &b->outline;
    size_t n = 0;
    G2Point *ends = arena_alloc_array(L->arena, sizeof *ends, 2 * b->edge_count);
    if (!ends) return fail(L, BOARD_OUT_OF_MEMORY, NULL, "out of memory");
    for (size_t i = 0; i < b->edge_count; i++) {
        const BoardEdge *e = &b->edges[i];
        if (e->kind == BOARD_EDGE_CIRCLE) continue;
        const G2Point pts[2] = {e->start, e->end};
        for (int k = 0; k < 2; k++) {               /* toggle: a point seen twice cancels */
            size_t j = 0;
            while (j < n && !(ends[j].x == pts[k].x && ends[j].y == pts[k].y)) j++;
            if (j < n) ends[j] = ends[--n];
            else ends[n++] = pts[k];
        }
    }
    o->dangling_ends = n;
    o->max_gap = n % 2 ? -1 : 0;
    if (n % 2) return BOARD_OK;
    bool *used = arena_alloc_array(L->arena, sizeof *used, n ? n : 1);
    if (!used) return fail(L, BOARD_OUT_OF_MEMORY, NULL, "out of memory");
    memset(used, 0, n ? n : 1);
    for (size_t pairs = 0; pairs < n / 2; pairs++) {
        uint64_t best = UINT64_MAX;
        size_t bi = 0, bj = 0;
        for (size_t i = 0; i < n; i++) {
            if (used[i]) continue;
            for (size_t j = i + 1; j < n; j++) {
                uint64_t q;
                if (used[j] || g2_distance_squared(ends[i], ends[j], &q) != G2_OK) continue;
                if (q < best) { best = q; bi = i; bj = j; }
            }
        }
        if (best == UINT64_MAX) { o->max_gap = -1; return BOARD_OK; }
        used[bi] = used[bj] = true;
        uint64_t r = nm_isqrt_u64(best);
        if (r * r < best) r++;
        if ((G2Nm)r > o->max_gap) o->max_gap = (G2Nm)r;
    }
    return BOARD_OK;
}

static BoardStatus outline_build(Loader *L)
{
    Board *b = L->b;
    BoardOutline *o = &b->outline;
    memset(o, 0, sizeof *o);
    L->item = 0;
    if (!b->edge_count) {
        if (!issue(L, BOARD_ISSUE_NO_OUTLINE, "Edge.Cuts")) return fail(L, BOARD_OUT_OF_MEMORY, NULL, "out of memory");
        return BOARD_OK;
    }
    bool arc_bounds_noted = false;
    for (size_t i = 0; i < b->edge_count; i++) {
        BoardEdge *e = &b->edges[i];
        if (e->kind == BOARD_EDGE_LINE) { box_add(o, e->start, 0); box_add(o, e->end, 0); continue; }
        if (e->kind == BOARD_EDGE_CIRCLE) {
            uint64_t q;
            if (g2_distance_squared(e->start, e->end, &q) != G2_OK) return fail(L, BOARD_RANGE_ERROR, "Edge.Cuts circle", "radius");
#if BOARD_HAVE_I128
            BoardI128 root = i128_isqrt((BoardI128)q);
            if (root * root < (BoardI128)q) root++;
            box_add(o, e->start, (G2Nm)root + 1);
#else
            box_add(o, e->end, 0);
#endif
            continue;
        }
        box_add(o, e->start, 0); box_add(o, e->mid, 0); box_add(o, e->end, 0);
#if BOARD_HAVE_I128
        G2Point c;
        G2Nm r;
        if (!arc_circle(e->start, e->mid, e->end, &c, &r)) {
            e->kind = BOARD_EDGE_LINE;
            if (!issue(L, BOARD_ISSUE_ARC_DEGENERATE, "Edge.Cuts arc")) return fail(L, BOARD_OUT_OF_MEMORY, NULL, "out of memory");
            continue;
        }
        static const int DIRS[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (int k = 0; k < 4; k++)
            if (arc_sweeps(e->start, e->mid, e->end, c, DIRS[k][0], DIRS[k][1])) {
                G2Point p = {c.x + (G2Nm)DIRS[k][0] * (r + 1), c.y + (G2Nm)DIRS[k][1] * (r + 1)};
                box_add(o, p, 0);
            }
#else
        if (!arc_bounds_noted && !issue(L, BOARD_ISSUE_ARC_BOUNDS_UNAVAILABLE, "Edge.Cuts arc")) return fail(L, BOARD_OUT_OF_MEMORY, NULL, "out of memory");
#endif
        arc_bounds_noted = true;
    }
    (void)arc_bounds_noted;

    /* Chain edges into loops by exact end-point equality (either direction). */
    for (size_t i = 0; i < b->edge_count; i++) {
        BoardEdge *e = &b->edges[i];
        if (e->loop != SIZE_MAX) continue;
        size_t loop = o->loop_count++;
        e->loop = loop;
        if (e->kind == BOARD_EDGE_CIRCLE) continue;
        G2Point head = e->start, tail = e->end;
        bool closed = head.x == tail.x && head.y == tail.y;
        while (!closed) {
            bool extended = false;
            for (size_t j = 0; j < b->edge_count && !extended; j++) {
                BoardEdge *n = &b->edges[j];
                if (n->loop != SIZE_MAX || n->kind == BOARD_EDGE_CIRCLE) continue;
                if (n->start.x == tail.x && n->start.y == tail.y) { tail = n->end; n->loop = loop; extended = true; }
                else if (n->end.x == tail.x && n->end.y == tail.y) { tail = n->start; n->loop = loop; extended = true; }
            }
            if (!extended) break;
            closed = head.x == tail.x && head.y == tail.y;
        }
        if (!closed) {
            /* Try to extend backwards from the head, then judge. */
            bool extended = true;
            while (extended && !closed) {
                extended = false;
                for (size_t j = 0; j < b->edge_count && !extended; j++) {
                    BoardEdge *n = &b->edges[j];
                    if (n->loop != SIZE_MAX || n->kind == BOARD_EDGE_CIRCLE) continue;
                    if (n->end.x == head.x && n->end.y == head.y) { head = n->start; n->loop = loop; extended = true; }
                    else if (n->start.x == head.x && n->start.y == head.y) { head = n->end; n->loop = loop; extended = true; }
                }
                closed = head.x == tail.x && head.y == tail.y;
            }
        }
        if (!closed) o->open_loops++;
    }
    o->closed = o->open_loops == 0;
    if (!o->closed) {
        if (!issue(L, BOARD_ISSUE_OUTLINE_OPEN, "Edge.Cuts")) return fail(L, BOARD_OUT_OF_MEMORY, NULL, "out of memory");
        return outline_gaps(L);
    }
    return BOARD_OK;
}

/* ------------------------------------------------------------------------------------------------ load */

static BoardStatus read_root(Arena *arena, const IRNode *root, Board *out, BoardError *err)
{
    Loader L;
    memset(&L, 0, sizeof L);
    memset(out, 0, sizeof *out);
    L.arena = arena; L.b = out; L.err = err;
    if (!is_form(root, "kicad_pcb")) return fail(&L, BOARD_NOT_A_BOARD, NULL, "not a (kicad_pcb ...) file");
    const IRNode *v = find(root, "version"), *g = find(root, "generator"), *gv = find(root, "generator_version");
    if (v && ir_atom_to_i64(arg(v, 1), &out->version) != IR_STATUS_OK) return fail(&L, BOARD_INVALID_CONTENT, "version", "(version N)");
    out->generator = g ? text_of(arg(g, 1)) : NULL;
    out->generator_version = gv ? text_of(arg(gv, 1)) : NULL;
    const IRNode *general = find(root, "general"), *th = general ? find(general, "thickness") : NULL;
    if (th && nm_of(arg(th, 1), &out->thickness)) return fail(&L, BOARD_INVALID_CONTENT, "general thickness", "(thickness MM)");
    const IRNode *layers = find(root, "layers");
    for (const IRNode *c = layers ? arg(layers, 1) : NULL; c; c = c->next) {
        const char *name = text_of(arg(c, 1)), *kind = text_of(arg(c, 2));
        if (name && kind && (board_layer_bits(name) & BOARD_LAYERS_CU) && board_layer_bits(name) != BOARD_LAYERS_CU) out->copper_layers++;
    }
    BoardStatus s = BOARD_OK;
    for (const IRNode *c = root->first_child->next; c && !s; c = c->next) {
        L.item++;
        if (c->kind != IR_NODE_LIST || !c->first_child || c->first_child->kind != IR_NODE_ATOM) continue;
        const char *h = c->first_child->text;
        if (!strcmp(h, "net")) {
            int64_t number;
            const char *name = text_of(arg(c, 2));
            if (ir_atom_to_i64(arg(c, 1), &number) != IR_STATUS_OK || number < 0 || number > INT32_MAX || !name)
                return fail(&L, BOARD_INVALID_CONTENT, "net", "(net N \"name\")");
            if (add_net(&L, (int32_t)number, name) == -2) return fail(&L, BOARD_OUT_OF_MEMORY, NULL, "out of memory");
        } else if (!strcmp(h, "footprint") || !strcmp(h, "module")) {
            s = read_footprint(&L, c);
        } else if (!strcmp(h, "segment")) {
            s = read_track(&L, c, false);
        } else if (!strcmp(h, "arc")) {
            s = read_track(&L, c, true);
        } else if (!strcmp(h, "via")) {
            s = read_via(&L, c);
        } else if (!strcmp(h, "zone")) {
            s = read_zone(&L, c, false);
        } else if (!strncmp(h, "gr_", 3) && on_edge_cuts(c)) {
            Frame id = {{0, 0}, 0, true};
            s = edge_item(&L, c, h + 3, &id, false, "");
        }
    }
    if (s) return s;
    if ((s = outline_build(&L))) return s;
    if (err) { memset(err, 0, sizeof *err); err->code = BOARD_OK; }
    return BOARD_OK;
}

static BoardStatus ir_failure(const IRError *ie, BoardError *err)
{
    BoardStatus s = ie->code == IR_STATUS_IO_ERROR ? BOARD_IO_ERROR
                  : ie->code == IR_STATUS_OUT_OF_MEMORY ? BOARD_OUT_OF_MEMORY : BOARD_PARSE_ERROR;
    if (err) {
        err->code = s; err->line = ie->line; err->column = ie->column; err->offset = ie->offset;
        err->item = 0; err->context = NULL; err->message = ie->message;
    }
    return s;
}

BoardStatus board_parse(Arena *arena, const char *text, size_t len, Board *out, BoardError *error)
{
    IRError ie;
    if (!arena || !text || !out) {
        if (error) { memset(error, 0, sizeof *error); error->code = BOARD_INVALID_ARGUMENT; error->message = "NULL argument"; }
        return BOARD_INVALID_ARGUMENT;
    }
    ir_error_clear(&ie);
    IRNode *root = ir_parse(arena, text, len, &ie);
    return root ? read_root(arena, root, out, error) : ir_failure(&ie, error);
}

BoardStatus board_load(Arena *arena, const char *path, Board *out, BoardError *error)
{
    IRError ie;
    if (!arena || !path || !out) {
        if (error) { memset(error, 0, sizeof *error); error->code = BOARD_INVALID_ARGUMENT; error->message = "NULL argument"; }
        return BOARD_INVALID_ARGUMENT;
    }
    ir_error_clear(&ie);
    IRNode *root = ir_parse_file(arena, path, &ie);
    return root ? read_root(arena, root, out, error) : ir_failure(&ie, error);
}

/* ------------------------------------------------------------------------------------------------ lookups */

const BoardFootprint *board_find_footprint(const Board *b, const char *reference)
{
    if (!b || !reference) return NULL;
    for (size_t i = 0; i < b->footprint_count; i++)
        if (!strcmp(b->footprints[i].reference, reference)) return &b->footprints[i];
    return NULL;
}

const BoardPad *board_find_pad(const Board *b, const char *reference, const char *number)
{
    const BoardFootprint *f = board_find_footprint(b, reference);
    if (!f || !number) return NULL;
    for (size_t i = 0; i < f->pad_count; i++)
        if (!strcmp(b->pads[f->first_pad + i].number, number)) return &b->pads[f->first_pad + i];
    return NULL;
}

int32_t board_find_net(const Board *b, const char *name)
{
    if (!b || !name) return -1;
    for (size_t i = 0; i < b->net_count; i++)
        if (!strcmp(b->nets[i].name, name)) return (int32_t)i;
    return -1;
}

size_t board_pads_on_net(const Board *b, int32_t net, size_t *out, size_t capacity)
{
    size_t n = 0;
    if (!b || net < 0) return 0;
    for (size_t i = 0; i < b->pad_count; i++)
        if (b->pads[i].net == net) {
            if (out && n < capacity) out[n] = i;
            n++;
        }
    return n;
}

bool board_outline_closed_within(const Board *b, G2Nm tolerance)
{
    if (!b || !b->edge_count) return false;
    if (b->outline.closed) return true;
    return b->outline.max_gap >= 0 && b->outline.max_gap <= tolerance;
}

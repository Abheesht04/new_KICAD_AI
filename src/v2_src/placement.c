/* placement.c - deterministic integer-nanometre placement (see placement.h). No floating point, no libm, no
 * printf or exit: every result is a PlStatus, every length an int64 nm. */
#include "placement.h"

#include "IR.h"
#include "nmath.h"

#include <string.h>

#define PL_MARGIN 2          /* nm added to every "too close" test: the Python bridge rounds each float coordinate
                                to the nearest nm (error <= 0.5 nm per side), so C is never less strict than the
                                float rules it replaces (tools/layout.py Board.legal, whose tolerance is 1 nm) */
#define PL_PPM INT64_C(1000000)
#define PL_WEIGHT_MAX_PPM INT64_C(1000000000)   /* a weight above 1000 is refused (keeps cost terms in int64) */
#define PL_SHIFT_SPAN 8      /* grid steps of the widest annealing shift, shrinking linearly to 1 */
#define PL_REFINE_REACH 4    /* refinement tries +-4 half-steps around each part */
#define PL_REFINE_PASSES 4

static const char *const STATUS_NAMES[] = {
    "OK", "INVALID_INPUT", "PARSE_ERROR", "OUT_OF_MEMORY", "RANGE_ERROR",
    "NO_LEGAL_POSITION", "ILLEGAL_START", "IO_ERROR"};

const char *pl_status_name(PlStatus status)
{
    if ((unsigned)status < sizeof STATUS_NAMES / sizeof STATUS_NAMES[0]) return STATUS_NAMES[status];
    return "UNKNOWN_STATUS";
}

static PlStatus fail(PlError *e, PlStatus s, size_t item, const char *message)
{
    if (e) { e->code = s; e->line = 0; e->offset = 0; e->item = item; e->message = message; }
    return s;
}

/* ------------------------------------------------------------------------------------------------ parsing */

static bool is_atom(const IRNode *n, const char *text)
{
    return n && n->kind == IR_NODE_ATOM && n->text && strcmp(n->text, text) == 0;
}

static bool is_form(const IRNode *n, const char *head)
{
    return n && n->kind == IR_NODE_LIST && is_atom(n->first_child, head);
}

static size_t form_len(const IRNode *n) { return ir_list_length(n); }

static PlStatus nm_at(const IRNode *form, size_t index, G2Nm *out)
{
    const IRNode *a = ir_child_at(form, index);
    int64_t v;
    if (!a) return PL_PARSE_ERROR;
    IRStatus st = ir_atom_to_nm(a, &v);
    if (st == IR_STATUS_RANGE_ERROR) return PL_RANGE_ERROR;
    if (st != IR_STATUS_OK) return PL_PARSE_ERROR;
    if (v > G2_COORD_MAX_NM || v < -G2_COORD_MAX_NM) return PL_RANGE_ERROR;
    *out = v;
    return PL_OK;
}

static PlStatus int_at(const IRNode *form, size_t index, int64_t lo, int64_t hi, int64_t *out)
{
    const IRNode *a = ir_child_at(form, index);
    if (!a) return PL_PARSE_ERROR;
    if (ir_atom_to_i64(a, out) != IR_STATUS_OK) return PL_PARSE_ERROR;
    return (*out < lo || *out > hi) ? PL_RANGE_ERROR : PL_OK;
}

static const char *name_at(const IRNode *form, size_t index)
{
    const IRNode *a = ir_child_at(form, index);
    if (!a || (a->kind != IR_NODE_STRING && a->kind != IR_NODE_ATOM) || !a->text || !a->text[0]) return NULL;
    return a->text;
}

static PlStatus rect_at(const IRNode *form, size_t first, PlRect *r)
{
    PlStatus s;
    if ((s = nm_at(form, first, &r->x1)) || (s = nm_at(form, first + 1, &r->y1)) ||
        (s = nm_at(form, first + 2, &r->x2)) || (s = nm_at(form, first + 3, &r->y2)))
        return s;
    return (r->x1 > r->x2 || r->y1 > r->y2) ? PL_INVALID_INPUT : PL_OK;
}

static PlStatus keepout_from(const IRNode *form, PlKeepout *k)
{
    int64_t flags;
    PlStatus s;
    if (form_len(form) != 6) return PL_PARSE_ERROR;
    if ((s = rect_at(form, 1, &k->r))) return s;
    if ((s = int_at(form, 5, 0, 0xFFFF, &flags))) return s;
    k->flags = (uint32_t)flags;
    return PL_OK;
}

static void grow(PlRect *a, PlRect b)
{
    if (b.x1 < a->x1) a->x1 = b.x1;
    if (b.y1 < a->y1) a->y1 = b.y1;
    if (b.x2 > a->x2) a->x2 = b.x2;
    if (b.y2 > a->y2) a->y2 = b.y2;
}

static PlStatus shape_from(Arena *arena, const IRNode *form, size_t net_count, PlShape *sh)
{
    size_t pads = 0, keeps = 0, bodies = 0;
    PlStatus s;
    memset(sh, 0, sizeof *sh);
    for (const IRNode *c = form->first_child ? form->first_child->next : NULL; c; c = c->next) {
        if (c == ir_child_at(form, 1)) continue;   /* the rotation */
        if (is_form(c, "pad")) pads++;
        else if (is_form(c, "keepout")) keeps++;
        else if (is_form(c, "body")) bodies++;
        else if (!is_form(c, "silk")) return PL_PARSE_ERROR;
    }
    if (bodies != 1) return PL_PARSE_ERROR;
    if (pads) {
        sh->pads = arena_alloc_array(arena, sizeof *sh->pads, pads);
        if (!sh->pads) return PL_OUT_OF_MEMORY;
    }
    if (keeps) {
        sh->keepouts = arena_alloc_array(arena, sizeof *sh->keepouts, keeps);
        if (!sh->keepouts) return PL_OUT_OF_MEMORY;
    }
    for (const IRNode *c = ir_child_at(form, 2); c; c = c->next) {
        if (is_form(c, "body")) {
            if (form_len(c) != 5) return PL_PARSE_ERROR;
            if ((s = rect_at(c, 1, &sh->body))) return s;
        } else if (is_form(c, "silk")) {
            if (form_len(c) != 5 || sh->has_silk) return PL_PARSE_ERROR;
            if ((s = rect_at(c, 1, &sh->silk))) return s;
            sh->has_silk = true;
        } else if (is_form(c, "pad")) {
            PlPad *p = &sh->pads[sh->pad_count++];
            int64_t net;
            if (form_len(c) != 8) return PL_PARSE_ERROR;
            if ((s = int_at(c, 1, -1, (int64_t)net_count - 1, &net))) return s;
            if ((s = nm_at(c, 2, &p->cx)) || (s = nm_at(c, 3, &p->cy)) || (s = rect_at(c, 4, &p->r))) return s;
            p->net = (int32_t)net;
            if (sh->has_env) grow(&sh->env, p->r);
            else { sh->env = p->r; sh->has_env = true; }
        } else if (is_form(c, "keepout")) {
            if ((s = keepout_from(c, &sh->keepouts[sh->keepout_count++]))) return s;
        }
    }
    sh->extent = sh->body;
    if (sh->has_silk) grow(&sh->extent, sh->silk);
    if (sh->has_env) grow(&sh->extent, sh->env);
    for (size_t k = 0; k < sh->keepout_count; k++) grow(&sh->extent, sh->keepouts[k].r);
    return PL_OK;
}

static int rot_index(int64_t degrees)
{
    switch (degrees) {
    case 0: return 0;
    case 90: return 1;
    case 180: return 2;
    case 270: return 3;
    default: return -1;
    }
}

static PlStatus part_from(Arena *arena, const IRNode *form, size_t net_count, PlPart *part)
{
    bool have[4] = {false, false, false, false};
    PlStatus s;
    memset(part, 0, sizeof *part);
    part->ref = name_at(form, 1);
    if (!part->ref) return PL_PARSE_ERROR;
    for (const IRNode *c = ir_child_at(form, 2); c; c = c->next) {
        if (is_form(c, "fixed")) {
            if (form_len(c) != 1) return PL_PARSE_ERROR;
            part->fixed = true;
        } else if (is_form(c, "at")) {
            int64_t deg;
            int r;
            if (form_len(c) != 4 || part->has_given) return PL_PARSE_ERROR;
            if ((s = nm_at(c, 1, &part->gx)) || (s = nm_at(c, 2, &part->gy))) return s;
            if ((s = int_at(c, 3, 0, 359, &deg))) return s;
            if ((r = rot_index(deg)) < 0) return PL_INVALID_INPUT;
            part->grot = (uint8_t)r;
            part->has_given = true;
        } else if (is_form(c, "shape")) {
            int64_t deg;
            int r;
            if ((s = int_at(c, 1, 0, 359, &deg))) return s;
            if ((r = rot_index(deg)) < 0 || have[r]) return PL_INVALID_INPUT;
            if ((s = shape_from(arena, c, net_count, &part->shape[r]))) return s;
            have[r] = true;
        } else {
            return PL_PARSE_ERROR;
        }
    }
    if (!have[0] || !have[1] || !have[2] || !have[3]) return PL_INVALID_INPUT;
    if (part->fixed && !part->has_given) return PL_INVALID_INPUT;
    /* Every rotation is the same footprint: the same pads, on the same nets, in the same order. */
    for (int r = 1; r < 4; r++) {
        if (part->shape[r].pad_count != part->shape[0].pad_count) return PL_INVALID_INPUT;
        for (size_t p = 0; p < part->shape[0].pad_count; p++)
            if (part->shape[r].pads[p].net != part->shape[0].pads[p].net) return PL_INVALID_INPUT;
    }
    /* Distinct nets, ascending (insertion into a small sorted array). */
    if (part->shape[0].pad_count) {
        part->nets = arena_alloc_array(arena, sizeof *part->nets, part->shape[0].pad_count);
        if (!part->nets) return PL_OUT_OF_MEMORY;
    }
    for (size_t p = 0; p < part->shape[0].pad_count; p++) {
        int32_t n = part->shape[0].pads[p].net;
        size_t at = 0;
        if (n < 0) continue;
        while (at < part->net_count && part->nets[at] < n) at++;
        if (at < part->net_count && part->nets[at] == n) continue;
        memmove(&part->nets[at + 1], &part->nets[at], (part->net_count - at) * sizeof *part->nets);
        part->nets[at] = n;
        part->net_count++;
    }
    return PL_OK;
}

static PlStatus weights_from(const IRNode *form, PlWeights *w)
{
    for (const IRNode *c = ir_child_at(form, 1); c; c = c->next) {
        int64_t *slot;
        G2Nm v;
        PlStatus s;
        if (is_form(c, "placement_area_mm2")) slot = &w->area_ppm;
        else if (is_form(c, "decoupling_mm")) slot = &w->decoupling_ppm;
        else if (is_form(c, "keepout_edge_mm")) slot = &w->keepout_edge_ppm;
        else return PL_PARSE_ERROR;
        if (form_len(c) != 2) return PL_PARSE_ERROR;
        if ((s = nm_at(c, 1, &v))) return s;   /* a decimal with 6 places, read exactly: the value x 1e6 */
        if (v < 0 || v > PL_WEIGHT_MAX_PPM) return PL_RANGE_ERROR;
        *slot = v;
    }
    return PL_OK;
}

static size_t part_by_ref(const PlProblem *p, const char *ref)
{
    for (size_t i = 0; i < p->part_count; i++)
        if (strcmp(p->parts[i].ref, ref) == 0) return i;
    return SIZE_MAX;
}

static PlStatus parse_root(Arena *arena, const IRNode *root, PlProblem *out, PlError *e)
{
    size_t nets = 0, keeps = 0, parts = 0, decs = 0, item = 0;
    bool version = false, board = false, edge = false, gap = false, step = false;
    PlStatus s;
    memset(out, 0, sizeof *out);
    if (!is_form(root, "placement_problem")) return fail(e, PL_PARSE_ERROR, 0, "not a (placement_problem ...)");
    for (const IRNode *c = root->first_child->next; c; c = c->next) {
        item++;
        if (is_form(c, "nets")) nets += form_len(c) - 1;
        else if (is_form(c, "keepout")) keeps++;
        else if (is_form(c, "part")) parts++;
        else if (is_form(c, "decoupling")) decs++;
    }
    out->net_count = nets;
    out->net_names = nets ? arena_alloc_array(arena, sizeof *out->net_names, nets) : NULL;
    out->keepouts = keeps ? arena_alloc_array(arena, sizeof *out->keepouts, keeps) : NULL;
    out->parts = parts ? arena_alloc_array(arena, sizeof *out->parts, parts) : NULL;
    out->decoupling = decs ? arena_alloc_array(arena, sizeof *out->decoupling, decs) : NULL;
    if ((nets && !out->net_names) || (keeps && !out->keepouts) || (parts && !out->parts) || (decs && !out->decoupling))
        return fail(e, PL_OUT_OF_MEMORY, 0, "out of memory");

    item = 0;
    size_t named = 0;
    for (const IRNode *c = root->first_child->next; c; c = c->next) {
        item++;
        if (is_form(c, "version")) {
            int64_t v;
            if (form_len(c) != 2 || int_at(c, 1, 1, 1, &v)) return fail(e, PL_PARSE_ERROR, item, "only (version 1) is supported");
            version = true;
        } else if (is_form(c, "board")) {
            if (form_len(c) != 3) return fail(e, PL_PARSE_ERROR, item, "(board W H) expects two lengths");
            if ((s = nm_at(c, 1, &out->width)) || (s = nm_at(c, 2, &out->height)))
                return fail(e, s, item, "(board W H): two lengths of at most 1 m");
            if (out->width <= 0 || out->height <= 0) return fail(e, PL_INVALID_INPUT, item, "board size must be positive");
            board = true;
        } else if (is_form(c, "edge") || is_form(c, "gap") || is_form(c, "step")) {
            G2Nm v;
            bool is_step = is_form(c, "step");
            if (form_len(c) != 2) return fail(e, PL_PARSE_ERROR, item, "expects one length");
            if ((s = nm_at(c, 1, &v))) return fail(e, s, item, "expects one length of at most 1 m");
            if (v < 0 || (is_step && v == 0)) return fail(e, PL_INVALID_INPUT, item, "edge and gap must be >= 0, step > 0");
            if (is_form(c, "edge")) { out->edge = v; edge = true; }
            else if (is_form(c, "gap")) { out->gap = v; gap = true; }
            else { out->step = v; step = true; }
        } else if (is_form(c, "weights")) {
            if ((s = weights_from(c, &out->weights))) return fail(e, s, item, "weights: (NAME VALUE), 0 <= VALUE <= 1000");
        } else if (is_form(c, "nets")) {
            for (size_t i = 1; i < form_len(c); i++) {
                const char *name = name_at(c, i);
                if (!name) return fail(e, PL_PARSE_ERROR, item, "a net name must be a non-empty string");
                out->net_names[named++] = name;
            }
        } else if (is_form(c, "keepout")) {
            if ((s = keepout_from(c, &out->keepouts[out->keepout_count++])))
                return fail(e, s, item, "(keepout X1 Y1 X2 Y2 FLAGS) with X1 <= X2, Y1 <= Y2");
        } else if (is_form(c, "part")) {
            PlPart *part = &out->parts[out->part_count];
            if ((s = part_from(arena, c, nets, part)))
                return fail(e, s, item, s == PL_OUT_OF_MEMORY ? "out of memory"
                    : "(part REF [(fixed)] [(at X Y ROT)] (shape ROT (body ...) [(silk ...)] (pad NET CX CY X1 Y1 X2 Y2)... "
                      "(keepout ...)...) x 4): one shape per rotation 0/90/180/270, the same pads in each; a fixed part needs (at)");
            if (part_by_ref(out, part->ref) != SIZE_MAX) return fail(e, PL_INVALID_INPUT, item, "duplicate part reference");
            out->part_count++;
        } else if (!is_form(c, "decoupling")) {
            return fail(e, PL_PARSE_ERROR, item, "unknown form in a placement problem");
        }
    }
    if (!version || !board || !edge || !gap || !step)
        return fail(e, PL_PARSE_ERROR, 0, "a problem needs (version 1), (board W H), (edge E), (gap G) and (step S)");

    item = 0;
    for (const IRNode *c = root->first_child->next; c; c = c->next) {
        item++;
        if (!is_form(c, "decoupling")) continue;
        PlDecoupling *d = &out->decoupling[out->decoupling_count++];
        const char *cap = name_at(c, 1), *mcu = name_at(c, 3);
        int64_t net;
        if (form_len(c) != 4 || !cap || !mcu || int_at(c, 2, 0, (int64_t)nets - 1, &net))
            return fail(e, PL_PARSE_ERROR, item, "(decoupling CAP NET MCU) with NET an index into (nets ...)");
        d->cap = part_by_ref(out, cap);
        d->mcu = part_by_ref(out, mcu);
        d->net = (int32_t)net;
        if (d->cap == SIZE_MAX || d->mcu == SIZE_MAX) return fail(e, PL_INVALID_INPUT, item, "decoupling names an unknown part");
    }

    /* Per net, the parts with a pad on it (ascending). */
    if (nets) {
        out->net_parts = arena_alloc_array(arena, sizeof *out->net_parts, nets);
        out->net_part_count = arena_alloc_array(arena, sizeof *out->net_part_count, nets);
        if (!out->net_parts || !out->net_part_count) return fail(e, PL_OUT_OF_MEMORY, 0, "out of memory");
        memset(out->net_part_count, 0, nets * sizeof *out->net_part_count);
        for (size_t i = 0; i < out->part_count; i++)
            for (size_t k = 0; k < out->parts[i].net_count; k++) out->net_part_count[out->parts[i].nets[k]]++;
        for (size_t n = 0; n < nets; n++) {
            out->net_parts[n] = out->net_part_count[n] ? arena_alloc_array(arena, sizeof **out->net_parts, out->net_part_count[n]) : NULL;
            if (out->net_part_count[n] && !out->net_parts[n]) return fail(e, PL_OUT_OF_MEMORY, 0, "out of memory");
            out->net_part_count[n] = 0;
        }
        for (size_t i = 0; i < out->part_count; i++)
            for (size_t k = 0; k < out->parts[i].net_count; k++) {
                int32_t n = out->parts[i].nets[k];
                out->net_parts[n][out->net_part_count[n]++] = i;
            }
    }
    if (e) { e->code = PL_OK; e->line = 0; e->offset = 0; e->item = 0; e->message = NULL; }
    return PL_OK;
}

static PlStatus from_ir_error(const IRError *ie, PlError *e)
{
    PlStatus s = ie->code == IR_STATUS_IO_ERROR ? PL_IO_ERROR
               : ie->code == IR_STATUS_OUT_OF_MEMORY ? PL_OUT_OF_MEMORY : PL_PARSE_ERROR;
    if (e) { e->code = s; e->line = ie->line; e->offset = ie->offset; e->item = 0; e->message = ie->message; }
    return s;
}

PlStatus pl_problem_parse(Arena *arena, const char *text, size_t len, PlProblem *out, PlError *error)
{
    IRError ie;
    if (!arena || !text || !out) return fail(error, PL_INVALID_INPUT, 0, "NULL argument");
    ir_error_clear(&ie);
    IRNode *root = ir_parse(arena, text, len, &ie);
    if (!root) return from_ir_error(&ie, error);
    return parse_root(arena, root, out, error);
}

PlStatus pl_problem_load(Arena *arena, const char *path, PlProblem *out, PlError *error)
{
    IRError ie;
    if (!arena || !path || !out) return fail(error, PL_INVALID_INPUT, 0, "NULL argument");
    ir_error_clear(&ie);
    IRNode *root = ir_parse_file(arena, path, &ie);
    if (!root) return from_ir_error(&ie, error);
    return parse_root(arena, root, out, error);
}

/* ------------------------------------------------------------------------------------------------ placements */

PlStatus pl_placement_alloc(Arena *arena, const PlProblem *problem, PlPlacement *out)
{
    size_t n = problem->part_count ? problem->part_count : 1;
    out->x = arena_alloc_array(arena, sizeof *out->x, n);
    out->y = arena_alloc_array(arena, sizeof *out->y, n);
    out->rot = arena_alloc_array(arena, sizeof *out->rot, n);
    out->placed = arena_alloc_array(arena, sizeof *out->placed, n);
    if (!out->x || !out->y || !out->rot || !out->placed) return PL_OUT_OF_MEMORY;
    memset(out->x, 0, n * sizeof *out->x);
    memset(out->y, 0, n * sizeof *out->y);
    memset(out->rot, 0, n * sizeof *out->rot);
    memset(out->placed, 0, n * sizeof *out->placed);
    return PL_OK;
}

void pl_placement_copy(const PlProblem *problem, PlPlacement *dst, const PlPlacement *src)
{
    size_t n = problem->part_count;
    memcpy(dst->x, src->x, n * sizeof *dst->x);
    memcpy(dst->y, src->y, n * sizeof *dst->y);
    memcpy(dst->rot, src->rot, n * sizeof *dst->rot);
    memcpy(dst->placed, src->placed, n * sizeof *dst->placed);
}

/* ------------------------------------------------------------------------------------------------ legality */

/* Rectangles a (at offset ax, ay) and b (at bx, by) closer than gap; touching counts (tools/layout.py overlap). */
static bool ov(PlRect a, G2Nm ax, G2Nm ay, PlRect b, G2Nm bx, G2Nm by, G2Nm gap)
{
    return a.x1 + ax < b.x2 + bx + gap && b.x1 + bx < a.x2 + ax + gap &&
           a.y1 + ay < b.y2 + by + gap && b.y1 + by < a.y2 + ay + gap;
}

static bool pads_touch(const PlShape *s, G2Nm sx, G2Nm sy, PlRect area, G2Nm ax, G2Nm ay, G2Nm gap)
{
    if (!s->has_env || !ov(s->env, sx, sy, area, ax, ay, gap)) return false;
    for (size_t p = 0; p < s->pad_count; p++)
        if (ov(s->pads[p].r, sx, sy, area, ax, ay, gap)) return true;
    return false;
}

/* Keepout k of a part at (kx, ky) against shape s at (sx, sy). */
static bool keepout_hit(const PlKeepout *k, G2Nm kx, G2Nm ky, const PlShape *s, G2Nm sx, G2Nm sy)
{
    if ((k->flags & PL_KEEP_BODIES) && ov(s->body, sx, sy, k->r, kx, ky, PL_MARGIN)) return true;
    return (k->flags & PL_KEEP_PADS) && pads_touch(s, sx, sy, k->r, kx, ky, PL_MARGIN);
}

static G2Nm min4(G2Nm a, G2Nm b, G2Nm c, G2Nm d)
{
    G2Nm m = a < b ? a : b;
    if (c < m) m = c;
    return d < m ? d : m;
}

bool pl_part_legal(const PlProblem *P, const PlPlacement *pl, size_t i)
{
    const PlShape *s = &P->parts[i].shape[pl->rot[i]];
    G2Nm x = pl->x[i], y = pl->y[i];
    if (s->body.x1 + x < 0 || s->body.y1 + y < 0 || s->body.x2 + x > P->width || s->body.y2 + y > P->height)
        return false;
    if (s->has_env && min4(s->env.x1 + x, s->env.y1 + y, P->width - (s->env.x2 + x), P->height - (s->env.y2 + y))
                          < P->edge + PL_MARGIN)
        return false;
    for (size_t k = 0; k < P->keepout_count; k++)
        if (keepout_hit(&P->keepouts[k], 0, 0, s, x, y)) return false;
    for (size_t j = 0; j < P->part_count; j++) {
        if (j == i || !pl->placed[j]) continue;
        const PlShape *t = &P->parts[j].shape[pl->rot[j]];
        G2Nm u = pl->x[j], v = pl->y[j];
        if (!ov(s->extent, x, y, t->extent, u, v, P->gap + PL_MARGIN)) continue;   /* nothing can touch */
        if (ov(s->body, x, y, t->body, u, v, P->gap + PL_MARGIN)) return false;
        if (s->has_silk && ((t->has_silk && ov(s->silk, x, y, t->silk, u, v, PL_MARGIN)) ||
                            pads_touch(t, u, v, s->silk, x, y, PL_MARGIN)))
            return false;
        if (t->has_silk && pads_touch(s, x, y, t->silk, u, v, PL_MARGIN)) return false;
        for (size_t k = 0; k < t->keepout_count; k++)
            if (keepout_hit(&t->keepouts[k], u, v, s, x, y)) return false;
        for (size_t k = 0; k < s->keepout_count; k++)
            if (keepout_hit(&s->keepouts[k], x, y, t, u, v)) return false;
    }
    return true;
}

/* ------------------------------------------------------------------------------------------------ objective */

static int64_t scaled(int64_t v, int64_t ppm)
{
    return (v / PL_PPM) * ppm + (v % PL_PPM) * ppm / PL_PPM;   /* floor(v * ppm / 1e6) without overflow */
}

/* Half-perimeter of the pad centres on net n, over placed parts (0 with fewer than two pads). */
static int64_t net_hpwl(const PlProblem *P, const PlPlacement *pl, int32_t n)
{
    G2Nm x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    size_t pins = 0;
    for (size_t k = 0; k < P->net_part_count[n]; k++) {
        size_t i = P->net_parts[n][k];
        if (!pl->placed[i]) continue;
        const PlShape *s = &P->parts[i].shape[pl->rot[i]];
        for (size_t p = 0; p < s->pad_count; p++) {
            if (s->pads[p].net != n) continue;
            G2Nm cx = s->pads[p].cx + pl->x[i], cy = s->pads[p].cy + pl->y[i];
            if (!pins++) { x1 = x2 = cx; y1 = y2 = cy; continue; }
            if (cx < x1) x1 = cx;
            if (cx > x2) x2 = cx;
            if (cy < y1) y1 = cy;
            if (cy > y2) y2 = cy;
        }
    }
    return pins > 1 ? (x2 - x1) + (y2 - y1) : 0;
}

static int64_t keepout_edge(const PlProblem *P, const PlPlacement *pl, size_t i)
{
    const PlShape *s = &P->parts[i].shape[pl->rot[i]];
    int64_t sum = 0;
    for (size_t k = 0; k < s->keepout_count; k++) {
        PlRect r = s->keepouts[k].r;
        G2Nm d = min4(r.x1 + pl->x[i], r.y1 + pl->y[i], P->width - (r.x2 + pl->x[i]), P->height - (r.y2 + pl->y[i]));
        if (d > 0) sum += d;
    }
    return sum;
}

/* Everything but wirelength: occupied area, decoupling distance and keepout-to-edge distance, weighted. */
static int64_t placement_terms(const PlProblem *P, const PlPlacement *pl)
{
    G2Nm bx1 = 0, by1 = 0, bx2 = 0, by2 = 0;
    bool any = false;
    int64_t edge = 0, dec = 0;
    for (size_t i = 0; i < P->part_count; i++) {
        if (!pl->placed[i]) continue;
        const PlShape *s = &P->parts[i].shape[pl->rot[i]];
        PlRect b = {s->body.x1 + pl->x[i], s->body.y1 + pl->y[i], s->body.x2 + pl->x[i], s->body.y2 + pl->y[i]};
        if (!any) { bx1 = b.x1; by1 = b.y1; bx2 = b.x2; by2 = b.y2; any = true; }
        else {
            if (b.x1 < bx1) bx1 = b.x1;
            if (b.y1 < by1) by1 = b.y1;
            if (b.x2 > bx2) bx2 = b.x2;
            if (b.y2 > by2) by2 = b.y2;
        }
        edge += keepout_edge(P, pl, i);
    }
    for (size_t d = 0; d < P->decoupling_count; d++) {
        const PlDecoupling *c = &P->decoupling[d];
        if (!pl->placed[c->cap] || !pl->placed[c->mcu]) continue;
        const PlShape *a = &P->parts[c->cap].shape[pl->rot[c->cap]], *b = &P->parts[c->mcu].shape[pl->rot[c->mcu]];
        bool found = false;
        uint64_t best = 0;
        for (size_t p = 0; p < a->pad_count; p++) {
            if (a->pads[p].net != c->net) continue;
            for (size_t q = 0; q < b->pad_count; q++) {
                if (b->pads[q].net != c->net) continue;
                int64_t dx = (a->pads[p].cx + pl->x[c->cap]) - (b->pads[q].cx + pl->x[c->mcu]);
                int64_t dy = (a->pads[p].cy + pl->y[c->cap]) - (b->pads[q].cy + pl->y[c->mcu]);
                uint64_t adx = (uint64_t)(dx < 0 ? -dx : dx), ady = (uint64_t)(dy < 0 ? -dy : dy);
                uint64_t dist = nm_isqrt_u64(adx * adx + ady * ady);   /* each |d| <= 2e9: the sum < 2^64 */
                if (!found || dist < best) { best = dist; found = true; }
            }
        }
        if (found) dec += (int64_t)best;
    }
    int64_t area_um2 = any ? ((bx2 - bx1) * (by2 - by1)) / PL_PPM : 0;   /* nm^2 -> um^2 (|side| <= 1 m) */
    /* area: um^2 x (ppm of mm per mm^2) / 1e6 = nm of cost; lengths: nm x ppm / 1e6. */
    return scaled(area_um2, P->weights.area_ppm) + scaled(dec, P->weights.decoupling_ppm) +
           scaled(edge, P->weights.keepout_edge_ppm);
}

int64_t pl_cost(const PlProblem *P, const PlPlacement *pl)
{
    int64_t total = placement_terms(P, pl);
    for (size_t n = 0; n < P->net_count; n++) total += net_hpwl(P, pl, (int32_t)n);
    return total;
}

/* ------------------------------------------------------------------------------------------------ constructive */

static size_t shared_nets(const PlPart *a, const PlPart *b)
{
    size_t i = 0, j = 0, n = 0;
    while (i < a->net_count && j < b->net_count) {
        if (a->nets[i] == b->nets[j]) { n++; i++; j++; }
        else if (a->nets[i] < b->nets[j]) i++;
        else j++;
    }
    return n;
}

static G2Nm iabs(G2Nm v) { return v < 0 ? -v : v; }

PlStatus pl_constructive(Arena *arena, const PlProblem *P, PlPlacement *out)
{
    (void)arena;
    bool any_failed = false, anything = false;
    if (!P || !out) return PL_INVALID_INPUT;
    for (size_t i = 0; i < P->part_count; i++) {
        out->placed[i] = false;
        if (P->parts[i].fixed) {
            out->x[i] = P->parts[i].gx; out->y[i] = P->parts[i].gy; out->rot[i] = P->parts[i].grot;
            out->placed[i] = true;
            anything = true;
        }
    }
    bool *pending = NULL;
    size_t left = 0;
    for (size_t i = 0; i < P->part_count; i++) left += !P->parts[i].fixed;
    if (left) {
        pending = arena_alloc_array(arena, sizeof *pending, P->part_count);
        if (!pending) return PL_OUT_OF_MEMORY;
        for (size_t i = 0; i < P->part_count; i++) pending[i] = !P->parts[i].fixed;
    }
    for (; left; left--) {
        /* Priority: parts with keepouts first, then most nets shared with what is placed, then most nets, then ref. */
        size_t pick = SIZE_MAX, pick_shared = 0;
        for (size_t i = 0; i < P->part_count; i++) {
            if (!pending[i]) continue;
            size_t sh = 0;
            for (size_t j = 0; j < P->part_count; j++)
                if (out->placed[j]) sh += shared_nets(&P->parts[i], &P->parts[j]);
            if (pick == SIZE_MAX) { pick = i; pick_shared = sh; continue; }
            const PlPart *a = &P->parts[i], *b = &P->parts[pick];
            bool ka = a->shape[0].keepout_count > 0, kb = b->shape[0].keepout_count > 0;
            int better = ka != kb ? ka : sh != pick_shared ? sh > pick_shared
                       : a->net_count != b->net_count ? a->net_count > b->net_count : strcmp(a->ref, b->ref) < 0;
            if (better) { pick = i; pick_shared = sh; }
        }
        pending[pick] = false;
        const PlPart *part = &P->parts[pick];
        bool found = false;
        int64_t best_cost = 0;
        G2Nm best_x = 0, best_y = 0;
        uint8_t best_rot = 0;
        out->placed[pick] = true;
        for (uint8_t r = 0; r < 4; r++) {
            const PlShape *s = &part->shape[r];
            out->rot[pick] = r;
            for (G2Nm y = 0; y <= P->height; y += P->step) {
                for (G2Nm x = 0; x <= P->width; x += P->step) {
                    if (s->body.x1 + x < 0 || s->body.y1 + y < 0 || s->body.x2 + x > P->width || s->body.y2 + y > P->height)
                        continue;
                    out->x[pick] = x; out->y[pick] = y;
                    if (!pl_part_legal(P, out, pick)) continue;
                    int64_t cost = scaled(keepout_edge(P, out, pick), P->weights.keepout_edge_ppm);
                    for (size_t k = 0; k < part->net_count; k++) cost += net_hpwl(P, out, part->nets[k]);
                    if (!anything) cost += iabs(2 * x - P->width) / 2 + iabs(2 * y - P->height) / 2;
                    /* Key (cost, rot, y, x): rotations and rows are scanned in that order, so the first minimum wins. */
                    if (!found || cost < best_cost) { found = true; best_cost = cost; best_x = x; best_y = y; best_rot = r; }
                }
            }
        }
        if (!found) { out->placed[pick] = false; any_failed = true; continue; }
        out->x[pick] = best_x; out->y[pick] = best_y; out->rot[pick] = best_rot;
        anything = true;
    }
    return any_failed ? PL_NO_LEGAL_POSITION : PL_OK;
}

/* ------------------------------------------------------------------------------------------------ annealing */

static uint64_t splitmix64(uint64_t *state)
{
    uint64_t z = (*state += UINT64_C(0x9E3779B97F4A7C15));
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}

static uint64_t below(uint64_t *state, uint64_t n)   /* uniform in [0, n), unbiased */
{
    uint64_t limit = UINT64_MAX - UINT64_MAX % n, v;
    do v = splitmix64(state); while (v >= limit);
    return v % n;
}

#define Q32_ONE (UINT64_C(1) << 32)
/* exp(-n) for n = 0..16 and exp(-2^k / 256) for k = 0..7, in Q32, rounded from 50-digit values. */
static const uint64_t EXP_INT[17] = {
    UINT64_C(4294967296), 1580030169u, 581260615u, 213833830u, 78665070u, 28939262u, 10646160u, 3916503u,
    1440801u, 530041u, 194991u, 71733u, 26389u, 9708u, 3571u, 1314u, 483u};
static const uint64_t EXP_FRAC[8] = {
    4278222805u, 4261543595u, 4228380000u, 4162825044u, 4034748382u, 3790295335u, 3344923893u, 2605029347u};

uint64_t pl_exp_neg_q32(uint64_t q)
{
    uint64_t n = q >> 8, acc;
    if (n > 16) return 0;
    acc = EXP_INT[n];
    for (unsigned k = 0; k < 8; k++) {
        if (!((q >> k) & 1u)) continue;
        acc = acc >= Q32_ONE ? EXP_FRAC[k] : (acc * EXP_FRAC[k] + (UINT64_C(1) << 31)) >> 32;
    }
    return acc;
}

typedef struct Annealer {
    const PlProblem *P;
    PlPlacement cur;
    int64_t *net_h;        /* per-net HPWL of `cur` */
    int64_t *trial_h;      /* scratch: new HPWL of the nets a move touches */
    int32_t *touched;      /* scratch: those nets */
    uint8_t *mark;
    size_t touched_count;
    int64_t hpwl, cost;
} Annealer;

static void touch_nets(Annealer *A, size_t i)
{
    const PlPart *part = &A->P->parts[i];
    for (size_t k = 0; k < part->net_count; k++) {
        int32_t n = part->nets[k];
        if (A->mark[n]) continue;
        A->mark[n] = 1;
        A->touched[A->touched_count++] = n;
    }
}

/* Cost of `cur` after parts a (and b, or SIZE_MAX) moved; fills trial_h for the touched nets. */
static int64_t trial_cost(Annealer *A, size_t a, size_t b)
{
    int64_t h = A->hpwl;
    A->touched_count = 0;
    touch_nets(A, a);
    if (b != SIZE_MAX) touch_nets(A, b);
    for (size_t k = 0; k < A->touched_count; k++) {
        int32_t n = A->touched[k];
        A->mark[n] = 0;
        A->trial_h[k] = net_hpwl(A->P, &A->cur, n);
        h += A->trial_h[k] - A->net_h[n];
    }
    return h + placement_terms(A->P, &A->cur);
}

static void commit(Annealer *A, int64_t cost)
{
    for (size_t k = 0; k < A->touched_count; k++) {
        int32_t n = A->touched[k];
        A->hpwl += A->trial_h[k] - A->net_h[n];
        A->net_h[n] = A->trial_h[k];
    }
    A->cost = cost;
}

static G2Nm floor_div(G2Nm a, G2Nm b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
static G2Nm snap(G2Nm v, G2Nm step) { return floor_div(v + step / 2, step) * step; }

static void sort_nm(G2Nm *v, size_t n)
{
    for (size_t i = 1; i < n; i++) {
        G2Nm t = v[i];
        size_t j = i;
        while (j && v[j - 1] > t) { v[j] = v[j - 1]; j--; }
        v[j] = t;
    }
}

/* The median of the bounding boxes of part i's nets over the other placed parts (its optimal region's centre),
 * minus the centroid of its own pads: where the part's origin should go. False when no net reaches another part. */
static bool pull_target(const Annealer *A, size_t i, G2Nm *xs, G2Nm *ys, G2Nm *tx, G2Nm *ty)
{
    const PlProblem *P = A->P;
    const PlPart *part = &P->parts[i];
    const PlShape *own = &part->shape[A->cur.rot[i]];
    size_t m = 0;
    for (size_t k = 0; k < part->net_count; k++) {
        int32_t n = part->nets[k];
        G2Nm x1 = 0, y1 = 0, x2 = 0, y2 = 0;
        bool any = false;
        for (size_t q = 0; q < P->net_part_count[n]; q++) {
            size_t j = P->net_parts[n][q];
            if (j == i || !A->cur.placed[j]) continue;
            const PlShape *s = &P->parts[j].shape[A->cur.rot[j]];
            for (size_t p = 0; p < s->pad_count; p++) {
                if (s->pads[p].net != n) continue;
                G2Nm cx = s->pads[p].cx + A->cur.x[j], cy = s->pads[p].cy + A->cur.y[j];
                if (!any) { x1 = x2 = cx; y1 = y2 = cy; any = true; continue; }
                if (cx < x1) x1 = cx;
                if (cx > x2) x2 = cx;
                if (cy < y1) y1 = cy;
                if (cy > y2) y2 = cy;
            }
        }
        if (!any) continue;
        xs[m] = x1; ys[m] = y1; m++;
        xs[m] = x2; ys[m] = y2; m++;
    }
    if (!m || !own->pad_count) return false;
    sort_nm(xs, m);
    sort_nm(ys, m);
    G2Nm sx = 0, sy = 0;
    for (size_t p = 0; p < own->pad_count; p++) { sx += own->pads[p].cx; sy += own->pads[p].cy; }
    *tx = snap(xs[(m - 1) / 2] - floor_div(sx, (G2Nm)own->pad_count), P->step);
    *ty = snap(ys[(m - 1) / 2] - floor_div(sy, (G2Nm)own->pad_count), P->step);
    return true;
}

static uint64_t acceptance_q(int64_t delta, int64_t t)
{
    if (t > INT64_MAX / 512) { t /= 512; delta /= 512; if (t < 1) t = 1; }
    int64_t whole = delta / t;
    if (whole > 16) return UINT64_MAX;
    return (uint64_t)whole * 256u + (uint64_t)((delta % t) * 256 / t);
}

PlStatus pl_anneal(Arena *arena, const PlProblem *P, const PlPlacement *start, uint64_t seed,
                   uint32_t iterations, PlPlacement *out, PlStats *stats)
{
    PlStats local;
    PlStatus s;
    Annealer A;
    size_t *movable = NULL, mcount = 0, max_nets = 0;
    G2Nm *xs = NULL, *ys = NULL;
    if (!arena || !P || !start || !out) return PL_INVALID_INPUT;
    if (!stats) stats = &local;
    memset(stats, 0, sizeof *stats);
    for (size_t i = 0; i < P->part_count; i++) {
        if (!start->placed[i]) return PL_ILLEGAL_START;
        if (!P->parts[i].fixed) mcount++;
        if (P->parts[i].net_count > max_nets) max_nets = P->parts[i].net_count;
    }
    memset(&A, 0, sizeof A);
    A.P = P;
    if ((s = pl_placement_alloc(arena, P, &A.cur))) return s;
    pl_placement_copy(P, &A.cur, start);
    for (size_t i = 0; i < P->part_count; i++)
        if (!P->parts[i].fixed && !pl_part_legal(P, &A.cur, i)) return PL_ILLEGAL_START;
    pl_placement_copy(P, out, start);
    stats->start_cost = stats->best_cost = pl_cost(P, start);
    if (!mcount) return PL_OK;

    size_t nets = P->net_count ? P->net_count : 1;
    movable = arena_alloc_array(arena, sizeof *movable, mcount);
    A.net_h = arena_alloc_array(arena, sizeof *A.net_h, nets);
    A.trial_h = arena_alloc_array(arena, sizeof *A.trial_h, nets);
    A.touched = arena_alloc_array(arena, sizeof *A.touched, nets);
    A.mark = arena_alloc_array(arena, sizeof *A.mark, nets);
    xs = arena_alloc_array(arena, sizeof *xs, 2 * max_nets + 2);
    ys = arena_alloc_array(arena, sizeof *ys, 2 * max_nets + 2);
    if (!movable || !A.net_h || !A.trial_h || !A.touched || !A.mark || !xs || !ys) return PL_OUT_OF_MEMORY;
    memset(A.mark, 0, nets);
    mcount = 0;
    for (size_t i = 0; i < P->part_count; i++)
        if (!P->parts[i].fixed) movable[mcount++] = i;
    A.hpwl = 0;
    for (size_t n = 0; n < P->net_count; n++) { A.net_h[n] = net_hpwl(P, &A.cur, (int32_t)n); A.hpwl += A.net_h[n]; }
    A.cost = A.hpwl + placement_terms(P, &A.cur);

    uint64_t rng = seed;
    int64_t temperature = A.cost / 20 > G2_NM_PER_MM ? A.cost / 20 : G2_NM_PER_MM;   /* 5 % of the start, >= 1 mm */
    int64_t best = A.cost;

    for (uint32_t step = 0; step < iterations; step++) {
        G2Nm span = (G2Nm)((UINT64_C(2) * PL_SHIFT_SPAN * (iterations - step) + iterations) / (UINT64_C(2) * iterations));
        if (span < 1) span = 1;
        for (size_t m = 0; m < mcount; m++) {
            size_t a = movable[below(&rng, mcount)], b = SIZE_MAX;
            G2Nm ax = A.cur.x[a], ay = A.cur.y[a], bx = 0, by = 0;
            uint8_t ar = A.cur.rot[a];
            uint64_t kind = below(&rng, 1000);
            stats->moves++;
            if (kind < 400) {                                   /* shift by up to `span` grid steps */
                G2Nm dx = (G2Nm)below(&rng, (uint64_t)(2 * span + 1)) - span;
                G2Nm dy = (G2Nm)below(&rng, (uint64_t)(2 * span + 1)) - span;
                if (!dx && !dy) continue;
                A.cur.x[a] = ax + dx * P->step; A.cur.y[a] = ay + dy * P->step;
            } else if (kind < 550) {                            /* rotate */
                A.cur.rot[a] = (uint8_t)((ar + 1 + below(&rng, 3)) & 3u);
            } else if (kind < 700) {                            /* swap two movable parts' positions */
                if (mcount < 2) continue;
                b = movable[below(&rng, mcount)];
                if (b == a) continue;
                bx = A.cur.x[b]; by = A.cur.y[b];
                A.cur.x[a] = bx; A.cur.y[a] = by; A.cur.x[b] = ax; A.cur.y[b] = ay;
            } else {                                            /* pull toward the median of its nets */
                G2Nm tx, ty;
                if (!pull_target(&A, a, xs, ys, &tx, &ty)) continue;
                if (below(&rng, 2)) { tx += ((G2Nm)below(&rng, 3) - 1) * P->step; ty += ((G2Nm)below(&rng, 3) - 1) * P->step; }
                if (tx == ax && ty == ay) continue;
                A.cur.x[a] = tx; A.cur.y[a] = ty;
            }
            if (!pl_part_legal(P, &A.cur, a) || (b != SIZE_MAX && !pl_part_legal(P, &A.cur, b))) {
                A.cur.x[a] = ax; A.cur.y[a] = ay; A.cur.rot[a] = ar;
                if (b != SIZE_MAX) { A.cur.x[b] = bx; A.cur.y[b] = by; }
                continue;
            }
            stats->legal_moves++;
            int64_t trial = trial_cost(&A, a, b), delta = trial - A.cost;
            bool accept = delta <= 0;
            if (!accept) {
                uint64_t q = acceptance_q(delta, temperature);
                accept = q != UINT64_MAX && (splitmix64(&rng) >> 32) < pl_exp_neg_q32(q);
            }
            if (!accept) {
                A.cur.x[a] = ax; A.cur.y[a] = ay; A.cur.rot[a] = ar;
                if (b != SIZE_MAX) { A.cur.x[b] = bx; A.cur.y[b] = by; }
                continue;
            }
            stats->accepted++;
            commit(&A, trial);
            if (A.cost < best) {
                best = A.cost;
                stats->improved++;
                pl_placement_copy(P, out, &A.cur);
            }
        }
        temperature -= temperature * 3 / 1000;                  /* x 0.997 per step */
        if (temperature < 1) temperature = 1;
    }

    /* Refinement: from the best state, move each part to the cheapest legal spot within +-4 half-steps (any
     * rotation), until a pass changes nothing. Strict improvement only, so it terminates. */
    pl_placement_copy(P, &A.cur, out);
    A.hpwl = 0;
    for (size_t n = 0; n < P->net_count; n++) { A.net_h[n] = net_hpwl(P, &A.cur, (int32_t)n); A.hpwl += A.net_h[n]; }
    A.cost = A.hpwl + placement_terms(P, &A.cur);
    G2Nm half = P->step / 2 ? P->step / 2 : 1;
    for (int pass = 0; pass < PL_REFINE_PASSES; pass++) {
        bool changed = false;
        for (size_t m = 0; m < mcount; m++) {
            size_t a = movable[m];
            G2Nm ox = A.cur.x[a], oy = A.cur.y[a], bx2 = ox, by2 = oy;
            uint8_t orot = A.cur.rot[a], brot = orot;
            int64_t bcost = A.cost;
            for (uint8_t r = 0; r < 4; r++)
                for (G2Nm dy = -PL_REFINE_REACH; dy <= PL_REFINE_REACH; dy++)
                    for (G2Nm dx = -PL_REFINE_REACH; dx <= PL_REFINE_REACH; dx++) {
                        if (!dx && !dy && r == orot) continue;
                        A.cur.x[a] = ox + dx * half; A.cur.y[a] = oy + dy * half; A.cur.rot[a] = r;
                        stats->refine_moves++;
                        if (!pl_part_legal(P, &A.cur, a)) continue;
                        int64_t c = trial_cost(&A, a, SIZE_MAX);
                        if (c < bcost) { bcost = c; bx2 = A.cur.x[a]; by2 = A.cur.y[a]; brot = r; }
                    }
            A.cur.x[a] = bx2; A.cur.y[a] = by2; A.cur.rot[a] = brot;
            if (bcost < A.cost) {
                (void)trial_cost(&A, a, SIZE_MAX);   /* re-measure the chosen spot's nets */
                commit(&A, bcost);
                changed = true;
            }
        }
        if (!changed) break;
    }
    pl_placement_copy(P, out, &A.cur);
    stats->best_cost = A.cost;
    return PL_OK;
}

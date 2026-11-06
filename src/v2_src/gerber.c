/* gerber.c - Gerber RS-274X / X2 reader (see gerber.h). Integer nm, no floating point, no printf/exit. */
#include "gerber.h"

#include "board.h"   /* board_rotate: exact for multiples of 90 degrees, Q62 fixed point otherwise */

#include <stdio.h>
#include <string.h>

#if defined(__SIZEOF_INT128__)
__extension__ typedef __int128 GI128;
#define GERBER_HAVE_I128 1
#else
#define GERBER_HAVE_I128 0
#endif

#define MICRO INT64_C(1000000)          /* numbers are held x 1e6 in the file's unit (mm or inch) */

static const char *const STATUS_NAMES[] = {"OK", "INVALID_ARGUMENT", "IO_ERROR", "OUT_OF_MEMORY", "CONTENT_ERROR"};
static const char *const ISSUE_NAMES[] = {
    "SYNTAX", "NO_FORMAT", "UNSUPPORTED_FORMAT", "UNDEFINED_APERTURE", "D03_IN_REGION", "BAD_APERTURE", "BAD_MACRO",
    "COORD_RANGE", "ARC_UNSUPPORTED", "UNSUPPORTED_COMMAND", "UNSUPPORTED_PRIMITIVE", "NON_CIRCULAR_DRAW", "ROUNDED",
    "INEXACT_ROTATION", "MISSING_M02", "REGION_NOT_CLOSED"};

const char *gerber_status_name(GerberStatus s) { return (unsigned)s < sizeof STATUS_NAMES / sizeof *STATUS_NAMES ? STATUS_NAMES[s] : "UNKNOWN_STATUS"; }
const char *gerber_issue_name(GerberIssueCode c) { return (unsigned)c < sizeof ISSUE_NAMES / sizeof *ISSUE_NAMES ? ISSUE_NAMES[c] : "UNKNOWN_ISSUE"; }

/* ------------------------------------------------------------------------------------------------ state */

typedef struct Macro { const char *name; const char **words; size_t word_count; size_t line; } Macro;

typedef struct Attrs { GerberAttribute *items; size_t count, cap; } Attrs;

typedef struct P {
    Arena *a;
    GerberFile *f;
    size_t line;
    size_t cap_ap, cap_obj, cap_issue;
    Macro *macros; size_t macro_count, macro_cap;
    Attrs tf, ta, to;
    bool have_fs, have_mo, no_format_reported, region, dark, rounded_reported;
    int mode;                    /* 1 linear, 2 cw, 3 ccw */
    size_t current;              /* aperture index or SIZE_MAX */
    G2Point point;
    G2Point *contour; size_t contour_count, contour_cap; size_t contour_line;
    bool oom;
} P;

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

static char *dupn(Arena *a, const char *s, size_t n)
{
    char *d = arena_alloc(a, n + 1);
    if (d) { memcpy(d, s, n); d[n] = 0; }
    return d;
}

static void issue(P *p, GerberIssueCode code, GerberSeverity sev, const char *detail)
{
    GerberFile *f = p->f;
    if (!grow(p->a, (void **)&f->issues, &p->cap_issue, f->issue_count + 1, sizeof *f->issues)) { p->oom = true; return; }
    GerberIssue *i = &f->issues[f->issue_count++];
    i->code = code; i->severity = sev; i->line = p->line; i->detail = detail;
    if (sev == GERBER_ERROR) f->error_count++; else f->warning_count++;
}

/* ------------------------------------------------------------------------------------------------ numbers */

/* A decimal "[+-]digits[.digits]" -> value x 1e6 (rounded beyond 6 decimals). Returns chars used, 0 on error. */
static size_t decimal(const char *s, size_t n, int64_t *out, bool *rounded)
{
    size_t i = 0;
    bool neg = false;
    if (i < n && (s[i] == '+' || s[i] == '-')) neg = s[i++] == '-';
    int64_t whole = 0, frac = 0;
    int digits = 0, any = 0, roundup = 0;
    while (i < n && s[i] >= '0' && s[i] <= '9') {
        if (whole > INT64_C(1000000000000)) return 0;
        whole = whole * 10 + (s[i++] - '0');
        any = 1;
    }
    if (i < n && s[i] == '.') {
        i++;
        while (i < n && s[i] >= '0' && s[i] <= '9') {
            if (digits < 6) { frac = frac * 10 + (s[i] - '0'); digits++; }
            else { if (digits == 6 && s[i] >= '5') roundup = 1; if (s[i] != '0') *rounded = true; digits++; }
            i++;
            any = 1;
        }
    }
    if (!any) return 0;
    for (int d = digits; d < 6; d++) frac *= 10;
    int64_t v = whole * MICRO + frac + roundup;
    *out = neg ? -v : v;
    return i;
}

/* File units x 1e6 -> nm. mm: exact; inch: x 25.4, rounded to the nearest nm. */
static G2Nm to_nm(P *p, int64_t micro, bool *rounded)
{
    if (p->f->units != GERBER_UNITS_INCH) return micro;
    int64_t t = micro * 254;                      /* |micro| <= ~4e10 in range checks below: fits */
    int64_t q = t / 10, r = t % 10;
    if (r) { *rounded = true; if (r >= 5) q++; else if (r <= -5) q--; }
    return q;
}

static void note_rounded(P *p, bool rounded)
{
    if (rounded && !p->rounded_reported) {
        issue(p, GERBER_ISSUE_ROUNDED, GERBER_WARNING, "values rounded to the nearest nm (inch data or > 6 decimals)");
        p->rounded_reported = true;
    }
}

/* A coordinate's digits under FS (leading zeros omitted, dec_digits implied decimals) -> nm. */
static bool coord(P *p, const char *s, size_t n, size_t *used, G2Nm *out)
{
    size_t i = 0;
    bool neg = false, rounded = false;
    if (i < n && (s[i] == '+' || s[i] == '-')) neg = s[i++] == '-';
    int64_t v = 0;
    size_t start = i;
    while (i < n && s[i] >= '0' && s[i] <= '9') {
        if (v > INT64_C(100000000000000)) return false;
        v = v * 10 + (s[i++] - '0');
    }
    if (i == start) return false;
    *used = i;
    int dec = p->f->dec_digits;
    int64_t micro;
    if (dec <= 6) { micro = v; for (int k = dec; k < 6; k++) micro *= 10; }
    else {
        int64_t div = 1;
        for (int k = 6; k < dec; k++) div *= 10;
        micro = v / div;
        if (v % div) { rounded = true; if ((v % div) * 2 >= div) micro++; }
    }
    if (neg) micro = -micro;
    G2Nm nm = to_nm(p, micro, &rounded);
    note_rounded(p, rounded);
    if (nm > G2_COORD_MAX_NM || nm < -G2_COORD_MAX_NM) return false;
    *out = nm;
    return true;
}

/* ------------------------------------------------------------------------------------------------ attributes */

static bool attr_parse(P *p, const char *s, size_t n, GerberAttribute *out)
{
    size_t k = 0;
    while (k < n && s[k] != ',') k++;
    out->name = dupn(p->a, s, k);
    out->value_count = 0;
    size_t count = 0;
    for (size_t i = k; i < n; i++) count += s[i] == ',';
    out->values = count ? arena_alloc_array(p->a, sizeof *out->values, count) : NULL;
    if (!out->name || (count && !out->values)) return false;
    size_t i = k;
    while (i < n) {
        size_t j = ++i;
        while (j < n && s[j] != ',') j++;
        out->values[out->value_count++] = dupn(p->a, s + i, j - i);
        i = j;
    }
    return true;
}

static void attr_set(P *p, Attrs *d, GerberAttribute at)
{
    for (size_t i = 0; i < d->count; i++)
        if (!strcmp(d->items[i].name, at.name)) { d->items[i] = at; return; }
    if (!grow(p->a, (void **)&d->items, &d->cap, d->count + 1, sizeof *d->items)) { p->oom = true; return; }
    d->items[d->count++] = at;
}

static void attr_delete(Attrs *d, const char *name)
{
    for (size_t i = 0; i < d->count; i++)
        if (!strcmp(d->items[i].name, name)) { d->items[i] = d->items[--d->count]; return; }
}

static const GerberAttribute *attr_get(const Attrs *d, const char *name)
{
    for (size_t i = 0; i < d->count; i++) if (!strcmp(d->items[i].name, name)) return &d->items[i];
    return NULL;
}

/* ------------------------------------------------------------------------------------------------ shapes */

static G2Nm half_up(G2Nm v) { return v >= 0 ? v / 2 + (v % 2 != 0) : -((-v) / 2 + ((-v) % 2 != 0)); }

typedef struct Builder { P *p; G2Shape *shapes; size_t count, cap; bool exact; } Builder;

static bool add_shape(Builder *b, G2Shape s)
{
    if (!grow(b->p->a, (void **)&b->shapes, &b->cap, b->count + 1, sizeof *b->shapes)) return false;
    b->shapes[b->count++] = s;
    return true;
}

static G2Shape circle(G2Point c, G2Nm r) { G2Shape s; memset(&s, 0, sizeof s); s.kind = G2_SHAPE_CIRCLE; s.as.circle.center = c; s.as.circle.radius = r; return s; }

static G2Shape rect(G2Nm cx, G2Nm cy, G2Nm w, G2Nm h)
{
    G2Shape s;
    memset(&s, 0, sizeof s);
    s.kind = G2_SHAPE_ROUNDED_RECT;
    G2Nm hx = half_up(w), hy = half_up(h);
    s.as.rounded_rect.core.min.x = cx - hx; s.as.rounded_rect.core.min.y = cy - hy;
    s.as.rounded_rect.core.max.x = cx + hx; s.as.rounded_rect.core.max.y = cy + hy;
    s.as.rounded_rect.radius = 0;
    return s;
}

/* Rotate about the origin counter-clockwise (Gerber, y up) by `udeg` micro-degrees. */
static G2Point rot_ccw(G2Point v, int64_t udeg, bool *exact)
{
    G2Nm x = v.x, y = v.y;
    bool ex = true;
    /* board_rotate turns clockwise in y-up terms (KiCad's y-down CCW): use the negative angle. */
    if (board_rotate(v.x, v.y, -udeg, &x, &y, &ex) != BOARD_OK) { *exact = false; return v; }
    if (!ex) *exact = false;
    G2Point r = {x, y};
    return r;
}

/* An outline as a G2 polygon. KiCad writes repeated vertices and back-tracking spikes (FreePoly pads); both add no
 * area and are removed. An outline that is still not a simple polygon is replaced by its bounding box (containing
 * it) and the aperture is marked inexact. An outline with no area adds nothing. */
static bool polygon_shape(Builder *b, const G2Point *pts, size_t n)
{
    G2Point *v = arena_alloc_array(b->p->a, sizeof *v, n ? n : 1);
    if (!v) return false;
    size_t m = 0;
    for (size_t i = 0; i < n; i++)
        if (!m || pts[i].x != v[m - 1].x || pts[i].y != v[m - 1].y) v[m++] = pts[i];
    while (m > 1 && v[0].x == v[m - 1].x && v[0].y == v[m - 1].y) m--;
    for (bool changed = true; changed && m >= 3;) {   /* drop spikes: b on the line a-c with a or c past it */
        changed = false;
        for (size_t i = 0; i < m && m >= 3; i++) {
            G2Point a = v[(i + m - 1) % m], c = v[(i + 1) % m], q = v[i];
            G2Order o;
            bool on1 = false, on2 = false;
            if (g2_orient2d(a, q, c, &o) != G2_OK || o != G2_ORDER_EQUAL) continue;
            if (g2_point_on_segment(a, q, c, &on1) != G2_OK || g2_point_on_segment(c, a, q, &on2) != G2_OK) continue;
            if (!on1 && !on2 && !(a.x == c.x && a.y == c.y)) continue;
            memmove(&v[i], &v[i + 1], (m - i - 1) * sizeof *v);
            m--;
            changed = true;
        }
        for (size_t i = 0; m >= 2 && i < m; i++)
            if (v[i].x == v[(i + 1) % m].x && v[i].y == v[(i + 1) % m].y) { memmove(&v[i], &v[i + 1], (m - i - 1) * sizeof *v); m--; changed = true; }
    }
    G2Shape s;
    memset(&s, 0, sizeof s);
    s.kind = G2_SHAPE_POLYGON;
    s.as.polygon.vertices = v;
    s.as.polygon.count = m;
    if (m >= 3 && g2_polygon_validate(s.as.polygon) == G2_OK) return add_shape(b, s);
    if (n < 3) return true;
    G2Aabb box = {pts[0], pts[0]};
    for (size_t i = 1; i < n; i++) {
        if (pts[i].x < box.min.x) box.min.x = pts[i].x;
        if (pts[i].y < box.min.y) box.min.y = pts[i].y;
        if (pts[i].x > box.max.x) box.max.x = pts[i].x;
        if (pts[i].y > box.max.y) box.max.y = pts[i].y;
    }
    if (box.min.x == box.max.x || box.min.y == box.max.y) return true;   /* no area: no copper */
    b->exact = false;
    issue(b->p, GERBER_ISSUE_UNSUPPORTED_PRIMITIVE, GERBER_WARNING, "outline is not a simple polygon: its bounding box is used");
    memset(&s, 0, sizeof s);
    s.kind = G2_SHAPE_ROUNDED_RECT;
    s.as.rounded_rect.core = box;
    return add_shape(b, s);
}

static bool quad_from(Builder *b, G2Point c0, G2Point c1, G2Point c2, G2Point c3)
{
    G2Point v[4] = {c0, c1, c2, c3};
    return polygon_shape(b, v, 4);
}

/* ------------------------------------------------------------------------------------------------ macros */

typedef struct Expr { const char *s; size_t n, i; const int64_t *vars; size_t var_count; bool rounded, bad; } Expr;

static int64_t expr_sum(Expr *e);

static int64_t expr_atom(Expr *e)
{
    while (e->i < e->n && e->s[e->i] == ' ') e->i++;
    if (e->i >= e->n) { e->bad = true; return 0; }
    char c = e->s[e->i];
    if (c == '(') { e->i++; int64_t v = expr_sum(e); if (e->i < e->n && e->s[e->i] == ')') e->i++; else e->bad = true; return v; }
    if (c == '-') { e->i++; return -expr_atom(e); }
    if (c == '+') { e->i++; return expr_atom(e); }
    if (c == '$') {
        size_t j = ++e->i, k = 0;
        while (e->i < e->n && e->s[e->i] >= '0' && e->s[e->i] <= '9') k = k * 10 + (size_t)(e->s[e->i++] - '0');
        if (e->i == j || k == 0) { e->bad = true; return 0; }
        return k <= e->var_count ? e->vars[k - 1] : 0;   /* undefined variables are 0 (Gerber spec) */
    }
    int64_t v;
    size_t used = decimal(e->s + e->i, e->n - e->i, &v, &e->rounded);
    if (!used) { e->bad = true; return 0; }
    e->i += used;
    return v;
}

static int64_t expr_product(Expr *e)
{
    int64_t v = expr_atom(e);
    for (;;) {
        while (e->i < e->n && e->s[e->i] == ' ') e->i++;
        if (e->i >= e->n || (e->s[e->i] != 'x' && e->s[e->i] != 'X' && e->s[e->i] != '/')) return v;
        char op = e->s[e->i++];
        int64_t w = expr_atom(e);
#if GERBER_HAVE_I128
        GI128 num = op == '/' ? (GI128)v * MICRO : (GI128)v * w, den = op == '/' ? w : MICRO;
        if (!den) { e->bad = true; return 0; }
        GI128 q = num / den, r = num % den;
        if (r) { e->rounded = true; if ((r < 0 ? -r : r) * 2 >= (den < 0 ? -den : den)) q += ((r < 0) != (den < 0)) ? -1 : 1; }
        if (q > INT64_MAX || q < -INT64_MAX) { e->bad = true; return 0; }
        v = (int64_t)q;
#else
        (void)w; (void)op;
        e->bad = true;
        return 0;
#endif
    }
}

static int64_t expr_sum(Expr *e)
{
    int64_t v = expr_product(e);
    for (;;) {
        while (e->i < e->n && e->s[e->i] == ' ') e->i++;
        if (e->i >= e->n || (e->s[e->i] != '+' && e->s[e->i] != '-')) return v;
        char op = e->s[e->i++];
        int64_t w = expr_product(e);
        v = op == '+' ? v + w : v - w;
    }
}

/* Split a primitive "c,a,b,..." into evaluated numbers (x 1e6). */
static size_t eval_list(P *p, const char *s, size_t n, const int64_t *vars, size_t nv, int64_t *out, size_t cap, bool *bad, bool *rounded)
{
    size_t count = 0, i = 0;
    (void)p;
    while (i <= n) {
        size_t j = i;
        int depth = 0;
        while (j < n && (s[j] != ',' || depth)) { if (s[j] == '(') depth++; if (s[j] == ')') depth--; j++; }
        if (count == cap) { *bad = true; return count; }
        Expr e = {s + i, j - i, 0, vars, nv, false, false};
        out[count++] = expr_sum(&e);
        while (e.i < e.n && e.s[e.i] == ' ') e.i++;
        if (e.bad || e.i != e.n) *bad = true;
        if (e.rounded) *rounded = true;
        i = j + 1;
        if (j >= n) break;
    }
    return count;
}

#define MAXV 64

static bool build_macro(P *p, Builder *b, const Macro *m, const int64_t *params, size_t np)
{
    int64_t vars[MAXV] = {0};
    size_t nv = np < MAXV ? np : MAXV;
    for (size_t i = 0; i < nv; i++) vars[i] = params[i];
    bool rounded = false;
    for (size_t w = 0; w < m->word_count; w++) {
        const char *s = m->words[w];
        size_t n = strlen(s);
        while (n && (s[0] == ' ' || s[0] == '\r' || s[0] == '\n')) { s++; n--; }
        if (!n) continue;
        if (s[0] == '0' && (n == 1 || s[1] == ' ' || s[1] == ',')) continue;          /* comment */
        if (s[0] == '$') {                                                            /* $k=expr */
            size_t k = 0, i = 1;
            while (i < n && s[i] >= '0' && s[i] <= '9') k = k * 10 + (size_t)(s[i++] - '0');
            if (i >= n || s[i] != '=' || !k || k > MAXV) return false;
            Expr e = {s + i + 1, n - i - 1, 0, vars, MAXV, false, false};
            vars[k - 1] = expr_sum(&e);
            if (e.bad) return false;
            if (e.rounded) rounded = true;
            if (k > nv) nv = k;
            continue;
        }
        int64_t v[MAXV * 2 + 8];
        bool bad = false;
        size_t c = eval_list(p, s, n, vars, MAXV, v, sizeof v / sizeof *v, &bad, &rounded);
        if (bad || c < 2) return false;
        int code = (int)(v[0] / MICRO);
        bool on = v[1] / MICRO != 0;
        if (!on) { b->exact = false; issue(p, GERBER_ISSUE_UNSUPPORTED_PRIMITIVE, GERBER_WARNING, "macro primitive with exposure off (clear): not subtracted"); continue; }
        bool rnd = false;
        bool *ex = &b->exact;
        if (code == 1) {                          /* circle: exp, diameter, cx, cy [, rotation] */
            if (c < 5) return false;
            G2Point ctr = {to_nm(p, v[3], &rnd), to_nm(p, v[4], &rnd)};
            if (c > 5 && v[5]) ctr = rot_ccw(ctr, v[5], ex);
            G2Nm d = to_nm(p, v[2], &rnd);
            if (d % 2) b->exact = false;
            if (d > 0 && !add_shape(b, circle(ctr, half_up(d)))) return false;
        } else if (code == 4) {                   /* outline: exp, n, x0, y0, ..., xn, yn, rotation */
            size_t pts = (size_t)(v[2] / MICRO);
            if (pts < 3 || c != 3 + 2 * (pts + 1) + 1) return false;
            G2Point q[MAXV * 2];
            if (pts > sizeof q / sizeof *q) return false;
            for (size_t k = 0; k < pts; k++) {        /* the last point repeats the first: dropped */
                q[k].x = to_nm(p, v[3 + 2 * k], &rnd);
                q[k].y = to_nm(p, v[4 + 2 * k], &rnd);
                if (v[c - 1]) q[k] = rot_ccw(q[k], v[c - 1], ex);
            }
            if (!polygon_shape(b, q, pts)) return false;
        } else if (code == 20) {                  /* vector line: exp, width, xs, ys, xe, ye, rotation */
            if (c < 7) return false;
            G2Nm wd = to_nm(p, v[2], &rnd);
            G2Point s0 = {to_nm(p, v[3], &rnd), to_nm(p, v[4], &rnd)}, e0 = {to_nm(p, v[5], &rnd), to_nm(p, v[6], &rnd)};
            G2Nm hw = half_up(wd);
            if (wd % 2) b->exact = false;
            G2Point k0, k1, k2, k3;
            if (s0.y == e0.y) { k0 = (G2Point){s0.x, s0.y - hw}; k1 = (G2Point){e0.x, e0.y - hw}; k2 = (G2Point){e0.x, e0.y + hw}; k3 = (G2Point){s0.x, s0.y + hw}; }
            else if (s0.x == e0.x) { k0 = (G2Point){s0.x - hw, s0.y}; k1 = (G2Point){e0.x - hw, e0.y}; k2 = (G2Point){e0.x + hw, e0.y}; k3 = (G2Point){s0.x + hw, s0.y}; }
            else {
                /* A slanted line: its corners need a square root; use the capsule of the same centreline, which
                 * contains the rectangle, and mark the aperture inexact. */
                G2Shape cap;
                memset(&cap, 0, sizeof cap);
                cap.kind = G2_SHAPE_CAPSULE;
                cap.as.capsule.a = s0; cap.as.capsule.b = e0; cap.as.capsule.radius = hw;
                b->exact = false;
                if (!add_shape(b, cap)) return false;
                continue;
            }
            int64_t r = c > 7 ? v[7] : 0;
            if (r) { k0 = rot_ccw(k0, r, ex); k1 = rot_ccw(k1, r, ex); k2 = rot_ccw(k2, r, ex); k3 = rot_ccw(k3, r, ex); }
            if (!quad_from(b, k0, k1, k2, k3)) return false;
        } else if (code == 21) {                  /* centre line: exp, width, height, cx, cy, rotation */
            if (c < 6) return false;
            G2Nm w0 = to_nm(p, v[2], &rnd), h0 = to_nm(p, v[3], &rnd);
            G2Nm cx = to_nm(p, v[4], &rnd), cy = to_nm(p, v[5], &rnd);
            int64_t r = c > 6 ? v[6] : 0;
            if (w0 % 2 || h0 % 2) b->exact = false;
            G2Nm hx = half_up(w0), hy = half_up(h0);
            G2Point k0 = {cx - hx, cy - hy}, k1 = {cx + hx, cy - hy}, k2 = {cx + hx, cy + hy}, k3 = {cx - hx, cy + hy};
            if (r) { k0 = rot_ccw(k0, r, ex); k1 = rot_ccw(k1, r, ex); k2 = rot_ccw(k2, r, ex); k3 = rot_ccw(k3, r, ex); }
            if (!quad_from(b, k0, k1, k2, k3)) return false;
        } else {
            b->exact = false;
            issue(p, GERBER_ISSUE_UNSUPPORTED_PRIMITIVE, GERBER_WARNING, code == 5 ? "macro primitive 5 (polygon)" : code == 7 ? "macro primitive 7 (thermal)" : "unknown macro primitive");
        }
        if (rnd) rounded = true;
    }
    if (rounded) { b->exact = false; note_rounded(p, true); }
    return true;
}

/* ------------------------------------------------------------------------------------------------ commands */

static const Macro *find_macro(const P *p, const char *name, size_t n)
{
    for (size_t i = 0; i < p->macro_count; i++)
        if (strlen(p->macros[i].name) == n && !memcmp(p->macros[i].name, name, n)) return &p->macros[i];
    return NULL;
}

static void cmd_ad(P *p, const char *s, size_t n)
{
    /* ADDnn<template>[,p1Xp2X...] */
    size_t i = 3, start;
    int num = 0;
    while (i < n && s[i] >= '0' && s[i] <= '9') num = num * 10 + (s[i++] - '0');
    if (i == 3 || num < 10) { issue(p, GERBER_ISSUE_BAD_APERTURE, GERBER_ERROR, "AD needs a D-code >= 10"); return; }
    start = i;
    while (i < n && s[i] != ',') i++;
    const char *tmpl = s + start;
    size_t tn = i - start;
    int64_t params[MAXV];
    size_t np = 0;
    bool rounded = false;
    while (i < n) {                               /* parameters separated by 'X' */
        i++;
        int64_t v;
        size_t used = decimal(s + i, n - i, &v, &rounded);
        if (!used || np == MAXV) { issue(p, GERBER_ISSUE_BAD_APERTURE, GERBER_ERROR, "AD parameter is not a number"); return; }
        params[np++] = v;
        i += used;
        if (i < n && s[i] != 'X') { issue(p, GERBER_ISSUE_BAD_APERTURE, GERBER_ERROR, "AD parameters are separated by X"); return; }
    }
    GerberFile *f = p->f;
    if (!grow(p->a, (void **)&f->apertures, &p->cap_ap, f->aperture_count + 1, sizeof *f->apertures)) { p->oom = true; return; }
    GerberAperture *ap = &f->apertures[f->aperture_count];
    memset(ap, 0, sizeof *ap);
    ap->number = num;
    ap->line = p->line;
    ap->param_count = np < 16 ? np : 16;
    Builder b = {p, NULL, 0, 0, true};
    bool rnd = false;
    for (size_t k = 0; k < ap->param_count; k++) ap->params[k] = params[k];
    bool ok = true;
    if (tn == 1 && (tmpl[0] == 'C' || tmpl[0] == 'R' || tmpl[0] == 'O' || tmpl[0] == 'P')) {
        char t = tmpl[0];
        ap->type = t == 'C' ? GERBER_AP_CIRCLE : t == 'R' ? GERBER_AP_RECT : t == 'O' ? GERBER_AP_OBROUND : GERBER_AP_POLYGON;
        size_t need = t == 'C' ? 1 : 2, holes = t == 'C' ? 1 : t == 'P' ? 3 : 2;
        if (np < need) { issue(p, GERBER_ISSUE_BAD_APERTURE, GERBER_ERROR, "too few aperture parameters"); return; }
        for (size_t k = 0; k < ap->param_count; k++) if (k < holes || t == 'P') ap->params[k] = to_nm(p, params[k], &rnd);
        if (np > holes) { b.exact = false; issue(p, GERBER_ISSUE_UNSUPPORTED_COMMAND, GERBER_WARNING, "aperture hole ignored (not subtracted)"); }
        G2Point o = {0, 0};
        if (t == 'C') {
            G2Nm d = to_nm(p, params[0], &rnd);
            if (d % 2) b.exact = false;
            ok = d <= 0 || add_shape(&b, circle(o, half_up(d)));   /* a zero-size aperture draws nothing */
        } else if (t == 'R') {
            G2Nm w = to_nm(p, params[0], &rnd), h = to_nm(p, params[1], &rnd);
            if (w % 2 || h % 2) b.exact = false;
            ok = w <= 0 || h <= 0 || add_shape(&b, rect(0, 0, w, h));
        } else if (t == 'O') {
            G2Nm w = to_nm(p, params[0], &rnd), h = to_nm(p, params[1], &rnd);
            if (w % 2 || h % 2) b.exact = false;
            G2Shape s2;
            memset(&s2, 0, sizeof s2);
            if (w == h) s2 = circle(o, half_up(w));
            else {
                s2.kind = G2_SHAPE_CAPSULE;
                G2Nm l = half_up(w > h ? w - h : h - w);
                s2.as.capsule.a = w > h ? (G2Point){-l, 0} : (G2Point){0, -l};
                s2.as.capsule.b = w > h ? (G2Point){l, 0} : (G2Point){0, l};
                s2.as.capsule.radius = half_up(w > h ? h : w);
            }
            ok = add_shape(&b, s2);
        } else {                                       /* P: outer diameter, vertices, [rotation] */
            G2Nm d = to_nm(p, params[0], &rnd);
            int64_t verts = params[1] / MICRO, rot0 = np > 2 ? params[2] : 0;
            if (verts < 3 || verts > 12) { issue(p, GERBER_ISSUE_BAD_APERTURE, GERBER_ERROR, "a polygon aperture has 3 to 12 vertices"); return; }
            G2Point q[12];
            for (int64_t k = 0; k < verts; k++) {
                G2Point r0 = {half_up(d), 0};
                q[k] = rot_ccw(r0, rot0 + k * (INT64_C(360000000) / verts), &b.exact);
            }
            if (!b.exact) issue(p, GERBER_ISSUE_INEXACT_ROTATION, GERBER_WARNING, "polygon aperture vertices rounded to the nearest nm");
            ok = polygon_shape(&b, q, (size_t)verts);
        }
    } else {
        const Macro *m = find_macro(p, tmpl, tn);
        if (!m) { issue(p, GERBER_ISSUE_BAD_APERTURE, GERBER_ERROR, "AD names an undefined macro"); return; }
        ap->type = GERBER_AP_MACRO;
        ap->macro = m->name;
        if (!build_macro(p, &b, m, params, np)) { issue(p, GERBER_ISSUE_BAD_MACRO, GERBER_ERROR, "macro does not evaluate"); return; }
    }
    if (!ok) { p->oom = true; return; }
    if (rnd || rounded) { b.exact = false; note_rounded(p, true); }
    ap->shapes = b.shapes;
    ap->shape_count = b.count;
    ap->exact = b.exact;
    for (size_t k = 0; k < b.count; k++) {
        G2Aabb box;
        if (g2_shape_aabb(b.shapes[k], &box) != G2_OK) { issue(p, GERBER_ISSUE_BAD_APERTURE, GERBER_ERROR, "aperture geometry out of range"); return; }
        if (!k) ap->bounds = box;
        else {
            if (box.min.x < ap->bounds.min.x) ap->bounds.min.x = box.min.x;
            if (box.min.y < ap->bounds.min.y) ap->bounds.min.y = box.min.y;
            if (box.max.x > ap->bounds.max.x) ap->bounds.max.x = box.max.x;
            if (box.max.y > ap->bounds.max.y) ap->bounds.max.y = box.max.y;
        }
    }
    if (p->ta.count) {
        ap->attributes = arena_alloc_array(p->a, sizeof *ap->attributes, p->ta.count);
        if (!ap->attributes) { p->oom = true; return; }
        memcpy(ap->attributes, p->ta.items, p->ta.count * sizeof *ap->attributes);
        ap->attribute_count = p->ta.count;
    }
    for (size_t k = 0; k + 1 <= f->aperture_count; k++)
        if (f->apertures[k].number == num) { f->apertures[k] = *ap; return; }   /* redefinition replaces */
    f->aperture_count++;
}

static GerberObject *new_object(P *p, GerberObjectKind kind)
{
    GerberFile *f = p->f;
    if (!grow(p->a, (void **)&f->objects, &p->cap_obj, f->object_count + 1, sizeof *f->objects)) { p->oom = true; return NULL; }
    GerberObject *o = &f->objects[f->object_count++];
    memset(o, 0, sizeof *o);
    o->kind = kind;
    o->dark = p->dark;
    o->aperture = p->current;
    o->line = p->line;
    const GerberAttribute *a = attr_get(&p->to, ".N");
    if (a && a->value_count) o->net = a->values[0];
    a = attr_get(&p->to, ".P");
    if (a && a->value_count >= 2) { o->pin_ref = a->values[0]; o->pin_number = a->values[1]; }
    a = attr_get(&p->to, ".C");
    if (a && a->value_count) o->component = a->values[0];
    switch (kind) {
    case GERBER_FLASH: f->flash_count++; break;
    case GERBER_DRAW: f->draw_count++; break;
    case GERBER_REGION: f->region_count++; break;
    case GERBER_ARC: f->arc_count++; break;
    }
    return o;
}

static void close_contour(P *p)
{
    if (p->contour_count == 0) return;
    if (p->contour_count < 2) { p->contour_count = 0; return; }   /* a lone D02 point is not a contour */
    size_t keep = p->line;
    p->line = p->contour_line;
    G2Point first = p->contour[0], last = p->contour[p->contour_count - 1];
    if (first.x != last.x || first.y != last.y) issue(p, GERBER_ISSUE_REGION_NOT_CLOSED, GERBER_ERROR, "a region contour does not end at its start");
    GerberObject *o = new_object(p, GERBER_REGION);
    if (o) {
        o->aperture = SIZE_MAX;
        o->contour = arena_alloc_array(p->a, sizeof *o->contour, p->contour_count);
        if (!o->contour) p->oom = true;
        else { memcpy((G2Point *)o->contour, p->contour, p->contour_count * sizeof *o->contour); o->contour_count = p->contour_count; o->at = first; }
    }
    p->contour_count = 0;
    p->line = keep;
}

static void contour_add(P *p, G2Point q)
{
    if (!p->contour_count) p->contour_line = p->line;
    if (!grow(p->a, (void **)&p->contour, &p->contour_cap, p->contour_count + 1, sizeof *p->contour)) { p->oom = true; return; }
    p->contour[p->contour_count++] = q;
}

static void operation(P *p, const char *s, size_t n)
{
    G2Point to = p->point, ij = {0, 0};
    size_t i = 0;
    int op = 0;
    if (!p->have_fs || !p->have_mo) {
        if (!p->no_format_reported) issue(p, GERBER_ISSUE_NO_FORMAT, GERBER_ERROR, "coordinates before FS and MO");
        p->no_format_reported = true;
        return;
    }
    while (i < n) {
        char c = s[i++];
        if (c == 'D') {
            int d = 0;
            size_t st = i;
            while (i < n && s[i] >= '0' && s[i] <= '9') d = d * 10 + (s[i++] - '0');
            if (i == st || i != n) { issue(p, GERBER_ISSUE_SYNTAX, GERBER_ERROR, "bad operation code"); return; }
            op = d;
            break;
        }
        size_t used = 0;
        G2Nm v;
        if (c != 'X' && c != 'Y' && c != 'I' && c != 'J') { issue(p, GERBER_ISSUE_SYNTAX, GERBER_ERROR, "unexpected character in a coordinate word"); return; }
        if (!coord(p, s + i, n - i, &used, &v)) { issue(p, GERBER_ISSUE_COORD_RANGE, GERBER_ERROR, "coordinate is not a number within +-1 m"); return; }
        i += used;
        if (c == 'X') to.x = v; else if (c == 'Y') to.y = v; else if (c == 'I') ij.x = v; else ij.y = v;
    }
    if (op < 1 || op > 3) { issue(p, GERBER_ISSUE_SYNTAX, GERBER_ERROR, "a coordinate word ends with D01, D02 or D03"); return; }
    if (op == 2) {
        if (p->region) close_contour(p), contour_add(p, to);
        p->point = to;
        return;
    }
    if (op == 3) {
        if (p->region) { issue(p, GERBER_ISSUE_D03_IN_REGION, GERBER_ERROR, "D03 inside a region"); p->point = to; return; }
        if (p->current == SIZE_MAX) { issue(p, GERBER_ISSUE_UNDEFINED_APERTURE, GERBER_ERROR, "D03 with no aperture selected"); p->point = to; return; }
        GerberObject *o = new_object(p, GERBER_FLASH);
        if (o) o->at = to;
        p->point = to;
        return;
    }
    /* D01 */
    if (p->region) {
        if (!p->contour_count) contour_add(p, p->point);
        if (p->mode != 1) issue(p, GERBER_ISSUE_ARC_UNSUPPORTED, GERBER_WARNING, "circular segment in a region: kept as a chord");
        contour_add(p, to);
        p->point = to;
        return;
    }
    if (p->current == SIZE_MAX) { issue(p, GERBER_ISSUE_UNDEFINED_APERTURE, GERBER_ERROR, "D01 with no aperture selected"); p->point = to; return; }
    GerberObject *o = new_object(p, p->mode == 1 ? GERBER_DRAW : GERBER_ARC);
    if (o) {
        o->at = p->point;
        o->end = to;
        o->centre_offset = ij;
        const GerberAperture *ap = &p->f->apertures[p->current];
        if (p->mode != 1) issue(p, GERBER_ISSUE_ARC_UNSUPPORTED, GERBER_WARNING, "G02/G03 circular interpolation is not supported");
        else if (ap->type == GERBER_AP_CIRCLE && ap->shape_count == 1) {
            o->has_capsule = true;
            o->capsule.a = p->point; o->capsule.b = to; o->capsule.radius = ap->shapes[0].as.circle.radius;
        } else {
            issue(p, GERBER_ISSUE_NON_CIRCULAR_DRAW, GERBER_WARNING, "D01 with a non-circular aperture: no shape");
        }
    }
    p->point = to;
}

static void select_aperture(P *p, int d)
{
    for (size_t k = 0; k < p->f->aperture_count; k++)
        if (p->f->apertures[k].number == d) { p->current = k; return; }
    p->current = SIZE_MAX;
    issue(p, GERBER_ISSUE_UNDEFINED_APERTURE, GERBER_ERROR, "Dnn selects an aperture that is not defined");
}

static bool starts(const char *s, size_t n, const char *w) { size_t k = strlen(w); return n >= k && !memcmp(s, w, k); }

/* One function-code word (without its '*'). Returns false at M02. */
static bool function_word(P *p, const char *s, size_t n)
{
    if (!n) return true;
    if (starts(s, n, "G04")) return true;
    if (starts(s, n, "M02")) { p->f->saw_m02 = true; return false; }
    if (s[0] == 'G') {
        int g = 0;
        size_t i = 1;
        while (i < n && s[i] >= '0' && s[i] <= '9') g = g * 10 + (s[i++] - '0');
        switch (g) {
        case 1: case 2: case 3: p->mode = g; break;
        case 36: if (p->region) issue(p, GERBER_ISSUE_SYNTAX, GERBER_ERROR, "G36 inside a region"); p->region = true; p->contour_count = 0; return true;
        case 37: close_contour(p); p->region = false; return true;
        case 74: case 75: return true;                                  /* quadrant mode: only matters for arcs */
        case 54: if (i < n && s[i] == 'D') { int d = 0; i++; while (i < n && s[i] >= '0' && s[i] <= '9') d = d * 10 + (s[i++] - '0'); select_aperture(p, d); } return true;
        case 90: return true;
        case 91: issue(p, GERBER_ISSUE_UNSUPPORTED_FORMAT, GERBER_ERROR, "G91 incremental coordinates"); return true;
        case 70: p->f->units = GERBER_UNITS_INCH; p->have_mo = true; issue(p, GERBER_ISSUE_UNSUPPORTED_COMMAND, GERBER_WARNING, "deprecated G70 (inch)"); return true;
        case 71: p->f->units = GERBER_UNITS_MM; p->have_mo = true; issue(p, GERBER_ISSUE_UNSUPPORTED_COMMAND, GERBER_WARNING, "deprecated G71 (mm)"); return true;
        default: issue(p, GERBER_ISSUE_UNSUPPORTED_COMMAND, GERBER_WARNING, dupn(p->a, s, n)); return true;
        }
        if (i < n) operation(p, s + i, n - i);                          /* deprecated combined form G01X..D01 */
        return true;
    }
    if (s[0] == 'D') {
        int d = 0;
        size_t i = 1;
        while (i < n && s[i] >= '0' && s[i] <= '9') d = d * 10 + (s[i++] - '0');
        if (i != n) { issue(p, GERBER_ISSUE_SYNTAX, GERBER_ERROR, "bad D code"); return true; }
        if (d >= 10) select_aperture(p, d);
        else operation(p, s, n);                                         /* a bare D01/D02/D03 at the current point */
        return true;
    }
    if (s[0] == 'X' || s[0] == 'Y' || s[0] == 'I' || s[0] == 'J') { operation(p, s, n); return true; }
    if (starts(s, n, "M00") || starts(s, n, "M01")) { issue(p, GERBER_ISSUE_UNSUPPORTED_COMMAND, GERBER_WARNING, "deprecated M00/M01"); return true; }
    issue(p, GERBER_ISSUE_SYNTAX, GERBER_ERROR, dupn(p->a, s, n));
    return true;
}

/* One extended-command block %...% split into its words. */
static void extended(P *p, const char *s, size_t n)
{
    /* Words end with '*'; AM keeps all of them. */
    if (starts(s, n, "AM")) {
        const char *star = memchr(s, '*', n);
        if (!star) { issue(p, GERBER_ISSUE_BAD_MACRO, GERBER_ERROR, "AM without '*'"); return; }
        if (!grow(p->a, (void **)&p->macros, &p->macro_cap, p->macro_count + 1, sizeof *p->macros)) { p->oom = true; return; }
        Macro *m = &p->macros[p->macro_count];
        m->name = dupn(p->a, s + 2, (size_t)(star - s - 2));
        m->line = p->line;
        size_t words = 0, cap = 0;
        const char **list = NULL;
        const char *w = star + 1, *end = s + n;
        while (w < end) {
            const char *e = memchr(w, '*', (size_t)(end - w));
            if (!e) e = end;
            size_t k = (size_t)(e - w);
            const char *t = w;
            while (k && (*t == '\r' || *t == '\n' || *t == ' ')) { t++; k--; }
            if (k) {   /* line breaks are not part of a word anywhere in it (KiCad wraps long outlines) */
                if (!grow(p->a, (void **)&list, &cap, words + 1, sizeof *list)) { p->oom = true; return; }
                char *word = arena_alloc(p->a, k + 1);
                if (!word) { p->oom = true; return; }
                size_t m = 0;
                for (size_t q = 0; q < k; q++) if (t[q] != '\r' && t[q] != '\n') word[m++] = t[q];
                word[m] = 0;
                list[words++] = word;
            }
            w = e + 1;
        }
        m->words = list;
        m->word_count = words;
        p->macro_count++;
        return;
    }
    const char *w = s, *end = s + n;
    while (w < end) {
        const char *e = memchr(w, '*', (size_t)(end - w));
        if (!e) { issue(p, GERBER_ISSUE_SYNTAX, GERBER_ERROR, "an extended command word without '*'"); return; }
        size_t k = (size_t)(e - w);
        while (k && (*w == '\r' || *w == '\n')) { w++; k--; }
        if (starts(w, k, "FS")) {
            /* FSLAXnmYnm: leading zeros omitted, absolute */
            if (k != 10 || w[2] != 'L' || w[3] != 'A' || w[4] != 'X' || w[7] != 'Y' || w[5] != w[8] || w[6] != w[9]) {
                issue(p, GERBER_ISSUE_UNSUPPORTED_FORMAT, GERBER_ERROR, "only FSLAXnmYnm (leading zeros omitted, absolute, same X/Y format)");
            } else {
                p->f->int_digits = w[5] - '0';
                p->f->dec_digits = w[6] - '0';
                p->have_fs = p->f->dec_digits >= 0 && p->f->dec_digits <= 9;
            }
        } else if (starts(w, k, "MO")) {
            if (k == 4 && !memcmp(w, "MOMM", 4)) p->f->units = GERBER_UNITS_MM;
            else if (k == 4 && !memcmp(w, "MOIN", 4)) p->f->units = GERBER_UNITS_INCH;
            else { issue(p, GERBER_ISSUE_SYNTAX, GERBER_ERROR, "MO is MM or IN"); w = e + 1; continue; }
            p->have_mo = true;
        } else if (starts(w, k, "AD")) {
            if (!p->have_mo) issue(p, GERBER_ISSUE_NO_FORMAT, GERBER_ERROR, "AD before MO");
            else cmd_ad(p, w, k);
        } else if (starts(w, k, "LP")) {
            if (k == 3 && w[2] == 'D') p->dark = true;
            else if (k == 3 && w[2] == 'C') p->dark = false;
            else issue(p, GERBER_ISSUE_SYNTAX, GERBER_ERROR, "LP is D or C");
        } else if (starts(w, k, "TF") || starts(w, k, "TA") || starts(w, k, "TO")) {
            GerberAttribute at;
            if (!attr_parse(p, w + 2, k - 2, &at)) { p->oom = true; return; }
            attr_set(p, w[1] == 'F' ? &p->tf : w[1] == 'A' ? &p->ta : &p->to, at);
        } else if (starts(w, k, "TD")) {
            if (k == 2) { p->ta.count = 0; p->to.count = 0; }
            else { char *name = dupn(p->a, w + 2, k - 2); if (name) { attr_delete(&p->ta, name); attr_delete(&p->to, name); } }
        } else if (starts(w, k, "SR")) {
            if (k > 2) issue(p, GERBER_ISSUE_UNSUPPORTED_COMMAND, GERBER_ERROR, "SR step and repeat is not supported");
        } else {
            issue(p, GERBER_ISSUE_UNSUPPORTED_COMMAND, GERBER_WARNING, dupn(p->a, w, k));
        }
        w = e + 1;
    }
}

GerberStatus gerber_parse(Arena *arena, const char *t, size_t n, GerberFile *out)
{
    if (!arena || !t || !out) return GERBER_INVALID_ARGUMENT;
    memset(out, 0, sizeof *out);
    P p;
    memset(&p, 0, sizeof p);
    p.a = arena; p.f = out; p.line = 1; p.dark = true; p.mode = 1; p.current = SIZE_MAX;
    size_t i = 0;
    bool running = true;
    while (i < n && running && !p.oom) {
        char c = t[i];
        if (c == '\n') { p.line++; i++; continue; }
        if (c == '\r' || c == ' ' || c == '\t') { i++; continue; }
        size_t word_line = p.line;
        if (c == '%') {
            size_t j = i + 1;
            while (j < n && t[j] != '%') j++;
            if (j >= n) { p.line = word_line; issue(&p, GERBER_ISSUE_SYNTAX, GERBER_ERROR, "unterminated '%'"); break; }
            p.line = word_line;
            extended(&p, t + i + 1, j - i - 1);
            for (size_t k = i; k < j; k++) if (t[k] == '\n') p.line++;
            i = j + 1;
            continue;
        }
        size_t j = i;
        while (j < n && t[j] != '*' && t[j] != '%') j++;
        if (j >= n || t[j] != '*') { issue(&p, GERBER_ISSUE_SYNTAX, GERBER_ERROR, "a word without '*'"); break; }
        size_t len = j - i;
        while (len && (t[i + len - 1] == '\r' || t[i + len - 1] == '\n' || t[i + len - 1] == ' ')) len--;
        running = function_word(&p, t + i, len);
        for (size_t k = i; k < j; k++) if (t[k] == '\n') p.line++;
        i = j + 1;
    }
    if (p.oom) return GERBER_OUT_OF_MEMORY;
    if (p.region) { issue(&p, GERBER_ISSUE_SYNTAX, GERBER_ERROR, "G36 region not closed by G37"); }
    if (!out->saw_m02) issue(&p, GERBER_ISSUE_MISSING_M02, GERBER_WARNING, "no M02 at the end");
    out->attributes = p.tf.items;
    out->attribute_count = p.tf.count;
    if (p.oom) return GERBER_OUT_OF_MEMORY;
    return out->error_count ? GERBER_CONTENT_ERROR : GERBER_OK;
}

GerberStatus gerber_parse_path(Arena *arena, const char *path, GerberFile *out)
{
    if (!arena || !path || !out) return GERBER_INVALID_ARGUMENT;
    memset(out, 0, sizeof *out);
    FILE *f = fopen(path, "rb");
    if (!f) return GERBER_IO_ERROR;
    size_t cap = 1 << 16, n = 0;
    char *buf = arena_alloc(arena, cap);
    while (buf) {
        n += fread(buf + n, 1, cap - n, f);
        if (n < cap) break;
        char *more = arena_alloc(arena, cap * 2);
        if (more) memcpy(more, buf, n);
        buf = more;
        cap *= 2;
    }
    bool bad = ferror(f) != 0;
    fclose(f);
    if (!buf) return GERBER_OUT_OF_MEMORY;
    if (bad) return GERBER_IO_ERROR;
    return gerber_parse(arena, buf, n, out);
}

/* ------------------------------------------------------------------------------------------------ queries */

const GerberAttribute *gerber_file_attribute(const GerberFile *f, const char *name)
{
    if (!f || !name) return NULL;
    for (size_t i = 0; i < f->attribute_count; i++) if (!strcmp(f->attributes[i].name, name)) return &f->attributes[i];
    return NULL;
}

const GerberAperture *gerber_find_aperture(const GerberFile *f, int number)
{
    if (!f) return NULL;
    for (size_t i = 0; i < f->aperture_count; i++) if (f->apertures[i].number == number) return &f->apertures[i];
    return NULL;
}

static G2Point add(G2Point a, G2Point b) { G2Point r = {a.x + b.x, a.y + b.y}; return r; }

G2Status gerber_flash_shape(const GerberFile *f, const GerberObject *o, size_t k, G2Shape *out)
{
    if (!f || !o || !out || o->kind != GERBER_FLASH || o->aperture >= f->aperture_count) return G2_ERR_ARGUMENT;
    const GerberAperture *ap = &f->apertures[o->aperture];
    if (k >= ap->shape_count) return G2_ERR_ARGUMENT;
    *out = ap->shapes[k];
    switch (out->kind) {
    case G2_SHAPE_CIRCLE: out->as.circle.center = add(out->as.circle.center, o->at); break;
    case G2_SHAPE_CAPSULE: out->as.capsule.a = add(out->as.capsule.a, o->at); out->as.capsule.b = add(out->as.capsule.b, o->at); break;
    case G2_SHAPE_ROUNDED_RECT:
        out->as.rounded_rect.core.min = add(out->as.rounded_rect.core.min, o->at);
        out->as.rounded_rect.core.max = add(out->as.rounded_rect.core.max, o->at);
        break;
    case G2_SHAPE_POLYGON:
        return G2_ERR_UNSUPPORTED;   /* polygons share their vertex array: use the aperture's shape plus o->at */
    default: return G2_ERR_UNSUPPORTED;
    }
    return g2_shape_validate(*out);
}

GerberStatus gerber_outline(Arena *arena, const GerberFile *f, GerberOutline *o)
{
    if (!arena || !f || !o) return GERBER_INVALID_ARGUMENT;
    memset(o, 0, sizeof *o);
    size_t n = 0;
    for (size_t i = 0; i < f->object_count; i++) n += f->objects[i].kind == GERBER_DRAW;
    o->segments = n;
    if (!n) return GERBER_OK;
    size_t *seg = arena_alloc_array(arena, sizeof *seg, n);
    bool *used = arena_alloc_array(arena, sizeof *used, n);
    if (!seg || !used) return GERBER_OUT_OF_MEMORY;
    n = 0;
    for (size_t i = 0; i < f->object_count; i++) if (f->objects[i].kind == GERBER_DRAW) seg[n++] = i;
    memset(used, 0, n);
    for (size_t k = 0; k < n; k++) {
        const GerberObject *d = &f->objects[seg[k]];
        const G2Point pts[2] = {d->at, d->end};
        for (int e = 0; e < 2; e++) {
            if (!k && !e) { o->bounds.min = o->bounds.max = pts[e]; continue; }
            if (pts[e].x < o->bounds.min.x) o->bounds.min.x = pts[e].x;
            if (pts[e].y < o->bounds.min.y) o->bounds.min.y = pts[e].y;
            if (pts[e].x > o->bounds.max.x) o->bounds.max.x = pts[e].x;
            if (pts[e].y > o->bounds.max.y) o->bounds.max.y = pts[e].y;
        }
    }
    for (size_t k = 0; k < n; k++) {
        if (used[k]) continue;
        used[k] = true;
        o->loops++;
        G2Point head = f->objects[seg[k]].at, tail = f->objects[seg[k]].end;
        bool closed = head.x == tail.x && head.y == tail.y, more = true;
        while (!closed && more) {
            more = false;
            for (size_t j = 0; j < n && !more; j++) {
                if (used[j]) continue;
                const GerberObject *d = &f->objects[seg[j]];
                if (d->at.x == tail.x && d->at.y == tail.y) { tail = d->end; used[j] = more = true; }
                else if (d->end.x == tail.x && d->end.y == tail.y) { tail = d->at; used[j] = more = true; }
            }
            closed = head.x == tail.x && head.y == tail.y;
        }
        if (!closed) o->open_loops++;
    }
    o->closed = o->open_loops == 0;
    return GERBER_OK;
}

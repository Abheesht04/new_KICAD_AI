/* The C placer (src/v2_src/placement.c): parsing with located errors, exact legality boundaries, a hand-computed
 * objective, constructive and annealed placements that are legal, deterministic and no worse than their start, and
 * the same on a problem the Python bridge wrote for a real board (tests/golden/placement/divider.problem). */
#include "placement.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

#define MM(v) ((int64_t)(v) * 1000000)

/* A 0603-sized two-pad part (body 3 x 1.5 mm, pads 0.8 x 0.8 mm at x -0.8 / +0.8) at all four rotations, in the
 * part frame. Rotation k turns (x, y) by k x 90 degrees: (x, y) -> (-y, x). */
static void rot_point(int k, int64_t x, int64_t y, int64_t *ox, int64_t *oy)
{
    for (int i = 0; i < k; i++) { int64_t t = x; x = -y; y = t; }
    *ox = x; *oy = y;
}

static void rot_rect(int k, int64_t x1, int64_t y1, int64_t x2, int64_t y2, int64_t r[4])
{
    int64_t ax, ay, bx, by;
    rot_point(k, x1, y1, &ax, &ay);
    rot_point(k, x2, y2, &bx, &by);
    r[0] = ax < bx ? ax : bx; r[1] = ay < by ? ay : by; r[2] = ax < bx ? bx : ax; r[3] = ay < by ? by : ay;
}

static void mm_text(char *out, int64_t nm)
{
    int64_t a = nm < 0 ? -nm : nm;
    sprintf(out, "%s%lld.%06lld", nm < 0 ? "-" : "", (long long)(a / 1000000), (long long)(a % 1000000));
}

static size_t put_rect(char *dst, const char *head, const int64_t r[4])
{
    char a[32], b[32], c[32], d[32];
    mm_text(a, r[0]); mm_text(b, r[1]); mm_text(c, r[2]); mm_text(d, r[3]);
    return (size_t)sprintf(dst, "(%s %s %s %s %s)", head, a, b, c, d);
}

static size_t part_text(char *dst, const char *ref, int net_a, int net_b, const char *extra)
{
    size_t n = (size_t)sprintf(dst, " (part \"%s\" %s", ref, extra);
    for (int k = 0; k < 4; k++) {
        int64_t r[4], cx, cy;
        n += (size_t)sprintf(dst + n, " (shape %d ", k * 90);
        rot_rect(k, -1500000, -750000, 1500000, 750000, r);
        n += put_rect(dst + n, "body", r);
        for (int p = 0; p < 2; p++) {
            int64_t px = p ? 800000 : -800000;
            char x[32], y[32], r0[32], r1[32], r2[32], r3[32];
            rot_point(k, px, 0, &cx, &cy);
            rot_rect(k, px - 400000, -400000, px + 400000, 400000, r);
            mm_text(x, cx); mm_text(y, cy);
            mm_text(r0, r[0]); mm_text(r1, r[1]); mm_text(r2, r[2]); mm_text(r3, r[3]);
            n += (size_t)sprintf(dst + n, " (pad %d %s %s %s %s %s %s)", p ? net_b : net_a, x, y, r0, r1, r2, r3);
        }
        n += (size_t)sprintf(dst + n, ")");
    }
    n += (size_t)sprintf(dst + n, ")\n");
    return n;
}

static char *problem_text(const char *board, const char *extra_forms, const char *r1_extra, const char *r2_extra,
                          bool with_r3)
{
    char *t = malloc(1 << 16);
    size_t n = (size_t)sprintf(t, "(placement_problem (version 1)\n %s (edge 0.3) (gap 0.25) (step 1)\n"
        " (weights (placement_area_mm2 0.01) (decoupling_mm 1) (keepout_edge_mm 0.5))\n (nets \"A\" \"B\" \"C\")\n%s\n",
        board, extra_forms);
    n += part_text(t + n, "R1", 0, 1, r1_extra);
    n += part_text(t + n, "R2", 1, 2, r2_extra);
    if (with_r3) n += part_text(t + n, "R3", 2, 0, "");
    sprintf(t + n, ")\n");
    return t;
}

static PlStatus parse(Arena *a, const char *text, PlProblem *p, PlError *e)
{
    return pl_problem_parse(a, text, strlen(text), p, e);
}

static bool all_legal(const PlProblem *p, const PlPlacement *pl)
{
    for (size_t i = 0; i < p->part_count; i++)
        if (!pl->placed[i] || !pl_part_legal(p, pl, i)) return false;
    return true;
}

static void test_parse_errors(void)
{
    Arena a;
    PlProblem p;
    PlError e;
    arena_init(&a, 1 << 16);
    CHECK(parse(&a, "(placement_problem (version 1)", &p, &e) == PL_PARSE_ERROR && e.line >= 1);
    CHECK(parse(&a, "(placement_problem (version 2) (board 10 10) (edge 0) (gap 0) (step 1))", &p, &e) == PL_PARSE_ERROR && e.item == 1);
    CHECK(parse(&a, "(placement_problem (version 1) (board 10 10) (edge 0) (gap 0) (step 1) (wings 3))", &p, &e) == PL_PARSE_ERROR && e.item == 6);
    CHECK(parse(&a, "(placement_problem (version 1) (board 10 10) (edge 0) (gap 0) (step 0))", &p, &e) == PL_INVALID_INPUT && e.item == 5);
    CHECK(parse(&a, "(placement_problem (version 1) (board 10 10) (edge 0) (gap 0))", &p, &e) == PL_PARSE_ERROR);
    CHECK(parse(&a, "(placement_problem (version 1) (board 10 10) (edge 0) (gap 0) (step 1) (keepout 5 0 1 1 8))", &p, &e) == PL_INVALID_INPUT && e.item == 6);
    CHECK(parse(&a, "(placement_problem (version 1) (board 2000 10) (edge 0) (gap 0) (step 1))", &p, &e) == PL_RANGE_ERROR);
    CHECK(parse(&a, "(placement_problem (version 1) (board 10 10) (edge 0) (gap 0) (step 1) (nets \"A\")"
                    " (part \"R1\" (shape 0 (body 0 0 1 1))))", &p, &e) == PL_INVALID_INPUT && e.item == 7);   /* 3 rotations missing */
    char *t = problem_text("(board 20 10)", "", "(fixed)", "", false);   /* fixed without (at); R1 is form 8 */
    CHECK(parse(&a, t, &p, &e) == PL_INVALID_INPUT && e.item == 8);
    free(t);
    t = problem_text("(board 20 10)", "(decoupling \"C9\" 0 \"R1\")", "", "", false);
    CHECK(parse(&a, t, &p, &e) == PL_INVALID_INPUT);
    free(t);
    arena_destroy(&a);
}

static void test_exact_cost_and_gap_boundary(void)
{
    Arena a;
    PlProblem p;
    PlPlacement pl;
    char *t = problem_text("(board 20 10)", "", "", "", false);
    arena_init(&a, 1 << 16);
    CHECK(parse(&a, t, &p, NULL) == PL_OK && p.part_count == 2 && p.net_count == 3);
    CHECK(pl_placement_alloc(&a, &p, &pl) == PL_OK);
    pl.placed[0] = pl.placed[1] = true;
    pl.x[0] = MM(5); pl.y[0] = MM(5); pl.x[1] = MM(10); pl.y[1] = MM(5);
    /* Net B: R1 pad 2 at x 5.8, R2 pad 1 at x 9.2 -> 3.4 mm. Area: x 3.5..11.5, y 4.25..5.75 = 12 mm^2 x 0.01 = 0.12 mm. */
    CHECK(pl_cost(&p, &pl) == 3400000 + 120000);
    CHECK(all_legal(&p, &pl));
    /* Courtyard gap 0.25 mm: R1's body ends at 6.5 mm, so R2's body (x - 1.5) may start at 6.75 mm + 2 nm, not closer. */
    pl.x[1] = MM(8) + 250000 + 2;
    CHECK(pl_part_legal(&p, &pl, 1) && pl_part_legal(&p, &pl, 0));
    pl.x[1] = MM(8) + 250000 + 1;
    CHECK(!pl_part_legal(&p, &pl, 1) && !pl_part_legal(&p, &pl, 0));
    /* Pad to edge 0.3 mm: R1's left pad starts at x - 1.2, so x = 1.5 mm + 2 nm is the closest legal spot. */
    pl.x[1] = MM(15);
    pl.x[0] = 1500000 + 2;
    CHECK(pl_part_legal(&p, &pl, 0));
    pl.x[0] = 1500000 + 1;
    CHECK(!pl_part_legal(&p, &pl, 0));
    /* Outside the outline. */
    pl.x[0] = MM(19);
    CHECK(!pl_part_legal(&p, &pl, 0));
    free(t);
    arena_destroy(&a);
}

static void test_constructive_and_anneal(void)
{
    Arena a;
    PlProblem p;
    PlPlacement c, x, y;
    PlStats s1, s2;
    char *t = problem_text("(board 20 10)", "", "", "", true);
    arena_init(&a, 1 << 16);
    CHECK(parse(&a, t, &p, NULL) == PL_OK);
    CHECK(pl_placement_alloc(&a, &p, &c) == PL_OK && pl_placement_alloc(&a, &p, &x) == PL_OK && pl_placement_alloc(&a, &p, &y) == PL_OK);
    CHECK(pl_constructive(&a, &p, &c) == PL_OK && all_legal(&p, &c));
    for (size_t i = 0; i < p.part_count; i++) CHECK(c.x[i] % MM(1) == 0 && c.y[i] % MM(1) == 0);   /* on the 1 mm grid */
    int64_t start = pl_cost(&p, &c);
    CHECK(pl_anneal(&a, &p, &c, 7, 200, &x, &s1) == PL_OK && all_legal(&p, &x));
    CHECK(pl_anneal(&a, &p, &c, 7, 200, &y, &s2) == PL_OK);
    CHECK(s1.start_cost == start && s1.best_cost == pl_cost(&p, &x) && s1.best_cost <= start);
    CHECK(s1.moves > 0 && s1.legal_moves > 0 && s1.refine_moves > 0);
    CHECK(s1.best_cost == s2.best_cost && s1.moves == s2.moves && s1.accepted == s2.accepted);   /* same seed, same run */
    for (size_t i = 0; i < p.part_count; i++) CHECK(x.x[i] == y.x[i] && x.y[i] == y.y[i] && x.rot[i] == y.rot[i]);
    CHECK(pl_anneal(&a, &p, &c, 8, 200, &y, &s2) == PL_OK && all_legal(&p, &y) && s2.best_cost <= start);
    /* A triangle of three two-pin nets: the three parts end up within a few mm of each other. */
    CHECK(s1.best_cost < MM(12));
    free(t);
    arena_destroy(&a);
}

static void test_fixed_keepout_and_failure(void)
{
    Arena a;
    PlProblem p;
    PlPlacement c, x;
    arena_init(&a, 1 << 16);
    char *t = problem_text("(board 20 10)", "", "(fixed) (at 15 5 90)", "", true);
    CHECK(parse(&a, t, &p, NULL) == PL_OK);
    CHECK(pl_placement_alloc(&a, &p, &c) == PL_OK && pl_placement_alloc(&a, &p, &x) == PL_OK);
    CHECK(pl_constructive(&a, &p, &c) == PL_OK);
    CHECK(pl_anneal(&a, &p, &c, 1, 100, &x, NULL) == PL_OK && all_legal(&p, &x));
    CHECK(x.x[0] == MM(15) && x.y[0] == MM(5) && x.rot[0] == 1);   /* the fixed part never moves */
    free(t);

    /* A board keepout over x < 12 mm (no bodies): every body ends up right of it. */
    t = problem_text("(board 20 10)", "(keepout 0 0 12 10 8)", "", "", true);
    CHECK(parse(&a, t, &p, NULL) == PL_OK);
    CHECK(pl_placement_alloc(&a, &p, &c) == PL_OK && pl_placement_alloc(&a, &p, &x) == PL_OK);
    CHECK(pl_constructive(&a, &p, &c) == PL_OK);
    CHECK(pl_anneal(&a, &p, &c, 3, 100, &x, NULL) == PL_OK && all_legal(&p, &x));
    for (size_t i = 0; i < p.part_count; i++) {
        const PlShape *s = &p.parts[i].shape[x.rot[i]];
        CHECK(s->body.x1 + x.x[i] >= MM(12));
        CHECK(s->body.x1 + c.x[i] >= MM(12));
    }
    free(t);

    /* Nowhere to go: a 2 x 2 mm board. Parts stay unplaced; annealing refuses the incomplete start. */
    t = problem_text("(board 2 2)", "", "", "", false);
    CHECK(parse(&a, t, &p, NULL) == PL_OK);
    CHECK(pl_placement_alloc(&a, &p, &c) == PL_OK && pl_placement_alloc(&a, &p, &x) == PL_OK);
    CHECK(pl_constructive(&a, &p, &c) == PL_NO_LEGAL_POSITION && !c.placed[0] && !c.placed[1]);
    CHECK(pl_anneal(&a, &p, &c, 1, 10, &x, NULL) == PL_ILLEGAL_START);
    free(t);
    arena_destroy(&a);
}

static void test_exp_table(void)
{
    CHECK(pl_exp_neg_q32(0) == (UINT64_C(1) << 32));
    CHECK(pl_exp_neg_q32(256) == 1580030169u);         /* e^-1 */
    CHECK(pl_exp_neg_q32(17 * 256) == 0);
    uint64_t prev = pl_exp_neg_q32(0);
    for (uint64_t q = 1; q < 17 * 256; q++) {
        uint64_t v = pl_exp_neg_q32(q);
        CHECK(v <= prev);
        prev = v;
    }
    /* e^-0.5 = 0.60653066 -> Q32 2604950... within 2 ulp-of-1e-6 of the true value */
    uint64_t half = pl_exp_neg_q32(128), truth = UINT64_C(2605029347);
    CHECK(half + 4 >= truth && half <= truth + 4);
}

static void test_golden_divider(void)
{
    Arena a;
    PlProblem p;
    PlError e;
    PlPlacement c, x;
    PlStats s;
    arena_init(&a, 1 << 20);
    PlStatus st = pl_problem_load(&a, PLACEMENT_GOLDEN_DIR "/divider.problem", &p, &e);
    CHECK(st == PL_OK);
    if (st != PL_OK) { fprintf(stderr, "golden: %s line %zu item %zu: %s\n", pl_status_name(st), e.line, e.item, e.message); arena_destroy(&a); return; }
    CHECK(p.part_count == 3);
    CHECK(pl_placement_alloc(&a, &p, &c) == PL_OK && pl_placement_alloc(&a, &p, &x) == PL_OK);
    CHECK(pl_constructive(&a, &p, &c) == PL_OK && all_legal(&p, &c));
    CHECK(pl_anneal(&a, &p, &c, 1, 300, &x, &s) == PL_OK && all_legal(&p, &x) && s.best_cost <= s.start_cost);
    arena_destroy(&a);
}

int main(void)
{
    test_parse_errors();
    test_exact_cost_and_gap_boundary();
    test_constructive_and_anneal();
    test_fixed_keepout_and_failure();
    test_exp_table();
    test_golden_divider();
    if (failures) { fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
    puts("placement: all checks passed");
    return 0;
}

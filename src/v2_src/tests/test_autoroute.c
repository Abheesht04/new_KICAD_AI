/* autoroute (src/v2_src/autoroute.c): the deterministic two-layer router.
 *
 * Fixtures (tests/fixtures/autoroute): two boards written by Copper's own compiler from its templates (an LED
 * indicator and the ESP32 + LED board), placed and unrouted. They are Copper-generated, not KiCad exports.
 * The router's output is checked here by code independent of the router: connectivity by a union-find over
 * copper contacts, and every pair of different-net copper items by exact integer distance. KiCad's own DRC of the
 * same boards is run by manus/selftest.ps1 (KiCad 10.0.5 on this machine: a development check). */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "autoroute.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FIX COPPER_TEST_SOURCE_DIR "/tests/fixtures/autoroute/"

static Arena arena;

static AutorouteRules default_rules(void)
{
    AutorouteRules r;
    memset(&r, 0, sizeof r);
    r.grid_nm = 250000; r.track_nm = 250000; r.clearance_nm = 300000; r.edge_clearance_nm = 500000;
    r.via_size_nm = 600000; r.via_drill_nm = 300000; r.via_cost = 8; r.passes = 4; r.max_cells = 4000000;
    return r;
}

static void load(const char *path, Board *b)
{
    BoardDiag d;
    memset(&d, 0, sizeof d);
    BoardStatus st = board_load_kicad(path, &arena, b, &d);
    if (st != BOARD_OK) fprintf(stderr, "load %s: %s\n", path, d.message);
    assert(st == BOARD_OK);
}

/* ---------------------------------------------------------------- independent checks */

typedef struct Uf { size_t *p; } Uf;
static size_t find(Uf *u, size_t x) { while (u->p[x] != x) { u->p[x] = u->p[u->p[x]]; x = u->p[x]; } return x; }
static void join(Uf *u, size_t a, size_t b) { a = find(u, a); b = find(u, b); if (a != b) u->p[a] = b; }

static bool layer_has(const BoardPad *p, BoardLayer l) { return (p->layers & BOARD_LAYER_BIT(l)) != 0; }

static bool point_touches_pad(G2Point q, const BoardPad *p, BoardLayer l)
{
    G2Aabb box;
    bool in = false;
    return layer_has(p, l) && board_pad_aabb(p, &box) == BOARD_OK && g2_aabb_contains_point(box, q, &in) == G2_OK && in;
}

static bool point_on_track(G2Point q, const BoardTrack *t)
{
    bool on = false;
    return g2_point_on_segment(q, t->a, t->b, &on) == G2_OK && on;
}

/* Every net's pads are joined through copper: a track end inside a pad, on another track of the same layer, or on a
 * via; pads of one footprint with the same number that overlap count as one pad. */
static void assert_connected(const Board *b, const AutorouteResult *res)
{
    size_t np = b->pad_count, nt = res->track_count, nv = res->via_count, n = np + nt + nv;
    Uf u = {malloc(n * sizeof(size_t))};
    assert(u.p);
    for (size_t i = 0; i < n; i++) u.p[i] = i;
    for (size_t i = 0; i < np; i++)
        for (size_t j = 0; j < i; j++) {
            const BoardPad *a = &b->pads[i], *c = &b->pads[j];
            G2Aabb ba, bc;
            bool hit = false;
            if (a->footprint == c->footprint && a->number && c->number && !strcmp(a->number, c->number) &&
                board_pad_aabb(a, &ba) == BOARD_OK && board_pad_aabb(c, &bc) == BOARD_OK && g2_aabb_intersects(ba, bc, &hit) == G2_OK && hit) join(&u, i, j);
        }
    for (size_t t = 0; t < nt; t++) {
        const BoardTrack *tr = &res->tracks[t];
        G2Point ends[2] = {tr->a, tr->b};
        for (int e = 0; e < 2; e++) {
            for (size_t i = 0; i < np; i++) if (b->pads[i].net == tr->net && point_touches_pad(ends[e], &b->pads[i], tr->layer)) join(&u, np + t, i);
            for (size_t k = 0; k < nt; k++)
                if (k != t && res->tracks[k].net == tr->net && res->tracks[k].layer == tr->layer && point_on_track(ends[e], &res->tracks[k])) join(&u, np + t, np + k);
            for (size_t v = 0; v < nv; v++)
                if (res->vias[v].net == tr->net && res->vias[v].pos.x == ends[e].x && res->vias[v].pos.y == ends[e].y) join(&u, np + t, np + nt + v);
        }
    }
    for (size_t i = 0; i < np; i++)
        for (size_t j = 0; j < i; j++)
            if (b->pads[i].net && b->pads[i].net == b->pads[j].net) {
                if (find(&u, i) != find(&u, j)) {
                    const BoardNet *net = board_net_by_id(b, b->pads[i].net);
                    fprintf(stderr, "net %s: pads %s.%s and %s.%s are not connected\n", net ? net->name : "?",
                            b->footprints[b->pads[i].footprint].reference, b->pads[i].number,
                            b->footprints[b->pads[j].footprint].reference, b->pads[j].number);
                    assert(!"pads of one net are not connected");
                }
            }
    free(u.p);
}

static G2Nm hup(G2Nm w) { return w / 2 + w % 2; }

/* No two copper items of different nets closer than `clr`; nothing in a keep-out; everything inside the edge margin. */
static void assert_clear(const Board *b, const AutorouteResult *res, G2Nm clr, const AutorouteRules *r)
{
    G2Order o;
    for (size_t t = 0; t < res->track_count; t++) {
        const BoardTrack *a = &res->tracks[t];
        G2Nm ha = hup(a->width);
        for (size_t i = 0; i < b->pad_count; i++) {
            const BoardPad *p = &b->pads[i];
            G2Aabb box;
            if (p->net == a->net || !layer_has(p, a->layer)) continue;
            assert(board_pad_aabb(p, &box) == BOARD_OK);
            assert(g2_compare_segment_aabb_distance(a->a, a->b, box, ha + clr, &o) == G2_OK && o != G2_ORDER_LESS);
        }
        for (size_t k = 0; k < t; k++) {
            const BoardTrack *c = &res->tracks[k];
            if (c->net == a->net || c->layer != a->layer) continue;
            assert(g2_compare_segment_segment_distance(a->a, a->b, c->a, c->b, ha + hup(c->width) + clr, &o) == G2_OK && o != G2_ORDER_LESS);
        }
        for (size_t v = 0; v < res->via_count; v++) {
            const BoardVia *vi = &res->vias[v];
            G2Circle c = {vi->pos, hup(vi->size)};
            if (vi->net == a->net) continue;
            assert(g2_compare_segment_circle_distance(a->a, a->b, c, ha + clr, &o) == G2_OK && o != G2_ORDER_LESS);
        }
        for (size_t k = 0; r && k < r->keepout_count; k++) {
            const AutorouteKeepout *ko = &r->keepouts[k];
            if ((a->layer == BOARD_LAYER_F_CU && !ko->front) || (a->layer == BOARD_LAYER_B_CU && !ko->back)) continue;
            assert(g2_compare_segment_aabb_distance(a->a, a->b, ko->box, ha, &o) == G2_OK && o != G2_ORDER_LESS);
        }
    }
    for (size_t v = 0; v < res->via_count; v++) {
        const BoardVia *vi = &res->vias[v];
        for (size_t i = 0; i < b->pad_count; i++) {
            G2Aabb box;
            if (b->pads[i].net == vi->net) continue;
            assert(board_pad_aabb(&b->pads[i], &box) == BOARD_OK);
            assert(g2_compare_point_aabb_distance(vi->pos, box, hup(vi->size) + clr, &o) == G2_OK && o != G2_ORDER_LESS);
        }
        for (size_t k = 0; k < v; k++)
            if (res->vias[k].net != vi->net)
                assert(g2_compare_distance(vi->pos, res->vias[k].pos, hup(vi->size) + hup(res->vias[k].size) + clr, &o) == G2_OK && o != G2_ORDER_LESS);
        for (size_t k = 0; r && k < r->keepout_count; k++)
            assert(g2_compare_point_aabb_distance(vi->pos, r->keepouts[k].box, hup(vi->size), &o) == G2_OK && o != G2_ORDER_LESS);
    }
}

/* ---------------------------------------------------------------- tests */

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    assert(f);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc((size_t)n + 1);
    assert(s && fread(s, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    s[n] = 0;
    *len = (size_t)n;
    return s;
}

static void test_led_board(void)
{
    Board b;
    load(FIX "led_indicator.kicad_pcb", &b);
    AutorouteRules r = default_rules();
    AutorouteResult res;
    AutorouteStatus st = autoroute_board(&arena, &b, &r, &res);
    if (st != AUTOROUTE_ROUTED) fprintf(stderr, "%s\n", res.message);
    assert(st == AUTOROUTE_ROUTED && res.nets_routed == 3 && res.nets_failed == 0 && res.track_count > 0);
    assert_connected(&b, &res);
    assert_clear(&b, &res, r.clearance_nm, &r);

    /* Written into the KiCad file, loaded back: the same copper, and a routed board is refused a second time. */
    size_t len = 0;
    char *text = slurp(FIX "led_indicator.kicad_pcb", &len);
    const char *out = NULL;
    size_t out_len = 0;
    assert(autoroute_write_kicad(&arena, text, len, &b, &res, "led", &out, &out_len) == AUTOROUTE_ROUTED);
    FILE *f = fopen("autoroute_test_led.kicad_pcb", "wb");
    assert(f && fwrite(out, 1, out_len, f) == out_len);
    fclose(f);
    Board b2;
    load("autoroute_test_led.kicad_pcb", &b2);
    assert(b2.track_count == res.track_count && b2.via_count == res.via_count);
    AutorouteResult again;
    assert(autoroute_board(&arena, &b2, &r, &again) == AUTOROUTE_INVALID_INPUT);

    /* Deterministic: the same board and rules give byte-identical output. */
    AutorouteResult res2;
    const char *out2 = NULL;
    size_t out2_len = 0;
    assert(autoroute_board(&arena, &b, &r, &res2) == AUTOROUTE_ROUTED);
    assert(autoroute_write_kicad(&arena, text, len, &b, &res2, "led", &out2, &out2_len) == AUTOROUTE_ROUTED);
    assert(out_len == out2_len && !memcmp(out, out2, out_len));
    free(text);
}

static void test_esp32_board_with_keepout_and_widths(void)
{
    Board b;
    load(FIX "esp32_led.kicad_pcb", &b);
    AutorouteRules r = default_rules();
    /* As written by `copper design generate` for the esp32_led template (routing.json). */
    static const AutorouteNetWidth widths[] = {{"+VIN", 500000}, {"+3V3", 500000}, {"GND", 500000}};
    AutorouteKeepout ko = {{{6000000, 260000}, {54000000, 21200000}}, true, true};   /* nm */
    r.widths = widths; r.width_count = 3;
    r.keepouts = &ko; r.keepout_count = 1;
    AutorouteResult res;
    AutorouteStatus st = autoroute_board(&arena, &b, &r, &res);
    if (st != AUTOROUTE_ROUTED)
        for (size_t i = 0; i < res.net_count; i++) fprintf(stderr, "  %s %s %s\n", res.nets[i].name, autoroute_net_status_name(res.nets[i].status), res.nets[i].message);
    assert(st == AUTOROUTE_ROUTED && res.nets_failed == 0);
    assert_connected(&b, &res);
    assert_clear(&b, &res, r.clearance_nm, &r);
    for (size_t t = 0; t < res.track_count; t++) {
        const BoardNet *n = board_net_by_id(&b, res.tracks[t].net);
        bool wide = n && (!strcmp(n->name, "+VIN") || !strcmp(n->name, "+3V3") || !strcmp(n->name, "GND"));
        assert(res.tracks[t].width == (wide ? 500000 : 250000));
    }
}

static void test_fail_closed(void)
{
    Board b;
    load(FIX "led_indicator.kicad_pcb", &b);
    AutorouteRules r = default_rules();
    AutorouteResult res;
    /* A keep-out across the whole board leaves nothing routable: every net FAILED with a reason, no copper. */
    AutorouteKeepout all = {{{0, 0}, {30000000, 20000000}}, true, true};
    r.keepouts = &all; r.keepout_count = 1;
    assert(autoroute_board(&arena, &b, &r, &res) == AUTOROUTE_PARTIAL);
    assert(res.nets_routed == 0 && res.track_count == 0 && res.via_count == 0);
    for (size_t i = 0; i < res.net_count; i++) assert(res.nets[i].status == AUTOROUTE_NET_FAILED && res.nets[i].message[0]);
    /* Rules out of range are refused before anything is routed. */
    r = default_rules();
    r.via_drill_nm = r.via_size_nm;
    assert(autoroute_board(&arena, &b, &r, &res) == AUTOROUTE_INVALID_INPUT);
    r = default_rules();
    r.max_cells = 100;
    assert(autoroute_board(&arena, &b, &r, &res) == AUTOROUTE_INVALID_INPUT);
}

/* KiCad writes a pad's angle in a .kicad_pcb as its absolute angle (footprint + pad's own),
   e.g. KiCad 10.0.5 demos/cm5_minima: a SOT-23-5 at 180 has pads at 180, a JST at 90 has pads at 90.
   The fixture is Copper-written following that convention, not KiCad-exported. */
static void test_pad_angle_is_absolute(void)
{
    Board b;
    load(FIX "rotated_pads.kicad_pcb", &b);
    assert(b.footprint_count == 3 && b.pad_count == 3);
    const int32_t want_rot[3] = {90000, 270000, 90000};
    const G2Point want_pos[3] = {{10000000, 9000000}, {20000000, 11000000}, {30000000, 10000000}};
    for (size_t i = 0; i < 3; ++i) {
        const BoardPad *p = &b.pads[b.footprints[i].first_pad];
        assert(p->rot_mdeg == want_rot[i]);
        assert(p->pos.x == want_pos[i].x && p->pos.y == want_pos[i].y);
        G2Aabb box;
        assert(board_pad_aabb(p, &box) == BOARD_OK);
        /* a 2 x 1 mm pad at 90 or 270 is 1 mm wide and 2 mm tall on the board */
        assert(box.max.x - box.min.x == 1000000 && box.max.y - box.min.y == 2000000);
    }
}

/* R5 (LAYOUT_RULES.md, Step 3c): a SOT-563 (pads copied from KiCad 10.0.5's SOT-563.kicad_mod: 0.675 x 0.35 mm at
   0.5 mm pitch, 0.15 mm apart; the fixture board is Copper-written) cannot be reached with a 0.3 mm clearance. With
   the fine-pitch rules, each of its pads escapes with a narrow stub that keeps the fine clearance to the other pads of
   its own footprint only; every other pair keeps the normal clearance. Checked independently below. */
static bool is_u3_stub(const Board *b, const BoardTrack *t, size_t *fp)
{
    for (size_t i = 0; i < b->pad_count; i++) {
        const BoardPad *p = &b->pads[i];
        const char *ref = b->footprints[p->footprint].reference;
        if (!ref || strcmp(ref, "U3") || p->net != t->net) continue;
        if ((t->a.x == p->pos.x && t->a.y == p->pos.y) || (t->b.x == p->pos.x && t->b.y == p->pos.y)) { *fp = p->footprint; return true; }
    }
    return false;
}

static void assert_clear_fine(const Board *b, const AutorouteResult *res, G2Nm clr, G2Nm fine, G2Nm neck)
{
    G2Order o;
    size_t stubs = 0;
    for (size_t t = 0; t < res->track_count; t++) {
        const BoardTrack *a = &res->tracks[t];
        size_t fp = SIZE_MAX;
        bool stub = is_u3_stub(b, a, &fp);
        if (stub) { stubs++; assert(a->width == neck); }
        else assert(a->width == 250000);
        G2Nm ha = hup(a->width);
        for (size_t i = 0; i < b->pad_count; i++) {
            const BoardPad *p = &b->pads[i];
            G2Aabb box;
            if (p->net == a->net || !layer_has(p, a->layer)) continue;
            assert(board_pad_aabb(p, &box) == BOARD_OK);
            G2Nm c = stub && p->footprint == fp && p->type == BOARD_PAD_SMD ? fine : clr;
            assert(g2_compare_segment_aabb_distance(a->a, a->b, box, ha + c, &o) == G2_OK && o != G2_ORDER_LESS);
        }
        for (size_t k = 0; k < t; k++) {
            const BoardTrack *c = &res->tracks[k];
            if (c->net == a->net || c->layer != a->layer) continue;
            assert(g2_compare_segment_segment_distance(a->a, a->b, c->a, c->b, ha + hup(c->width) + clr, &o) == G2_OK && o != G2_ORDER_LESS);
        }
        for (size_t v = 0; v < res->via_count; v++) {
            G2Circle c = {res->vias[v].pos, hup(res->vias[v].size)};
            if (res->vias[v].net == a->net) continue;
            assert(g2_compare_segment_circle_distance(a->a, a->b, c, ha + clr, &o) == G2_OK && o != G2_ORDER_LESS);
        }
    }
    assert(stubs >= 5);   /* U3 pads 1, 2, 4, 5, 6 (pad 3 has no net) */
    for (size_t v = 0; v < res->via_count; v++)
        for (size_t i = 0; i < b->pad_count; i++) {
            G2Aabb box;
            if (b->pads[i].net == res->vias[v].net) continue;
            assert(board_pad_aabb(&b->pads[i], &box) == BOARD_OK);
            assert(g2_compare_point_aabb_distance(res->vias[v].pos, box, hup(res->vias[v].size) + clr, &o) == G2_OK && o != G2_ORDER_LESS);
        }
}

static void test_fine_pitch(void)
{
    Board b;
    load(FIX "fine_pitch.kicad_pcb", &b);
    AutorouteRules r = default_rules();
    AutorouteResult res;
    /* without the fine-pitch rules: the four nets that reach U3 fail, with the reason */
    assert(autoroute_board(&arena, &b, &r, &res) == AUTOROUTE_PARTIAL);
    assert(res.nets_routed == 0);
    for (size_t i = 0; i < res.net_count; i++)
        if (res.nets[i].status == AUTOROUTE_NET_FAILED) assert(strstr(res.nets[i].message, "no free grid cell"));

    /* with them (routing_profiles/copper_2layer_default.json values): routed, and independently clear */
    r.fine_clearance_nm = 150000;
    r.fine_neck_nm = 200000;
    AutorouteStatus st = autoroute_board(&arena, &b, &r, &res);
    if (st != AUTOROUTE_ROUTED)
        for (size_t i = 0; i < res.net_count; i++) fprintf(stderr, "  %s %s %s\n", res.nets[i].name, autoroute_net_status_name(res.nets[i].status), res.nets[i].message);
    assert(st == AUTOROUTE_ROUTED && res.nets_routed == 4 && res.nets_failed == 0);
    assert_connected(&b, &res);
    assert_clear_fine(&b, &res, r.clearance_nm, r.fine_clearance_nm, r.fine_neck_nm);

    /* deterministic */
    AutorouteResult res2;
    assert(autoroute_board(&arena, &b, &r, &res2) == AUTOROUTE_ROUTED);
    assert(res2.track_count == res.track_count && res2.via_count == res.via_count);
    assert(!memcmp(res2.tracks, res.tracks, res.track_count * sizeof *res.tracks));

    /* the fine rules never apply away from fine-pitch footprints: the LED board routes byte-identically with them */
    Board led;
    load(FIX "led_indicator.kicad_pcb", &led);
    AutorouteRules plain = default_rules();
    AutorouteResult a, c;
    assert(autoroute_board(&arena, &led, &plain, &a) == AUTOROUTE_ROUTED);
    assert(autoroute_board(&arena, &led, &r, &c) == AUTOROUTE_ROUTED);
    assert(a.track_count == c.track_count && !memcmp(a.tracks, c.tracks, a.track_count * sizeof *a.tracks));

    /* refused: only one of the two set, a fine clearance above the normal one */
    AutorouteRules bad = r;
    bad.fine_neck_nm = 0;
    assert(autoroute_board(&arena, &b, &bad, &res) == AUTOROUTE_INVALID_INPUT);
    bad = r; bad.fine_clearance_nm = 0;
    assert(autoroute_board(&arena, &b, &bad, &res) == AUTOROUTE_INVALID_INPUT);
    bad = r; bad.fine_clearance_nm = r.clearance_nm + 1;
    assert(autoroute_board(&arena, &b, &bad, &res) == AUTOROUTE_INVALID_INPUT);
}

int main(void)
{
    arena_init(&arena, 1u << 24);
    test_fine_pitch();
    test_pad_angle_is_absolute();
    test_led_board();
    test_esp32_board_with_keepout_and_widths();
    test_fail_closed();
    arena_destroy(&arena);
    puts("autoroute: all tests passed");
    return 0;
}

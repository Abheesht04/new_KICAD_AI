/* V2 DFM (src/v2_src/dfm.c) against the JLCPCB 2-layer profile (fab_profiles/jlcpcb_2layer.json, values quoted from
 * jlcpcb.com/capabilities/pcb-capabilities, retrieved 2026-11-06).
 *
 * Every limit is tested at exactly the limit (PASS) and 1 nm beyond it (FAIL), on small boards written here; the
 * UNVERIFIED paths (arc tracks, rotated pads, custom pads, Edge.Cuts arcs, a limit the fabricator does not publish)
 * and NOT_IMPLEMENTED (zone fills) are exercised; and the Copper-generated r0603x5 board and its KiCad 10 re-save
 * give the same report. tests/dfm_test.c is the legacy double-mm DFM; this is the V2 module. */
#include "dfm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static DfmProfile JLC;

/* A 40 x 30 mm board, 1.6 mm thick, nets A and B, plus `items`. */
static char *board_text(const char *general, const char *outline, const char *items)
{
    char *t = malloc(1 << 15);
    snprintf(t, 1 << 15,
             "(kicad_pcb (version 20241229) (generator \"test\") %s\n"
             " (layers (0 \"F.Cu\" signal) (2 \"B.Cu\" signal) (25 \"Edge.Cuts\" user))\n"
             " (net 0 \"\") (net 1 \"A\") (net 2 \"B\")\n %s\n %s\n)\n",
             general ? general : "(general (thickness 1.6))",
             outline ? outline : "(gr_rect (start 0 0) (end 40 30) (layer \"Edge.Cuts\"))", items);
    return t;
}

typedef struct Run { Arena arena; Board board; DfmReport report; bool ok; } Run;

static void run(Run *r, const char *general, const char *outline, const char *items)
{
    char *t = board_text(general, outline, items);
    BoardError be;
    DfmError de;
    arena_init(&r->arena, 1 << 16);
    r->ok = board_parse(&r->arena, t, strlen(t), &r->board, &be) == BOARD_OK &&
            dfm_check(&r->arena, &r->board, &JLC, &r->report, &de) == DFM_OK;
    if (!r->ok) fprintf(stderr, "run failed: board %s / dfm %s %s\n", be.message ? be.message : "", de.message ? de.message : "", items);
    free(t);
}

static DfmVerdict verdict(const Run *r, DfmRule rule) { return r->report.rules[rule].verdict; }

static size_t count(const Run *r, DfmRule rule, DfmLimit limit)
{
    size_t n = 0;
    for (size_t i = 0; i < r->report.violation_count; i++)
        n += r->report.violations[i].rule == rule && r->report.violations[i].limit == limit;
    return n;
}

static const DfmViolation *first(const Run *r, DfmRule rule)
{
    for (size_t i = 0; i < r->report.violation_count; i++)
        if (r->report.violations[i].rule == rule) return &r->report.violations[i];
    return NULL;
}

/* Pass at `ok_items`, fail (one violation of `limit` under `rule`) at `bad_items`. */
static void boundary(const char *what, DfmRule rule, DfmLimit limit, const char *ok_items, const char *bad_items)
{
    Run a, b;
    run(&a, NULL, NULL, ok_items);
    run(&b, NULL, NULL, bad_items);
    bool good = a.ok && b.ok && verdict(&a, rule) == DFM_PASS && count(&a, rule, limit) == 0 &&
                verdict(&b, rule) == DFM_FAIL && count(&b, rule, limit) == 1;
    if (!good) fprintf(stderr, "boundary %s: at limit %s (%zu), beyond %s (%zu)\n", what, dfm_verdict_name(verdict(&a, rule)),
                       count(&a, rule, limit), dfm_verdict_name(verdict(&b, rule)), count(&b, rule, limit));
    CHECK(good);
    arena_destroy(&a.arena);
    arena_destroy(&b.arena);
}

#define SEG(x1, y1, x2, y2, w, net) "(segment (start " x1 " " y1 ") (end " x2 " " y2 ") (width " w ") (layer \"F.Cu\") (net " net "))"
#define FP(ref, x, y, pads) "(footprint \"T:P\" (layer \"F.Cu\") (at " x " " y ") (property \"Reference\" \"" ref "\") " pads ")"

static void test_profile(void)
{
    Arena a;
    DfmError e;
    DfmProfile p;
    arena_init(&a, 1 << 16);
    CHECK(dfm_profile_load(&a, DFM_ROOT "/fab_profiles/jlcpcb_2layer.json", &JLC, &e) == DFM_OK);
    CHECK(!strcmp(JLC.fabricator, "JLCPCB") && !strcmp(JLC.retrieved, "2026-11-06"));
    CHECK(!strcmp(JLC.source_url, "https://jlcpcb.com/capabilities/pcb-capabilities"));
    CHECK(JLC.limits[DFM_TRACK_WIDTH_MIN].present && JLC.limits[DFM_TRACK_WIDTH_MIN].nm == 100000);
    CHECK(JLC.limits[DFM_PTH_TO_TRACK_MIN].nm == 280000 && JLC.limits[DFM_PTH_ANNULAR_RING_MIN].nm == 180000);
    CHECK(JLC.limits[DFM_BOARD_MAX_X].nm == 670000000 && JLC.limits[DFM_BOARD_MAX_THIN_Y].nm == 497000000);
    CHECK(!JLC.limits[DFM_HOLE_TO_EDGE_MIN].present && !strcmp(JLC.limits[DFM_HOLE_TO_EDGE_MIN].quote, "not on the page"));
    for (int k = 0; k < DFM_LIMIT_COUNT; k++) CHECK(JLC.limits[k].quote && (!JLC.limits[k].present || JLC.limits[k].row));

    const char *head = "{\"format\": \"copper.fab_profile/1\", \"name\": \"x\", \"fabricator\": \"F\", "
                       "\"source\": {\"url\": \"u\", \"retrieved\": \"2026-11-06\"}, \"limits\": {";
    char t[4096];
    snprintf(t, sizeof t, "%s\"track_widht_min_mm\": {\"value\": 0.1, \"row\": \"r\", \"quote\": \"q\"}}}", head);
    CHECK(dfm_profile_parse(&a, t, strlen(t), &p, &e) == DFM_INVALID_PROFILE && !strcmp(e.key, "track_widht_min_mm"));
    snprintf(t, sizeof t, "%s\"track_width_min_mm\": {\"value\": 0.1, \"row\": \"r\", \"quote\": \"q\"}}}", head);
    CHECK(dfm_profile_parse(&a, t, strlen(t), &p, &e) == DFM_INVALID_PROFILE && !strcmp(e.key, "track_spacing_min_mm"));   /* missing */
    snprintf(t, sizeof t, "%s\"track_width_min_mm\": {\"value\": 0.1, \"row\": \"r\"}}}", head);
    CHECK(dfm_profile_parse(&a, t, strlen(t), &p, &e) == DFM_INVALID_PROFILE && !strcmp(e.key, "track_width_min_mm"));    /* no quote */
    snprintf(t, sizeof t, "%s\"track_width_min_mm\": {\"value\": 1e-1, \"row\": \"r\", \"quote\": \"q\"}}}", head);
    CHECK(dfm_profile_parse(&a, t, strlen(t), &p, &e) == DFM_INVALID_PROFILE && !strcmp(e.key, "track_width_min_mm"));    /* exponent */
    const char *nosrc = "{\"format\": \"copper.fab_profile/1\", \"name\": \"x\", \"fabricator\": \"F\", \"limits\": {}}";
    CHECK(dfm_profile_parse(&a, nosrc, strlen(nosrc), &p, &e) == DFM_INVALID_PROFILE && !strcmp(e.key, "source.url"));
    CHECK(dfm_profile_parse(&a, "{\"format\": 1,", 13, &p, &e) == DFM_PARSE_ERROR && e.line == 1);
    CHECK(dfm_profile_load(&a, "no/such/profile.json", &p, &e) == DFM_IO_ERROR);
    /* Keep the arena: JLC lives in it for the remaining tests. */
}

static void test_boundaries(void)
{
    /* Track width 0.10 mm. */
    boundary("track width", DFM_RULE_TRACK_WIDTH, DFM_TRACK_WIDTH_MIN, SEG("5", "5", "10", "5", "0.1", "1"), SEG("5", "5", "10", "5", "0.099999", "1"));
    /* Track spacing 0.10 mm: two 0.2 mm tracks of different nets, edges 0.1 apart. */
    boundary("track spacing", DFM_RULE_CLEARANCE, DFM_TRACK_SPACING_MIN,
             SEG("5", "10", "15", "10", "0.2", "1") SEG("5", "10.3", "15", "10.3", "0.2", "2"),
             SEG("5", "10", "15", "10", "0.2", "1") SEG("5", "10.299999", "15", "10.299999", "0.2", "2"));
    /* The same tracks on one net: no clearance applies. */
    {
        Run r;
        run(&r, NULL, NULL, SEG("5", "10", "15", "10", "0.2", "1") SEG("5", "10.25", "15", "10.25", "0.2", "1"));
        CHECK(r.ok && verdict(&r, DFM_RULE_CLEARANCE) == DFM_PASS);
        arena_destroy(&r.arena);
    }
    /* KiCad's net 0 ("") is no net: two unconnected tracks still need their spacing. */
    {
        Run r;
        run(&r, NULL, NULL, SEG("5", "10", "15", "10", "0.2", "0") SEG("5", "10.25", "15", "10.25", "0.2", "0"));
        CHECK(r.ok && count(&r, DFM_RULE_CLEARANCE, DFM_TRACK_SPACING_MIN) == 1);
        arena_destroy(&r.arena);
    }
    /* SMD pad to pad 0.15 mm: two 1 x 1 mm pads, centres 1.15 apart. */
    boundary("smd pad to pad", DFM_RULE_CLEARANCE, DFM_SMD_PAD_TO_PAD_MIN,
             FP("P1", "10", "10", "(pad \"1\" smd rect (at 0 0) (size 1 1) (layers \"F.Cu\") (net 1 \"A\")) (pad \"2\" smd rect (at 1.15 0) (size 1 1) (layers \"F.Cu\") (net 2 \"B\"))"),
             FP("P1", "10", "10", "(pad \"1\" smd rect (at 0 0) (size 1 1) (layers \"F.Cu\") (net 1 \"A\")) (pad \"2\" smd rect (at 1.149999 0) (size 1 1) (layers \"F.Cu\") (net 2 \"B\"))"));
    /* Pad to track 0.10 mm: a 1 x 1 SMD pad at (10, 10), a 0.2 mm track whose edge is 0.1 from the pad's edge. */
    boundary("pad to track", DFM_RULE_CLEARANCE, DFM_PAD_TO_TRACK_MIN,
             FP("P1", "10", "10", "(pad \"1\" smd rect (at 0 0) (size 1 1) (layers \"F.Cu\") (net 1 \"A\"))") SEG("5", "10.7", "15", "10.7", "0.2", "2"),
             FP("P1", "10", "10", "(pad \"1\" smd rect (at 0 0) (size 1 1) (layers \"F.Cu\") (net 1 \"A\"))") SEG("5", "10.699999", "15", "10.699999", "0.2", "2"));
    /* PTH to track 0.28 mm: a 1.6 mm round PTH pad, track edge 0.28 from its copper. */
    boundary("pth to track", DFM_RULE_CLEARANCE, DFM_PTH_TO_TRACK_MIN,
             FP("J1", "20", "20", "(pad \"1\" thru_hole circle (at 0 0) (size 1.6 1.6) (drill 0.8) (layers \"*.Cu\") (net 1 \"A\"))") SEG("10", "21.18", "30", "21.18", "0.2", "2"),
             FP("J1", "20", "20", "(pad \"1\" thru_hole circle (at 0 0) (size 1.6 1.6) (drill 0.8) (layers \"*.Cu\") (net 1 \"A\"))") SEG("10", "21.179999", "30", "21.179999", "0.2", "2"));
    /* PTH annular ring 0.18 mm. */
    boundary("pth annular ring", DFM_RULE_ANNULAR_RING, DFM_PTH_ANNULAR_RING_MIN,
             FP("J1", "20", "20", "(pad \"1\" thru_hole circle (at 0 0) (size 1.16 1.16) (drill 0.8) (layers \"*.Cu\") (net 1 \"A\"))"),
             FP("J1", "20", "20", "(pad \"1\" thru_hole circle (at 0 0) (size 1.159998 1.159998) (drill 0.8) (layers \"*.Cu\") (net 1 \"A\"))"));
    /* NPTH annular ring 0.45 mm, for a non-plated hole with a copper pad. */
    boundary("npth annular ring", DFM_RULE_ANNULAR_RING, DFM_NPTH_ANNULAR_RING_MIN,
             FP("H1", "20", "20", "(pad \"\" np_thru_hole circle (at 0 0) (size 1.9 1.9) (drill 1) (layers \"*.Cu\"))"),
             FP("H1", "20", "20", "(pad \"\" np_thru_hole circle (at 0 0) (size 1.899998 1.899998) (drill 1) (layers \"*.Cu\"))"));
    /* Via: diameter 0.1 over the hole, hole 0.15, diameter 0.25. */
    boundary("via over hole", DFM_RULE_ANNULAR_RING, DFM_VIA_DIAMETER_OVER_HOLE_MIN,
             "(via (at 5 5) (size 0.35) (drill 0.25) (layers \"F.Cu\" \"B.Cu\") (net 1))",
             "(via (at 5 5) (size 0.35) (drill 0.250001) (layers \"F.Cu\" \"B.Cu\") (net 1))");
    boundary("via hole", DFM_RULE_DRILL, DFM_VIA_HOLE_MIN,
             "(via (at 5 5) (size 0.4) (drill 0.15) (layers \"F.Cu\" \"B.Cu\") (net 1))",
             "(via (at 5 5) (size 0.4) (drill 0.149999) (layers \"F.Cu\" \"B.Cu\") (net 1))");
    boundary("via diameter", DFM_RULE_DRILL, DFM_VIA_DIAMETER_MIN,
             "(via (at 5 5) (size 0.25) (drill 0.15) (layers \"F.Cu\" \"B.Cu\") (net 1))",
             "(via (at 5 5) (size 0.249999) (drill 0.149999) (layers \"F.Cu\" \"B.Cu\") (net 1))");
    /* Drills: PTH 0.15, NPTH 0.50, plated slot 0.50 wide. */
    boundary("pth drill", DFM_RULE_DRILL, DFM_DRILL_MIN,
             FP("J1", "20", "20", "(pad \"1\" thru_hole circle (at 0 0) (size 1 1) (drill 0.15) (layers \"*.Cu\") (net 1 \"A\"))"),
             FP("J1", "20", "20", "(pad \"1\" thru_hole circle (at 0 0) (size 1 1) (drill 0.149999) (layers \"*.Cu\") (net 1 \"A\"))"));
    boundary("npth drill", DFM_RULE_DRILL, DFM_NPTH_DRILL_MIN,
             FP("H1", "20", "20", "(pad \"\" np_thru_hole circle (at 0 0) (size 0.5 0.5) (drill 0.5) (layers \"*.Cu\"))"),
             FP("H1", "20", "20", "(pad \"\" np_thru_hole circle (at 0 0) (size 0.499999 0.499999) (drill 0.499999) (layers \"*.Cu\"))"));
    boundary("plated slot", DFM_RULE_DRILL, DFM_PLATED_SLOT_WIDTH_MIN,
             FP("J1", "20", "20", "(pad \"1\" thru_hole oval (at 0 0) (size 1.5 2) (drill oval 0.5 1) (layers \"*.Cu\") (net 1 \"A\"))"),
             FP("J1", "20", "20", "(pad \"1\" thru_hole oval (at 0 0) (size 1.5 2) (drill oval 0.499999 1) (layers \"*.Cu\") (net 1 \"A\"))"));
    /* Hole to hole: vias 0.2 (same net, so copper clearance does not apply), pad holes 0.45. */
    boundary("via hole to hole", DFM_RULE_HOLE_TO_HOLE, DFM_VIA_HOLE_TO_HOLE_MIN,
             "(via (at 5 5) (size 0.6) (drill 0.3) (layers \"F.Cu\" \"B.Cu\") (net 1)) (via (at 5.5 5) (size 0.6) (drill 0.3) (layers \"F.Cu\" \"B.Cu\") (net 1))",
             "(via (at 5 5) (size 0.6) (drill 0.3) (layers \"F.Cu\" \"B.Cu\") (net 1)) (via (at 5.499999 5) (size 0.6) (drill 0.3) (layers \"F.Cu\" \"B.Cu\") (net 1))");
    boundary("pad hole to hole", DFM_RULE_HOLE_TO_HOLE, DFM_PAD_HOLE_TO_HOLE_MIN,
             FP("H1", "20", "20", "(pad \"\" np_thru_hole circle (at 0 0) (size 1 1) (drill 1) (layers \"*.Cu\")) (pad \"\" np_thru_hole circle (at 1.45 0) (size 1 1) (drill 1) (layers \"*.Cu\"))"),
             FP("H1", "20", "20", "(pad \"\" np_thru_hole circle (at 0 0) (size 1 1) (drill 1) (layers \"*.Cu\")) (pad \"\" np_thru_hole circle (at 1.449999 0) (size 1 1) (drill 1) (layers \"*.Cu\"))"));
    /* Via hole to track 0.2 mm (a different net): hole radius 0.15 + 0.2 + track half-width 0.1. */
    boundary("via hole to track", DFM_RULE_HOLE_TO_TRACK, DFM_VIA_HOLE_TO_TRACK_MIN,
             "(via (at 5 5) (size 0.6) (drill 0.3) (layers \"F.Cu\" \"B.Cu\") (net 1))" SEG("2", "5.45", "8", "5.45", "0.2", "2"),
             "(via (at 5 5) (size 0.6) (drill 0.3) (layers \"F.Cu\" \"B.Cu\") (net 1))" SEG("2", "5.449999", "8", "5.449999", "0.2", "2"));
    boundary("npth to track", DFM_RULE_HOLE_TO_TRACK, DFM_NPTH_TO_TRACK_MIN,
             FP("H1", "20", "20", "(pad \"\" np_thru_hole circle (at 0 0) (size 1 1) (drill 1) (layers \"*.Cu\"))") SEG("10", "20.8", "30", "20.8", "0.2", "1"),
             FP("H1", "20", "20", "(pad \"\" np_thru_hole circle (at 0 0) (size 1 1) (drill 1) (layers \"*.Cu\"))") SEG("10", "20.799999", "30", "20.799999", "0.2", "1"));
    /* Copper to the routed edge 0.2 mm. */
    boundary("copper to edge", DFM_RULE_COPPER_TO_EDGE, DFM_COPPER_TO_EDGE_MIN,
             SEG("5", "0.3", "10", "0.3", "0.2", "1"), SEG("5", "0.299999", "10", "0.299999", "0.2", "1"));
}

static void test_measured_values_and_order(void)
{
    Run r;
    run(&r, NULL, NULL, SEG("5", "20", "10", "20", "0.05", "1") SEG("5", "5", "10", "5", "0.08", "1")
        "(via (at 30 20) (size 0.2) (drill 0.1) (layers \"F.Cu\" \"B.Cu\") (net 2))");
    CHECK(r.ok && r.report.verdict == DFM_FAIL);
    const DfmViolation *v = first(&r, DFM_RULE_TRACK_WIDTH);
    CHECK(v && v->measured_nm == 80000 && v->limit_nm == 100000 && v->a.kind == DFM_ITEM_TRACK && v->a.index == 1);   /* y 5 first */
    CHECK(count(&r, DFM_RULE_TRACK_WIDTH, DFM_TRACK_WIDTH_MIN) == 2);
    CHECK(count(&r, DFM_RULE_DRILL, DFM_VIA_HOLE_MIN) == 1 && count(&r, DFM_RULE_DRILL, DFM_VIA_DIAMETER_MIN) == 1);
    for (size_t i = 1; i < r.report.violation_count; i++) CHECK(r.report.violations[i - 1].rule <= r.report.violations[i].rule);
    arena_destroy(&r.arena);
}

static void test_unverified_and_not_implemented(void)
{
    Run r;
    /* A clean board: everything passes except hole-to-edge, which JLCPCB does not publish for ordinary holes. */
    run(&r, NULL, NULL, SEG("5", "5", "10", "5", "0.2", "1"));
    CHECK(r.ok && verdict(&r, DFM_RULE_HOLE_TO_EDGE) == DFM_UNVERIFIED && r.report.verdict == DFM_UNVERIFIED);
    for (int k = 0; k < DFM_RULE_COUNT; k++) if (k != DFM_RULE_HOLE_TO_EDGE) CHECK(verdict(&r, (DfmRule)k) == DFM_PASS);
    arena_destroy(&r.arena);

    /* An arc track close to another net: its exact distance is not computed, so the pair is UNVERIFIED (not FAIL);
     * far away it passes. */
    run(&r, NULL, NULL, "(arc (start 5 10) (mid 7 8) (end 9 10) (width 0.2) (layer \"F.Cu\") (net 1))" SEG("5", "8.2", "9", "8.2", "0.2", "2"));
    CHECK(r.ok && verdict(&r, DFM_RULE_CLEARANCE) == DFM_UNVERIFIED && count(&r, DFM_RULE_CLEARANCE, DFM_TRACK_SPACING_MIN) == 0);
    arena_destroy(&r.arena);
    run(&r, NULL, NULL, "(arc (start 5 10) (mid 7 8) (end 9 10) (width 0.2) (layer \"F.Cu\") (net 1))" SEG("5", "20", "9", "20", "0.2", "2"));
    CHECK(r.ok && verdict(&r, DFM_RULE_CLEARANCE) == DFM_PASS);
    arena_destroy(&r.arena);
    /* A pad at 45 degrees: contained in its circumcircle; near a track that is UNVERIFIED, far it passes. */
    run(&r, NULL, NULL, "(footprint \"T:P\" (at 10 10 45) (property \"Reference\" \"P1\") (pad \"1\" smd rect (at 0 0 45) (size 1 1) (layers \"F.Cu\") (net 1 \"A\")))"
        SEG("5", "10.75", "15", "10.75", "0.2", "2"));
    CHECK(r.ok && verdict(&r, DFM_RULE_CLEARANCE) == DFM_UNVERIFIED);
    arena_destroy(&r.arena);
    run(&r, NULL, NULL, "(footprint \"T:P\" (at 10 10 45) (property \"Reference\" \"P1\") (pad \"1\" smd rect (at 0 0 45) (size 1 1) (layers \"F.Cu\") (net 1 \"A\")))"
        SEG("5", "11", "15", "11", "0.2", "2"));
    CHECK(r.ok && verdict(&r, DFM_RULE_CLEARANCE) == DFM_PASS);
    arena_destroy(&r.arena);
    /* A custom pad: UNVERIFIED as a whole. */
    run(&r, NULL, NULL, FP("P1", "10", "10", "(pad \"1\" smd custom (at 0 0) (size 1 1) (layers \"F.Cu\") (net 1 \"A\"))"));
    CHECK(r.ok && verdict(&r, DFM_RULE_CLEARANCE) == DFM_UNVERIFIED && r.report.rules[DFM_RULE_CLEARANCE].unverified == 1);
    arena_destroy(&r.arena);
    /* Rounded corners: copper near a corner arc is UNVERIFIED, copper on a straight edge is checked exactly. */
    const char *rounded =
        "(gr_line (start 2 0) (end 38 0) (layer \"Edge.Cuts\")) (gr_line (start 40 2) (end 40 28) (layer \"Edge.Cuts\"))"
        " (gr_line (start 38 30) (end 2 30) (layer \"Edge.Cuts\")) (gr_line (start 0 28) (end 0 2) (layer \"Edge.Cuts\"))"
        " (gr_arc (start 38 0) (mid 39.414214 0.585786) (end 40 2) (layer \"Edge.Cuts\"))"
        " (gr_arc (start 40 28) (mid 39.414214 29.414214) (end 38 30) (layer \"Edge.Cuts\"))"
        " (gr_arc (start 2 30) (mid 0.585786 29.414214) (end 0 28) (layer \"Edge.Cuts\"))"
        " (gr_arc (start 0 2) (mid 0.585786 0.585786) (end 2 0) (layer \"Edge.Cuts\"))";
    run(&r, NULL, rounded, SEG("1", "1", "1.5", "1.5", "0.2", "1"));
    CHECK(r.ok && r.board.outline.closed && verdict(&r, DFM_RULE_COPPER_TO_EDGE) == DFM_UNVERIFIED);
    arena_destroy(&r.arena);
    run(&r, NULL, rounded, SEG("10", "15", "20", "15", "0.2", "1"));
    CHECK(r.ok && verdict(&r, DFM_RULE_COPPER_TO_EDGE) == DFM_PASS);
    arena_destroy(&r.arena);
    /* Zone fills are not modelled: NOT_IMPLEMENTED, never PASS. */
    run(&r, NULL, NULL, "(zone (net 1) (net_name \"A\") (layer \"F.Cu\") (polygon (pts (xy 1 1) (xy 5 1) (xy 5 5)))"
        " (filled_polygon (layer \"F.Cu\") (pts (xy 1 1) (xy 5 1) (xy 5 5))))");
    CHECK(r.ok && verdict(&r, DFM_RULE_ZONE_CLEARANCE) == DFM_NOT_IMPLEMENTED && r.report.verdict == DFM_UNVERIFIED);
    arena_destroy(&r.arena);
}

static void test_board_size(void)
{
    Run r;
    run(&r, NULL, "(gr_rect (start 0 0) (end 2.999999 40) (layer \"Edge.Cuts\"))", "");
    CHECK(r.ok && count(&r, DFM_RULE_BOARD_SIZE, DFM_BOARD_MIN_X) == 1);
    arena_destroy(&r.arena);
    run(&r, NULL, "(gr_rect (start 0 0) (end 3 3) (layer \"Edge.Cuts\"))", "");
    CHECK(r.ok && verdict(&r, DFM_RULE_BOARD_SIZE) == DFM_PASS);
    arena_destroy(&r.arena);
    run(&r, NULL, "(gr_rect (start 0 0) (end 600 670) (layer \"Edge.Cuts\"))", "");   /* fits turned */
    CHECK(r.ok && verdict(&r, DFM_RULE_BOARD_SIZE) == DFM_PASS);
    arena_destroy(&r.arena);
    run(&r, NULL, "(gr_rect (start 0 0) (end 670.000001 100) (layer \"Edge.Cuts\"))", "");
    CHECK(r.ok && count(&r, DFM_RULE_BOARD_SIZE, DFM_BOARD_MAX_X) == 1);
    arena_destroy(&r.arena);
    /* 0.6 mm thick: the 599 x 497 thin-board maximum applies. */
    run(&r, "(general (thickness 0.6))", "(gr_rect (start 0 0) (end 650 100) (layer \"Edge.Cuts\"))", "");
    CHECK(r.ok && count(&r, DFM_RULE_BOARD_SIZE, DFM_BOARD_MAX_THIN_X) == 1);
    arena_destroy(&r.arena);
    /* 0.5 mm: below the minimum-size thickness, so the fabricator reviews it: UNVERIFIED. No thickness: UNVERIFIED. */
    run(&r, "(general (thickness 0.5))", "(gr_rect (start 0 0) (end 50 50) (layer \"Edge.Cuts\"))", "");
    CHECK(r.ok && verdict(&r, DFM_RULE_BOARD_SIZE) == DFM_UNVERIFIED);
    arena_destroy(&r.arena);
    run(&r, "(general)", "(gr_rect (start 0 0) (end 50 50) (layer \"Edge.Cuts\"))", "");
    CHECK(r.ok && verdict(&r, DFM_RULE_BOARD_SIZE) == DFM_UNVERIFIED);
    arena_destroy(&r.arena);
}

static void test_golden_boards(void)
{
    Arena a1, a2;
    Board b1, b2;
    DfmReport r1, r2;
    BoardError be;
    DfmError de;
    arena_init(&a1, 1 << 20);
    arena_init(&a2, 1 << 20);
    CHECK(board_load(&a1, DFM_ROOT "/tests/golden/board/r0603x5.kicad_pcb", &b1, &be) == BOARD_OK);
    CHECK(board_load(&a2, DFM_ROOT "/tests/golden/board/r0603x5.kicad10.kicad_pcb", &b2, &be) == BOARD_OK);
    CHECK(dfm_check(&a1, &b1, &JLC, &r1, &de) == DFM_OK && dfm_check(&a2, &b2, &JLC, &r2, &de) == DFM_OK);
    /* Built with Copper's 0.2 mm tracks and clearances, it meets every JLCPCB limit that is published. */
    for (int k = 0; k < DFM_RULE_COUNT; k++) {
        CHECK(r1.rules[k].verdict == (k == DFM_RULE_HOLE_TO_EDGE ? DFM_UNVERIFIED : DFM_PASS));
        CHECK(r1.rules[k].verdict == r2.rules[k].verdict && r1.rules[k].checked == r2.rules[k].checked);
        if (r1.rules[k].verdict != (k == DFM_RULE_HOLE_TO_EDGE ? DFM_UNVERIFIED : DFM_PASS))
            fprintf(stderr, "r0603x5 %s: %s, %zu violation(s)\n", dfm_rule_name((DfmRule)k), dfm_verdict_name(r1.rules[k].verdict), r1.rules[k].violations);
    }
    /* Routed at 0.2 mm, no copper of two nets comes within the 0.28 mm search reach: no pair needs an exact test. */
    CHECK(r1.rules[DFM_RULE_CLEARANCE].checked == 0 && r1.rules[DFM_RULE_ANNULAR_RING].checked == 3);
    CHECK(r1.rules[DFM_RULE_TRACK_WIDTH].checked == 38 && r1.rules[DFM_RULE_COPPER_TO_EDGE].checked == 17 * 12);
    arena_destroy(&a1);
    arena_destroy(&a2);
}

int main(void)
{
    test_profile();
    test_boundaries();
    test_measured_values_and_order();
    test_unverified_and_not_implemented();
    test_board_size();
    test_golden_boards();
    if (failures) { fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
    puts("dfm_v2: all checks passed");
    return 0;
}

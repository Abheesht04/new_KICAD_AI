/* The board model (src/v2_src/board.c, track B).
 *
 * Golden files and what they prove:
 *  - tests/golden/board/r0603x5.kicad_pcb: five 0603 resistors at 0/90/180/270/0 degrees and a 3-pin header,
 *    Copper-generated (tools/copper.py build of r0603x5.copper with the committed KiCad 9 library snapshot), NOT a
 *    KiCad export. Pad positions are checked against hand-computed values.
 *  - tests/golden/board/r0603x5.kicad10.kicad_pcb: the same board re-saved by KiCad 10.0.5 (`kicad-cli pcb upgrade
 *    --force`): KiCad's own writer, format 20260206, nets by name only. A development preview, not KiCad 9 evidence;
 *    it must give the identical model.
 *  - examples/generated/divider/design.kicad_pcb against manufacturing_audit/drill_test/design.drl, which KiCad
 *    9.0.6 exported from that board: every plated hole KiCad drilled is a through-hole pad here, to the nanometre.
 * KiCad-saved KiCad 9.0 boards are checked by tests/test_board_demos.py when a KiCad install is present. */
#include "board.h"
#include "excellon.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)
#define MM(v) ((G2Nm)((v) * 1000000.0 + ((v) < 0 ? -0.5 : 0.5)))   /* test-only literal conversion */

static bool at(const Board *b, const char *ref, const char *pad, G2Nm x, G2Nm y)
{
    const BoardPad *p = board_find_pad(b, ref, pad);
    if (!p) { fprintf(stderr, "no pad %s.%s\n", ref, pad); return false; }
    if (p->at.x != x || p->at.y != y)
        fprintf(stderr, "%s.%s at %lld,%lld, expected %lld,%lld\n", ref, pad, (long long)p->at.x, (long long)p->at.y,
                (long long)x, (long long)y);
    return p->at.x == x && p->at.y == y && p->exact;
}

static const char *net_of(const Board *b, const char *ref, const char *pad)
{
    const BoardPad *p = board_find_pad(b, ref, pad);
    return p && p->net >= 0 ? b->nets[p->net].name : NULL;
}

static void check_r0603x5(const Board *b)
{
    CHECK(b->footprint_count == 6 && b->pad_count == 13 && b->track_count == 38 && b->via_count == 0);
    CHECK(b->outline.closed && b->outline.loop_count == 1 && b->outline.has_bounds);
    CHECK(b->outline.bounds.min.x == 0 && b->outline.bounds.min.y == 0);
    CHECK(b->outline.bounds.max.x == MM(40) && b->outline.bounds.max.y == MM(30));
    /* R_0603_1608Metric pads at x -0.825 / +0.825 in the footprint; KiCad turns (x, y) by a to
     * (x cos a + y sin a, -x sin a + y cos a), y down. */
    CHECK(at(b, "R1", "1", MM(13.175), MM(8)) && at(b, "R1", "2", MM(14.825), MM(8)));      /* 0 */
    CHECK(at(b, "R2", "1", MM(20), MM(8.825)) && at(b, "R2", "2", MM(20), MM(7.175)));      /* 90 */
    CHECK(at(b, "R3", "1", MM(26.825), MM(8)) && at(b, "R3", "2", MM(25.175), MM(8)));      /* 180 */
    CHECK(at(b, "R4", "1", MM(26), MM(15.175)) && at(b, "R4", "2", MM(26), MM(16.825)));    /* 270 (KiCad 10: -90) */
    CHECK(at(b, "R5", "1", MM(19.175), MM(16)) && at(b, "R5", "2", MM(20.825), MM(16)));
    CHECK(at(b, "J1", "1", MM(6), MM(12)) && at(b, "J1", "2", MM(6), MM(14.54)) && at(b, "J1", "3", MM(6), MM(17.08)));
    const BoardPad *j = board_find_pad(b, "J1", "1");
    CHECK(j && j->type == BOARD_PAD_THRU_HOLE && j->has_drill && j->drill_x == MM(1) && (j->layers & BOARD_LAYER_B_CU));
    const BoardPad *r = board_find_pad(b, "R2", "1");
    CHECK(r && r->type == BOARD_PAD_SMD && r->shape == BOARD_SHAPE_ROUNDRECT && r->size_x == MM(0.8) && r->size_y == MM(0.95));
    CHECK(r && r->layers == (BOARD_LAYER_F_CU | BOARD_LAYER_F_MASK | BOARD_LAYER_F_PASTE));
    CHECK(net_of(b, "R1", "1") && !strcmp(net_of(b, "R1", "1"), "/VIN"));
    CHECK(net_of(b, "R4", "2") && !strcmp(net_of(b, "R4", "2"), "/OUT"));
    CHECK(net_of(b, "J1", "3") && !strcmp(net_of(b, "J1", "3"), "/GND"));
    int32_t out = board_find_net(b, "/OUT");
    size_t idx[8];
    CHECK(out >= 0 && board_pads_on_net(b, out, idx, 8) == 3);   /* J1.2, R4.2, R5.1 */
    for (size_t i = 0; i < b->track_count; i++) CHECK(b->tracks[i].net >= 0 && b->tracks[i].width > 0);
    CHECK(b->issue_count == 0);
}

static void test_golden_r0603x5(void)
{
    Arena a1, a2;
    Board c, k;
    BoardError e;
    arena_init(&a1, 1 << 20);
    arena_init(&a2, 1 << 20);
    BoardStatus s1 = board_load(&a1, BOARD_GOLDEN_DIR "/r0603x5.kicad_pcb", &c, &e);
    CHECK(s1 == BOARD_OK);
    if (s1) fprintf(stderr, "r0603x5: %s %s %s\n", board_status_name(s1), e.context ? e.context : "", e.message ? e.message : "");
    BoardStatus s2 = board_load(&a2, BOARD_GOLDEN_DIR "/r0603x5.kicad10.kicad_pcb", &k, &e);
    CHECK(s2 == BOARD_OK);
    if (s2) fprintf(stderr, "r0603x5 kicad10: %s %s %s\n", board_status_name(s2), e.context ? e.context : "", e.message ? e.message : "");
    if (s1 || s2) { arena_destroy(&a1); arena_destroy(&a2); return; }
    CHECK(c.version == 20241229 && !strcmp(c.generator, "copper_v2") && c.copper_layers == 2 && c.thickness == MM(1.6));
    CHECK(k.version == 20260206 && !strcmp(k.generator, "pcbnew") && !strcmp(k.generator_version, "10.0"));
    check_r0603x5(&c);
    check_r0603x5(&k);
    /* The same model from Copper's writer and KiCad's: every pad, position, net name, size, drill and layer set. */
    CHECK(c.pad_count == k.pad_count);
    for (size_t i = 0; i < c.pad_count && i < k.pad_count; i++) {
        const BoardPad *p = &c.pads[i];
        const BoardPad *q = board_find_pad(&k, c.footprints[p->footprint].reference, p->number);
        CHECK(q && q->at.x == p->at.x && q->at.y == p->at.y && q->size_x == p->size_x && q->size_y == p->size_y &&
              q->drill_x == p->drill_x && q->layers == p->layers && q->type == p->type && q->shape == p->shape);
        CHECK(q && (p->net < 0) == (q->net < 0) && (p->net < 0 || !strcmp(c.nets[p->net].name, k.nets[q->net].name)));
    }
    /* KiCad re-orders tracks when it saves: match them as a multiset (each Copper track to a distinct KiCad one). */
    CHECK(c.track_count == k.track_count);
    bool used[64] = {false};
    for (size_t i = 0; i < c.track_count && k.track_count <= 64; i++) {
        const BoardTrack *t = &c.tracks[i];
        bool matched = false;
        for (size_t j = 0; j < k.track_count && !matched; j++) {
            const BoardTrack *u = &k.tracks[j];
            bool same = !used[j] && u->width == t->width && u->layer == t->layer && u->is_arc == t->is_arc &&
                        !strcmp(c.nets[t->net].name, k.nets[u->net].name) &&
                        ((u->start.x == t->start.x && u->start.y == t->start.y && u->end.x == t->end.x && u->end.y == t->end.y) ||
                         (u->start.x == t->end.x && u->start.y == t->end.y && u->end.x == t->start.x && u->end.y == t->start.y));
            if (same) { used[j] = true; matched = true; }
        }
        CHECK(matched);
    }
    arena_destroy(&a1);
    arena_destroy(&a2);
}

static void test_divider_against_kicad_906_drill(void)
{
    Arena a;
    Board b;
    BoardError e;
    ExcFile f;
    arena_init(&a, 1 << 20);
    CHECK(board_load(&a, DIVIDER_BOARD, &b, &e) == BOARD_OK);
    CHECK(exc_parse_path(&a, KICAD9_DRILL, &f) == EXC_OK);
    size_t tht = 0;
    for (size_t i = 0; i < b.pad_count; i++) tht += b.pads[i].has_drill;
    CHECK(tht == f.hit_count && tht == 3);
    for (size_t h = 0; h < f.hit_count; h++) {   /* KiCad's absolute drill origin: Excellon y = -board y */
        bool found = false;
        for (size_t i = 0; i < b.pad_count; i++) {
            const BoardPad *p = &b.pads[i];
            if (p->has_drill && p->at.x == f.hits[h].pos.x && p->at.y == -f.hits[h].pos.y &&
                p->drill_x == f.tools[f.hits[h].tool].diameter && p->type == BOARD_PAD_THRU_HOLE)
                found = true;
        }
        CHECK(found);
    }
    arena_destroy(&a);
}

static const char *SNIPPET =
    "(kicad_pcb (version 20241229) (generator \"pcbnew\") (generator_version \"9.0\") (general (thickness 1.2))\n"
    " (layers (0 \"F.Cu\" signal) (4 \"In1.Cu\" signal) (2 \"B.Cu\" signal) (25 \"Edge.Cuts\" user))\n"
    " (net 0 \"\") (net 1 \"GND\")\n"
    " (footprint \"X:Y\" (layer \"B.Cu\") (at 10 10 45) (property \"Reference\" \"U1\" (at 0 0 0))\n"
    "   (pad \"1\" smd rect (at 1 0 45) (size 1 1) (layers \"B.Cu\") (net 1 \"GND\")))\n"
    " (footprint \"X:Z\" (layer \"F.Cu\") (at 20 10 90) (property \"Reference\" \"U2\") (property \"Value\" \"V\")\n"
    "   (pad \"1\" thru_hole oval (at 1 2 90) (size 1.7 1.7) (drill oval 1.2 0.8) (layers \"*.Cu\" \"*.Mask\") (net \"GND\"))\n"
    "   (pad \"\" np_thru_hole circle (at 0 0) (size 1 1) (drill 1) (layers \"*.Cu\")))\n"
    " (segment (start 1 1) (end 2 2) (width 0.25) (layer \"F.Cu\") (net 1))\n"
    " (arc (start 0 0) (mid 1 1) (end 2 0) (width 0.2) (layer \"B.Cu\") (net 1))\n"
    " (via (at 5 5) (size 0.6) (drill 0.3) (layers \"F.Cu\" \"B.Cu\") (net 1))\n"
    " (via blind (at 6 5) (size 0.6) (drill 0.3) (layers \"F.Cu\" \"In1.Cu\") (net 1))\n"
    " (zone (net 1) (net_name \"GND\") (layer \"F.Cu\") (polygon (pts (xy 0 0) (xy 10 0) (xy 10 10) (xy 0 10)))\n"
    "   (filled_polygon (layer \"F.Cu\") (pts (xy 0 0) (xy 1 0) (xy 1 1))))\n"
    " (zone (net 0) (net_name \"\") (layers \"F.Cu\" \"B.Cu\") (keepout (tracks not_allowed))\n"
    "   (polygon (pts (xy 1 1) (xy 2 1) (arc (start 2 1) (mid 2.5 1.5) (end 2 2)) (xy 1 2))))\n"
    /* A 50 x 40 board with 2 mm rounded corners, drawn in no particular order, plus a 2 mm round cut-out. */
    " (gr_line (start 2 0) (end 48 0) (layer \"Edge.Cuts\"))\n"
    " (gr_line (start 0 38) (end 0 2) (layer \"Edge.Cuts\"))\n"
    " (gr_arc (start 48 0) (mid 49.414214 0.585786) (end 50 2) (layer \"Edge.Cuts\"))\n"
    " (gr_line (start 50 2) (end 50 38) (layer \"Edge.Cuts\"))\n"
    " (gr_arc (start 2 40) (mid 0.585786 39.414214) (end 0 38) (layer \"Edge.Cuts\"))\n"
    " (gr_arc (start 50 38) (mid 49.414214 39.414214) (end 48 40) (layer \"Edge.Cuts\"))\n"
    " (gr_line (start 2 40) (end 48 40) (layer \"Edge.Cuts\"))\n"
    " (gr_arc (start 0 2) (mid 0.585786 0.585786) (end 2 0) (layer \"Edge.Cuts\"))\n"
    " (gr_circle (center 25 20) (end 26 20) (layer \"Edge.Cuts\"))\n"
    " (gr_line (start 0 0) (end 1 1) (layer \"F.SilkS\"))\n"
    ")\n";

static void test_snippet(void)
{
    Arena a;
    Board b;
    BoardError e;
    arena_init(&a, 1 << 16);
    BoardStatus s = board_parse(&a, SNIPPET, strlen(SNIPPET), &b, &e);
    CHECK(s == BOARD_OK);
    if (s) { fprintf(stderr, "snippet: %s item %zu %s %s\n", board_status_name(s), e.item, e.context ? e.context : "", e.message); arena_destroy(&a); return; }
    CHECK(b.thickness == MM(1.2) && b.copper_layers == 3 && b.net_count == 2);
    /* U1 on the back at 45 degrees: (1, 0) turns to (cos 45, -sin 45) = (0.70710678, -0.70710678) mm. */
    const BoardFootprint *u1 = board_find_footprint(&b, "U1");
    const BoardPad *p = board_find_pad(&b, "U1", "1");
    CHECK(u1 && u1->back && u1->angle_udeg == 45000000);
    CHECK(p && p->at.x == 10707107 && p->at.y == 9292893 && !p->exact && p->angle_udeg == 45000000);
    /* U2 at 90: (1, 2) -> (2, -1); its pad names its net only by name. */
    p = board_find_pad(&b, "U2", "1");
    CHECK(p && p->at.x == MM(22) && p->at.y == MM(9) && p->exact && p->net == board_find_net(&b, "GND"));
    CHECK(p && p->drill_oval && p->drill_x == MM(1.2) && p->drill_y == MM(0.8) && p->shape == BOARD_SHAPE_OVAL);
    CHECK(p && p->layers == (BOARD_LAYERS_CU | BOARD_LAYER_F_MASK | BOARD_LAYER_B_MASK));
    p = board_find_pad(&b, "U2", "");
    CHECK(p && p->type == BOARD_PAD_NP_THRU_HOLE && p->net == -1 && p->at.x == MM(20));
    CHECK(!strcmp(board_find_footprint(&b, "U2")->value, "V"));
    CHECK(b.track_count == 2 && !b.tracks[0].is_arc && b.tracks[1].is_arc && b.tracks[1].mid.y == MM(1));
    CHECK(b.tracks[1].layer == BOARD_LAYER_B_CU && b.tracks[0].width == MM(0.25));
    CHECK(b.via_count == 2 && b.vias[0].type == BOARD_VIA_THROUGH && b.vias[1].type == BOARD_VIA_BLIND);
    CHECK(b.vias[1].layers == (BOARD_LAYER_F_CU | BOARD_LAYER_IN_CU(1)) && b.vias[0].drill == MM(0.3));
    CHECK(b.zone_count == 2 && b.zones[0].net == 1 && b.zones[0].outline_count == 4 && b.zones[0].filled_polygon_count == 1);
    CHECK(b.zones[1].keepout && b.zones[1].outline_count == 4 && b.zones[1].layers == (BOARD_LAYER_F_CU | BOARD_LAYER_B_CU));
    /* Outline: the rounded rectangle is one closed loop and the cut-out another; the bounds are the board's,
     * widened by at most a few nm by the conservative arc extremes. */
    CHECK(b.edge_count == 9 && b.outline.loop_count == 2 && b.outline.closed && b.outline.open_loops == 0);
    CHECK(b.outline.bounds.min.x <= 0 && b.outline.bounds.min.x >= -3 && b.outline.bounds.min.y <= 0 && b.outline.bounds.min.y >= -3);
    CHECK(b.outline.bounds.max.x >= MM(50) && b.outline.bounds.max.x <= MM(50) + 3);
    CHECK(b.outline.bounds.max.y >= MM(40) && b.outline.bounds.max.y <= MM(40) + 3);
    /* Issues: the 45-degree footprint and the arc in a zone outline, nothing else. */
    size_t inexact = 0, zone_arc = 0;
    for (size_t i = 0; i < b.issue_count; i++) {
        inexact += b.issues[i].code == BOARD_ISSUE_INEXACT_ROTATION;
        zone_arc += b.issues[i].code == BOARD_ISSUE_ZONE_ARC;
    }
    CHECK(inexact == 1 && zone_arc == 1 && b.issue_count == 2);
    arena_destroy(&a);
}

static void test_open_outline_and_errors(void)
{
    Arena a;
    Board b;
    BoardError e;
    arena_init(&a, 1 << 16);
    const char *open = "(kicad_pcb (version 20241229) (gr_line (start 0 0) (end 10 0) (layer \"Edge.Cuts\"))"
                       " (gr_line (start 10 0) (end 10 5) (layer \"Edge.Cuts\")))";
    CHECK(board_parse(&a, open, strlen(open), &b, &e) == BOARD_OK);
    CHECK(!b.outline.closed && b.outline.open_loops == 1 && b.outline.loop_count == 1);
    CHECK(b.issue_count == 1 && b.issues[0].code == BOARD_ISSUE_OUTLINE_OPEN);
    /* the loose ends (0, 0) and (10, 5) are sqrt(125) mm = 11180339.9 nm apart, rounded up */
    CHECK(b.outline.dangling_ends == 2 && b.outline.max_gap == 11180340 && !board_outline_closed_within(&b, 11180339) &&
          board_outline_closed_within(&b, 11180340));
    /* A square whose last corner misses by 3 nm: open exactly, closed within 3 nm. */
    const char *gap = "(kicad_pcb (version 1) (gr_line (start 0 0) (end 10 0) (layer \"Edge.Cuts\"))"
                      " (gr_line (start 10 0) (end 10 10) (layer \"Edge.Cuts\")) (gr_line (start 10 10) (end 0 10) (layer \"Edge.Cuts\"))"
                      " (gr_line (start 0 10) (end 0.000003 0) (layer \"Edge.Cuts\")))";
    CHECK(board_parse(&a, gap, strlen(gap), &b, &e) == BOARD_OK && !b.outline.closed && b.outline.max_gap == 3);
    CHECK(board_outline_closed_within(&b, 3) && !board_outline_closed_within(&b, 2));
    const char *none = "(kicad_pcb (version 20241229))";
    CHECK(board_parse(&a, none, strlen(none), &b, &e) == BOARD_OK && !b.outline.closed && b.issues[0].code == BOARD_ISSUE_NO_OUTLINE);

    CHECK(board_load(&a, BOARD_GOLDEN_DIR "/does-not-exist.kicad_pcb", &b, &e) == BOARD_IO_ERROR);
    const char *broken = "(kicad_pcb (version 20241229)\n (net 0 \"\")\n (segment (start 1";
    CHECK(board_parse(&a, broken, strlen(broken), &b, &e) == BOARD_PARSE_ERROR && e.line == 3);
    const char *sch = "(kicad_sch (version 20250114))";
    CHECK(board_parse(&a, sch, strlen(sch), &b, &e) == BOARD_NOT_A_BOARD);
    const char *no_at = "(kicad_pcb (version 1) (net 0 \"\") (footprint \"X:Y\" (property \"Reference\" \"R9\")))";
    CHECK(board_parse(&a, no_at, strlen(no_at), &b, &e) == BOARD_INVALID_CONTENT && e.item == 3 &&
          e.context && !strcmp(e.context, "footprint R9"));
    const char *bad_pad = "(kicad_pcb (version 1) (footprint \"X:Y\" (at 0 0) (property \"Reference\" \"R9\")"
                          " (pad \"2\" smd rect (at 0 0) (layers \"F.Cu\"))))";
    CHECK(board_parse(&a, bad_pad, strlen(bad_pad), &b, &e) == BOARD_INVALID_CONTENT && e.item == 2 &&
          e.context && strstr(e.context, "footprint R9 pad 2"));
    const char *far = "(kicad_pcb (version 1) (via (at 2000 0) (size 1) (drill 0.5)))";
    CHECK(board_parse(&a, far, strlen(far), &b, &e) == BOARD_RANGE_ERROR && e.item == 2);
    const char *bad_type = "(kicad_pcb (version 1) (footprint \"X:Y\" (at 0 0) (pad \"1\" glued rect (at 0 0) (size 1 1))))";
    CHECK(board_parse(&a, bad_type, strlen(bad_type), &b, &e) == BOARD_INVALID_CONTENT);
    arena_destroy(&a);
}

static void test_rotate(void)
{
    G2Nm x, y;
    bool exact;
    CHECK(board_rotate(1000, 2000, 90000000, &x, &y, &exact) == BOARD_OK && exact && x == 2000 && y == -1000);
    CHECK(board_rotate(1000, 2000, -90000000, &x, &y, &exact) == BOARD_OK && exact && x == -2000 && y == 1000);
    CHECK(board_rotate(1000, 2000, 540000000, &x, &y, &exact) == BOARD_OK && exact && x == -1000 && y == -2000);
    CHECK(board_rotate(1000000, 0, 30000000, &x, &y, &exact) == BOARD_OK && !exact && x == 866025 && y == -500000);
    CHECK(board_rotate(1000000000, 0, 45000000, &x, &y, &exact) == BOARD_OK && x == 707106781 && y == -707106781);
    CHECK(board_rotate(0, 1000000000, 60000000, &x, &y, &exact) == BOARD_OK && x == 866025404 && y == 500000000);
    CHECK(board_rotate(1000000, 0, 359999999, &x, &y, &exact) == BOARD_OK && x == 1000000 && y == 0);   /* 1e-6 deg */
    CHECK(board_layer_bits("In30.Cu") == BOARD_LAYER_IN_CU(30) && board_layer_bits("In31.Cu") == BOARD_LAYER_OTHER);
    CHECK(board_layer_bits("F&B.Cu") == (BOARD_LAYER_F_CU | BOARD_LAYER_B_CU) && board_layer_bits("User.1") == BOARD_LAYER_OTHER);
}

int main(void)
{
    test_rotate();
    test_snippet();
    test_open_outline_and_errors();
    test_golden_r0603x5();
    test_divider_against_kicad_906_drill();
    if (failures) { fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
    puts("board: all checks passed");
    return 0;
}

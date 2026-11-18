#ifdef NDEBUG
#undef NDEBUG
#endif
#include "design_board.h"
#include "placer.h"
#include "copper_spice.h"
#include "jlc_catalogue.h"
#include "fab_rules.h"
#include "report.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void test_integer_rotation(void) {
  G2Point out;
  bool exact = false;
  assert(board_rotate((G2Point){1000000, 2000000}, 90000, &out, &exact) == BOARD_OK);
  assert(exact && out.x == 2000000 && out.y == -1000000);
  assert(board_rotate((G2Point){1000000, 0}, 45000, &out, &exact) == BOARD_OK);
  assert(!exact);
  assert(out.x >= 707105 && out.x <= 707109);
  assert(out.y >= -707109 && out.y <= -707105);
  assert(board_rotate((G2Point){1000000, 0}, 120000, &out, &exact) == BOARD_OK);
  assert(!exact);
  assert(out.x >= -500002 && out.x <= -499998);
  assert(out.y >= -866028 && out.y <= -866022);
}

static void test_pad_aabb(void) {
  BoardPad p;
  memset(&p, 0, sizeof(p));
  p.pos = (G2Point){100, 200};
  p.size = (G2Point){101, 201};
  G2Aabb box;
  p.rot_mdeg = 0;
  assert(board_pad_aabb(&p, &box) == BOARD_OK);
  assert(box.min.x == 49 && box.max.x == 151);
  assert(box.min.y == 99 && box.max.y == 301);
  p.rot_mdeg = 90000;
  assert(board_pad_aabb(&p, &box) == BOARD_OK);
  assert(box.min.x == -1 && box.max.x == 201);
  assert(box.min.y == 149 && box.max.y == 251);
}

static void test_board_bounds(void) {
  Board b;
  memset(&b, 0, sizeof(b));
  BoardEdge edge;
  memset(&edge, 0, sizeof(edge));
  edge.kind = BOARD_EDGE_RECT;
  edge.p0 = (G2Point){0, 0};
  edge.p1 = (G2Point){10000000, 5000000};
  b.edges = &edge;
  b.edge_count = 1;
  G2Aabb bounds;
  assert(board_bounds(&b, &bounds) == BOARD_OK);
  assert(bounds.min.x == 0 && bounds.min.y == 0);
  assert(bounds.max.x == 10000000 && bounds.max.y == 5000000);
}

static void test_placer_candidate_limit(void) {
  Arena arena;
  arena_init(&arena, 4096);
  Board b;
  memset(&b, 0, sizeof(b));
  BoardEdge edge;
  memset(&edge, 0, sizeof(edge));
  edge.kind = BOARD_EDGE_RECT;
  edge.p0 = (G2Point){0, 0};
  edge.p1 = (G2Point){10000000, 10000000};
  b.edges = &edge; b.edge_count = 1;

  BoardFootprint fps[2];
  memset(fps, 0, sizeof(fps));
  fps[0].pos = (G2Point){1000000, 1000000};
  fps[1].pos = (G2Point){3000000, 1000000};
  b.footprints = fps; b.footprint_count = 2;
  G2Point court[8] = {
    {500000,500000},{1500000,500000},{1500000,1500000},{500000,1500000},
    {2500000,500000},{3500000,500000},{3500000,1500000},{2500000,1500000}
  };
  b.court_pts = court; b.court_pt_count = 8;
  fps[0].courtyard_exact = fps[1].courtyard_exact = true;
  fps[0].first_court = 0; fps[0].court_count = 4;
  fps[1].first_court = 4; fps[1].court_count = 4;
  PlacerOptions opts = {1000000, 0, 0, 1};
  PlacerResult result;
  PlacerStatus status = copper_place_grid(&b, &opts, &arena, &result);
  assert(status == PLACER_PARTIAL);
  assert(result.placement_count == 2);
  assert(result.placements[0].placed || result.placements[1].placed);
  /* Unvisited footprints must never inherit a fabricated success flag. */
  assert(result.placed_count <= 1);
  for (size_t i = 0; i < result.placement_count; ++i)
    if (!result.placements[i].fixed && result.placements[i].placed)
      assert(result.placements[i].position.x != 0 || result.placements[i].position.y != 0);
  arena_destroy(&arena);
}


static void test_gnd_alias(void) {
  Arena arena;
  arena_init(&arena, 8192);
  CspNetlist net;
  size_t line = 0;
  const char *deck = "gnd alias regression\nR1 n1 gnd 1k\n";
  CspStatus parse_status = csp_parse(&arena, deck, &net, &line);
  if (parse_status != CSP_OK) fprintf(stderr, "gnd deck parse status=%d line=%zu\\n", (int)parse_status, line);
  assert(parse_status == CSP_OK);
  assert(net.node_count == 1);
  Circuit circuit;
  assert(csp_build_circuit(&arena, &net, &circuit) == CSP_OK);
  assert(circuit.node_count == 1);
  assert(circuit.elements[0].n1 == 1 && circuit.elements[0].n2 == 0);
  arena_destroy(&arena);
}


static void test_catalogue_stale_and_indexes(void) {
  Arena arena;
  arena_init(&arena, 8192);
  JlcCatalogue catalogue;
  memset(&catalogue, 0, sizeof(catalogue));
  assert(jlc_catalogue_load(COPPER_TEST_SOURCE_DIR "/tests/catalogue_stale.sx",
                            &arena, "2026-11-15", 30, &catalogue) == JLC_CATALOGUE_OK);
  assert(catalogue.part_count == 2);
  const JlcPart *a = jlc_catalogue_find_part_id(&catalogue, "PART_A");
  const JlcPart *b = jlc_catalogue_find_lcsc_id(&catalogue, "C200");
  assert(a && a->availability_stale);
  assert(b && !b->availability_stale);
  assert(jlc_catalogue_find_part_id(&catalogue, "MISSING") == NULL);
  arena_destroy(&arena);
}


static void test_fab_rule_colour_selection_data(void) {
  Arena arena;
  arena_init(&arena, 16384);
  FabRuleSet rules;
  memset(&rules, 0, sizeof(rules));
  assert(fab_rules_load(COPPER_TEST_SOURCE_DIR "/jlcpcb_rules.rules",
                        &arena, "2026-11-15", 90, &rules) == FAB_OK);
  size_t standard = 0, black_white = 0;
  for (size_t i = 0; i < rules.rule_count; ++i) {
    const FabRule *r = &rules.rules[i];
    if (strcmp(r->kind, "min_mask_bridge") != 0) continue;
    if (r->mask_color && strcmp(r->mask_color, "standard") == 0) ++standard;
    if (r->mask_color && strcmp(r->mask_color, "black_white") == 0) ++black_white;
  }
  assert(standard == 1 && black_white == 1);
  const FabRule *selected[4];
  size_t selected_count = 0;
  assert(fab_rules_select(&rules, "min_mask_bridge", 2, 10, NULL,
                          selected, 4, &selected_count) == FAB_ERR_RANGE);
  assert(fab_rules_select_ex(&rules, "min_mask_bridge", 2, 10, NULL,
                             "black_white", selected, 4,
                             &selected_count) == FAB_OK);
  assert(selected_count == 1);
  assert(strcmp(selected[0]->id, "mask_bridge_1oz_bw") == 0);
  arena_destroy(&arena);
}

static void test_report_verdict(void) {
  CopperReport r;
  ReportFinding f;
  memset(&r, 0, sizeof(r));
  r.findings = &f; r.capacity = 1;
  f.status = REPORT_UNVERIFIED;
  assert(report_add(&r, &f));
  assert(report_overall(&r) == REPORT_UNVERIFIED);
}

int main(void) {
  test_integer_rotation();
  test_pad_aabb();
  test_board_bounds();
  test_placer_candidate_limit();
  test_gnd_alias();
  test_catalogue_stale_and_indexes();
  test_fab_rule_colour_selection_data();
  test_report_verdict();
  puts("Copper core tests: PASS");
  return 0;
}

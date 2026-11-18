#include "optimization_loop.h"
#include "optimization_loop_json.h"
#include "fab_rules.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef COPPER_TEST_SOURCE_DIR
#define COPPER_TEST_SOURCE_DIR "."
#endif

static bool choose_first_eligible(void *context, size_t iteration,
                                  const CopperOptMetrics *baseline,
                                  const CopperOptCandidate *candidates,
                                  size_t candidate_count,
                                  uint64_t *selected_candidate_id) {
    (void)context; (void)iteration;
    const CopperOptCandidate *best = NULL;
    for (size_t i = 0; i < candidate_count; ++i) {
        if (!candidates[i].eligible) continue;
        if (!best || copper_opt_metrics_preferred(&candidates[i].metrics, &best->metrics))
            best = &candidates[i];
    }
    if (!best || !copper_opt_metrics_preferred(&best->metrics, baseline)) {
        *selected_candidate_id = 0;
        return true;
    }
    *selected_candidate_id = best->action.candidate_id;
    return true;
}


typedef struct MockPhysicsContext {
    size_t calls;
} MockPhysicsContext;

static bool mock_physics_evaluate(void *context, const Board *candidate,
                                  CopperOptPhysicsFeedback *out) {
    MockPhysicsContext *mock = (MockPhysicsContext *)context;
    if (!mock || !candidate || !out) return false;
    ++mock->calls;
    memset(out, 0, sizeof(*out));
    out->valid = true;
    out->fail_count = 0u;
    out->unverified_count = 0u;
    out->has_objective = true;
    out->objective = (double)candidate->footprints[0].pos.x;
    out->summary = "mock deterministic physics check: no violations";
    return true;
}

static void make_test_board(Arena *arena, Board *board) {
    assert(board_init(board, arena) == BOARD_OK);
    board->rules.copper_layers = 2;
    board->layers = arena_alloc_array(arena, sizeof(*board->layers), 2);
    board->nets = arena_alloc_array(arena, sizeof(*board->nets), 1);
    board->footprints = arena_alloc_array(arena, sizeof(*board->footprints), 2);
    board->pads = arena_alloc_array(arena, sizeof(*board->pads), 2);
    board->court_pts = arena_alloc_array(arena, sizeof(*board->court_pts), 8);
    board->edges = arena_alloc_array(arena, sizeof(*board->edges), 1);
    assert(board->layers && board->nets && board->footprints && board->pads &&
           board->court_pts && board->edges);
    board->layer_count = 2; board->net_count = 1; board->footprint_count = 2;
    board->pad_count = 2; board->court_pt_count = 8; board->edge_count = 1;
    board->layers[0] = (BoardLayerInfo){0, BOARD_LAYER_F_CU, BOARD_LAYER_TYPE_SIGNAL, NULL};
    board->layers[1] = (BoardLayerInfo){1, BOARD_LAYER_EDGE_CUTS, BOARD_LAYER_TYPE_USER, NULL};
    board->nets[0] = (BoardNet){1u, "LED_NET"};
    memset(board->footprints, 0, 2 * sizeof(*board->footprints));
    board->footprints[0].reference = "R1";
    board->footprints[0].uuid = "fp-r1";
    board->footprints[0].pos = (G2Point){20000000, 20000000};
    board->footprints[0].rotation_exact = true;
    board->footprints[0].placed = true;
    board->footprints[0].courtyard_exact = true;
    board->footprints[0].first_pad = 0; board->footprints[0].pad_count = 1;
    board->footprints[0].first_court = 0; board->footprints[0].court_count = 4;
    board->footprints[1].reference = "D1";
    board->footprints[1].uuid = "fp-d1";
    board->footprints[1].pos = (G2Point){60000000, 20000000};
    board->footprints[1].rotation_exact = true;
    board->footprints[1].placed = true;
    board->footprints[1].courtyard_exact = true;
    board->footprints[1].first_pad = 1; board->footprints[1].pad_count = 1;
    board->footprints[1].first_court = 4; board->footprints[1].court_count = 4;
    const G2Nm half = 1000000;
    for (size_t fi = 0; fi < 2; ++fi) {
        G2Point c = board->footprints[fi].pos;
        size_t p = fi * 4;
        board->court_pts[p + 0] = (G2Point){c.x-half, c.y-half};
        board->court_pts[p + 1] = (G2Point){c.x+half, c.y-half};
        board->court_pts[p + 2] = (G2Point){c.x+half, c.y+half};
        board->court_pts[p + 3] = (G2Point){c.x-half, c.y+half};
    }
    memset(board->pads, 0, 2 * sizeof(*board->pads));
    for (size_t i = 0; i < 2; ++i) {
        board->pads[i].footprint = (uint32_t)i;
        board->pads[i].number = "1";
        board->pads[i].type = BOARD_PAD_SMD;
        board->pads[i].shape = BOARD_PAD_RECT;
        board->pads[i].pos = board->footprints[i].pos;
        board->pads[i].size = (G2Point){1000000, 1000000};
        board->pads[i].layers = BOARD_LAYER_BIT(BOARD_LAYER_F_CU) |
                                BOARD_LAYER_BIT(BOARD_LAYER_F_MASK) |
                                BOARD_LAYER_BIT(BOARD_LAYER_F_PASTE);
        board->pads[i].net = 1u;
        board->pads[i].rotation_exact = true;
    }
    board->edges[0].kind = BOARD_EDGE_RECT;
    board->edges[0].p0 = (G2Point){0, 0};
    board->edges[0].p1 = (G2Point){100000000, 80000000};
    board->edges[0].width = 100000;
}

int main(void) {
    Arena arena;
    arena_init(&arena, 65536u);
    Board board;
    make_test_board(&arena, &board);
    BoardDiag diag;
    memset(&diag, 0, sizeof(diag));
    assert(board_validate(&board, &diag) == BOARD_OK);

    FabRuleSet rules;
    memset(&rules, 0, sizeof(rules));
    assert(fab_rules_load(COPPER_TEST_SOURCE_DIR "/jlcpcb_rules.rules",
                          &arena, "2026-11-17", 3650u, &rules) == FAB_OK);

    CopperOptOptions options = copper_opt_default_options();
    options.max_candidates = 32u;
    options.max_iterations = 2u;
    options.move_step_nm = 500000;
    options.component_gap_nm = 200000;
    options.move_edge_clearance_nm = 200000;
    options.propose_routes = true;
    options.router_options.layer = BOARD_LAYER_F_CU;
    options.router_options.grid_pitch_nm = 1000000;
    options.router_options.trace_width_nm = 200000;
    options.router_options.clearance_nm = 200000;
    options.router_options.edge_clearance_nm = 200000;
    options.router_options.max_cells = 100000u;
    options.dfm_options.copper_oz_x10 = 10u;
    options.dfm_options.edge_type = "routed";
    options.dfm_options.mask_color = "standard";

    CopperOptEvaluation evaluation;
    CopperOptStatus es = copper_opt_evaluate(&arena, &board, &diag, &rules,
                                              &options, &evaluation);
    assert(es == COPPER_OPT_OK || es == COPPER_OPT_STALLED);
    assert(evaluation.baseline.board_valid);
    assert(evaluation.baseline.dfm_available);
    assert(evaluation.candidate_count > 0u);
    /* Candidate eligibility is an evidence/safety gate, not a hard-coded
       objective decision. The decision provider ranks eligible alternatives. */
    bool has_eligible = false;
    const CopperOptCandidate *safe_nonpreferred = NULL;
    for (size_t ci = 0; ci < evaluation.candidate_count; ++ci) {
        if (!evaluation.candidates[ci].eligible) continue;
        has_eligible = true;
        if (!copper_opt_metrics_preferred(&evaluation.candidates[ci].metrics,
                                          &evaluation.baseline))
            safe_nonpreferred = &evaluation.candidates[ci];
    }
    assert(has_eligible);
    assert(safe_nonpreferred != NULL);
    CopperOptApplyResult policy_choice;
    assert(copper_opt_apply(&arena, &board, &diag, &rules, &options,
                            &safe_nonpreferred->action, &policy_choice) == COPPER_OPT_OK);
    assert(policy_choice.committed); /* provider may choose trade-offs after seeing metrics */

    /* A pad-rule change must invalidate an action even when its coordinates do not change. */
    Board changed_rules_board = board;
    BoardPad changed_pads[2];
    memcpy(changed_pads, board.pads, sizeof(changed_pads));
    changed_pads[0].clearance += 1;
    changed_rules_board.pads = changed_pads;
    assert(copper_opt_board_hash(&changed_rules_board) != evaluation.board_hash);
    CopperOptApplyResult stale_geometry;
    assert(copper_opt_apply(&arena, &changed_rules_board, &diag, &rules, &options,
                            &safe_nonpreferred->action, &stale_geometry) ==
           COPPER_OPT_ERR_STALE_ACTION);

    /* Fabrication rule revisions are part of the evaluation context hash. */
    FabRuleSet changed_fab_rules = rules;
    FabRule *changed_rule_array = arena_alloc_array(&arena, sizeof(*changed_rule_array),
                                                     rules.rule_count);
    assert(changed_rule_array != NULL);
    memcpy(changed_rule_array, rules.rules, rules.rule_count * sizeof(*changed_rule_array));
    changed_rule_array[0].value += 1;
    changed_fab_rules.rules = changed_rule_array;
    CopperOptApplyResult stale_fab_rules;
    assert(copper_opt_apply(&arena, &board, &diag, &changed_fab_rules, &options,
                            &safe_nonpreferred->action, &stale_fab_rules) ==
           COPPER_OPT_ERR_STALE_ACTION);

    CopperOptOptions changed_eval_options = options;
    changed_eval_options.move_step_nm += 1;
    CopperOptApplyResult stale_eval_config;
    assert(copper_opt_apply(&arena, &board, &diag, &rules, &changed_eval_options,
                            &safe_nonpreferred->action, &stale_eval_config) ==
           COPPER_OPT_ERR_STALE_ACTION);

    /* A policy can require physics evidence; without a callback it must wait. */
    CopperOptOptions missing_physics_options = options;
    missing_physics_options.require_physics_for_acceptance = true;
    CopperOptEvaluation missing_physics_evaluation;
    assert(copper_opt_evaluate(&arena, &board, &diag, &rules,
                               &missing_physics_options,
                               &missing_physics_evaluation) == COPPER_OPT_AWAITING_DECISION);
    assert(missing_physics_evaluation.eligible_count == 0u);

    /* Physics evidence is evaluated independently of DFM and is retained in
       the caller's arena for JSON serialization after candidate scratch arenas end. */
    MockPhysicsContext physics_context = {0};
    CopperOptOptions unidentified_physics_options = options;
    unidentified_physics_options.physics_evaluate = mock_physics_evaluate;
    unidentified_physics_options.physics_context = &physics_context;
    CopperOptEvaluation unidentified_physics_evaluation;
    assert(copper_opt_evaluate(&arena, &board, &diag, &rules,
                              &unidentified_physics_options,
                              &unidentified_physics_evaluation) == COPPER_OPT_ERR_ARGUMENT);

    CopperOptOptions physics_options = options;
    physics_options.physics_evaluate = mock_physics_evaluate;
    physics_options.physics_context = &physics_context;
    physics_options.physics_provider_id = "test.mock-physics.v1";
    physics_options.require_physics_for_acceptance = true;
    CopperOptEvaluation physics_evaluation;
    CopperOptStatus physics_status = copper_opt_evaluate(
        &arena, &board, &diag, &rules, &physics_options, &physics_evaluation);
    assert(physics_status == COPPER_OPT_OK || physics_status == COPPER_OPT_STALLED);
    assert(physics_evaluation.baseline.physics_available);
    assert(physics_evaluation.baseline.has_physics_objective);
    assert(physics_evaluation.baseline.physics_summary != NULL);
    assert(physics_context.calls >= physics_evaluation.candidate_count + 1u);
    char *physics_json = copper_opt_evaluation_json(&physics_evaluation, NULL);
    assert(physics_json != NULL);
    assert(strstr(physics_json, "physics_available") != NULL);
    assert(strstr(physics_json, "mock deterministic physics check") != NULL);
    assert(strstr(physics_json, "manufacturing_release_ready\":false") != NULL);
    free(physics_json);

    bool found_move = false;
    const CopperOptCandidate *selected = NULL;
    for (size_t i = 0; i < evaluation.candidate_count; ++i) {
        const CopperOptCandidate *c = &evaluation.candidates[i];
        if (c->action.kind == COPPER_OPT_ACTION_MOVE_FOOTPRINT && c->eligible) {
            found_move = true; selected = c; break;
        }
    }
    assert(found_move && selected);
    assert(selected->metrics.hpwl_nm < evaluation.baseline.hpwl_nm);

    const CopperOptCandidate *route_candidate = NULL;
    for (size_t i = 0; i < evaluation.candidate_count; ++i) {
        const CopperOptCandidate *c = &evaluation.candidates[i];
        if (c->action.kind == COPPER_OPT_ACTION_ROUTE_TWO_PAD_NET && c->eligible) {
            route_candidate = c; break;
        }
    }
    assert(route_candidate);
    CopperOptApplyResult routed;
    assert(copper_opt_apply(&arena, &board, &diag, &rules, &options,
                            &route_candidate->action, &routed) == COPPER_OPT_OK);
    assert(routed.committed && routed.added_track_count > 0u);
    assert(routed.board.track_count == board.track_count + routed.added_track_count);
    assert(routed.after.two_pad_nets_without_tracks < routed.before.two_pad_nets_without_tracks);

    CopperOptApplyResult applied;
    assert(copper_opt_apply(&arena, &board, &diag, &rules, &options,
                            &selected->action, &applied) == COPPER_OPT_OK);
    assert(applied.committed);
    assert(applied.after.hpwl_nm < applied.before.hpwl_nm);
    assert(board.footprints[selected->action.footprint_index].pos.x !=
           applied.board.footprints[selected->action.footprint_index].pos.x ||
           board.footprints[selected->action.footprint_index].pos.y !=
           applied.board.footprints[selected->action.footprint_index].pos.y);

    CopperOptApplyResult stale;
    assert(copper_opt_apply(&arena, &applied.board, &diag, &rules, &options,
                            &selected->action, &stale) == COPPER_OPT_ERR_STALE_ACTION);

    size_t json_length = 0;
    char *json = copper_opt_evaluation_json(&evaluation, &json_length);
    assert(json && json_length > 100u);
    assert(strstr(json, "copper.optimization-evaluation") != NULL);
    assert(strstr(json, "candidate_id") != NULL);
    free(json);
    json = copper_opt_apply_json(&applied, &json_length);
    assert(json && strstr(json, "native_kicad_export") != NULL);
    free(json);

    CopperOptRunResult run;
    assert(copper_opt_run(&arena, &board, &diag, &rules, &options,
                          choose_first_eligible, NULL, &run) == COPPER_OPT_OK ||
           run.status == COPPER_OPT_STALLED);
    assert(run.history_count >= 1u);
    assert(run.accepted_actions <= options.max_iterations);
    json = copper_opt_run_json(&run, &json_length);
    assert(json && strstr(json, "decision_callback") != NULL);
    free(json);

    arena_destroy(&arena);
    puts("Copper optimization loop tests passed.");
    return 0;
}

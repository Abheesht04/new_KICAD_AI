#ifndef COPPER_OPTIMIZATION_LOOP_H
#define COPPER_OPTIMIZATION_LOOP_H

#include "design_board.h"
#include "dfm_check.h"
#include "fab_rules.h"
#include "router.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum CopperOptStatus {
    COPPER_OPT_OK = 0,
    COPPER_OPT_AWAITING_DECISION = 1,
    COPPER_OPT_STALLED = 2,
    COPPER_OPT_STOPPED = 3,
    COPPER_OPT_ERR_ARGUMENT = -1,
    COPPER_OPT_ERR_MEMORY = -2,
    COPPER_OPT_ERR_BOARD = -3,
    COPPER_OPT_ERR_RULES = -4,
    COPPER_OPT_ERR_STALE_ACTION = -5,
    COPPER_OPT_ERR_INFEASIBLE = -6
} CopperOptStatus;

typedef enum CopperOptActionKind {
    COPPER_OPT_ACTION_MOVE_FOOTPRINT = 1,
    COPPER_OPT_ACTION_ROUTE_TWO_PAD_NET = 2
} CopperOptActionKind;

typedef struct CopperOptPhysicsFeedback {
    bool valid;
    size_t fail_count;
    size_t unverified_count;
    bool has_objective;
    double objective;
    const char *summary;
} CopperOptPhysicsFeedback;

typedef bool (*CopperOptPhysicsEvaluateFn)(void *context, const Board *candidate,
                                           CopperOptPhysicsFeedback *out);

typedef struct CopperOptMetrics {
    bool board_valid;
    bool dfm_available;
    size_t dfm_fail_count;
    size_t dfm_unverified_count;
    uint64_t hpwl_nm;
    uint64_t track_manhattan_nm;
    size_t two_pad_nets_without_tracks;
    size_t footprint_count;
    size_t track_count;
    bool physics_available;
    size_t physics_fail_count;
    size_t physics_unverified_count;
    bool has_physics_objective;
    double physics_objective;
    const char *physics_summary;
} CopperOptMetrics;

typedef struct CopperOptAction {
    uint64_t candidate_id;
    uint64_t base_board_hash;
    uint64_t evaluation_hash; /* board + fabrication rules + deterministic options */
    CopperOptActionKind kind;
    uint32_t footprint_index;
    G2Point delta_nm;
    uint32_t net_id;
    RouterOptions router;
} CopperOptAction;

typedef struct CopperOptCandidate {
    CopperOptAction action;
    CopperOptMetrics metrics;
    bool eligible;
    RouterStatus router_status;
    size_t added_track_count;
    const char *reason;
} CopperOptCandidate;

typedef struct CopperOptOptions {
    size_t max_candidates;
    size_t max_iterations;
    G2Nm move_step_nm;
    G2Nm component_gap_nm;
    G2Nm move_edge_clearance_nm;
    bool propose_moves;
    bool propose_routes;
    bool require_dfm_for_acceptance;
    bool require_physics_for_acceptance;
    CopperOptPhysicsEvaluateFn physics_evaluate;
    void *physics_context;
    const char *physics_provider_id; /* stable model/version identity required with callback */
    size_t dfm_finding_capacity;
    DfmOptions dfm_options;
    RouterOptions router_options;
} CopperOptOptions;

typedef struct CopperOptEvaluation {
    CopperOptStatus status;
    uint64_t board_hash;
    uint64_t evaluation_hash;
    CopperOptOptions options; /* snapshot of deterministic evaluation settings */
    CopperOptMetrics baseline;
    CopperOptCandidate *candidates;
    size_t candidate_count;
    size_t eligible_count;
    bool candidates_truncated;
    const char *message;
} CopperOptEvaluation;

typedef struct CopperOptApplyResult {
    CopperOptStatus status;
    Board board; /* arena-owned candidate board; source Board remains unchanged */
    CopperOptMetrics before;
    CopperOptMetrics after;
    CopperOptAction applied_action;
    size_t added_track_count;
    RouterStatus route_status;
    const char *message;
    bool committed;
} CopperOptApplyResult;

typedef struct CopperOptIterationSummary {
    size_t iteration;
    CopperOptMetrics before;
    CopperOptMetrics after;
    size_t candidate_count;
    size_t eligible_count;
    uint64_t selected_candidate_id;
    CopperOptAction selected_action;
    bool action_committed;
} CopperOptIterationSummary;

typedef struct CopperOptRunResult {
    CopperOptStatus status;
    Board final_board; /* arena-owned; valid after at least one successful init */
    CopperOptMetrics final_metrics;
    CopperOptIterationSummary *history;
    size_t history_count;
    size_t accepted_actions;
    size_t max_iterations;
    CopperOptOptions options; /* snapshot; callback pointers are not serialized */
    const char *message;
} CopperOptRunResult;

/* Optional physics callback can invoke MNA, component stress, thermal or a
 * future extracted-parasitic solver for each candidate Board. It must be
 * deterministic for a fixed board/options. If configured, its FAIL/UNVERIFIED
 * counts participate in candidate safety gates. The callback owns its scratch
 * arenas and must not mutate Board. Set physics_provider_id to a stable model/version
 * string; it is incorporated into candidate identity so stale evidence is rejected. */

/* A decision provider receives all evidence and every candidate that passes
 * configured safety gates. Eligibility does NOT mean that Copper prefers the
 * candidate: the provider owns objective trade-offs and must return an eligible
 * candidate_id, or zero to stop. This is the AI integration boundary. */
typedef bool (*CopperOptDecisionFn)(
    void *context, size_t iteration,
    const CopperOptMetrics *baseline,
    const CopperOptCandidate *candidates, size_t candidate_count,
    uint64_t *selected_candidate_id);

CopperOptOptions copper_opt_default_options(void);
/* Deterministic developer-policy ranking helper. It does not gate candidate
 * admissibility; an external decision provider may choose any eligible action. */
bool copper_opt_metrics_preferred(const CopperOptMetrics *a,
                                  const CopperOptMetrics *b);
uint64_t copper_opt_board_hash(const Board *board);
CopperOptStatus copper_opt_evaluate(
    Arena *arena, const Board *board, const BoardDiag *board_diag,
    const FabRuleSet *rules, const CopperOptOptions *options,
    CopperOptEvaluation *out);
CopperOptStatus copper_opt_apply(
    Arena *arena, const Board *board, const BoardDiag *board_diag,
    const FabRuleSet *rules, const CopperOptOptions *options,
    const CopperOptAction *action, CopperOptApplyResult *out);
CopperOptStatus copper_opt_run(
    Arena *arena, const Board *initial_board, const BoardDiag *board_diag,
    const FabRuleSet *rules, const CopperOptOptions *options,
    CopperOptDecisionFn decision, void *decision_context,
    CopperOptRunResult *out);
const char *copper_opt_status_name(CopperOptStatus status);
const char *copper_opt_action_name(CopperOptActionKind kind);

#ifdef __cplusplus
}
#endif
#endif

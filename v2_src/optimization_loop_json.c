#include "optimization_loop_json.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OPT_JSON_LIMIT (16u * 1024u * 1024u)

typedef struct OptJsonBuffer {
    char *data;
    size_t length;
    size_t capacity;
    bool failed;
} OptJsonBuffer;

static bool reserve(OptJsonBuffer *b, size_t extra) {
    if (!b || b->failed || extra > OPT_JSON_LIMIT - b->length - 1u) return false;
    size_t need = b->length + extra + 1u;
    if (need <= b->capacity) return true;
    size_t capacity = b->capacity ? b->capacity : 1024u;
    while (capacity < need) {
        if (capacity > OPT_JSON_LIMIT / 2u) { capacity = OPT_JSON_LIMIT; break; }
        capacity *= 2u;
    }
    char *next = (char *)realloc(b->data, capacity);
    if (!next) { b->failed = true; return false; }
    b->data = next; b->capacity = capacity;
    if (b->length == 0u) b->data[0] = '\0';
    return true;
}

static void raw(OptJsonBuffer *b, const char *s) {
    if (!b || !s) return;
    size_t n = strlen(s);
    if (!reserve(b, n)) { b->failed = true; return; }
    memcpy(b->data + b->length, s, n);
    b->length += n; b->data[b->length] = '\0';
}

static void fmt(OptJsonBuffer *b, const char *format, ...) {
    if (!b || !format || b->failed) return;
    char temp[192];
    va_list args;
    va_start(args, format);
    int count = vsnprintf(temp, sizeof(temp), format, args);
    va_end(args);
    if (count < 0 || (size_t)count >= sizeof(temp) || !reserve(b, (size_t)count)) {
        b->failed = true; return;
    }
    memcpy(b->data + b->length, temp, (size_t)count);
    b->length += (size_t)count; b->data[b->length] = '\0';
}

static void str(OptJsonBuffer *b, const char *s) {
    raw(b, "\"");
    if (!s) { raw(b, "\""); return; }
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        char escaped[7];
        switch (*p) {
        case '"': raw(b, "\\\""); break;
        case '\\': raw(b, "\\\\"); break;
        case '\b': raw(b, "\\b"); break;
        case '\f': raw(b, "\\f"); break;
        case '\n': raw(b, "\\n"); break;
        case '\r': raw(b, "\\r"); break;
        case '\t': raw(b, "\\t"); break;
        default:
            if (*p < 0x20u) {
                (void)snprintf(escaped, sizeof(escaped), "\\u%04x", (unsigned)*p);
                raw(b, escaped);
            } else {
                if (!reserve(b, 1u)) { b->failed = true; break; }
                b->data[b->length++] = (char)*p;
                b->data[b->length] = '\0';
            }
            break;
        }
    }
    raw(b, "\"");
}

static void boolean(OptJsonBuffer *b, bool value) { raw(b, value ? "true" : "false"); }


static void options_json(OptJsonBuffer *b, const CopperOptOptions *o) {
    if (!o) { raw(b, "null"); return; }
    raw(b, "{\"max_candidates\":"); fmt(b, "%zu", o->max_candidates);
    raw(b, ",\"max_iterations\":"); fmt(b, "%zu", o->max_iterations);
    raw(b, ",\"move_step_nm\":"); fmt(b, "%lld", (long long)o->move_step_nm);
    raw(b, ",\"component_gap_nm\":"); fmt(b, "%lld", (long long)o->component_gap_nm);
    raw(b, ",\"move_edge_clearance_nm\":"); fmt(b, "%lld", (long long)o->move_edge_clearance_nm);
    raw(b, ",\"propose_moves\":"); boolean(b, o->propose_moves);
    raw(b, ",\"propose_routes\":"); boolean(b, o->propose_routes);
    raw(b, ",\"require_dfm_for_acceptance\":"); boolean(b, o->require_dfm_for_acceptance);
    raw(b, ",\"require_physics_for_acceptance\":"); boolean(b, o->require_physics_for_acceptance);
    raw(b, ",\"physics_evaluate_configured\":"); boolean(b, o->physics_evaluate != NULL);
    raw(b, ",\"physics_provider_id\":"); if (o->physics_provider_id) str(b, o->physics_provider_id); else raw(b, "null");
    raw(b, ",\"dfm\":{\"copper_oz_x10\":"); fmt(b, "%u", o->dfm_options.copper_oz_x10);
    raw(b, ",\"edge_type\":"); if (o->dfm_options.edge_type) str(b, o->dfm_options.edge_type); else raw(b, "null");
    raw(b, ",\"mask_color\":"); if (o->dfm_options.mask_color) str(b, o->dfm_options.mask_color); else raw(b, "null");
    raw(b, "},\"router\":{\"layer\":"); str(b, board_layer_name(o->router_options.layer));
    raw(b, ",\"grid_pitch_nm\":"); fmt(b, "%lld", (long long)o->router_options.grid_pitch_nm);
    raw(b, ",\"trace_width_nm\":"); fmt(b, "%lld", (long long)o->router_options.trace_width_nm);
    raw(b, ",\"clearance_nm\":"); fmt(b, "%lld", (long long)o->router_options.clearance_nm);
    raw(b, ",\"edge_clearance_nm\":"); fmt(b, "%lld", (long long)o->router_options.edge_clearance_nm);
    raw(b, ",\"max_cells\":"); fmt(b, "%zu", o->router_options.max_cells);
    raw(b, "}}");
}

static void metrics_json(OptJsonBuffer *b, const CopperOptMetrics *m) {
    raw(b, "{\"board_valid\":"); boolean(b, m->board_valid);
    raw(b, ",\"dfm_available\":"); boolean(b, m->dfm_available);
    raw(b, ",\"dfm_fail_count\":"); fmt(b, "%zu", m->dfm_fail_count);
    raw(b, ",\"dfm_unverified_count\":"); fmt(b, "%zu", m->dfm_unverified_count);
    raw(b, ",\"hpwl_nm\":"); fmt(b, "%llu", (unsigned long long)m->hpwl_nm);
    raw(b, ",\"track_manhattan_nm\":"); fmt(b, "%llu", (unsigned long long)m->track_manhattan_nm);
    raw(b, ",\"two_pad_nets_without_tracks\":"); fmt(b, "%zu", m->two_pad_nets_without_tracks);
    raw(b, ",\"footprint_count\":"); fmt(b, "%zu", m->footprint_count);
    raw(b, ",\"track_count\":"); fmt(b, "%zu", m->track_count);
    raw(b, ",\"physics_available\":"); boolean(b, m->physics_available);
    raw(b, ",\"physics_fail_count\":"); fmt(b, "%zu", m->physics_fail_count);
    raw(b, ",\"physics_unverified_count\":"); fmt(b, "%zu", m->physics_unverified_count);
    raw(b, ",\"has_physics_objective\":"); boolean(b, m->has_physics_objective);
    raw(b, ",\"physics_objective\":");
    if (m->has_physics_objective) fmt(b, "%.17g", m->physics_objective);
    else raw(b, "null");
    raw(b, ",\"physics_summary\":");
    if (m->physics_summary) str(b, m->physics_summary); else raw(b, "null");
    raw(b, "}");
}

static void action_json(OptJsonBuffer *b, const CopperOptAction *a) {
    raw(b, "{\"candidate_id\":"); fmt(b, "\"%016llx\"", (unsigned long long)a->candidate_id);
    raw(b, ",\"base_board_hash\":"); fmt(b, "\"%016llx\"", (unsigned long long)a->base_board_hash);
    raw(b, ",\"evaluation_hash\":"); fmt(b, "\"%016llx\"", (unsigned long long)a->evaluation_hash);
    raw(b, ",\"kind\":"); str(b, copper_opt_action_name(a->kind));
    raw(b, ",\"footprint_index\":"); fmt(b, "%u", a->footprint_index);
    raw(b, ",\"delta_nm\":{\"x\":"); fmt(b, "%lld", (long long)a->delta_nm.x);
    raw(b, ",\"y\":"); fmt(b, "%lld", (long long)a->delta_nm.y); raw(b, "}");
    raw(b, ",\"net_id\":"); fmt(b, "%u", a->net_id);
    raw(b, ",\"router\":{\"layer\":"); str(b, board_layer_name(a->router.layer));
    raw(b, ",\"grid_pitch_nm\":"); fmt(b, "%lld", (long long)a->router.grid_pitch_nm);
    raw(b, ",\"trace_width_nm\":"); fmt(b, "%lld", (long long)a->router.trace_width_nm);
    raw(b, ",\"clearance_nm\":"); fmt(b, "%lld", (long long)a->router.clearance_nm);
    raw(b, ",\"edge_clearance_nm\":"); fmt(b, "%lld", (long long)a->router.edge_clearance_nm);
    raw(b, ",\"max_cells\":"); fmt(b, "%zu", a->router.max_cells);
    raw(b, "}}");
}

char *copper_opt_evaluation_json(const CopperOptEvaluation *e,
                                 size_t *out_length) {
    if (out_length) *out_length = 0;
    if (!e) return NULL;
    OptJsonBuffer b = {0};
    raw(&b, "{\"schema\":\"copper.optimization-evaluation\",\"schema_version\":1,\"status\":");
    str(&b, copper_opt_status_name(e->status));
        raw(&b, ",\"decision_authority\":\"external_agent_or_user\",\"manufacturing_release_ready\":false,\"board_hash\":");
    fmt(&b, "\"%016llx\"", (unsigned long long)e->board_hash);
    raw(&b, ",\"evaluation_hash\":"); fmt(&b, "\"%016llx\"", (unsigned long long)e->evaluation_hash);
    raw(&b, ",\"evaluation_config\":"); options_json(&b, &e->options);
    raw(&b, ",\"message\":"); str(&b, e->message);
    raw(&b, ",\"baseline\":"); metrics_json(&b, &e->baseline);
    raw(&b, ",\"summary\":{\"candidate_count\":"); fmt(&b, "%zu", e->candidate_count);
    raw(&b, ",\"eligible_count\":"); fmt(&b, "%zu", e->eligible_count);
    raw(&b, ",\"candidates_truncated\":"); boolean(&b, e->candidates_truncated); raw(&b, "}");
    raw(&b, ",\"candidates\":[");
    for (size_t i = 0; i < e->candidate_count; ++i) {
        const CopperOptCandidate *c = &e->candidates[i];
        if (i) raw(&b, ",");
        raw(&b, "{\"action\":"); action_json(&b, &c->action);
        raw(&b, ",\"eligible_for_iteration\":"); boolean(&b, c->eligible);
    raw(&b, ",\"manufacturing_release_ready\":false");
        raw(&b, ",\"router_status\":");
        if (c->action.kind == COPPER_OPT_ACTION_ROUTE_TWO_PAD_NET)
            fmt(&b, "%d", (int)c->router_status);
        else raw(&b, "null");
        raw(&b, ",\"added_track_count\":"); fmt(&b, "%zu", c->added_track_count);
        raw(&b, ",\"metrics\":"); metrics_json(&b, &c->metrics);
        raw(&b, ",\"reason\":"); str(&b, c->reason); raw(&b, "}");
    }
    raw(&b, "]}");
    if (b.failed) { free(b.data); return NULL; }
    if (out_length) *out_length = b.length;
    return b.data;
}

static void board_snapshot_json(OptJsonBuffer *b, const Board *board) {
    raw(b, "{\"snapshot_scope\":\"footprint_positions_and_tracks_only\",\"is_partial_snapshot\":true,\"board_hash\":");
    if (!board) { raw(b, "null,\"footprints\":[],\"tracks\":[]}"); return; }
    fmt(b, "\"%016llx\"", (unsigned long long)copper_opt_board_hash(board));
    raw(b, ",\"footprints\":[");
    for (size_t i = 0; i < board->footprint_count; ++i) {
        const BoardFootprint *fp = &board->footprints[i];
        if (i) raw(b, ",");
        raw(b, "{\"reference\":"); str(b, fp->reference);
        raw(b, ",\"uuid\":"); str(b, fp->uuid);
        raw(b, ",\"x_nm\":"); fmt(b, "%lld", (long long)fp->pos.x);
        raw(b, ",\"y_nm\":"); fmt(b, "%lld", (long long)fp->pos.y);
        raw(b, ",\"rotation_mdeg\":"); fmt(b, "%d", fp->rot_mdeg);
        raw(b, "}");
    }
    raw(b, "],\"tracks\":[");
    for (size_t i = 0; i < board->track_count; ++i) {
        const BoardTrack *t = &board->tracks[i];
        if (i) raw(b, ",");
        raw(b, "{\"net_id\":"); fmt(b, "%u", t->net);
        raw(b, ",\"layer\":"); str(b, board_layer_name(t->layer));
        raw(b, ",\"width_nm\":"); fmt(b, "%lld", (long long)t->width);
        raw(b, ",\"a\":{\"x\":"); fmt(b, "%lld", (long long)t->a.x);
        raw(b, ",\"y\":"); fmt(b, "%lld", (long long)t->a.y); raw(b, "}");
        raw(b, ",\"b\":{\"x\":"); fmt(b, "%lld", (long long)t->b.x);
        raw(b, ",\"y\":"); fmt(b, "%lld", (long long)t->b.y); raw(b, "}}");
    }
    raw(b, "]}");
}

char *copper_opt_apply_json(const CopperOptApplyResult *r,
                            size_t *out_length) {
    if (out_length) *out_length = 0;
    if (!r) return NULL;
    OptJsonBuffer b = {0};
    raw(&b, "{\"schema\":\"copper.optimization-apply\",\"schema_version\":1,\"status\":");
    str(&b, copper_opt_status_name(r->status));
    raw(&b, ",\"committed\":"); boolean(&b, r->committed);
    raw(&b, ",\"manufacturing_release_ready\":false");
    raw(&b, ",\"message\":"); str(&b, r->message);
    raw(&b, ",\"action\":"); action_json(&b, &r->applied_action);
    raw(&b, ",\"added_track_count\":"); fmt(&b, "%zu", r->added_track_count);
    raw(&b, ",\"router_status\":");
    if (r->applied_action.kind == COPPER_OPT_ACTION_ROUTE_TWO_PAD_NET) fmt(&b, "%d", (int)r->route_status);
    else raw(&b, "null");
    raw(&b, ",\"before\":"); metrics_json(&b, &r->before);
    raw(&b, ",\"after\":"); metrics_json(&b, &r->after);
    raw(&b, ",\"board_state\":");
    board_snapshot_json(&b, &r->board);
    raw(&b, ",\"native_kicad_export\":\"NOT_IMPLEMENTED\"}");
    if (b.failed) { free(b.data); return NULL; }
    if (out_length) *out_length = b.length;
    return b.data;
}

char *copper_opt_run_json(const CopperOptRunResult *r,
                          size_t *out_length) {
    if (out_length) *out_length = 0;
    if (!r) return NULL;
    OptJsonBuffer b = {0};
    raw(&b, "{\"schema\":\"copper.optimization-run\",\"schema_version\":1,\"status\":");
    str(&b, copper_opt_status_name(r->status));
    raw(&b, ",\"message\":"); str(&b, r->message);
    raw(&b, ",\"manufacturing_release_ready\":false");
    raw(&b, ",\"decision_authority\":\"decision_callback\",\"run_config\":");
    options_json(&b, &r->options);
    raw(&b, ",\"summary\":{\"iterations\":");
    fmt(&b, "%zu", r->history_count);
    raw(&b, ",\"accepted_actions\":"); fmt(&b, "%zu", r->accepted_actions);
    raw(&b, ",\"max_iterations\":"); fmt(&b, "%zu", r->max_iterations); raw(&b, "}");
    raw(&b, ",\"final_metrics\":"); metrics_json(&b, &r->final_metrics);
    raw(&b, ",\"history\":[");
    for (size_t i = 0; i < r->history_count; ++i) {
        const CopperOptIterationSummary *it = &r->history[i];
        if (i) raw(&b, ",");
        raw(&b, "{\"iteration\":"); fmt(&b, "%zu", it->iteration);
        raw(&b, ",\"before\":"); metrics_json(&b, &it->before);
        raw(&b, ",\"after\":"); metrics_json(&b, &it->after);
        raw(&b, ",\"candidate_count\":"); fmt(&b, "%zu", it->candidate_count);
        raw(&b, ",\"eligible_count\":"); fmt(&b, "%zu", it->eligible_count);
        raw(&b, ",\"selected_candidate_id\":");
        if (it->selected_candidate_id)
            fmt(&b, "\"%016llx\"", (unsigned long long)it->selected_candidate_id);
        else raw(&b, "null");
        raw(&b, ",\"selected_action\":");
        if (it->selected_candidate_id) action_json(&b, &it->selected_action);
        else raw(&b, "null");
        raw(&b, ",\"action_committed\":"); boolean(&b, it->action_committed); raw(&b, "}");
    }
    raw(&b, "],\"final_board_hash\":");
    fmt(&b, "\"%016llx\"", (unsigned long long)copper_opt_board_hash(&r->final_board));
    raw(&b, ",\"final_board_state\":");
    board_snapshot_json(&b, &r->final_board);
    raw(&b, "}");
    if (b.failed) { free(b.data); return NULL; }
    if (out_length) *out_length = b.length;
    return b.data;
}

FsStatus copper_opt_write_json(const char *path, const char *json,
                               size_t json_length) {
    if (!path || !json) return FS_ERR_ARGUMENT;
    return fs_write_file_atomic_replace(path, json, json_length);
}

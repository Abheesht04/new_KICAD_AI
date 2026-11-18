#include "optimization_loop.h"

#include <limits.h>
#include <string.h>
#include <math.h>

#define OPT_FNV_OFFSET UINT64_C(1469598103934665603)
#define OPT_FNV_PRIME UINT64_C(1099511628211)

static uint64_t hash_u64(uint64_t h, uint64_t v) {
    for (unsigned i = 0; i < 8u; ++i) {
        h ^= (uint8_t)(v & UINT64_C(0xff));
        h *= OPT_FNV_PRIME;
        v >>= 8;
    }
    return h;
}

static uint64_t hash_i64(uint64_t h, int64_t v) { return hash_u64(h, (uint64_t)v); }

static uint64_t hash_text(uint64_t h, const char *text) {
    if (!text) return hash_u64(h, 0u);
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        h ^= *p;
        h *= OPT_FNV_PRIME;
    }
    return hash_u64(h, UINT64_C(0xff));
}

static uint64_t hash_bytes(uint64_t h, const char *bytes, size_t length) {
    if (!bytes) return hash_u64(h, 0u);
    h = hash_u64(h, length);
    for (size_t i = 0; i < length; ++i) {
        h ^= (unsigned char)bytes[i];
        h *= OPT_FNV_PRIME;
    }
    return h;
}

static bool hash_ir_node(uint64_t *hash, const IRNode *node,
                         size_t depth, size_t *node_count) {
    if (!hash || !node || !node_count || depth > 2048u ||
        *node_count >= 2000000u) return false;
    ++*node_count;
    *hash = hash_u64(*hash, (uint64_t)node->kind);
    if (node->kind == IR_NODE_ATOM || node->kind == IR_NODE_STRING)
        *hash = hash_bytes(*hash, node->text, node->text_len);
    else if (node->kind != IR_NODE_LIST) return false;
    for (const IRNode *child = node->first_child; child; child = child->next) {
        if (!hash_ir_node(hash, child, depth + 1u, node_count)) return false;
    }
    *hash = hash_u64(*hash, UINT64_C(0xfedcba9876543210));
    return true;
}

static uint64_t sat_add_u64(uint64_t a, uint64_t b) {
    return b > UINT64_MAX - a ? UINT64_MAX : a + b;
}

static uint64_t abs_diff_i64(int64_t a, int64_t b) {
    if (a >= b) return (uint64_t)a - (uint64_t)b;
    return (uint64_t)b - (uint64_t)a;
}

static bool add_nm(int64_t a, int64_t b, int64_t *out) {
    if (!out || (b > 0 && a > INT64_MAX - b) ||
        (b < 0 && a < INT64_MIN - b)) return false;
    *out = a + b;
    return true;
}

static bool copy_array(Arena *arena, const void *source, size_t count,
                       size_t element_size, void **out) {
    if (!out || !arena) return false;
    *out = NULL;
    if (count == 0u) return true;
    if (!source) return false;
    void *memory = arena_alloc_array(arena, element_size, count);
    if (!memory) return false;
    memcpy(memory, source, count * element_size);
    *out = memory;
    return true;
}

static bool board_clone(Arena *arena, const Board *source, Board *out) {
    if (!arena || !source || !out) return false;
    *out = *source;
#define CLONE_FIELD(field, count) \
    do { \
        void *copy = NULL; \
        if (!copy_array(arena, source->field, source->count, \
                        sizeof(*source->field), &copy)) return false; \
        out->field = copy; \
    } while (0)
    CLONE_FIELD(layers, layer_count);
    CLONE_FIELD(nets, net_count);
    CLONE_FIELD(footprints, footprint_count);
    CLONE_FIELD(pads, pad_count);
    CLONE_FIELD(tracks, track_count);
    CLONE_FIELD(arc_tracks, arc_track_count);
    CLONE_FIELD(vias, via_count);
    CLONE_FIELD(zones, zone_count);
    CLONE_FIELD(edges, edge_count);
    CLONE_FIELD(court_pts, court_pt_count);
    CLONE_FIELD(zone_pts, zone_pt_count);
    CLONE_FIELD(edge_pts, edge_pt_count);
#undef CLONE_FIELD
    out->arena = arena;
    return true;
}

uint64_t copper_opt_board_hash(const Board *board) {
    if (!board || (board->layer_count && !board->layers) ||
        (board->net_count && !board->nets) ||
        (board->footprint_count && !board->footprints) ||
        (board->pad_count && !board->pads) ||
        (board->track_count && !board->tracks) ||
        (board->arc_track_count && !board->arc_tracks) ||
        (board->via_count && !board->vias) ||
        (board->zone_count && !board->zones) ||
        (board->edge_count && !board->edges) ||
        (board->court_pt_count && !board->court_pts) ||
        (board->zone_pt_count && !board->zone_pts) ||
        (board->edge_pt_count && !board->edge_pts)) return 0;
    uint64_t h = OPT_FNV_OFFSET;
    h = hash_u64(h, board->layer_count); h = hash_u64(h, board->net_count);
    h = hash_u64(h, board->footprint_count); h = hash_u64(h, board->pad_count);
    h = hash_u64(h, board->track_count); h = hash_u64(h, board->arc_track_count);
    h = hash_u64(h, board->via_count); h = hash_u64(h, board->zone_count);
    h = hash_u64(h, board->edge_count); h = hash_u64(h, board->court_pt_count);
    h = hash_u64(h, board->zone_pt_count); h = hash_u64(h, board->edge_pt_count);
    h = hash_u64(h, board->rules.copper_layers);
    h = hash_i64(h, board->rules.board_thickness);
    h = hash_i64(h, board->rules.min_clearance);
    h = hash_i64(h, board->rules.min_track_width);
    h = hash_i64(h, board->rules.min_via_size);
    h = hash_i64(h, board->rules.min_via_drill);
    h = hash_i64(h, board->rules.min_hole_to_hole);
    h = hash_i64(h, board->rules.min_annular);
    h = hash_i64(h, board->rules.min_through_hole);
    h = hash_i64(h, board->rules.edge_clearance);
    h = hash_i64(h, board->rules.mask_margin);
    h = hash_i64(h, board->rules.paste_margin);
    h = hash_u64(h, board->kicad_file_version);
    for (size_t i = 0; i < board->layer_count; ++i) {
        h = hash_u64(h, board->layers[i].ordinal);
        h = hash_u64(h, (uint32_t)board->layers[i].id);
        h = hash_u64(h, (uint32_t)board->layers[i].type);
        h = hash_text(h, board->layers[i].user_name);
    }
    for (size_t i = 0; i < board->net_count; ++i) {
        h = hash_u64(h, board->nets[i].id);
        h = hash_text(h, board->nets[i].name);
    }
    for (size_t i = 0; i < board->footprint_count; ++i) {
        const BoardFootprint *fp = &board->footprints[i];
        h = hash_i64(h, fp->pos.x); h = hash_i64(h, fp->pos.y);
        h = hash_u64(h, (uint32_t)fp->rot_mdeg);
        h = hash_u64(h, fp->locked ? 1u : 0u);
        h = hash_text(h, fp->uuid); h = hash_text(h, fp->reference);
        h = hash_text(h, fp->value); h = hash_text(h, fp->lib_id);
        h = hash_text(h, fp->part_id); h = hash_text(h, fp->manufacturer);
        h = hash_text(h, fp->mpn); h = hash_text(h, fp->lcsc_id);
        h = hash_text(h, fp->path);
        h = hash_u64(h, (uint32_t)fp->layer);
        h = hash_u64(h, fp->on_back ? 1u : 0u);
        h = hash_u64(h, fp->rotation_exact ? 1u : 0u);
        h = hash_u64(h, fp->placed ? 1u : 0u);
        h = hash_u64(h, fp->dnp ? 1u : 0u);
        h = hash_u64(h, fp->exclude_from_bom ? 1u : 0u);
        h = hash_u64(h, fp->exclude_from_pos ? 1u : 0u);
        h = hash_u64(h, fp->courtyard_exact ? 1u : 0u);
        h = hash_u64(h, fp->first_pad); h = hash_u64(h, fp->pad_count);
        h = hash_u64(h, fp->first_court); h = hash_u64(h, fp->court_count);
    }
    for (size_t i = 0; i < board->pad_count; ++i) {
        const BoardPad *pad = &board->pads[i];
        h = hash_i64(h, pad->pos.x); h = hash_i64(h, pad->pos.y);
        h = hash_u64(h, pad->net); h = hash_u64(h, pad->footprint);
        h = hash_text(h, pad->number);
        h = hash_u64(h, (uint32_t)pad->type);
        h = hash_u64(h, (uint32_t)pad->shape);
        h = hash_i64(h, pad->rot_mdeg);
        h = hash_i64(h, pad->size.x); h = hash_i64(h, pad->size.y);
        h = hash_i64(h, pad->roundrect_ratio_ppm);
        h = hash_i64(h, pad->drill.x); h = hash_i64(h, pad->drill.y);
        h = hash_i64(h, pad->drill_offset.x); h = hash_i64(h, pad->drill_offset.y);
        h = hash_u64(h, pad->layers);
        h = hash_i64(h, pad->clearance); h = hash_i64(h, pad->mask_margin);
        h = hash_i64(h, pad->paste_margin);
        h = hash_u64(h, pad->rotation_exact ? 1u : 0u);
        h = hash_u64(h, pad->locked ? 1u : 0u);
    }
    for (size_t i = 0; i < board->track_count; ++i) {
        const BoardTrack *track = &board->tracks[i];
        h = hash_i64(h, track->a.x); h = hash_i64(h, track->a.y);
        h = hash_i64(h, track->b.x); h = hash_i64(h, track->b.y);
        h = hash_i64(h, track->width); h = hash_u64(h, track->net);
        h = hash_u64(h, (uint32_t)track->layer);
        h = hash_u64(h, track->locked ? 1u : 0u); h = hash_text(h, track->uuid);
    }
    for (size_t i = 0; i < board->arc_track_count; ++i) {
        const BoardArcTrack *track = &board->arc_tracks[i];
        h = hash_i64(h, track->start.x); h = hash_i64(h, track->start.y);
        h = hash_i64(h, track->mid.x); h = hash_i64(h, track->mid.y);
        h = hash_i64(h, track->end.x); h = hash_i64(h, track->end.y);
        h = hash_i64(h, track->width); h = hash_u64(h, track->net);
        h = hash_u64(h, (uint32_t)track->layer);
        h = hash_u64(h, track->locked ? 1u : 0u); h = hash_text(h, track->uuid);
    }
    for (size_t i = 0; i < board->via_count; ++i) {
        const BoardVia *via = &board->vias[i];
        h = hash_i64(h, via->pos.x); h = hash_i64(h, via->pos.y);
        h = hash_i64(h, via->size); h = hash_i64(h, via->drill);
        h = hash_u64(h, via->net); h = hash_u64(h, (uint32_t)via->from);
        h = hash_u64(h, (uint32_t)via->to);
        h = hash_u64(h, via->plated ? 1u : 0u);
        h = hash_u64(h, (uint32_t)via->type);
        h = hash_u64(h, via->locked ? 1u : 0u); h = hash_text(h, via->uuid);
    }
    for (size_t i = 0; i < board->zone_count; ++i) {
        const BoardZone *zone = &board->zones[i];
        h = hash_u64(h, zone->net); h = hash_u64(h, zone->layers);
        h = hash_u64(h, zone->first_pt); h = hash_u64(h, zone->pt_count);
        h = hash_u64(h, zone->is_keepout ? 1u : 0u);
        h = hash_u64(h, (uint32_t)zone->layer);
        h = hash_i64(h, zone->clearance); h = hash_i64(h, zone->min_thickness);
        h = hash_u64(h, zone->no_tracks ? 1u : 0u);
        h = hash_u64(h, zone->no_vias ? 1u : 0u);
        h = hash_u64(h, zone->no_pads ? 1u : 0u);
        h = hash_u64(h, zone->no_copper ? 1u : 0u);
        h = hash_u64(h, zone->no_footprints ? 1u : 0u);
        h = hash_u64(h, zone->locked ? 1u : 0u);
    }
    for (size_t i = 0; i < board->edge_count; ++i) {
        const BoardEdge *edge = &board->edges[i];
        h = hash_u64(h, (uint32_t)edge->kind);
        h = hash_i64(h, edge->p0.x); h = hash_i64(h, edge->p0.y);
        h = hash_i64(h, edge->p1.x); h = hash_i64(h, edge->p1.y);
        h = hash_i64(h, edge->mid.x); h = hash_i64(h, edge->mid.y);
        h = hash_i64(h, edge->width);
        h = hash_u64(h, edge->first_pt); h = hash_u64(h, edge->pt_count);
    }
    const G2Point *point_arrays[3] = {board->court_pts, board->zone_pts, board->edge_pts};
    const size_t point_counts[3] = {board->court_pt_count, board->zone_pt_count, board->edge_pt_count};
    for (size_t a = 0; a < 3u; ++a)
        for (size_t i = 0; i < point_counts[a]; ++i) {
            h = hash_i64(h, point_arrays[a][i].x);
            h = hash_i64(h, point_arrays[a][i].y);
        }
    if (board->source_root) {
        size_t node_count = 0;
        if (!hash_ir_node(&h, board->source_root, 0u, &node_count)) return 0;
    }
    return h;
}

static uint64_t action_id(const CopperOptAction *action) {
    uint64_t h = OPT_FNV_OFFSET;
    h = hash_u64(h, action->base_board_hash);
    h = hash_u64(h, action->evaluation_hash);
    h = hash_u64(h, (uint64_t)action->kind);
    h = hash_u64(h, action->footprint_index);
    h = hash_i64(h, action->delta_nm.x);
    h = hash_i64(h, action->delta_nm.y);
    h = hash_u64(h, action->net_id);
    h = hash_u64(h, (uint32_t)action->router.layer);
    h = hash_i64(h, action->router.trace_width_nm);
    h = hash_i64(h, action->router.clearance_nm);
    h = hash_i64(h, action->router.edge_clearance_nm);
    h = hash_i64(h, action->router.grid_pitch_nm);
    h = hash_u64(h, action->router.max_cells);
    return h == 0 ? UINT64_C(1) : h;
}

static uint64_t evaluation_context_hash(uint64_t board_hash,
                                        const BoardDiag *diag,
                                        const FabRuleSet *rules,
                                        const CopperOptOptions *options) {
    if (!options) return 0u;
    uint64_t h = OPT_FNV_OFFSET;
    h = hash_u64(h, board_hash);
    if (diag) {
        h = hash_u64(h, (uint64_t)diag->status);
        h = hash_u64(h, diag->unmodelled_records);
        h = hash_u64(h, diag->unmodelled_geometry_records);
        h = hash_u64(h, diag->inexact_rotations);
        h = hash_u64(h, diag->unsupported_layers);
        h = hash_text(h, diag->first_unmodelled);
    }
    if (rules) {
        h = hash_text(h, rules->ruleset);
        h = hash_text(h, rules->fabricator);
        h = hash_u64(h, rules->rule_count);
        for (size_t i = 0; i < rules->rule_count; ++i) {
            const FabRule *r = &rules->rules[i];
            h = hash_text(h, r->id);
            h = hash_text(h, r->kind);
            h = hash_i64(h, r->value);
            h = hash_u64(h, r->layers_min);
            h = hash_u64(h, r->layers_max);
            h = hash_u64(h, r->copper_oz_x10);
            h = hash_text(h, r->edge);
            h = hash_text(h, r->mask_color);
            h = hash_u64(h, (uint64_t)r->severity);
            h = hash_text(h, r->note);
            h = hash_text(h, r->source_url);
            h = hash_text(h, r->retrieved);
        }
    } else {
        h = hash_u64(h, UINT64_C(0x72756c65732d6e6f));
    }
    h = hash_u64(h, options->max_candidates);
    h = hash_u64(h, options->max_iterations);
    h = hash_i64(h, options->move_step_nm);
    h = hash_i64(h, options->component_gap_nm);
    h = hash_i64(h, options->move_edge_clearance_nm);
    h = hash_u64(h, options->propose_moves ? 1u : 0u);
    h = hash_u64(h, options->propose_routes ? 1u : 0u);
    h = hash_u64(h, options->require_dfm_for_acceptance ? 1u : 0u);
    h = hash_u64(h, options->require_physics_for_acceptance ? 1u : 0u);
    h = hash_u64(h, options->dfm_finding_capacity);
    h = hash_u64(h, options->dfm_options.copper_oz_x10);
    h = hash_text(h, options->dfm_options.edge_type);
    h = hash_text(h, options->dfm_options.mask_color);
    h = hash_u64(h, options->dfm_options.check_same_net_clearance ? 1u : 0u);
    h = hash_u64(h, (uint32_t)options->router_options.layer);
    h = hash_u64(h, options->router_options.net);
    h = hash_i64(h, options->router_options.grid_pitch_nm);
    h = hash_i64(h, options->router_options.trace_width_nm);
    h = hash_i64(h, options->router_options.clearance_nm);
    h = hash_i64(h, options->router_options.edge_clearance_nm);
    h = hash_u64(h, options->router_options.max_cells);
    h = hash_u64(h, options->physics_evaluate ? 1u : 0u);
    h = hash_text(h, options->physics_provider_id);
    return h == 0u ? UINT64_C(1) : h;
}

static bool courtyard_aabb(const Board *board, const BoardFootprint *fp,
                           G2Aabb *out) {
    if (!board || !fp || !out || !fp->courtyard_exact || fp->court_count < 3u ||
        (size_t)fp->first_court + fp->court_count > board->court_pt_count) return false;
    out->min.x = INT64_MAX; out->min.y = INT64_MAX;
    out->max.x = INT64_MIN; out->max.y = INT64_MIN;
    for (size_t i = 0; i < fp->court_count; ++i) {
        G2Point p = board->court_pts[fp->first_court + i];
        if (p.x < out->min.x) out->min.x = p.x;
        if (p.x > out->max.x) out->max.x = p.x;
        if (p.y < out->min.y) out->min.y = p.y;
        if (p.y > out->max.y) out->max.y = p.y;
    }
    return out->max.x > out->min.x && out->max.y > out->min.y;
}

static bool aabb_gap_violation(G2Aabb a, G2Aabb b, G2Nm gap) {
    int64_t ax, bx, ay, by;
    bool x_overlap, y_overlap;
    if (!add_nm(a.max.x, gap, &ax) || !add_nm(b.max.x, gap, &bx) ||
        !add_nm(a.max.y, gap, &ay) || !add_nm(b.max.y, gap, &by)) return true;
    x_overlap = ax > b.min.x && bx > a.min.x;
    y_overlap = ay > b.min.y && by > a.min.y;
    return x_overlap && y_overlap;
}

static bool move_is_legal(const Board *board, uint32_t footprint_index,
                          G2Point delta, G2Nm gap, G2Nm edge_clearance) {
    if (!board || footprint_index >= board->footprint_count ||
        board->edge_count != 1u || board->edges[0].kind != BOARD_EDGE_RECT) return false;
    const BoardFootprint *fp = &board->footprints[footprint_index];
    if (fp->locked || !fp->courtyard_exact) return false;
    G2Aabb old_box;
    if (!courtyard_aabb(board, fp, &old_box)) return false;
    G2Aabb moved = old_box;
    if (!add_nm(moved.min.x, delta.x, &moved.min.x) ||
        !add_nm(moved.max.x, delta.x, &moved.max.x) ||
        !add_nm(moved.min.y, delta.y, &moved.min.y) ||
        !add_nm(moved.max.y, delta.y, &moved.max.y)) return false;
    G2Aabb board_box;
    if (board_bounds(board, &board_box) != BOARD_OK) return false;
    int64_t minx, miny, maxx, maxy;
    if (!add_nm(board_box.min.x, edge_clearance, &minx) ||
        !add_nm(board_box.min.y, edge_clearance, &miny) ||
        !add_nm(board_box.max.x, -edge_clearance, &maxx) ||
        !add_nm(board_box.max.y, -edge_clearance, &maxy)) return false;
    if (moved.min.x < minx || moved.min.y < miny ||
        moved.max.x > maxx || moved.max.y > maxy) return false;
    for (size_t pi = 0; pi < board->pad_count; ++pi) {
        if (board->pads[pi].footprint != footprint_index) continue;
        BoardPad moved_pad = board->pads[pi];
        if (!add_nm(moved_pad.pos.x, delta.x, &moved_pad.pos.x) ||
            !add_nm(moved_pad.pos.y, delta.y, &moved_pad.pos.y)) return false;
        G2Aabb pad_box;
        if (board_pad_aabb(&moved_pad, &pad_box) != BOARD_OK ||
            pad_box.min.x < minx || pad_box.min.y < miny ||
            pad_box.max.x > maxx || pad_box.max.y > maxy) return false;
    }
    for (size_t i = 0; i < board->footprint_count; ++i) {
        if (i == footprint_index) continue;
        G2Aabb other;
        if (!courtyard_aabb(board, &board->footprints[i], &other)) return false;
        if (aabb_gap_violation(moved, other, gap)) return false;
    }
    return true;
}

static bool translate_footprint(Board *board, uint32_t index, G2Point delta) {
    if (!board || index >= board->footprint_count) return false;
    BoardFootprint *fp = &board->footprints[index];
    if (!add_nm(fp->pos.x, delta.x, &fp->pos.x) ||
        !add_nm(fp->pos.y, delta.y, &fp->pos.y)) return false;
    for (size_t i = 0; i < board->pad_count; ++i) {
        BoardPad *pad = &board->pads[i];
        if (pad->footprint != index) continue;
        if (!add_nm(pad->pos.x, delta.x, &pad->pos.x) ||
            !add_nm(pad->pos.y, delta.y, &pad->pos.y)) return false;
    }
    if ((size_t)fp->first_court + fp->court_count > board->court_pt_count) return false;
    for (size_t i = 0; i < fp->court_count; ++i) {
        G2Point *p = &board->court_pts[fp->first_court + i];
        if (!add_nm(p->x, delta.x, &p->x) || !add_nm(p->y, delta.y, &p->y)) return false;
    }
    return true;
}

static bool append_route(Board *board, const RouterResult *route,
                         const RouterOptions *options, size_t *added) {
    if (added) *added = 0;
    if (!board || !route || !options || route->status != ROUTER_OK ||
        route->point_count < 2u) return false;
    size_t segments = route->point_count - 1u;
    if (segments > SIZE_MAX - board->track_count) return false;
    BoardTrack *tracks = arena_alloc_array(board->arena, sizeof(*tracks),
                                           board->track_count + segments);
    if (!tracks) return false;
    if (board->track_count) memcpy(tracks, board->tracks,
                                   board->track_count * sizeof(*tracks));
    for (size_t i = 0; i < segments; ++i) {
        tracks[board->track_count + i] = (BoardTrack){
            route->points[i], route->points[i + 1u], options->trace_width_nm,
            options->layer, options->net, false, NULL};
    }
    board->tracks = tracks;
    board->track_count += segments;
    if (added) *added = segments;
    return true;
}

static uint64_t board_hpwl(const Board *board) {
    uint64_t total = 0;
    if (!board) return 0;
    for (size_t ni = 0; ni < board->net_count; ++ni) {
        uint32_t id = board->nets[ni].id;
        if (id == 0u) continue;
        size_t count = 0;
        G2Nm minx = INT64_MAX, miny = INT64_MAX;
        G2Nm maxx = INT64_MIN, maxy = INT64_MIN;
        for (size_t pi = 0; pi < board->pad_count; ++pi) {
            const BoardPad *pad = &board->pads[pi];
            if (pad->net != id) continue;
            ++count;
            if (pad->pos.x < minx) minx = pad->pos.x;
            if (pad->pos.x > maxx) maxx = pad->pos.x;
            if (pad->pos.y < miny) miny = pad->pos.y;
            if (pad->pos.y > maxy) maxy = pad->pos.y;
        }
        if (count > 1u) {
            uint64_t hp = sat_add_u64(abs_diff_i64(maxx, minx),
                                      abs_diff_i64(maxy, miny));
            total = sat_add_u64(total, hp);
        }
    }
    return total;
}

static uint64_t board_track_manhattan(const Board *board) {
    uint64_t total = 0;
    if (!board) return 0;
    for (size_t i = 0; i < board->track_count; ++i) {
        const BoardTrack *t = &board->tracks[i];
        total = sat_add_u64(total, abs_diff_i64(t->a.x, t->b.x));
        total = sat_add_u64(total, abs_diff_i64(t->a.y, t->b.y));
    }
    return total;
}

static size_t count_two_pad_nets_without_tracks(const Board *board) {
    size_t result = 0;
    if (!board) return 0;
    for (size_t ni = 0; ni < board->net_count; ++ni) {
        uint32_t id = board->nets[ni].id;
        if (id == 0u) continue;
        size_t pads = 0;
        bool has_track = false;
        for (size_t pi = 0; pi < board->pad_count; ++pi)
            if (board->pads[pi].net == id) ++pads;
        if (pads != 2u) continue;
        for (size_t ti = 0; ti < board->track_count; ++ti)
            if (board->tracks[ti].net == id) { has_track = true; break; }
        if (!has_track) ++result;
    }
    return result;
}

static const char *copy_summary(Arena *arena, const char *summary) {
    if (!arena || !summary) return NULL;
    size_t length = strlen(summary);
    char *copy = (char *)arena_alloc(arena, length + 1u);
    if (!copy) return NULL;
    memcpy(copy, summary, length + 1u);
    return copy;
}

static bool copy_options(Arena *arena, const CopperOptOptions *source,
                         CopperOptOptions *out) {
    if (!arena || !source || !out) return false;
    *out = *source;
    if (source->dfm_options.edge_type) {
        out->dfm_options.edge_type = copy_summary(arena, source->dfm_options.edge_type);
        if (!out->dfm_options.edge_type) return false;
    }
    if (source->dfm_options.mask_color) {
        out->dfm_options.mask_color = copy_summary(arena, source->dfm_options.mask_color);
        if (!out->dfm_options.mask_color) return false;
    }
    if (source->physics_provider_id) {
        out->physics_provider_id = copy_summary(arena, source->physics_provider_id);
        if (!out->physics_provider_id) return false;
    }
    return true;
}

static bool collect_metrics(Arena *scratch_arena, Arena *result_arena,
                            const Board *board, const BoardDiag *source_diag,
                            const FabRuleSet *rules,
                            const CopperOptOptions *options,
                            CopperOptMetrics *out) {
    if (!scratch_arena || !result_arena || !board || !options || !out) return false;
    memset(out, 0, sizeof(*out));
    BoardDiag diag;
    memset(&diag, 0, sizeof(diag));
    BoardStatus bs = board_validate(board, &diag);
    out->board_valid = bs == BOARD_OK;
    if (source_diag) {
        diag.unmodelled_records = source_diag->unmodelled_records;
        diag.unmodelled_geometry_records = source_diag->unmodelled_geometry_records;
        diag.inexact_rotations = source_diag->inexact_rotations;
        diag.unsupported_layers = source_diag->unsupported_layers;
    }
    out->hpwl_nm = board_hpwl(board);
    out->track_manhattan_nm = board_track_manhattan(board);
    out->two_pad_nets_without_tracks = count_two_pad_nets_without_tracks(board);
    out->footprint_count = board->footprint_count;
    out->track_count = board->track_count;

    /* DFM and physics are independent evidence providers. A DFM configuration
       failure must not suppress a configured physics evaluation. */
    if (out->board_valid && rules && options->dfm_options.copper_oz_x10 != 0u &&
        options->dfm_options.edge_type && options->dfm_options.edge_type[0]) {
        CopperReport report;
        report_init(&report, scratch_arena,
                    options->dfm_finding_capacity ? options->dfm_finding_capacity : 8192u);
        DfmStatus ds = dfm_check_board(board, &diag, rules, &options->dfm_options,
                                       scratch_arena, &report);
        if (ds == DFM_OK) {
            out->dfm_available = true;
            out->dfm_fail_count = report.fail_count;
            out->dfm_unverified_count = report.unverified_count;
        }
    }

    if (out->board_valid && options->physics_evaluate) {
        CopperOptPhysicsFeedback feedback;
        memset(&feedback, 0, sizeof(feedback));
        if (options->physics_evaluate(options->physics_context, board, &feedback) &&
            feedback.valid && (!feedback.has_objective || isfinite(feedback.objective))) {
            out->physics_available = true;
            out->physics_fail_count = feedback.fail_count;
            out->physics_unverified_count = feedback.unverified_count;
            out->has_physics_objective = feedback.has_objective;
            out->physics_objective = feedback.has_objective ? feedback.objective : 0.0;
            if (feedback.summary) {
                out->physics_summary = copy_summary(result_arena, feedback.summary);
                if (!out->physics_summary) return false;
            }
        }
    }
    return true;
}

bool copper_opt_metrics_preferred(const CopperOptMetrics *a,
                                  const CopperOptMetrics *b) {
    if (!a || !b || !a->board_valid) return false;
    if (a->dfm_available && b->dfm_available && a->dfm_fail_count != b->dfm_fail_count)
        return a->dfm_fail_count < b->dfm_fail_count;
    if (a->physics_available && b->physics_available &&
        a->physics_fail_count != b->physics_fail_count)
        return a->physics_fail_count < b->physics_fail_count;
    if (a->dfm_available && b->dfm_available &&
        a->dfm_unverified_count != b->dfm_unverified_count)
        return a->dfm_unverified_count < b->dfm_unverified_count;
    if (a->physics_available && b->physics_available &&
        a->physics_unverified_count != b->physics_unverified_count)
        return a->physics_unverified_count < b->physics_unverified_count;
    if (a->two_pad_nets_without_tracks != b->two_pad_nets_without_tracks)
        return a->two_pad_nets_without_tracks < b->two_pad_nets_without_tracks;
    if (a->hpwl_nm != b->hpwl_nm) return a->hpwl_nm < b->hpwl_nm;
    if (a->track_manhattan_nm != b->track_manhattan_nm)
        return a->track_manhattan_nm < b->track_manhattan_nm;
    if (a->physics_available && b->physics_available &&
        a->has_physics_objective && b->has_physics_objective &&
        a->physics_objective != b->physics_objective)
        return a->physics_objective < b->physics_objective;
    return false;
}

static bool metrics_non_worsening(const CopperOptMetrics *before,
                                  const CopperOptMetrics *after,
                                  const CopperOptOptions *options) {
    if (!before || !after || !options || !after->board_valid) return false;
    if (options->require_dfm_for_acceptance &&
        (!after->dfm_available || !before->dfm_available)) return false;
    if (before->dfm_available != after->dfm_available) return false;
    if (before->dfm_available && after->dfm_available &&
        (after->dfm_fail_count > before->dfm_fail_count ||
         after->dfm_unverified_count > before->dfm_unverified_count)) return false;
    if (options->require_physics_for_acceptance &&
        (!before->physics_available || !after->physics_available)) return false;
    if (before->physics_available != after->physics_available) return false;
    if (before->physics_available && after->physics_available &&
        (after->physics_fail_count > before->physics_fail_count ||
         after->physics_unverified_count > before->physics_unverified_count)) return false;
    return true;
}

static CopperOptStatus apply_internal(Arena *arena, const Board *board,
                                      const BoardDiag *board_diag,
                                      const FabRuleSet *rules,
                                      const CopperOptOptions *options,
                                      const CopperOptAction *action,
                                      const CopperOptMetrics *baseline,
                                      CopperOptApplyResult *out) {
    if (!arena || !board || !options || !action || !out)
        return COPPER_OPT_ERR_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (action->base_board_hash != copper_opt_board_hash(board)) {
        out->status = COPPER_OPT_ERR_STALE_ACTION;
        out->message = "Candidate was generated for a different board revision.";
        return out->status;
    }
    if (action->evaluation_hash != evaluation_context_hash(
            action->base_board_hash, board_diag, rules, options)) {
        out->status = COPPER_OPT_ERR_STALE_ACTION;
        out->message = "Candidate evidence is stale: fabrication rules or evaluation configuration changed.";
        return out->status;
    }
    if (action->candidate_id != action_id(action)) {
        out->status = COPPER_OPT_ERR_ARGUMENT;
        out->message = "Candidate ID does not match its action payload.";
        return out->status;
    }
    if (!board_clone(arena, board, &out->board)) {
        out->status = COPPER_OPT_ERR_MEMORY;
        out->message = "Could not clone the board candidate.";
        return out->status;
    }
    Arena metrics_arena;
    arena_init(&metrics_arena, 32768u);
    if (!collect_metrics(&metrics_arena, arena, board, board_diag, rules, options,
                         &out->before)) {
        arena_destroy(&metrics_arena);
        out->status = COPPER_OPT_ERR_MEMORY;
        out->message = "Could not evaluate baseline board metrics.";
        return out->status;
    }
    (void)baseline;
    if (action->kind == COPPER_OPT_ACTION_MOVE_FOOTPRINT) {
        if (!move_is_legal(board, action->footprint_index, action->delta_nm,
                           options->component_gap_nm,
                           options->move_edge_clearance_nm) ||
            !translate_footprint(&out->board, action->footprint_index,
                                 action->delta_nm)) {
            arena_destroy(&metrics_arena);
            out->status = COPPER_OPT_ERR_INFEASIBLE;
            out->message = "Footprint translation violates conservative courtyard/edge constraints or is unsupported.";
            return out->status;
        }
    } else if (action->kind == COPPER_OPT_ACTION_ROUTE_TWO_PAD_NET) {
        RouterResult route;
        memset(&route, 0, sizeof(route));
        out->route_status = ROUTER_ERR_ARGUMENT;
        Arena route_arena;
        arena_init(&route_arena, 65536u);
        const BoardPad *ends[2] = {NULL, NULL};
        size_t end_count = 0;
        for (size_t i = 0; i < board->pad_count; ++i) {
            if (board->pads[i].net == action->net_id) {
                if (end_count < 2u) ends[end_count] = &board->pads[i];
                ++end_count;
            }
        }
        RouterStatus rs = ROUTER_ERR_ARGUMENT;
        if (end_count == 2u && ends[0] && ends[1])
            rs = copper_route_astar(board, ends[0]->pos, ends[1]->pos,
                                    &action->router, &route_arena, &route);
        out->route_status = rs;
        if (rs != ROUTER_OK || !append_route(&out->board, &route,
                                              &action->router,
                                              &out->added_track_count)) {
            arena_destroy(&route_arena);
            arena_destroy(&metrics_arena);
            out->status = COPPER_OPT_ERR_INFEASIBLE;
            out->message = (rs == ROUTER_ERR_UNVERIFIED) ? "Router reports unsupported/unverified board geometry." :
                           (rs == ROUTER_ERR_NO_PATH) ? "No route exists on the configured grid." :
                           "Could not generate and append a valid two-terminal route.";
            return out->status;
        }
        arena_destroy(&route_arena);
    } else {
        arena_destroy(&metrics_arena);
        out->status = COPPER_OPT_ERR_ARGUMENT;
        out->message = "Unknown optimization action kind.";
        return out->status;
    }
    BoardDiag after_diag;
    memset(&after_diag, 0, sizeof(after_diag));
    if (board_validate(&out->board, &after_diag) != BOARD_OK) {
        arena_destroy(&metrics_arena);
        out->status = COPPER_OPT_ERR_BOARD;
        out->message = "Candidate board failed structural validation.";
        return out->status;
    }
    if (!collect_metrics(&metrics_arena, arena, &out->board, board_diag, rules,
                         options, &out->after)) {
        arena_destroy(&metrics_arena);
        out->status = COPPER_OPT_ERR_MEMORY;
        out->message = "Could not evaluate candidate board metrics.";
        return out->status;
    }
    arena_destroy(&metrics_arena);
    if (!metrics_non_worsening(&out->before, &out->after, options)) {
        out->status = COPPER_OPT_ERR_INFEASIBLE;
        out->message = "Candidate violates required evidence gates or worsens an existing DFM/physics finding.";
        return out->status;
    }
    out->applied_action = *action;
    out->committed = true;
    out->status = COPPER_OPT_OK;
    out->message = "Candidate applied to an arena-owned board copy; source board remains unchanged.";
    return out->status;
}

CopperOptOptions copper_opt_default_options(void) {
    CopperOptOptions options;
    memset(&options, 0, sizeof(options));
    options.max_candidates = 64u;
    options.max_iterations = 8u;
    options.move_step_nm = 500000;
    options.component_gap_nm = 200000;
    options.move_edge_clearance_nm = 200000;
    options.propose_moves = true;
    options.propose_routes = false;
    options.require_dfm_for_acceptance = true;
    options.dfm_finding_capacity = 8192u;
    return options;
}

static bool make_candidate(Arena *arena, const Board *board,
                           const BoardDiag *diag, const FabRuleSet *rules,
                           const CopperOptOptions *options,
                           CopperOptAction action, CopperOptCandidate *candidate,
                           const CopperOptMetrics *baseline) {
    Arena trial_arena;
    arena_init(&trial_arena, 65536u);
    CopperOptApplyResult applied;
    CopperOptStatus status = apply_internal(&trial_arena, board, diag, rules,
                                            options, &action, baseline, &applied);
    memset(candidate, 0, sizeof(*candidate));
    candidate->action = action;
    candidate->action.candidate_id = action_id(&candidate->action);
    candidate->router_status = applied.route_status;
    if (status == COPPER_OPT_OK) {
        candidate->metrics = applied.after;
        if (applied.after.physics_summary) {
            candidate->metrics.physics_summary = copy_summary(arena, applied.after.physics_summary);
            if (!candidate->metrics.physics_summary) {
                arena_destroy(&trial_arena);
                return false;
            }
        }
        candidate->eligible = true;
        candidate->reason = "Passes structural and configured evidence gates; objective selection belongs to the external decision provider.";
        if (action.kind == COPPER_OPT_ACTION_ROUTE_TWO_PAD_NET)
            candidate->added_track_count = applied.added_track_count;
    } else {
        candidate->eligible = false;
        candidate->reason = applied.message ? applied.message : "Candidate could not be evaluated.";
        candidate->metrics = baseline ? *baseline : (CopperOptMetrics){0};
    }
    arena_destroy(&trial_arena);
    if (!arena || !candidate) return false;
    return true;
}

CopperOptStatus copper_opt_evaluate(Arena *arena, const Board *board,
                                    const BoardDiag *board_diag,
                                    const FabRuleSet *rules,
                                    const CopperOptOptions *given_options,
                                    CopperOptEvaluation *out) {
    if (!arena || !board || !out) return COPPER_OPT_ERR_ARGUMENT;
    memset(out, 0, sizeof(*out));
    CopperOptOptions defaults = copper_opt_default_options();
    const CopperOptOptions *options = given_options ? given_options : &defaults;
    if (options->physics_evaluate &&
        (!options->physics_provider_id || !options->physics_provider_id[0])) {
        out->status = COPPER_OPT_ERR_ARGUMENT;
        out->message = "A configured physics callback requires a stable physics_provider_id.";
        return out->status;
    }
    out->board_hash = copper_opt_board_hash(board);
    out->evaluation_hash = evaluation_context_hash(out->board_hash, board_diag, rules, options);
    if (!copy_options(arena, options, &out->options)) return COPPER_OPT_ERR_MEMORY;
    out->candidates = options->max_candidates ? arena_alloc_array(
        arena, sizeof(*out->candidates), options->max_candidates) : NULL;
    if (options->max_candidates && !out->candidates) return COPPER_OPT_ERR_MEMORY;
    Arena baseline_arena;
    arena_init(&baseline_arena, 65536u);
    if (!collect_metrics(&baseline_arena, arena, board, board_diag, rules, options,
                         &out->baseline)) {
        arena_destroy(&baseline_arena);
        return COPPER_OPT_ERR_MEMORY;
    }
    arena_destroy(&baseline_arena);
    if (!out->baseline.board_valid) {
        out->status = COPPER_OPT_ERR_BOARD;
        out->message = "Source board is structurally invalid.";
        return out->status;
    }
    if (options->require_dfm_for_acceptance && !out->baseline.dfm_available) {
        out->status = COPPER_OPT_AWAITING_DECISION;
        out->message = "DFM evidence unavailable; candidates cannot be accepted under the current safety policy.";
    } else if (options->require_physics_for_acceptance && !out->baseline.physics_available) {
        out->status = COPPER_OPT_AWAITING_DECISION;
        out->message = "Physics evidence unavailable; configure a deterministic physics callback before accepting candidates.";
    } else {
        out->status = COPPER_OPT_OK;
        out->message = "Measured baseline and candidate actions evaluated. External agent/user chooses the next action.";
    }

    if (options->propose_moves && options->move_step_nm > 0) {
        static const G2Point directions[8] = {
            {1,0}, {-1,0}, {0,1}, {0,-1},
            {1,1}, {1,-1}, {-1,1}, {-1,-1}
        };
        for (size_t fi = 0; fi < board->footprint_count; ++fi) {
            if (board->footprints[fi].locked || !board->footprints[fi].courtyard_exact) continue;
            for (size_t di = 0; di < 8u; ++di) {
                if (out->candidate_count >= options->max_candidates) {
                    out->candidates_truncated = true; break;
                }
                CopperOptAction action;
                memset(&action, 0, sizeof(action));
                action.base_board_hash = out->board_hash;
                action.evaluation_hash = out->evaluation_hash;
                action.kind = COPPER_OPT_ACTION_MOVE_FOOTPRINT;
                action.footprint_index = (uint32_t)fi;
                if (directions[di].x != 0 &&
                    options->move_step_nm > INT64_MAX) continue;
                action.delta_nm.x = directions[di].x * options->move_step_nm;
                action.delta_nm.y = directions[di].y * options->move_step_nm;
                if (!move_is_legal(board, action.footprint_index, action.delta_nm,
                                   options->component_gap_nm,
                                   options->move_edge_clearance_nm)) continue;
                action.candidate_id = action_id(&action);
                CopperOptCandidate *candidate = &out->candidates[out->candidate_count++];
                if (!make_candidate(arena, board, board_diag, rules, options,
                                    action, candidate, &out->baseline))
                    return COPPER_OPT_ERR_MEMORY;
                if (candidate->eligible) ++out->eligible_count;
            }
            if (out->candidates_truncated) break;
        }
    }

    if (options->propose_routes && !out->candidates_truncated &&
        options->router_options.grid_pitch_nm > 0 &&
        options->router_options.trace_width_nm > 0 &&
        options->router_options.clearance_nm >= 0 &&
        options->router_options.edge_clearance_nm >= 0 &&
        board->edge_count == 1u && board->edges[0].kind == BOARD_EDGE_RECT) {
        for (size_t ni = 0; ni < board->net_count; ++ni) {
            uint32_t net_id = board->nets[ni].id;
            if (net_id == 0u) continue;
            size_t pad_count = 0;
            bool has_track = false;
            for (size_t pi = 0; pi < board->pad_count; ++pi)
                if (board->pads[pi].net == net_id) ++pad_count;
            for (size_t ti = 0; ti < board->track_count; ++ti)
                if (board->tracks[ti].net == net_id) { has_track = true; break; }
            if (pad_count != 2u || has_track) continue;
            if (out->candidate_count >= options->max_candidates) {
                out->candidates_truncated = true; break;
            }
            CopperOptAction action;
            memset(&action, 0, sizeof(action));
            action.base_board_hash = out->board_hash;
            action.evaluation_hash = out->evaluation_hash;
            action.kind = COPPER_OPT_ACTION_ROUTE_TWO_PAD_NET;
            action.net_id = net_id;
            action.router = options->router_options;
            action.router.net = net_id;
            action.candidate_id = action_id(&action);
            CopperOptCandidate *candidate = &out->candidates[out->candidate_count++];
            if (!make_candidate(arena, board, board_diag, rules, options,
                                action, candidate, &out->baseline))
                return COPPER_OPT_ERR_MEMORY;
            if (candidate->eligible) ++out->eligible_count;
        }
    }
    if (out->eligible_count == 0u) {
        bool evidence_ready = (!options->require_dfm_for_acceptance || out->baseline.dfm_available) &&
                              (!options->require_physics_for_acceptance || out->baseline.physics_available);
        out->status = evidence_ready ? COPPER_OPT_STALLED : COPPER_OPT_AWAITING_DECISION;
        out->message = evidence_ready ?
            "No candidate passes the configured evidence gates." :
            "Candidate suggestions are informational only until required DFM and physics evidence are available.";
    }
    return out->status;
}

CopperOptStatus copper_opt_apply(Arena *arena, const Board *board,
                                 const BoardDiag *board_diag,
                                 const FabRuleSet *rules,
                                 const CopperOptOptions *options,
                                 const CopperOptAction *action,
                                 CopperOptApplyResult *out) {
    if (!arena || !board || !action || !out) return COPPER_OPT_ERR_ARGUMENT;
    CopperOptOptions defaults = copper_opt_default_options();
    const CopperOptOptions *use_options = options ? options : &defaults;
    return apply_internal(arena, board, board_diag, rules, use_options, action,
                          NULL, out);
}

CopperOptStatus copper_opt_run(Arena *arena, const Board *initial_board,
                               const BoardDiag *board_diag,
                               const FabRuleSet *rules,
                               const CopperOptOptions *given_options,
                               CopperOptDecisionFn decision,
                               void *decision_context,
                               CopperOptRunResult *out) {
    if (!arena || !initial_board || !out) return COPPER_OPT_ERR_ARGUMENT;
    memset(out, 0, sizeof(*out));
    CopperOptOptions defaults = copper_opt_default_options();
    const CopperOptOptions *options = given_options ? given_options : &defaults;
    out->max_iterations = options->max_iterations ? options->max_iterations : 1u;
    if (!copy_options(arena, options, &out->options)) {
        out->status = COPPER_OPT_ERR_MEMORY;
        out->message = "Could not snapshot optimization configuration.";
        return out->status;
    }
    out->history = arena_alloc_array(arena, sizeof(*out->history), out->max_iterations);
    Board *working = arena_alloc(arena, sizeof(*working));
    if (!out->history || !working || !board_clone(arena, initial_board, working)) {
        out->status = COPPER_OPT_ERR_MEMORY;
        out->message = "Could not allocate optimization run state.";
        return out->status;
    }
    for (size_t iteration = 0; iteration < out->max_iterations; ++iteration) {
        CopperOptEvaluation evaluation;
        CopperOptStatus eval_status = copper_opt_evaluate(
            arena, working, board_diag, rules, options, &evaluation);
        CopperOptIterationSummary *summary = &out->history[out->history_count++];
        memset(summary, 0, sizeof(*summary));
        summary->iteration = iteration + 1u;
        summary->before = evaluation.baseline;
        summary->after = evaluation.baseline;
        summary->candidate_count = evaluation.candidate_count;
        summary->eligible_count = evaluation.eligible_count;
        if (eval_status < 0 || evaluation.eligible_count == 0u) {
            out->status = eval_status < 0 ? eval_status : COPPER_OPT_STALLED;
            out->message = eval_status < 0 ? evaluation.message :
                "Optimization stalled: no eligible candidate actions remain.";
            break;
        }
        if (!decision) {
            out->status = COPPER_OPT_AWAITING_DECISION;
            out->message = "Candidate actions evaluated; waiting for an external decision provider.";
            break;
        }
        uint64_t selected_id = 0;
        bool has_decision = decision(decision_context, iteration + 1u,
                                     &evaluation.baseline, evaluation.candidates,
                                     evaluation.candidate_count, &selected_id);
        if (!has_decision || selected_id == 0u) {
            out->status = COPPER_OPT_STOPPED;
            out->message = "External decision provider stopped the optimization loop.";
            break;
        }
        const CopperOptCandidate *selected = NULL;
        for (size_t i = 0; i < evaluation.candidate_count; ++i)
            if (evaluation.candidates[i].action.candidate_id == selected_id) {
                selected = &evaluation.candidates[i]; break;
            }
        if (!selected || !selected->eligible) {
            out->status = COPPER_OPT_ERR_INFEASIBLE;
            out->message = "Decision provider selected an unknown or ineligible candidate.";
            break;
        }
        CopperOptApplyResult applied;
        CopperOptStatus apply_status = copper_opt_apply(
            arena, working, board_diag, rules, options, &selected->action, &applied);
        if (apply_status != COPPER_OPT_OK) {
            out->status = apply_status;
            out->message = applied.message;
            break;
        }
        *working = applied.board;
        summary->selected_candidate_id = selected_id;
        summary->selected_action = selected->action;
        summary->action_committed = true;
        summary->after = applied.after;
        ++out->accepted_actions;
        if (iteration + 1u == out->max_iterations) {
            out->status = COPPER_OPT_OK;
            out->message = "Configured optimization iteration limit reached.";
        }
    }
    out->final_board = *working;
    Arena final_metrics_arena;
    arena_init(&final_metrics_arena, 65536u);
    if (!collect_metrics(&final_metrics_arena, arena, working, board_diag, rules,
                         options, &out->final_metrics)) {
        arena_destroy(&final_metrics_arena);
        out->status = COPPER_OPT_ERR_MEMORY;
        out->message = "Could not calculate final optimization metrics.";
        return out->status;
    }
    arena_destroy(&final_metrics_arena);
    if (out->status == 0 && !decision) out->status = COPPER_OPT_AWAITING_DECISION;
    return out->status;
}

const char *copper_opt_status_name(CopperOptStatus status) {
    switch (status) {
    case COPPER_OPT_OK: return "OK";
    case COPPER_OPT_AWAITING_DECISION: return "AWAITING_DECISION";
    case COPPER_OPT_STALLED: return "STALLED";
    case COPPER_OPT_STOPPED: return "STOPPED";
    case COPPER_OPT_ERR_ARGUMENT: return "ERR_ARGUMENT";
    case COPPER_OPT_ERR_MEMORY: return "ERR_MEMORY";
    case COPPER_OPT_ERR_BOARD: return "ERR_BOARD";
    case COPPER_OPT_ERR_RULES: return "ERR_RULES";
    case COPPER_OPT_ERR_STALE_ACTION: return "ERR_STALE_ACTION";
    case COPPER_OPT_ERR_INFEASIBLE: return "ERR_INFEASIBLE";
    default: return "UNKNOWN";
    }
}

const char *copper_opt_action_name(CopperOptActionKind kind) {
    switch (kind) {
    case COPPER_OPT_ACTION_MOVE_FOOTPRINT: return "MOVE_FOOTPRINT";
    case COPPER_OPT_ACTION_ROUTE_TWO_PAD_NET: return "ROUTE_TWO_PAD_NET";
    default: return "UNKNOWN";
    }
}

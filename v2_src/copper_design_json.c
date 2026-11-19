#include "copper_design_json.h"
#include "filesystem.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct JsonBuffer {
    char *data;
    size_t length;
    size_t capacity;
    bool failed;
} JsonBuffer;
static bool jb_reserve(JsonBuffer *b, size_t extra) {
    if (extra > SIZE_MAX - b->length - 1u) return false;
    size_t required = b->length + extra + 1u;
    if (required <= b->capacity) return true;
    size_t cap = b->capacity ? b->capacity : 4096u;
    while (cap < required) {
        if (cap > SIZE_MAX / 2u) { cap = required; break; }
        cap *= 2u;
    }
    char *p = (char *)realloc(b->data, cap);
    if (!p) return false;
    b->data = p; b->capacity = cap; return true;
}
static void jb_raw_n(JsonBuffer *b, const char *s, size_t n) {
    if (!b || !s || b->failed) return;
    if (!jb_reserve(b, n)) { b->failed = true; return; }
    memcpy(b->data + b->length, s, n); b->length += n; b->data[b->length] = '\0';
}
static void jb_raw(JsonBuffer *b, const char *s) { if (s) jb_raw_n(b, s, strlen(s)); }
static void jb_fmt(JsonBuffer *b, const char *fmt, ...) {
    va_list ap, cp; va_start(ap, fmt); va_copy(cp, ap);
    int n = vsnprintf(NULL, 0, fmt, cp); va_end(cp);
    if (n < 0 || !jb_reserve(b, (size_t)n)) { b->failed = true; va_end(ap); return; }
    (void)vsnprintf(b->data + b->length, b->capacity - b->length, fmt, ap);
    va_end(ap); b->length += (size_t)n;
}
static void jb_string(JsonBuffer *b, const char *s) {
    if (!s) { jb_raw(b, "null"); return; }
    jb_raw(b, "\"");
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        switch (*p) {
        case '"': jb_raw(b, "\\\""); break;
        case '\\': jb_raw(b, "\\\\"); break;
        case '\b': jb_raw(b, "\\b"); break;
        case '\f': jb_raw(b, "\\f"); break;
        case '\n': jb_raw(b, "\\n"); break;
        case '\r': jb_raw(b, "\\r"); break;
        case '\t': jb_raw(b, "\\t"); break;
        default:
            if (*p < 0x20) jb_fmt(b, "\\u%04x", (unsigned)*p);
            else jb_raw_n(b, (const char *)p, 1);
            break;
        }
    }
    jb_raw(b, "\"");
}
static const char *bool_text(bool v) { return v ? "true" : "false"; }
static const char *label_kind_name(SchematicLabelKind k) {
    switch (k) {
    case SCHEMATIC_LABEL_GLOBAL: return "global";
    case SCHEMATIC_LABEL_HIERARCHICAL: return "hierarchical";
    default: return "local";
    }
}
static const char *pad_type_name(BoardPadType t) {
    switch (t) { case BOARD_PAD_THRU: return "through_hole"; case BOARD_PAD_NPTH: return "npth"; case BOARD_PAD_CONNECT: return "connect"; default: return "smd"; }
}
static const char *pad_shape_name(BoardPadShape s) {
    switch (s) { case BOARD_PAD_RECT: return "rect"; case BOARD_PAD_OVAL: return "oval"; case BOARD_PAD_ROUNDRECT: return "roundrect"; case BOARD_PAD_TRAPEZOID: return "trapezoid"; case BOARD_PAD_CUSTOM: return "custom"; default: return "circle"; }
}
static const char *edge_kind_name(BoardEdgeKind k) {
    switch (k) { case BOARD_EDGE_ARC: return "arc"; case BOARD_EDGE_CIRCLE: return "circle"; case BOARD_EDGE_RECT: return "rect"; case BOARD_EDGE_POLYGON: return "polygon"; default: return "line"; }
}
static const char *via_type_name(BoardViaType t) {
    switch (t) { case BOARD_VIA_BLIND_BURIED: return "blind_buried"; case BOARD_VIA_MICRO: return "micro"; case BOARD_VIA_UNKNOWN: return "unknown"; default: return "through"; }
}
static void write_point(JsonBuffer *b, G2Point p) { jb_fmt(b, "{\"x_nm\":%lld,\"y_nm\":%lld}", (long long)p.x, (long long)p.y); }
static void write_nm_or_null(JsonBuffer *b, int64_t value) {
    if (value == INT64_MIN) jb_raw(b, "null");
    else jb_fmt(b, "%lld", (long long)value);
}
static const SchematicLibPin *find_lib_pin(const SchematicIR *s,
                                            const SchematicSymbol *symbol,
                                            const char *number) {
    if (!s || !symbol || !number) return NULL;
    const SchematicLibrarySymbol *lib = schematic_find_library_symbol(s, symbol->lib_id);
    if (!lib) return NULL;
    for (size_t i = 0; i < lib->pin_count; ++i) {
        const SchematicLibPin *pin = &s->library_pins[lib->first_pin + i];
        if (pin->number && strcmp(pin->number, number) == 0) return pin;
    }
    return NULL;
}
static void write_sources(JsonBuffer *b, const CopperDesign *d) {
    jb_raw(b, "\"sources\":[");
    for (size_t i = 0; i < d->source_count; ++i) {
        const CopperDesignSource *s = &d->sources[i]; char base[FS_PATH_MAX] = {0};
        if (i) jb_raw(b, ",");
        (void)fs_path_basename(base, sizeof(base), s->path ? s->path : "");
        jb_raw(b, "{\"kind\":"); jb_string(b, s->kind);
        jb_raw(b, ",\"filename\":"); jb_string(b, s->present ? base : NULL);
        jb_raw(b, ",\"sha256\":"); jb_string(b, s->sha256);
        jb_raw(b, ",\"present\":"); jb_raw(b, bool_text(s->present)); jb_raw(b, "}");
    }
    jb_raw(b, "],");
}
static void write_schematic(JsonBuffer *b, const SchematicIR *s) {
    jb_raw(b, "\"schematic\":{");
    jb_fmt(b, "\"file_version\":%u,", s->file_version);
    jb_raw(b, "\"generator\":"); jb_string(b, s->generator);
    jb_raw(b, ",\"generator_version\":"); jb_string(b, s->generator_version);
    jb_raw(b, ",\"root_uuid\":"); jb_string(b, s->root_uuid);
    jb_raw(b, ",\"paper\":"); jb_string(b, s->paper);
    jb_raw(b, ",\"library_symbols\":[");
    for (size_t i = 0; i < s->library_symbol_count; ++i) {
        const SchematicLibrarySymbol *lib = &s->library_symbols[i];
        if (i) jb_raw(b, ",");
        jb_raw(b, "{\"lib_id\":"); jb_string(b, lib->lib_id);
        jb_raw(b, ",\"reference_prefix\":"); jb_string(b, lib->reference_prefix);
        jb_raw(b, ",\"default_value\":"); jb_string(b, lib->default_value);
        jb_raw(b, ",\"description\":"); jb_string(b, lib->description);
        jb_raw(b, ",\"pins\":[");
        for (size_t p = 0; p < lib->pin_count; ++p) {
            const SchematicLibPin *pin = &s->library_pins[lib->first_pin + p];
            if (p) jb_raw(b, ",");
            jb_raw(b, "{\"number\":"); jb_string(b, pin->number);
            jb_raw(b, ",\"name\":"); jb_string(b, pin->name);
            jb_raw(b, ",\"electrical_type\":"); jb_string(b, pin->electrical_type);
            jb_fmt(b, ",\"unit\":%u}", pin->unit);
        }
        jb_raw(b, "]}");
    }
    jb_raw(b, "],\"instances\":[");
    for (size_t i = 0; i < s->symbol_count; ++i) {
        const SchematicSymbol *sym = &s->symbols[i];
        if (i) jb_raw(b, ",");
        jb_raw(b, "{\"uuid\":"); jb_string(b, sym->uuid);
        jb_raw(b, ",\"lib_id\":"); jb_string(b, sym->lib_id);
        jb_raw(b, ",\"reference\":"); jb_string(b, sym->reference);
        jb_raw(b, ",\"value\":"); jb_string(b, sym->value);
        jb_raw(b, ",\"footprint\":"); jb_string(b, sym->footprint);
        jb_raw(b, ",\"datasheet\":"); jb_string(b, sym->datasheet);
        jb_raw(b, ",\"description\":"); jb_string(b, sym->description);
        jb_raw(b, ",\"sheet_path\":"); jb_string(b, sym->sheet_path);
        jb_raw(b, ",\"position\":"); write_point(b, sym->position);
        jb_fmt(b, ",\"rotation_mdeg\":%d,\"unit\":%u,\"mirror_x\":%s,\"mirror_y\":%s,\"in_bom\":%s,\"on_board\":%s,\"dnp\":%s,\"pins\":[",
               sym->rotation_mdeg, sym->unit, bool_text(sym->mirrored_x), bool_text(sym->mirrored_y),
               bool_text(sym->in_bom), bool_text(sym->on_board), bool_text(sym->dnp));
        for (size_t p = 0; p < sym->pin_count; ++p) {
            const SchematicSymbolPin *pin = &s->symbol_pins[sym->first_pin + p];
            if (p) jb_raw(b, ",");
            jb_raw(b, "{\"number\":"); jb_string(b, pin->number);
            jb_raw(b, ",\"uuid\":"); jb_string(b, pin->uuid); jb_raw(b, "}");
        }
        jb_raw(b, "]}");
    }
    jb_raw(b, "],\"wires\":[");
    for (size_t i = 0; i < s->wire_count; ++i) {
        const SchematicWire *w = &s->wires[i];
        if (i) jb_raw(b, ",");
        jb_raw(b, "{\"uuid\":"); jb_string(b, w->uuid);
        jb_raw(b, ",\"points\":[");
        for (size_t p = 0; p < w->point_count; ++p) {
            if (p) jb_raw(b, ",");
            write_point(b, s->wire_points[w->first_point + p].position);
        }
        jb_raw(b, "]}");
    }
    jb_raw(b, "],\"labels\":[");
    for (size_t i = 0; i < s->label_count; ++i) {
        const SchematicLabel *l = &s->labels[i]; if (i) jb_raw(b, ",");
        jb_raw(b, "{\"kind\":"); jb_string(b, label_kind_name(l->kind));
        jb_raw(b, ",\"text\":"); jb_string(b, l->text);
        jb_raw(b, ",\"uuid\":"); jb_string(b, l->uuid);
        jb_raw(b, ",\"position\":"); write_point(b, l->position);
        jb_fmt(b, ",\"rotation_mdeg\":%d}", l->rotation_mdeg);
    }
    jb_raw(b, "],\"junctions\":[");
    for (size_t i = 0; i < s->junction_count; ++i) {
        if (i) jb_raw(b, ",");
        jb_raw(b, "{\"uuid\":"); jb_string(b, s->junctions[i].uuid);
        jb_raw(b, ",\"position\":"); write_point(b, s->junctions[i].position); jb_raw(b, "}");
    }
    jb_raw(b, "],\"no_connects\":[");
    for (size_t i = 0; i < s->no_connect_count; ++i) {
        if (i) jb_raw(b, ",");
        jb_raw(b, "{\"uuid\":"); jb_string(b, s->no_connects[i].uuid);
        jb_raw(b, ",\"position\":"); write_point(b, s->no_connects[i].position); jb_raw(b, "}");
    }
    jb_raw(b, "],\"sheet_instances\":[");
    for (size_t i = 0; i < s->sheet_instance_count; ++i) {
        if (i) jb_raw(b, ",");
        jb_raw(b, "{\"path\":"); jb_string(b, s->sheet_instances[i].path);
        jb_raw(b, ",\"page\":"); jb_string(b, s->sheet_instances[i].page); jb_raw(b, "}");
    }
    jb_raw(b, "],\"unmodelled_top_level_records\":");
    jb_fmt(b, "%zu,\"first_unmodelled_top_level\":", s->unmodelled_top_level_records);
    jb_string(b, s->first_unmodelled_top_level);
    jb_raw(b, ",\"typed_coverage_complete\":false}");
}
static void write_board(JsonBuffer *b, const Board *board, const BoardDiag *diag, bool present) {
    if (!present) { jb_raw(b, "\"pcb\":null"); return; }
    jb_raw(b, "\"pcb\":{");
    jb_fmt(b, "\"kicad_file_version\":%u,\"copper_layer_count\":%u,\"thickness_nm\":",
           board->kicad_file_version, board->rules.copper_layers);
    write_nm_or_null(b, board->rules.board_thickness);
    jb_raw(b, ",\"rules\":{\"min_clearance_nm\":"); write_nm_or_null(b, board->rules.min_clearance);
    jb_raw(b, ",\"min_track_width_nm\":"); write_nm_or_null(b, board->rules.min_track_width);
    jb_raw(b, ",\"min_via_size_nm\":"); write_nm_or_null(b, board->rules.min_via_size);
    jb_raw(b, ",\"min_via_drill_nm\":"); write_nm_or_null(b, board->rules.min_via_drill);
    jb_raw(b, ",\"edge_clearance_nm\":"); write_nm_or_null(b, board->rules.edge_clearance);
    jb_raw(b, "},");
    jb_raw(b, "\"layers\":[");
    for (size_t i = 0; i < board->layer_count; ++i) {
        if (i) jb_raw(b, ",");
        jb_fmt(b, "{\"ordinal\":%u,\"id\":%u,\"name\":", board->layers[i].ordinal, (unsigned)board->layers[i].id);
        jb_string(b, board_layer_name(board->layers[i].id));
        jb_fmt(b, ",\"type\":%u,\"user_name\":", (unsigned)board->layers[i].type);
        jb_string(b, board->layers[i].user_name); jb_raw(b, "}");
    }
    jb_raw(b, "],\"nets\":[");
    for (size_t i = 0; i < board->net_count; ++i) {
        if (i) jb_raw(b, ",");
        jb_fmt(b, "{\"id\":%u,\"name\":", board->nets[i].id);
        jb_string(b, board->nets[i].name); jb_raw(b, "}");
    }
    jb_raw(b, "],\"footprints\":[");
    for (size_t i = 0; i < board->footprint_count; ++i) {
        const BoardFootprint *fp = &board->footprints[i]; if (i) jb_raw(b, ",");
        jb_raw(b, "{\"reference\":"); jb_string(b, fp->reference);
        jb_raw(b, ",\"value\":"); jb_string(b, fp->value);
        jb_raw(b, ",\"lib_id\":"); jb_string(b, fp->lib_id);
        jb_raw(b, ",\"uuid\":"); jb_string(b, fp->uuid);
        jb_raw(b, ",\"path\":"); jb_string(b, fp->path);
        jb_raw(b, ",\"position\":"); write_point(b, fp->pos);
        jb_fmt(b, ",\"rotation_mdeg\":%d,\"layer\":%u,\"on_back\":%s,\"rotation_exact\":%s,\"locked\":%s,\"placed\":%s,\"dnp\":%s,\"exclude_from_bom\":%s,\"exclude_from_pos\":%s,\"courtyard_exact\":%s,\"first_pad\":%u,\"pad_count\":%u}",
               fp->rot_mdeg, (unsigned)fp->layer, bool_text(fp->on_back), bool_text(fp->rotation_exact),
               bool_text(fp->locked), bool_text(fp->placed), bool_text(fp->dnp),
               bool_text(fp->exclude_from_bom), bool_text(fp->exclude_from_pos),
               bool_text(fp->courtyard_exact), fp->first_pad, fp->pad_count);
    }
    jb_raw(b, "],\"pads\":[");
    for (size_t i = 0; i < board->pad_count; ++i) {
        const BoardPad *p = &board->pads[i]; const BoardNet *net = board_net_by_id(board, p->net);
        if (i) jb_raw(b, ",");
        jb_fmt(b, "{\"footprint_index\":%u,\"number\":", p->footprint);
        jb_string(b, p->number); jb_raw(b, ",\"type\":"); jb_string(b, pad_type_name(p->type));
        jb_raw(b, ",\"shape\":"); jb_string(b, pad_shape_name(p->shape));
        jb_raw(b, ",\"position\":"); write_point(b, p->pos);
        jb_raw(b, ",\"size\":"); write_point(b, p->size);
        jb_fmt(b, ",\"rotation_mdeg\":%d,\"roundrect_ratio_ppm\":%lld,\"drill\":", p->rot_mdeg, (long long)p->roundrect_ratio_ppm);
        write_point(b, p->drill); jb_raw(b, ",\"drill_offset\":"); write_point(b, p->drill_offset);
        jb_fmt(b, ",\"layer_mask\":%llu,\"net_id\":%u,\"net_name\":", (unsigned long long)p->layers, p->net);
        jb_string(b, net ? net->name : NULL);
        jb_raw(b, ",\"clearance_nm\":"); write_nm_or_null(b, p->clearance);
        jb_raw(b, ",\"mask_margin_nm\":"); write_nm_or_null(b, p->mask_margin);
        jb_raw(b, ",\"paste_margin_nm\":"); write_nm_or_null(b, p->paste_margin);
        jb_fmt(b, ",\"rotation_exact\":%s,\"locked\":%s}",
               bool_text(p->rotation_exact), bool_text(p->locked));
    }
    jb_raw(b, "],\"tracks\":[");
    for (size_t i = 0; i < board->track_count; ++i) {
        const BoardTrack *t = &board->tracks[i]; if (i) jb_raw(b, ",");
        jb_raw(b, "{\"start\":"); write_point(b, t->a); jb_raw(b, ",\"end\":"); write_point(b, t->b);
        jb_fmt(b, ",\"width_nm\":%lld,\"layer\":", (long long)t->width); jb_string(b, board_layer_name(t->layer));
        jb_fmt(b, ",\"net_id\":%u,\"uuid\":", t->net); jb_string(b, t->uuid);
        jb_fmt(b, ",\"locked\":%s}", bool_text(t->locked));
    }
    jb_raw(b, "],\"arc_tracks\":[");
    for (size_t i = 0; i < board->arc_track_count; ++i) {
        const BoardArcTrack *t = &board->arc_tracks[i]; if (i) jb_raw(b, ",");
        jb_raw(b, "{\"start\":"); write_point(b, t->start); jb_raw(b, ",\"mid\":"); write_point(b, t->mid); jb_raw(b, ",\"end\":"); write_point(b, t->end);
        jb_fmt(b, ",\"width_nm\":%lld,\"layer\":", (long long)t->width); jb_string(b, board_layer_name(t->layer));
        jb_fmt(b, ",\"net_id\":%u,\"uuid\":", t->net); jb_string(b, t->uuid);
        jb_fmt(b, ",\"locked\":%s}", bool_text(t->locked));
    }
    jb_raw(b, "],\"vias\":[");
    for (size_t i = 0; i < board->via_count; ++i) {
        const BoardVia *v = &board->vias[i]; if (i) jb_raw(b, ",");
        jb_raw(b, "{\"position\":"); write_point(b, v->pos);
        jb_fmt(b, ",\"size_nm\":%lld,\"drill_nm\":%lld,\"from_layer\":", (long long)v->size, (long long)v->drill);
        jb_string(b, board_layer_name(v->from)); jb_raw(b, ",\"to_layer\":"); jb_string(b, board_layer_name(v->to));
        jb_fmt(b, ",\"net_id\":%u,\"plated\":%s,\"type\":", v->net, bool_text(v->plated)); jb_string(b, via_type_name(v->type));
        jb_raw(b, ",\"uuid\":"); jb_string(b, v->uuid); jb_fmt(b, ",\"locked\":%s}", bool_text(v->locked));
    }
    jb_raw(b, "],\"zones\":[");
    for (size_t i = 0; i < board->zone_count; ++i) {
        const BoardZone *z = &board->zones[i]; const BoardNet *net = board_net_by_id(board, z->net);
        if (i) jb_raw(b, ",");
        jb_fmt(b, "{\"net_id\":%u,\"net_name\":", z->net); jb_string(b, net ? net->name : NULL);
        jb_raw(b, ",\"layer\":"); jb_string(b, board_layer_name(z->layer));
        jb_fmt(b, ",\"layer_mask\":%llu,\"first_point\":%u,\"point_count\":%u,\"clearance_nm\":%lld,\"min_thickness_nm\":%lld,\"keepout\":%s,\"no_tracks\":%s,\"no_vias\":%s,\"no_pads\":%s,\"no_copper\":%s,\"no_footprints\":%s}",
               (unsigned long long)z->layers, z->first_pt, z->pt_count, (long long)z->clearance,
               (long long)z->min_thickness, bool_text(z->is_keepout), bool_text(z->no_tracks),
               bool_text(z->no_vias), bool_text(z->no_pads), bool_text(z->no_copper), bool_text(z->no_footprints));
    }
    jb_raw(b, "],\"zone_points\":[");
    for (size_t i = 0; i < board->zone_pt_count; ++i) { if (i) jb_raw(b, ","); write_point(b, board->zone_pts[i]); }
    jb_raw(b, "],\"edges\":[");
    for (size_t i = 0; i < board->edge_count; ++i) {
        const BoardEdge *e = &board->edges[i]; if (i) jb_raw(b, ",");
        jb_raw(b, "{\"kind\":"); jb_string(b, edge_kind_name(e->kind));
        jb_raw(b, ",\"p0\":"); write_point(b, e->p0); jb_raw(b, ",\"p1\":"); write_point(b, e->p1); jb_raw(b, ",\"mid\":"); write_point(b, e->mid);
        jb_fmt(b, ",\"width_nm\":%lld,\"first_point\":%u,\"point_count\":%u}", (long long)e->width, e->first_pt, e->pt_count);
    }
    jb_raw(b, "],\"edge_points\":[");
    for (size_t i = 0; i < board->edge_pt_count; ++i) { if (i) jb_raw(b, ","); write_point(b, board->edge_pts[i]); }
    jb_raw(b, "],\"coverage\":{");
    jb_fmt(b, "\"unmodelled_records\":%zu,\"unmodelled_geometry_records\":%zu,\"inexact_rotations\":%zu,\"unsupported_layers\":%zu,\"typed_coverage_complete\":false}}",
           diag ? diag->unmodelled_records : 0u,
           diag ? diag->unmodelled_geometry_records : 0u,
           diag ? diag->inexact_rotations : 0u,
           diag ? diag->unsupported_layers : 0u);
}
static void write_project_settings(JsonBuffer *b, const ProjectSettingsIR *p) {
    jb_fmt(b, "\"project_settings\":{\"metadata_version\":%u,\"filename\":", p->metadata_version);
    jb_string(b, p->filename); jb_raw(b, ",\"net_classes\":[");
    for (size_t i = 0; i < p->net_class_count; ++i) {
        const ProjectNetClassIR *n = &p->net_classes[i]; if (i) jb_raw(b, ",");
        jb_raw(b, "{\"name\":"); jb_string(b, n->name);
        jb_fmt(b, ",\"clearance_nm\":%lld,\"track_width_nm\":%lld,\"diff_pair_width_nm\":%lld,\"diff_pair_gap_nm\":%lld,\"diff_pair_via_gap_nm\":%lld,\"via_diameter_nm\":%lld,\"via_drill_nm\":%lld,\"microvia_diameter_nm\":%lld,\"microvia_drill_nm\":%lld,\"bus_width\":%lld,\"priority\":%lld,\"raw_fields\":%s}",
               (long long)n->clearance_nm, (long long)n->track_width_nm,
               (long long)n->diff_pair_width_nm, (long long)n->diff_pair_gap_nm,
               (long long)n->diff_pair_via_gap_nm, (long long)n->via_diameter_nm,
               (long long)n->via_drill_nm, (long long)n->microvia_diameter_nm,
               (long long)n->microvia_drill_nm, (long long)n->bus_width,
               (long long)n->priority, n->raw_object_json ? n->raw_object_json : "{}");
    }
    jb_raw(b, "],\"board_rules\":{");
    for (size_t i = 0; i < p->board_rule_count; ++i) {
        if (i) jb_raw(b, ",");
        jb_string(b, p->board_rules[i].name); jb_raw(b, ":");
        jb_raw(b, p->board_rules[i].value_json ? p->board_rules[i].value_json : "null");
    }
    jb_raw(b, "},\"board_rule_severities\":{");
    for (size_t i = 0; i < p->board_rule_severity_count; ++i) {
        if (i) jb_raw(b, ",");
        jb_string(b, p->board_rule_severities[i].name); jb_raw(b, ":");
        jb_raw(b, p->board_rule_severities[i].value_json ? p->board_rule_severities[i].value_json : "null");
    }
    jb_raw(b, "},\"erc_rule_severities\":{");
    for (size_t i = 0; i < p->erc_rule_severity_count; ++i) {
        if (i) jb_raw(b, ",");
        jb_string(b, p->erc_rule_severities[i].name); jb_raw(b, ":");
        jb_raw(b, p->erc_rule_severities[i].value_json ? p->erc_rule_severities[i].value_json : "null");
    }
    jb_raw(b, "}},");
}
char *copper_design_json_to_string(const CopperDesign *d, size_t *out_length) {
    JsonBuffer b = {0}; size_t diag_len = 0; char *diag_json = NULL;
    if (out_length) *out_length = 0;
    if (!d || !d->has_project_settings || !d->has_schematic) return NULL;
    jb_raw(&b, "{\"schema\":\"copper.design\",\"schema_version\":1,\"project\":{\"name\":");
    jb_string(&b, d->name); jb_raw(&b, ",\"root_uuid\":"); jb_string(&b, d->schematic.root_uuid);
    jb_raw(&b, ",\"kicad_version\":"); jb_string(&b, d->schematic.generator_version);
    jb_raw(&b, "},\"units\":{\"length\":\"nm\",\"angle\":\"mdeg\",\"temperature\":\"K\",\"voltage\":\"V\",\"current\":\"A\",\"power\":\"W\"},");
    write_sources(&b, d);
    jb_raw(&b, "\"electrical\":{\"components\":[");
    for (size_t i = 0; i < d->schematic.symbol_count; ++i) {
        const SchematicSymbol *s = &d->schematic.symbols[i]; if (i) jb_raw(&b, ",");
        jb_raw(&b, "{\"id\":"); jb_string(&b, s->uuid); jb_raw(&b, ",\"reference\":"); jb_string(&b, s->reference);
        jb_raw(&b, ",\"value\":"); jb_string(&b, s->value); jb_raw(&b, ",\"symbol\":"); jb_string(&b, s->lib_id);
        jb_raw(&b, ",\"footprint\":"); jb_string(&b, s->footprint);
        jb_fmt(&b, ",\"in_bom\":%s,\"on_board\":%s,\"dnp\":%s,\"pins\":[",
               bool_text(s->in_bom), bool_text(s->on_board), bool_text(s->dnp));
        for (size_t p = 0; p < s->pin_count; ++p) {
            const SchematicSymbolPin *pin = &d->schematic.symbol_pins[s->first_pin + p];
            const SchematicLibPin *definition = find_lib_pin(&d->schematic, s, pin->number);
            if (p) jb_raw(&b, ",");
            jb_raw(&b, "{\"number\":"); jb_string(&b, pin->number);
            jb_raw(&b, ",\"uuid\":"); jb_string(&b, pin->uuid);
            jb_raw(&b, ",\"name\":"); jb_string(&b, definition ? definition->name : NULL);
            jb_raw(&b, ",\"electrical_type\":"); jb_string(&b, definition ? definition->electrical_type : NULL);
            jb_raw(&b, "}");
        }
        jb_raw(&b, "]}");
    }
    jb_raw(&b, "],\"nets\":[");
    if (d->has_netlist) for (size_t i = 0; i < d->netlist.net_count; ++i) {
        const KicadNetlistNet *n = &d->netlist.nets[i]; if (i) jb_raw(&b, ",");
        jb_raw(&b, "{\"code\":"); jb_string(&b, n->code); jb_raw(&b, ",\"name\":"); jb_string(&b, n->name);
        jb_raw(&b, ",\"class\":"); jb_string(&b, n->net_class); jb_raw(&b, ",\"nodes\":[");
        for (size_t j = 0; j < n->node_count; ++j) {
            const KicadNetlistNode *node = &d->netlist.nodes[n->first_node + j]; if (j) jb_raw(&b, ",");
            jb_raw(&b, "{\"reference\":"); jb_string(&b, node->reference); jb_raw(&b, ",\"pin\":"); jb_string(&b, node->pin);
            jb_raw(&b, ",\"pin_function\":"); jb_string(&b, node->pin_function); jb_raw(&b, ",\"pin_type\":"); jb_string(&b, node->pin_type); jb_raw(&b, "}");
        }
        jb_raw(&b, "]}");
    }
    jb_raw(&b, "],\"connectivity_status\":");
    jb_string(&b, d->has_netlist ? "IMPORTED_FROM_KICAD_NETLIST" : "NOT_IMPORTED"); jb_raw(&b, "},");
    write_schematic(&b, &d->schematic);
    jb_raw(&b, ",");
    write_board(&b, &d->board, &d->board_diag, d->has_board);
    jb_raw(&b, ",");
    write_project_settings(&b, &d->project_settings);
    jb_raw(&b, "\"netlist\":");
    if (!d->has_netlist) jb_raw(&b, "null");
    else {
        jb_raw(&b, "{\"components\":[");
        for (size_t i = 0; i < d->netlist.component_count; ++i) {
            const KicadNetlistComponent *c = &d->netlist.components[i]; if (i) jb_raw(&b, ",");
            jb_raw(&b, "{\"reference\":"); jb_string(&b, c->reference); jb_raw(&b, ",\"value\":"); jb_string(&b, c->value);
            jb_raw(&b, ",\"footprint\":"); jb_string(&b, c->footprint); jb_raw(&b, ",\"library\":"); jb_string(&b, c->library);
            jb_raw(&b, ",\"part\":"); jb_string(&b, c->part); jb_raw(&b, ",\"uuid\":"); jb_string(&b, c->uuid); jb_raw(&b, "}");
        }
        jb_raw(&b, "],\"nets\":[");
        for (size_t i = 0; i < d->netlist.net_count; ++i) {
            const KicadNetlistNet *n = &d->netlist.nets[i]; if (i) jb_raw(&b, ",");
            jb_raw(&b, "{\"code\":"); jb_string(&b, n->code); jb_raw(&b, ",\"name\":"); jb_string(&b, n->name);
            jb_raw(&b, ",\"class\":"); jb_string(&b, n->net_class); jb_raw(&b, ",\"nodes\":[");
            for (size_t j = 0; j < n->node_count; ++j) {
                const KicadNetlistNode *node = &d->netlist.nodes[n->first_node + j]; if (j) jb_raw(&b, ",");
                jb_raw(&b, "{\"reference\":"); jb_string(&b, node->reference); jb_raw(&b, ",\"pin\":"); jb_string(&b, node->pin);
                jb_raw(&b, ",\"pin_function\":"); jb_string(&b, node->pin_function); jb_raw(&b, ",\"pin_type\":"); jb_string(&b, node->pin_type); jb_raw(&b, "}");
            }
            jb_raw(&b, "]}");
        }
        jb_raw(&b, "]}");
    }
    jb_raw(&b, ",\"parity\":{");
    jb_fmt(&b, "\"matched_components\":%zu,\"missing_footprints\":%zu,\"symbols_without_footprint_assignment\":%zu,\"extra_footprints\":%zu,\"reference_mismatches\":%zu,\"footprint_mismatches\":%zu,\"checked_pad_nets\":%zu,\"pad_net_mismatches\":%zu,\"netlist_available\":%s},",
           d->parity.matched_components, d->parity.missing_footprints,
           d->parity.symbols_without_footprint_assignment, d->parity.extra_footprints,
           d->parity.reference_mismatches, d->parity.footprint_mismatches,
           d->parity.checked_pad_nets, d->parity.pad_net_mismatches, bool_text(d->parity.netlist_available));
    diag_json = diagnostic_report_to_json(&d->diagnostics, &diag_len);
    if (!diag_json) { b.failed = true; }
    else { jb_raw(&b, "\"diagnostics\":"); jb_raw_n(&b, diag_json, diag_len); free(diag_json); }
    jb_raw(&b, "}\n");
    if (b.failed) { free(b.data); return NULL; }
    if (!b.data) { b.data = (char *)malloc(1); if (!b.data) return NULL; b.data[0] = '\0'; }
    if (out_length) *out_length = b.length;
    return b.data;
}
FsStatus copper_design_json_write_file(const CopperDesign *d,
                                       const char *output_path) {
    char *json; size_t length = 0; FsStatus fs;
    if (!d || !output_path) return FS_ERR_ARGUMENT;
    json = copper_design_json_to_string(d, &length);
    if (!json) return FS_ERR_OUT_OF_MEMORY;
    fs = fs_write_file_atomic_replace(output_path, json, length);
    free(json); return fs;
}


char *copper_project_manifest_to_string(const CopperDesign *d,
                                        const char *design_path,
                                        const char *report_path,
                                        size_t *out_length) {
    JsonBuffer b = {0};
    char design_hash[FS_SHA256_HEX_SIZE];
    char report_hash[FS_SHA256_HEX_SIZE];
    char design_base[FS_PATH_MAX] = {0};
    char report_base[FS_PATH_MAX] = {0};
    if (out_length) *out_length = 0;
    if (!d || !design_path || !report_path ||
        fs_sha256_file_hex(design_path, design_hash) != FS_OK ||
        fs_sha256_file_hex(report_path, report_hash) != FS_OK ||
        fs_path_basename(design_base, sizeof(design_base), design_path) != FS_OK ||
        fs_path_basename(report_base, sizeof(report_base), report_path) != FS_OK)
        return NULL;
    jb_raw(&b, "{\"schema\":\"copper.project_manifest\",\"schema_version\":1,\"project\":");
    jb_string(&b, d->name);
    jb_raw(&b, ",\"sources\":[");
    for (size_t i = 0; i < d->source_count; ++i) {
        const CopperDesignSource *source = &d->sources[i];
        char base[FS_PATH_MAX] = {0};
        if (i) jb_raw(&b, ",");
        if (source->present && source->path)
            (void)fs_path_basename(base, sizeof(base), source->path);
        jb_raw(&b, "{\"kind\":"); jb_string(&b, source->kind);
        jb_raw(&b, ",\"filename\":"); jb_string(&b, source->present ? base : NULL);
        jb_raw(&b, ",\"sha256\":"); jb_string(&b, source->sha256);
        jb_fmt(&b, ",\"present\":%s}", bool_text(source->present));
    }
    jb_raw(&b, "],\"artifacts\":[{\"kind\":\"design\",\"path\":");
    jb_string(&b, design_path);
    jb_raw(&b, ",\"filename\":"); jb_string(&b, design_base);
    jb_raw(&b, ",\"sha256\":"); jb_string(&b, design_hash);
    jb_raw(&b, "},{\"kind\":\"import_report\",\"path\":");
    jb_string(&b, report_path);
    jb_raw(&b, ",\"filename\":"); jb_string(&b, report_base);
    jb_raw(&b, ",\"sha256\":"); jb_string(&b, report_hash);
    jb_raw(&b, "}]}");
    jb_raw(&b, "\n");
    if (b.failed) { free(b.data); return NULL; }
    if (!b.data) {
        b.data = (char *)malloc(1u);
        if (!b.data) return NULL;
        b.data[0] = '\0';
    }
    if (out_length) *out_length = b.length;
    return b.data;
}

FsStatus copper_project_manifest_write_file(const CopperDesign *d,
                                             const char *design_path,
                                             const char *report_path,
                                             const char *manifest_path) {
    char *json;
    size_t length = 0;
    FsStatus status;
    if (!d || !design_path || !report_path || !manifest_path)
        return FS_ERR_ARGUMENT;
    json = copper_project_manifest_to_string(d, design_path, report_path, &length);
    if (!json) return FS_ERR_IO;
    status = fs_write_file_atomic_replace(manifest_path, json, length);
    free(json);
    return status;
}

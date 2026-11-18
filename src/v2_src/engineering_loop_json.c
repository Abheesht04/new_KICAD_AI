#include "engineering_loop_json.h"
#include "topology_json.h"

#include <stdarg.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Small deterministic JSON writer. It deliberately owns the buffer and
   emits finite numeric values only; unavailable measurements are JSON null. */
typedef struct JsonBuf { char *p; size_t n, cap; bool bad; } JsonBuf;
static bool jb_reserve(JsonBuf *b, size_t extra) {
    if (extra > SIZE_MAX - b->n - 1u) return false;
    size_t need = b->n + extra + 1u;
    if (need <= b->cap) return true;
    size_t cap = b->cap ? b->cap : 2048u;
    while (cap < need) {
        if (cap > SIZE_MAX / 2u) { cap = need; break; }
        cap *= 2u;
    }
    char *p = (char *)realloc(b->p, cap);
    if (!p) return false;
    b->p = p; b->cap = cap; return true;
}
static void jb_raw(JsonBuf *b, const char *s) {
    size_t n = strlen(s);
    if (!jb_reserve(b, n)) { b->bad = true; return; }
    memcpy(b->p + b->n, s, n); b->n += n; b->p[b->n] = '\0';
}
static void jb_fmt(JsonBuf *b, const char *fmt, ...) {
    va_list ap, cp; va_start(ap, fmt); va_copy(cp, ap);
    int n = vsnprintf(NULL, 0, fmt, cp); va_end(cp);
    if (n < 0 || !jb_reserve(b, (size_t)n)) { b->bad = true; va_end(ap); return; }
    (void)vsnprintf(b->p + b->n, b->cap - b->n, fmt, ap);
    va_end(ap); b->n += (size_t)n;
}
static void jb_string(JsonBuf *b, const char *s) {
    if (!s) { jb_raw(b, "null"); return; }
    jb_raw(b, "\"");
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        char q[7];
        switch (*p) {
        case '"': jb_raw(b, "\\\""); break;
        case '\\': jb_raw(b, "\\\\"); break;
        case '\b': jb_raw(b, "\\b"); break;
        case '\f': jb_raw(b, "\\f"); break;
        case '\n': jb_raw(b, "\\n"); break;
        case '\r': jb_raw(b, "\\r"); break;
        case '\t': jb_raw(b, "\\t"); break;
        default:
            if (*p < 0x20) { (void)snprintf(q, sizeof(q), "\\u%04x", (unsigned)*p); jb_raw(b, q); }
            else { if (!jb_reserve(b, 1u)) { b->bad = true; return; } b->p[b->n++] = (char)*p; b->p[b->n] = '\0'; }
            break;
        }
    }
    jb_raw(b, "\"");
}
static void jb_number(JsonBuf *b, double v) {
    if (!isfinite(v)) jb_raw(b, "null");
    else jb_fmt(b, "%.17g", v);
}

char *engineering_loop_json_to_string(const CopperTopology *topology,
                                      const EngineeringLoopResult *r,
                                      size_t *out_length) {
    if (out_length) *out_length = 0;
    if (!topology || !r) return NULL;
    JsonBuf b = {0};
    char *top = copper_topology_json_to_string(topology, NULL);
    if (!top) return NULL;
    jb_raw(&b, "{\"schema\":\"copper.engineering-design\",\"schema_version\":1,\"status\":");
    jb_string(&b, engineering_loop_status_name(r->status));
    jb_raw(&b, ",\"design_ready\":"); jb_raw(&b, r->design_ready ? "true" : "false");
    jb_raw(&b, ",\"findings_truncated\":"); jb_raw(&b, r->findings_truncated ? "true" : "false");
    jb_raw(&b, ",\"iterations\":"); jb_fmt(&b, "%zu", r->iterations);
    size_t pass_count = 0u, fail_count = 0u, unverified_count = 0u;
    for (size_t i = 0; i < r->finding_count; ++i) {
        if (r->findings[i].status == ENGINEERING_FINDING_PASS) ++pass_count;
        else if (r->findings[i].status == ENGINEERING_FINDING_FAIL) ++fail_count;
        else if (r->findings[i].status == ENGINEERING_FINDING_UNVERIFIED) ++unverified_count;
    }
    jb_raw(&b, ",\"summary\":{\"components\":"); jb_fmt(&b, "%zu", topology->component_count);
    jb_raw(&b, ",\"nets\":"); jb_fmt(&b, "%zu", topology->net_count);
    jb_raw(&b, ",\"terminals\":"); jb_fmt(&b, "%zu", topology->terminal_count);
    jb_raw(&b, ",\"connections\":"); jb_fmt(&b, "%zu", topology->connection_count);
    jb_raw(&b, ",\"feedback_pass\":"); jb_fmt(&b, "%zu", pass_count);
    jb_raw(&b, ",\"feedback_fail\":"); jb_fmt(&b, "%zu", fail_count);
    jb_raw(&b, ",\"feedback_unverified\":"); jb_fmt(&b, "%zu", unverified_count); jb_raw(&b, "}");
    jb_raw(&b, ",\"parity\":{\"checked\":"); jb_raw(&b, r->parity_checked ? "true" : "false");
    jb_raw(&b, ",\"matched_components\":"); jb_fmt(&b, "%zu", r->parity_matched_components);
    jb_raw(&b, ",\"missing_footprints\":"); jb_fmt(&b, "%zu", r->parity_missing_footprints);
    jb_raw(&b, ",\"pad_net_mismatches\":"); jb_fmt(&b, "%zu", r->parity_pad_net_mismatches); jb_raw(&b, "}");
    jb_raw(&b, ",\"topology\":"); jb_raw(&b, top); free(top);
    jb_raw(&b, ",\"resolved_components\":[");
    for (size_t i = 0; i < r->resolved_component_count; ++i) {
        const EngineeringResolvedComponent *c = &r->resolved_components[i]; if (i) jb_raw(&b, ",");
        jb_raw(&b, "{\"reference\":"); jb_string(&b, c->reference);
        jb_raw(&b, ",\"part_id\":"); jb_string(&b, c->part_id);
        jb_raw(&b, ",\"kind\":"); jb_string(&b, part_kind_name(c->part_kind));
        jb_raw(&b, ",\"manufacturer\":"); jb_string(&b, c->manufacturer);
        jb_raw(&b, ",\"mpn\":"); jb_string(&b, c->mpn);
        jb_raw(&b, ",\"symbol_id\":"); jb_string(&b, c->symbol_id);
        jb_raw(&b, ",\"footprint_id\":"); jb_string(&b, c->footprint_id);
        jb_raw(&b, ",\"model_id\":"); jb_string(&b, c->model_id);
        jb_raw(&b, ",\"circuit_model\":"); jb_string(&b, circuit_element_kind_name(c->circuit_kind));
        jb_raw(&b, ",\"catalogue_part_resolved\":"); jb_raw(&b, c->has_catalogue_part ? "true" : "false");
        jb_raw(&b, ",\"explicit_model_supplied\":"); jb_raw(&b, c->has_explicit_model ? "true" : "false");
        jb_raw(&b, ",\"identity_source_status\":");
        if (c->part_record) jb_string(&b, c->part_record->identity_source.status == PART_SOURCE_VERIFIED ? "VERIFIED" : "UNVERIFIED");
        else jb_raw(&b, "null");
        jb_raw(&b, ",\"datasheet\":"); jb_string(&b, c->part_record ? c->part_record->datasheet : NULL);
        jb_raw(&b, ",\"supplier\":"); jb_string(&b, c->part_record ? c->part_record->supplier : NULL);
        jb_raw(&b, ",\"ratings\":[");
        if (c->part_record) for (size_t ri = 0; ri < c->part_record->rating_count; ++ri) {
            const PartRating *rating = &c->part_record->ratings[ri]; if (ri) jb_raw(&b, ",");
            jb_raw(&b, "{\"name\":"); jb_string(&b, part_rating_name(rating->name));
            jb_raw(&b, ",\"bound\":"); jb_string(&b, part_rating_bound_name(rating->bound));
            jb_raw(&b, ",\"value\":"); jb_fmt(&b, "%lld", (long long)rating->value);
            jb_raw(&b, ",\"unit\":"); jb_string(&b, part_rating_unit_name(rating->unit));
            jb_raw(&b, ",\"source_status\":"); jb_string(&b, rating->source.status == PART_SOURCE_VERIFIED ? "VERIFIED" : "UNVERIFIED"); jb_raw(&b, "}");
        }
        jb_raw(&b, "]}");
    }
    jb_raw(&b, "]");

    jb_raw(&b, ",\"circuit\":{\"built\":"); jb_raw(&b, r->circuit_built ? "true" : "false");
    jb_raw(&b, ",\"node_count\":"); jb_fmt(&b, "%u", r->circuit_built ? r->circuit.node_count : 0u);
    jb_raw(&b, ",\"elements\":[");
    if (r->circuit_built) for (size_t i = 0; i < r->circuit.element_count; ++i) {
        const CircuitElement *e = &r->circuit.elements[i];
        if (i) jb_raw(&b, ",");
        jb_raw(&b, "{\"reference\":"); jb_string(&b, e->reference);
        jb_raw(&b, ",\"kind\":"); jb_string(&b, circuit_element_kind_name(e->kind));
        jb_raw(&b, ",\"n1\":"); jb_fmt(&b, "%u", e->n1);
        jb_raw(&b, ",\"n2\":"); jb_fmt(&b, "%u", e->n2);
        jb_raw(&b, ",\"n3\":"); jb_fmt(&b, "%u", e->n3);
        jb_raw(&b, ",\"n4\":"); jb_fmt(&b, "%u", e->n4);
        jb_raw(&b, ",\"value_si\":"); jb_number(&b, e->value_si); jb_raw(&b, "}");
    }
    jb_raw(&b, "]},\"simulation\":{\"attempted\":"); jb_raw(&b, r->simulation_attempted ? "true" : "false");
    jb_raw(&b, ",\"converged\":"); jb_raw(&b, r->simulation_converged ? "true" : "false");
    jb_raw(&b, ",\"status\":"); jb_string(&b, r->simulation_attempted ? mna_status_name(r->simulation_status) : "NOT_RUN");
    jb_raw(&b, ",\"iterations\":"); jb_fmt(&b, "%u", r->operating_point.iterations);
    jb_raw(&b, ",\"scaled_residual\":"); jb_number(&b, r->operating_point.scaled_residual);
    jb_raw(&b, ",\"node_voltages_v\":[");
    if (r->simulation_converged) for (size_t i = 0; i < r->operating_point.node_voltage_count; ++i) {
        if (i) { jb_raw(&b, ","); }
        jb_number(&b, r->operating_point.node_voltages_v[i]);
    }
    jb_raw(&b, "],\"element_currents_a\":[");
    if (r->simulation_converged) for (size_t i = 0; i < r->operating_point.element_current_count; ++i) {
        if (i) { jb_raw(&b, ","); }
        jb_number(&b, r->operating_point.element_currents_a[i]);
    }
    jb_raw(&b, "]},\"component_stress\":[");
    for (size_t i = 0; i < r->stress_count; ++i) {
        const EngineeringComponentStress *s = &r->stresses[i]; if (i) jb_raw(&b, ",");
        jb_raw(&b, "{\"reference\":"); jb_string(&b, s->reference);
        jb_raw(&b, ",\"voltage_v\":"); if (s->has_voltage) jb_number(&b, s->voltage_v); else jb_raw(&b, "null");
        jb_raw(&b, ",\"current_a\":"); if (s->has_current) jb_number(&b, s->current_a); else jb_raw(&b, "null");
        jb_raw(&b, ",\"power_w\":"); if (s->has_power) jb_number(&b, s->power_w); else jb_raw(&b, "null");
        jb_raw(&b, ",\"voltage_status\":"); jb_string(&b, engineering_finding_status_name(s->voltage_status));
        jb_raw(&b, ",\"current_status\":"); jb_string(&b, engineering_finding_status_name(s->current_status));
        jb_raw(&b, ",\"power_status\":"); jb_string(&b, engineering_finding_status_name(s->power_status)); jb_raw(&b, "}");
    }
    jb_raw(&b, "],\"physical\":{\"board_supplied\":"); jb_raw(&b, r->physical_board_supplied ? "true" : "false");
    jb_raw(&b, ",\"placement_attempted\":"); jb_raw(&b, r->placement_attempted ? "true" : "false");
    jb_raw(&b, ",\"placement_status\":");
    if (r->placement_attempted) jb_fmt(&b, "%d", (int)r->placement.status); else jb_raw(&b, "null");
    jb_raw(&b, ",\"placement_count\":"); jb_fmt(&b, "%zu", r->placement_attempted ? r->placement.placement_count : 0u);
    jb_raw(&b, ",\"placed_count\":"); jb_fmt(&b, "%zu", r->placement_attempted ? r->placement.placed_count : 0u);
    jb_raw(&b, ",\"placements\":[");
    if (r->placement_attempted) for (size_t i = 0; i < r->placement.placement_count; ++i) {
        const PlacerPlacement *p = &r->placement.placements[i]; if (i) jb_raw(&b, ",");
        jb_raw(&b, "{\"footprint_index\":"); jb_fmt(&b, "%u", p->footprint_index);
        jb_raw(&b, ",\"x_nm\":"); jb_fmt(&b, "%lld", (long long)p->position.x);
        jb_raw(&b, ",\"y_nm\":"); jb_fmt(&b, "%lld", (long long)p->position.y);
        jb_raw(&b, ",\"rotation_mdeg\":"); jb_fmt(&b, "%d", p->rotation_mdeg);
        jb_raw(&b, ",\"placed\":"); jb_raw(&b, p->placed ? "true" : "false"); jb_raw(&b, "}");
    }
    jb_raw(&b, "],\"routes_attempted\":"); jb_raw(&b, r->routes_attempted ? "true" : "false");
    jb_raw(&b, ",\"route_count\":"); jb_fmt(&b, "%zu", r->route_count);
    jb_raw(&b, ",\"routes\":[");
    for (size_t ri = 0; ri < r->route_count; ++ri) {
        const EngineeringRouteProposal *rp = &r->routes[ri];
        if (ri) jb_raw(&b, ",");
        jb_raw(&b, "{\"topology_net_index\":"); jb_fmt(&b, "%u", rp->topology_net_index);
        jb_raw(&b, ",\"net_name\":"); jb_string(&b, rp->net_name);
        jb_raw(&b, ",\"status\":");
        switch (rp->status) {
        case ROUTER_OK: jb_string(&b, "OK"); break;
        case ROUTER_ERR_ARGUMENT: jb_string(&b, "ERR_ARGUMENT"); break;
        case ROUTER_ERR_MEMORY: jb_string(&b, "ERR_MEMORY"); break;
        case ROUTER_ERR_RANGE: jb_string(&b, "ERR_RANGE"); break;
        case ROUTER_ERR_NO_PATH: jb_string(&b, "ERR_NO_PATH"); break;
        case ROUTER_ERR_UNVERIFIED: jb_string(&b, "UNVERIFIED"); break;
        case ROUTER_ERR_GEOMETRY: jb_string(&b, "ERR_GEOMETRY"); break;
        default: jb_string(&b, "UNKNOWN"); break;
        }
        jb_raw(&b, ",\"expanded_nodes\":"); jb_fmt(&b, "%zu", rp->route.expanded_nodes);
        jb_raw(&b, ",\"cost\":"); jb_fmt(&b, "%llu", (unsigned long long)rp->route.cost);
        jb_raw(&b, ",\"message\":"); jb_string(&b, rp->route.message);
        jb_raw(&b, ",\"points\":[");
        for (size_t pi = 0; pi < rp->route.point_count; ++pi) {
            if (pi) jb_raw(&b, ",");
            jb_raw(&b, "{\"x_nm\":"); jb_fmt(&b, "%lld", (long long)rp->route.points[pi].x);
            jb_raw(&b, ",\"y_nm\":"); jb_fmt(&b, "%lld", (long long)rp->route.points[pi].y);
            jb_raw(&b, "}");
        }
        jb_raw(&b, "]}");
    }
    jb_raw(&b, "],\"dfm_attempted\":"); jb_raw(&b, r->dfm_attempted ? "true" : "false");
    jb_raw(&b, ",\"dfm_verdict\":"); if (r->dfm_attempted) jb_string(&b, report_status_name(report_overall(&r->dfm_report))); else jb_raw(&b, "null");
    jb_raw(&b, ",\"dfm_findings\":[");
    if (r->dfm_attempted) for (size_t i = 0; i < r->dfm_report.count; ++i) {
        const ReportFinding *f = &r->dfm_report.findings[i]; if (i) jb_raw(&b, ",");
        jb_raw(&b, "{\"status\":"); jb_string(&b, report_status_name(f->status));
        jb_raw(&b, ",\"rule_id\":"); jb_string(&b, f->rule_id);
        jb_raw(&b, ",\"kind\":"); jb_string(&b, f->kind);
        jb_raw(&b, ",\"object\":"); jb_string(&b, f->object);
        jb_raw(&b, ",\"detail\":"); jb_string(&b, f->detail);
        jb_raw(&b, ",\"measured_nm\":"); if (f->has_measurement) jb_fmt(&b, "%lld", (long long)f->measured_nm); else jb_raw(&b, "null");
        jb_raw(&b, ",\"limit_nm\":"); if (f->has_measurement) jb_fmt(&b, "%lld", (long long)f->limit_nm); else jb_raw(&b, "null"); jb_raw(&b, "}");
    }
    jb_raw(&b, "]},\"feedback\":[");
    for (size_t i = 0; i < r->finding_count; ++i) {
        const EngineeringFinding *f = &r->findings[i]; if (i) jb_raw(&b, ",");
        jb_raw(&b, "{\"status\":"); jb_string(&b, engineering_finding_status_name(f->status));
        jb_raw(&b, ",\"code\":"); jb_string(&b, f->code);
        jb_raw(&b, ",\"reference\":"); jb_string(&b, f->reference);
        jb_raw(&b, ",\"net\":"); jb_string(&b, f->net);
        jb_raw(&b, ",\"message\":"); jb_string(&b, f->message);
        jb_raw(&b, ",\"measured\":"); if (f->has_measurement) jb_number(&b, f->measured); else jb_raw(&b, "null");
        jb_raw(&b, ",\"limit\":"); if (f->has_measurement) jb_number(&b, f->limit); else jb_raw(&b, "null"); jb_raw(&b, "}");
    }
    jb_raw(&b, "]}");
    if (b.bad) { free(b.p); return NULL; }
    if (out_length) *out_length = b.n;
    return b.p;
}

FsStatus engineering_loop_json_write_file(const CopperTopology *topology,
                                          const EngineeringLoopResult *result,
                                          const char *path) {
    size_t length = 0;
    char *json = engineering_loop_json_to_string(topology, result, &length);
    if (!json) return FS_ERR_OUT_OF_MEMORY;
    FsStatus status = fs_write_file_atomic_replace(path, json, length);
    free(json); return status;
}

typedef struct ReportTextBuf {
    char *data;
    size_t length;
    size_t capacity;
    bool failed;
} ReportTextBuf;

static void report_text_appendf(ReportTextBuf *b, const char *fmt, ...) {
    va_list ap, copy;
    va_start(ap, fmt);
    va_copy(copy, ap);
    int needed = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (needed < 0 || (size_t)needed > SIZE_MAX - b->length - 1u) {
        b->failed = true;
        va_end(ap);
        return;
    }
    size_t required = b->length + (size_t)needed + 1u;
    if (required > b->capacity) {
        size_t capacity = b->capacity ? b->capacity : 1024u;
        while (capacity < required) {
            if (capacity > SIZE_MAX / 2u) { capacity = required; break; }
            capacity *= 2u;
        }
        char *next = (char *)realloc(b->data, capacity);
        if (!next) {
            b->failed = true;
            va_end(ap);
            return;
        }
        b->data = next;
        b->capacity = capacity;
    }
    (void)vsnprintf(b->data + b->length, b->capacity - b->length, fmt, ap);
    va_end(ap);
    b->length += (size_t)needed;
}

char *engineering_loop_report_to_text(const EngineeringLoopResult *r,
                                      size_t *out_length) {
    if (out_length) *out_length = 0;
    if (!r) return NULL;
    ReportTextBuf b = {0};
    report_text_appendf(&b,
        "COPPER ENGINEERING LOOP\nStatus: %s\nDesign ready: %s\nPasses: %zu\n"
        "Circuit built: %s\nSimulation: %s\nPhysical board supplied: %s\n"
        "Placement attempted: %s\nRouting proposals attempted: %s (%zu)\n"
        "DFM attempted: %s\n\n",
        engineering_loop_status_name(r->status), r->design_ready ? "YES" : "NO",
        r->iterations, r->circuit_built ? "YES" : "NO",
        r->simulation_attempted ? mna_status_name(r->simulation_status) : "NOT RUN",
        r->physical_board_supplied ? "YES" : "NO",
        r->placement_attempted ? "YES" : "NO",
        r->routes_attempted ? "YES" : "NO", r->route_count,
        r->dfm_attempted ? "YES" : "NO");

    if (r->simulation_converged) {
        report_text_appendf(&b, "DC OPERATING POINT\nResidual: %.9g\n",
                            r->operating_point.scaled_residual);
        for (size_t i = 0; i < r->operating_point.node_voltage_count; ++i)
            report_text_appendf(&b, "  Node %zu: %.9g V\n", i + 1u,
                                r->operating_point.node_voltages_v[i]);
        report_text_appendf(&b, "\nCOMPONENT STRESS\n");
        for (size_t i = 0; i < r->stress_count; ++i) {
            const EngineeringComponentStress *st = &r->stresses[i];
            report_text_appendf(&b, "  %s: V=", st->reference ? st->reference : "?");
            if (st->has_voltage) report_text_appendf(&b, "%.9g V [%s]", st->voltage_v,
                engineering_finding_status_name(st->voltage_status));
            else report_text_appendf(&b, "unknown");
            report_text_appendf(&b, ", I=");
            if (st->has_current) report_text_appendf(&b, "%.9g A [%s]", st->current_a,
                engineering_finding_status_name(st->current_status));
            else report_text_appendf(&b, "unknown");
            report_text_appendf(&b, ", P=");
            if (st->has_power) report_text_appendf(&b, "%.9g W [%s]", st->power_w,
                engineering_finding_status_name(st->power_status));
            else report_text_appendf(&b, "unknown");
            report_text_appendf(&b, "\n");
        }
    }
    if (r->placement_attempted) {
        report_text_appendf(&b, "\nPLACEMENT PROPOSAL\nStatus: %d\nPlaced: %zu / %zu\n",
            (int)r->placement.status, r->placement.placed_count,
            r->placement.placement_count);
    }
    if (r->routes_attempted) {
        report_text_appendf(&b, "\nROUTE PROPOSALS (not committed)\n");
        for (size_t i = 0; i < r->route_count; ++i) {
            const EngineeringRouteProposal *rp = &r->routes[i];
            report_text_appendf(&b, "  Net %s: status=%d points=%zu expanded=%zu cost=%llu\n",
                rp->net_name ? rp->net_name : "?", (int)rp->status,
                rp->route.point_count, rp->route.expanded_nodes,
                (unsigned long long)rp->route.cost);
        }
    }
    if (r->dfm_attempted) {
        report_text_appendf(&b, "\nDFM FINDINGS (%zu)\n", r->dfm_report.count);
        for (size_t i = 0; i < r->dfm_report.count; ++i) {
            const ReportFinding *f = &r->dfm_report.findings[i];
            report_text_appendf(&b, "  [%s] %s / %s: %s",
                report_status_name(f->status), f->rule_id ? f->rule_id : "?",
                f->object ? f->object : "?", f->detail ? f->detail : "");
            if (f->has_measurement)
                report_text_appendf(&b, " (measured %lld nm, limit %lld nm)",
                    (long long)f->measured_nm, (long long)f->limit_nm);
            report_text_appendf(&b, "\n");
        }
    }
    report_text_appendf(&b, "\nENGINEERING FEEDBACK (%zu)\n", r->finding_count);
    for (size_t i = 0; i < r->finding_count; ++i) {
        const EngineeringFinding *f = &r->findings[i];
        report_text_appendf(&b, "[%s] %s%s%s: %s",
            engineering_finding_status_name(f->status),
            f->code ? f->code : "GENERAL",
            f->reference ? " / " : "", f->reference ? f->reference : "",
            f->message ? f->message : "");
        if (f->has_measurement)
            report_text_appendf(&b, " (measured %.9g, limit %.9g)", f->measured, f->limit);
        report_text_appendf(&b, "\n");
    }
    if (b.failed) {
        free(b.data);
        return NULL;
    }
    if (!b.data) {
        b.data = (char *)calloc(1u, 1u);
        if (!b.data) return NULL;
    }
    if (out_length) *out_length = b.length;
    return b.data;
}

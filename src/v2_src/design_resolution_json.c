#include "design_resolution_json.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct OutBuffer {
    char *data;
    size_t length;
    size_t capacity;
    bool failed;
} OutBuffer;

static bool reserve(OutBuffer *b, size_t extra) {
    if (extra > SIZE_MAX - b->length - 1u) return false;
    size_t need = b->length + extra + 1u;
    if (need <= b->capacity) return true;
    size_t cap = b->capacity ? b->capacity : 4096u;
    while (cap < need) {
        if (cap > SIZE_MAX / 2u) { cap = need; break; }
        cap *= 2u;
    }
    char *p = (char *)realloc(b->data, cap);
    if (!p) return false;
    b->data = p; b->capacity = cap;
    return true;
}
static void raw_n(OutBuffer *b, const char *s, size_t n) {
    if (!b || !s || b->failed) return;
    if (!reserve(b, n)) { b->failed = true; return; }
    memcpy(b->data + b->length, s, n);
    b->length += n; b->data[b->length] = '\0';
}
static void raw(OutBuffer *b, const char *s) { if (s) raw_n(b, s, strlen(s)); }
static void fmt(OutBuffer *b, const char *format, ...) {
    va_list ap, cp;
    va_start(ap, format); va_copy(cp, ap);
    int n = vsnprintf(NULL, 0, format, cp); va_end(cp);
    if (n < 0 || !reserve(b, (size_t)n)) { b->failed = true; va_end(ap); return; }
    (void)vsnprintf(b->data + b->length, b->capacity - b->length, format, ap);
    va_end(ap); b->length += (size_t)n;
}
static void string(OutBuffer *b, const char *s) {
    if (!s) { raw(b, "null"); return; }
    raw(b, "\"");
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        switch (*p) {
        case '"': raw(b, "\\\""); break;
        case '\\': raw(b, "\\\\"); break;
        case '\b': raw(b, "\\b"); break;
        case '\f': raw(b, "\\f"); break;
        case '\n': raw(b, "\\n"); break;
        case '\r': raw(b, "\\r"); break;
        case '\t': raw(b, "\\t"); break;
        default:
            if (*p < 0x20) fmt(b, "\\u%04x", (unsigned)*p);
            else raw_n(b, (const char *)p, 1u);
            break;
        }
    }
    raw(b, "\"");
}
static const char *kind_name(PartKind k) { return part_kind_name(k); }
static void write_source(OutBuffer *b, const PartSource *s) {
    raw(b, "{\"url\":"); string(b, s->url);
    raw(b, ",\"document\":"); string(b, s->document);
    raw(b, ",\"revision\":"); string(b, s->revision);
    fmt(b, ",\"page\":%u,\"status\":", s->page);
    string(b, s->status == PART_SOURCE_VERIFIED ? "VERIFIED" : "UNVERIFIED");
    raw(b, ",\"verified_by\":"); string(b, s->verified_by);
    raw(b, ",\"verified_on\":"); string(b, s->verified_on); raw(b, "}");
}
static void write_rating(OutBuffer *b, const PartRating *r) {
    raw(b, "{\"name\":"); string(b, part_rating_name(r->name));
    raw(b, ",\"bound\":"); string(b, part_rating_bound_name(r->bound));
    fmt(b, ",\"value\":%lld,\"unit\":", (long long)r->value);
    string(b, part_rating_unit_name(r->unit));
    raw(b, ",\"source\":"); write_source(b, &r->source); raw(b, "}");
}
static void write_pin_map(OutBuffer *b, const PartPinMap *p) {
    raw(b, "{\"terminal\":"); string(b, p->terminal);
    raw(b, ",\"symbol_pin\":"); string(b, p->symbol_pin);
    raw(b, ",\"footprint_pad\":"); string(b, p->footprint_pad); raw(b, "}");
}
static void write_missing_fields(OutBuffer *b, uint32_t mask) {
    struct MissingName { uint32_t bit; const char *name; };
    static const struct MissingName names[] = {
        {DESIGN_MISSING_KIND, "kind"},
        {DESIGN_MISSING_MANUFACTURER, "manufacturer"},
        {DESIGN_MISSING_MPN, "mpn"},
        {DESIGN_MISSING_SYMBOL, "symbol"},
        {DESIGN_MISSING_FOOTPRINT, "footprint"},
        {DESIGN_MISSING_MODEL, "model"},
        {DESIGN_MISSING_PIN_MAP, "pin_map"},
        {DESIGN_MISSING_PROVENANCE, "provenance"},
        {DESIGN_GENERATED_ID, "generated_id"}
    };
    bool first = true;
    raw(b, "[");
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if ((mask & names[i].bit) == 0u) continue;
        if (!first) raw(b, ",");
        string(b, names[i].name);
        first = false;
    }
    raw(b, "]");
}

static void write_part(OutBuffer *b, const PartRecord *p,
                       const DesignResolveCandidate *candidate,
                       const KicadPartAssetResolution *assets) {
    raw(b, "{\"id\":"); string(b, p->id);
    raw(b, ",\"id_generated\":"); raw(b, p->id_generated ? "true" : "false");
    raw(b, ",\"kind\":"); string(b, kind_name(p->kind));
    raw(b, ",\"manufacturer\":"); string(b, p->manufacturer);
    raw(b, ",\"mpn\":"); string(b, p->mpn);
    raw(b, ",\"package\":"); string(b, p->package);
    raw(b, ",\"description\":"); string(b, p->description);
    raw(b, ",\"datasheet\":"); string(b, p->datasheet);
    raw(b, ",\"supplier\":"); string(b, p->supplier);
    raw(b, ",\"aliases\":[");
    for (size_t i = 0; i < p->alias_count; ++i) { if (i) raw(b, ","); string(b, p->aliases[i]); }
    raw(b, "],\"identity_source\":"); write_source(b, &p->identity_source);
    raw(b, ",\"ratings\":[");
    for (size_t i = 0; i < p->rating_count; ++i) { if (i) raw(b, ","); write_rating(b, &p->ratings[i]); }
    raw(b, "],\"pin_map\":[");
    for (size_t i = 0; i < p->pin_count; ++i) { if (i) raw(b, ","); write_pin_map(b, &p->pins[i]); }
    raw(b, "],\"source_record_preserved\":");
    raw(b, p->source_record ? "true" : "false");
    raw(b, ",\"resolution\":{\"score\":"); fmt(b, "%d", candidate ? candidate->score : 0);
    raw(b, ",\"priority\":"); fmt(b, "%d", candidate ? candidate->priority : 0);
    raw(b, ",\"catalogue\":"); string(b, candidate ? candidate->catalogue_name : NULL);
    raw(b, ",\"source\":"); string(b, candidate ? design_catalogue_source_name(candidate->source) : NULL);
    raw(b, ",\"missing_fields_mask\":"); fmt(b, "%u", candidate ? candidate->missing_fields : 0u);
    raw(b, ",\"missing_fields\":"); write_missing_fields(b, candidate ? candidate->missing_fields : 0u);
    raw(b, "},\"assets\":{");
    raw(b, "\"symbol\":{\"id\":"); string(b, assets && assets->symbol_found ? assets->effective_symbol_id : p->symbol_id);
    raw(b, ",\"status\":"); string(b, assets && assets->symbol_found ? (assets->symbol_is_suggestion ? "SUGGESTION_RESOLVED" : "FOUND") : "NOT_RESOLVED");
    raw(b, ",\"path\":"); string(b, assets ? assets->symbol_path : NULL);
    raw(b, ",\"suggested_id\":"); string(b, assets ? assets->suggested_symbol_id : NULL);
    raw(b, ",\"footprint_default\":"); string(b, assets && assets->symbol ? assets->symbol->footprint_default : NULL);
    raw(b, ",\"footprint_filter\":"); string(b, assets ? assets->suggested_footprint_filter : NULL);
    raw(b, ",\"pins\":[");
    if (assets && assets->symbol) {
        for (size_t i = 0; i < assets->symbol->pin_count; ++i) {
            const KicadSymbolPin *pin = &assets->symbol->pins[i]; if (i) raw(b, ",");
            raw(b, "{\"number\":"); string(b, pin->number);
            raw(b, ",\"name\":"); string(b, pin->name);
            raw(b, ",\"electrical_type\":"); string(b, pin->electrical_type);
            fmt(b, ",\"unit\":%u,\"body_style\":%u}", pin->unit, pin->body_style);
        }
    }
    raw(b, "]},\"footprint\":{\"id\":");
    string(b, assets && assets->effective_footprint_id ? assets->effective_footprint_id : p->footprint_id);
    raw(b, ",\"suggested_id\":"); string(b, assets ? assets->suggested_footprint_id : NULL);
    raw(b, ",\"status\":"); string(b, assets && assets->footprint_found ? (assets->footprint_is_suggestion ? "SUGGESTION_RESOLVED" : "FOUND") : "NOT_RESOLVED");
    raw(b, ",\"path\":"); string(b, assets ? assets->footprint_path : NULL);
    raw(b, ",\"pads\":[");
    if (assets && assets->footprint) {
        const KicadFootprintAsset *fp = assets->footprint;
        for (size_t i = 0; i < fp->pad_count; ++i) {
            const KicadFootprintPad *pad = &fp->pads[i]; if (i) raw(b, ",");
            raw(b, "{\"number\":"); string(b, pad->number);
            raw(b, ",\"type\":"); string(b, pad->type);
            raw(b, ",\"shape\":"); string(b, pad->shape);
            fmt(b, ",\"x_nm\":%lld,\"y_nm\":%lld,\"rotation_mdeg\":%lld,\"size_x_nm\":%lld,\"size_y_nm\":%lld,\"drill_x_nm\":%lld,\"drill_y_nm\":%lld}",
                (long long)pad->x_nm, (long long)pad->y_nm,
                (long long)pad->rotation_mdeg, (long long)pad->size_x_nm,
                (long long)pad->size_y_nm, (long long)pad->drill_x_nm,
                (long long)pad->drill_y_nm);
        }
    }
    raw(b, "]},\"pin_mapping\":{\"status\":");
    string(b, assets ? kicad_pin_map_resolution_name(assets->pin_map_resolution) : "NOT_CHECKED");
    raw(b, ",\"explicit_valid\":"); raw(b, assets && assets->explicit_mapping_valid ? "true" : "false");
    raw(b, ",\"inferred_possible\":"); raw(b, assets && assets->inferred_mapping_possible ? "true" : "false");
    fmt(b, ",\"unique_symbol_pin_count\":%zu,\"matching_pad_count\":%zu,\"missing_pad_count\":%zu}",
        assets ? assets->unique_symbol_pin_count : 0u,
        assets ? assets->matching_pad_count : 0u,
        assets ? assets->missing_pad_count : 0u);
    raw(b, "},\"capabilities\":{");
    raw(b, "\"symbol_ready\":"); raw(b, assets && assets->symbol_found && !assets->symbol_is_suggestion ? "true" : "false");
    raw(b, ",\"footprint_asset_ready\":"); raw(b, assets && assets->footprint_found && !assets->footprint_is_suggestion ? "true" : "false");
    raw(b, ",\"pin_map_verified\":"); raw(b, assets && assets->explicit_mapping_valid ? "true" : "false");
    raw(b, ",\"simulation_ready\":"); raw(b, candidate && candidate->usable_for_simulation ? "true" : "false");
    raw(b, ",\"manufacturing_identity_ready\":"); raw(b, candidate && candidate->identity_verified && p->manufacturer && p->mpn ? "true" : "false");
    raw(b, "},\"resolution_message\":"); string(b, assets ? assets->message : NULL);
    raw(b, ",\"feedback\":\"Catalogue metadata and KiCad assets have been resolved. Missing or inferred fields remain explicitly unverified.\"}");
}

char *design_resolution_json_to_string(const DesignResolutionJsonInput *in,
                                       size_t *out_length) {
    OutBuffer b = {0};
    if (out_length) *out_length = 0;
    if (!in || !in->resolution) return NULL;
    raw(&b, "{\"schema\":\"copper.design\",\"schema_version\":2,\"generation\":{\"phase\":\"PART_RESOLUTION\",\"status\":");
    const char *status = design_resolve_status_name(in->resolution->status);
    if (in->resolution->status == DESIGN_RESOLVE_OK) {
        if (!in->assets) status = "ASSET_RESOLUTION_NOT_RUN";
        else if (in->assets->symbol_found && in->assets->footprint_found &&
                 in->assets->explicit_mapping_valid &&
                 !in->assets->symbol_is_suggestion &&
                 !in->assets->footprint_is_suggestion) status = "PART_ASSETS_RESOLVED";
        else status = "ASSET_RESOLUTION_INCOMPLETE";
    }
    string(&b, status);
    raw(&b, ",\"topology\":\"NOT_BUILT\",\"schematic\":\"NOT_GENERATED\",\"pcb\":\"NOT_GENERATED\",\"manufacturing\":\"NOT_STARTED\"},");
    raw(&b, "\"project\":{\"name\":"); string(&b, in->project_name);
    raw(&b, "},\"units\":{\"length\":\"nm\",\"angle\":\"mdeg\",\"voltage\":\"V\",\"current\":\"A\",\"power\":\"W\"},");
    raw(&b, "\"request\":{\"kind\":"); string(&b, in->query_kind);
    raw(&b, ",\"mpn\":"); string(&b, in->query_mpn);
    raw(&b, ",\"manufacturer\":"); string(&b, in->query_manufacturer);
    raw(&b, ",\"text\":"); string(&b, in->query_text); raw(&b, "},");
    raw(&b, "\"catalogue\":{\"path\":"); string(&b, in->catalogue_path);
    raw(&b, ",\"candidate_count\":"); fmt(&b, "%zu", in->resolution->candidate_count);
    raw(&b, ",\"selected_part_id\":"); string(&b, in->resolution->best ? in->resolution->best->part->id : NULL);
    raw(&b, "},\"components\":[");
    if (in->resolution->best && in->resolution->best->part)
        write_part(&b, in->resolution->best->part, in->resolution->best, in->assets);
    raw(&b, "],\"candidate_ranking\":[");
    for (size_t i = 0; i < in->resolution->candidate_count; ++i) {
        const DesignResolveCandidate *c = &in->resolution->candidates[i];
        if (i) raw(&b, ",");
        raw(&b, "{\"part_id\":"); string(&b, c->part->id);
        raw(&b, ",\"manufacturer\":"); string(&b, c->part->manufacturer);
        raw(&b, ",\"mpn\":"); string(&b, c->part->mpn);
        fmt(&b, ",\"score\":%d,\"priority\":%d,\"missing_fields_mask\":%u,\"identity_verified\":%s,\"simulation_ready\":%s,\"manufacturing_ready\":%s,\"missing_fields\":",
            c->score, c->priority, c->missing_fields,
            c->identity_verified ? "true" : "false",
            c->usable_for_simulation ? "true" : "false",
            c->usable_for_manufacturing ? "true" : "false");
        write_missing_fields(&b, c->missing_fields);
        raw(&b, "}");
    }
    raw(&b, "],\"asset_roots\":{\"symbols_available\":");
    raw(&b, in->asset_roots && in->asset_roots->symbols_available ? "true" : "false");
    raw(&b, ",\"symbol_root\":"); string(&b, in->asset_roots ? in->asset_roots->symbol_root : NULL);
    raw(&b, ",\"symbol_source\":"); string(&b, in->asset_roots ? in->asset_roots->symbol_source : NULL);
    raw(&b, ",\"symbol_table_path\":"); string(&b, in->asset_roots ? in->asset_roots->symbol_table_path : NULL);
    raw(&b, ",\"footprints_available\":");
    raw(&b, in->asset_roots && in->asset_roots->footprints_available ? "true" : "false");
    raw(&b, ",\"footprint_root\":"); string(&b, in->asset_roots ? in->asset_roots->footprint_root : NULL);
    raw(&b, ",\"footprint_source\":"); string(&b, in->asset_roots ? in->asset_roots->footprint_source : NULL);
    raw(&b, ",\"footprint_table_path\":"); string(&b, in->asset_roots ? in->asset_roots->footprint_table_path : NULL);
    raw(&b, "},\"electrical\":{\"status\":\"NOT_BUILT\",\"nets\":[]},\"schematic\":{\"status\":\"NOT_GENERATED\"},\"pcb\":{\"status\":\"NOT_GENERATED\"},\"manufacturing\":{\"status\":\"NOT_STARTED\",\"gerbers\":\"NOT_STARTED\",\"drill\":\"NOT_STARTED\",\"bom\":\"NOT_STARTED\",\"pick_and_place\":\"NOT_STARTED\"},\"feedback\":{\"topology\":\"Pending: part resolution does not imply electrical connectivity.\",\"physics\":\"Pending: no circuit topology or simulation model has been constructed.\",\"physical_design\":\"Pending: no placement, routing or manufacturing geometry has been generated.\"}}\n");
    if (b.failed) { free(b.data); return NULL; }
    if (!b.data) { b.data = (char *)malloc(1u); if (!b.data) return NULL; b.data[0] = '\0'; }
    if (out_length) *out_length = b.length;
    return b.data;
}

FsStatus design_resolution_json_write_file(const DesignResolutionJsonInput *in,
                                           const char *output_path) {
    if (!in || !output_path) return FS_ERR_ARGUMENT;
    size_t length = 0;
    char *json = design_resolution_json_to_string(in, &length);
    if (!json) return FS_ERR_OUT_OF_MEMORY;
    FsStatus status = fs_write_file_atomic_replace(output_path, json, length);
    free(json);
    return status;
}

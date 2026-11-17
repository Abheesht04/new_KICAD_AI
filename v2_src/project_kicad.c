#include "project_kicad.h"
#include "filesystem.h"

#include <stdio.h>
#include <string.h>
#include <limits.h>

typedef struct PJsonValue PJsonValue;
typedef struct PJsonMember {
    const char *key;
    PJsonValue *value;
    struct PJsonMember *next;
} PJsonMember;
struct PJsonValue {
    ProjectJsonValueKind kind;
    const char *text;
    size_t text_length;
    const char *raw_json;
    size_t raw_length;
    bool boolean;
    PJsonMember *members;
    PJsonMember *last_member;
    PJsonValue *items;
    PJsonValue *last_item;
    PJsonValue *next;
    size_t count;
};
typedef struct PJsonParser {
    Arena *arena;
    const char *source;
    size_t length;
    size_t position;
    size_t line;
    size_t column;
    unsigned depth;
    ProjectSettingsDiag *diag;
} PJsonParser;

static void pdiag(ProjectSettingsDiag *diag, ProjectSettingsStatus status,
                  const char *message, size_t line, size_t column) {
    if (!diag) return;
    diag->status = status;
    snprintf(diag->message, sizeof(diag->message), "%s", message ? message : "");
    diag->line = line;
    diag->column = column;
}
static void skip_ws(PJsonParser *p) {
    while (p->position < p->length) {
        char c = p->source[p->position];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
        ++p->position;
        if (c == '\n') { ++p->line; p->column = 1; }
        else ++p->column;
    }
}
static char *pdup_n(Arena *a, const char *s, size_t n) {
    char *d = (char *)arena_alloc(a, n + 1u);
    if (!d) return NULL;
    if (n) memcpy(d, s, n);
    d[n] = '\0';
    return d;
}
static void advance(PJsonParser *p) {
    if (p->position >= p->length) return;
    if (p->source[p->position] == '\n') { ++p->line; p->column = 1; }
    else ++p->column;
    ++p->position;
}
static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static bool append_utf8(char *out, size_t cap, size_t *used, uint32_t cp) {
    unsigned char bytes[4]; size_t n;
    if (cp == 0) return false;
    if (cp <= 0x7f) { bytes[0] = (unsigned char)cp; n = 1; }
    else if (cp <= 0x7ff) { bytes[0] = (unsigned char)(0xc0u | (cp >> 6)); bytes[1] = (unsigned char)(0x80u | (cp & 0x3fu)); n = 2; }
    else if (cp <= 0xffff) { bytes[0] = (unsigned char)(0xe0u | (cp >> 12)); bytes[1] = (unsigned char)(0x80u | ((cp >> 6) & 0x3fu)); bytes[2] = (unsigned char)(0x80u | (cp & 0x3fu)); n = 3; }
    else if (cp <= 0x10ffff) { bytes[0] = (unsigned char)(0xf0u | (cp >> 18)); bytes[1] = (unsigned char)(0x80u | ((cp >> 12) & 0x3fu)); bytes[2] = (unsigned char)(0x80u | ((cp >> 6) & 0x3fu)); bytes[3] = (unsigned char)(0x80u | (cp & 0x3fu)); n = 4; }
    else return false;
    if (*used > cap || n > cap - *used) return false;
    memcpy(out + *used, bytes, n); *used += n; return true;
}
static bool parse_u4(PJsonParser *p, uint32_t *out) {
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) {
        if (p->position >= p->length) return false;
        int h = hex_digit(p->source[p->position]); if (h < 0) return false;
        value = (value << 4) | (uint32_t)h; advance(p);
    }
    *out = value; return true;
}
static char *parse_string(PJsonParser *p, size_t *out_len) {
    size_t start, max_len, used = 0;
    char *out;
    if (p->position >= p->length || p->source[p->position] != '"') return NULL;
    advance(p); start = p->position; max_len = p->length - start;
    out = (char *)arena_alloc(p->arena, max_len + 1u);
    if (!out) return NULL;
    while (p->position < p->length) {
        unsigned char c = (unsigned char)p->source[p->position];
        if (c == '"') { advance(p); out[used] = '\0'; if (out_len) *out_len = used; return out; }
        if (c < 0x20) return NULL;
        if (c != '\\') { out[used++] = (char)c; advance(p); continue; }
        advance(p);
        if (p->position >= p->length) return NULL;
        char e = p->source[p->position]; advance(p);
        switch (e) {
        case '"': out[used++] = '"'; break;
        case '\\': out[used++] = '\\'; break;
        case '/': out[used++] = '/'; break;
        case 'b': out[used++] = '\b'; break;
        case 'f': out[used++] = '\f'; break;
        case 'n': out[used++] = '\n'; break;
        case 'r': out[used++] = '\r'; break;
        case 't': out[used++] = '\t'; break;
        case 'u': {
            uint32_t cp = 0;
            if (!parse_u4(p, &cp)) return NULL;
            if (cp >= 0xd800 && cp <= 0xdbff) {
                if (p->position + 2u > p->length || p->source[p->position] != '\\' || p->source[p->position + 1u] != 'u') return NULL;
                advance(p); advance(p);
                uint32_t low = 0; if (!parse_u4(p, &low) || low < 0xdc00 || low > 0xdfff) return NULL;
                cp = 0x10000u + (((cp - 0xd800u) << 10) | (low - 0xdc00u));
            } else if (cp >= 0xdc00 && cp <= 0xdfff) return NULL;
            if (!append_utf8(out, max_len + 1u, &used, cp)) return NULL;
            break;
        }
        default: return NULL;
        }
    }
    return NULL;
}
static PJsonValue *parse_value(PJsonParser *p);
static PJsonValue *new_value(PJsonParser *p, ProjectJsonValueKind kind,
                             size_t start) {
    PJsonValue *v = (PJsonValue *)arena_alloc(p->arena, sizeof(*v));
    if (!v) return NULL;
    memset(v, 0, sizeof(*v)); v->kind = kind; v->raw_json = p->source + start;
    return v;
}
static PJsonValue *parse_object(PJsonParser *p, size_t start) {
    PJsonValue *v = new_value(p, PROJECT_JSON_OBJECT, start);
    if (!v) return NULL;
    advance(p); skip_ws(p);
    if (p->position < p->length && p->source[p->position] == '}') { advance(p); v->raw_length = p->position - start; return v; }
    for (;;) {
        PJsonMember *m; size_t key_len = 0; char *key;
        if (p->position >= p->length || p->source[p->position] != '"') return NULL;
        key = parse_string(p, &key_len); (void)key_len;
        if (!key) return NULL;
        skip_ws(p); if (p->position >= p->length || p->source[p->position] != ':') return NULL;
        advance(p); skip_ws(p);
        PJsonValue *value = parse_value(p); if (!value) return NULL;
        m = (PJsonMember *)arena_alloc(p->arena, sizeof(*m)); if (!m) return NULL;
        m->key = key; m->value = value; m->next = NULL;
        if (v->last_member) v->last_member->next = m; else v->members = m;
        v->last_member = m; ++v->count;
        skip_ws(p);
        if (p->position >= p->length) return NULL;
        if (p->source[p->position] == '}') { advance(p); v->raw_length = p->position - start; return v; }
        if (p->source[p->position] != ',') return NULL;
        advance(p); skip_ws(p);
    }
}
static PJsonValue *parse_array(PJsonParser *p, size_t start) {
    PJsonValue *v = new_value(p, PROJECT_JSON_ARRAY, start);
    if (!v) return NULL;
    advance(p); skip_ws(p);
    if (p->position < p->length && p->source[p->position] == ']') { advance(p); v->raw_length = p->position - start; return v; }
    for (;;) {
        PJsonValue *item = parse_value(p); if (!item) return NULL;
        item->next = NULL;
        if (v->last_item) v->last_item->next = item; else v->items = item;
        v->last_item = item; ++v->count;
        skip_ws(p);
        if (p->position >= p->length) return NULL;
        if (p->source[p->position] == ']') { advance(p); v->raw_length = p->position - start; return v; }
        if (p->source[p->position] != ',') return NULL;
        advance(p); skip_ws(p);
    }
}
static PJsonValue *parse_number(PJsonParser *p, size_t start) {
    size_t begin = p->position;
    if (p->source[p->position] == '-') advance(p);
    if (p->position >= p->length) return NULL;
    if (p->source[p->position] == '0') advance(p);
    else {
        if (p->source[p->position] < '1' || p->source[p->position] > '9') return NULL;
        while (p->position < p->length && p->source[p->position] >= '0' && p->source[p->position] <= '9') advance(p);
    }
    if (p->position < p->length && p->source[p->position] == '.') {
        advance(p); size_t digits = p->position;
        while (p->position < p->length && p->source[p->position] >= '0' && p->source[p->position] <= '9') advance(p);
        if (digits == p->position) return NULL;
    }
    if (p->position < p->length && (p->source[p->position] == 'e' || p->source[p->position] == 'E')) {
        advance(p); if (p->position < p->length && (p->source[p->position] == '+' || p->source[p->position] == '-')) advance(p);
        size_t digits = p->position;
        while (p->position < p->length && p->source[p->position] >= '0' && p->source[p->position] <= '9') advance(p);
        if (digits == p->position) return NULL;
    }
    PJsonValue *v = new_value(p, PROJECT_JSON_NUMBER, start);
    if (!v) return NULL;
    v->text = pdup_n(p->arena, p->source + begin, p->position - begin);
    if (!v->text) return NULL;
    v->text_length = p->position - begin; v->raw_length = p->position - start;
    return v;
}
static PJsonValue *parse_value(PJsonParser *p) {
    if (++p->depth > 128u) { --p->depth; return NULL; }
    skip_ws(p);
    size_t start = p->position;
    if (start >= p->length) { --p->depth; return NULL; }
    char c = p->source[p->position]; PJsonValue *v = NULL;
    if (c == '{') v = parse_object(p, start);
    else if (c == '[') v = parse_array(p, start);
    else if (c == '"') {
        size_t n = 0; char *s = parse_string(p, &n);
        if (s) { v = new_value(p, PROJECT_JSON_STRING, start); if (v) { v->text = s; v->text_length = n; v->raw_length = p->position - start; } }
    } else if (c == '-' || (c >= '0' && c <= '9')) v = parse_number(p, start);
    else if (p->length - p->position >= 4u && memcmp(p->source + p->position, "true", 4) == 0) {
        v = new_value(p, PROJECT_JSON_BOOLEAN, start); advance(p); advance(p); advance(p); advance(p); if (v) { v->boolean = true; v->raw_length = 4; }
    } else if (p->length - p->position >= 5u && memcmp(p->source + p->position, "false", 5) == 0) {
        v = new_value(p, PROJECT_JSON_BOOLEAN, start); for (unsigned i = 0; i < 5; ++i) advance(p); if (v) { v->boolean = false; v->raw_length = 5; }
    } else if (p->length - p->position >= 4u && memcmp(p->source + p->position, "null", 4) == 0) {
        v = new_value(p, PROJECT_JSON_NULL, start); for (unsigned i = 0; i < 4; ++i) advance(p); if (v) v->raw_length = 4;
    }
    --p->depth; return v;
}
static const PJsonValue *object_get(const PJsonValue *v, const char *key) {
    if (!v || v->kind != PROJECT_JSON_OBJECT || !key) return NULL;
    for (PJsonMember *m = v->members; m; m = m->next)
        if (strcmp(m->key, key) == 0) return m->value;
    return NULL;
}
static const PJsonValue *path_get(const PJsonValue *v, const char *a,
                                  const char *b, const char *c) {
    v = object_get(v, a); if (b) v = object_get(v, b); if (c) v = object_get(v, c); return v;
}
static const char *json_raw_copy(Arena *arena, const PJsonValue *v) {
    return v ? pdup_n(arena, v->raw_json, v->raw_length) : NULL;
}
static bool json_i64(const PJsonValue *v, int64_t *out) {
    int64_t n = 0; bool neg = false; size_t i = 0;
    if (!v || v->kind != PROJECT_JSON_NUMBER || !out || !v->text) return false;
    if (v->text[i] == '-') { neg = true; ++i; }
    if (i >= v->text_length) return false;
    for (; i < v->text_length; ++i) {
        char c = v->text[i]; if (c < '0' || c > '9') return false;
        int d = c - '0';
        if (n > (INT64_MAX - d) / 10) return false;
        n = n * 10 + d;
    }
    *out = neg ? -n : n; return true;
}
static bool json_mm_to_nm(const PJsonValue *v, int64_t *out) {
    const char *s; size_t i = 0; bool neg = false; uint64_t whole = 0, frac = 0, scale = 1; unsigned fd = 0;
    if (!v || v->kind != PROJECT_JSON_NUMBER || !v->text || !out) return false;
    s = v->text; if (s[i] == '-') { neg = true; ++i; }
    if (i >= v->text_length) return false;
    for (; i < v->text_length && s[i] != '.'; ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        unsigned d = (unsigned)(s[i] - '0');
        if (whole > (UINT64_MAX - d) / 10u) return false;
        whole = whole * 10u + d;
    }
    if (i < v->text_length && s[i] == '.') {
        ++i;
        for (; i < v->text_length; ++i) {
            if (s[i] < '0' || s[i] > '9') return false;
            if (fd < 6) { frac = frac * 10u + (unsigned)(s[i] - '0'); scale *= 10u; ++fd; }
            else if (fd == 6) { if (s[i] >= '5') frac += 1u; ++fd; }
            else ++fd;
        }
    }
    if (whole > (uint64_t)INT64_MAX / 1000000u) return false;
    uint64_t nm = whole * 1000000u;
    if (scale > 1) {
        uint64_t fractional_nm = (frac * 1000000u + scale / 2u) / scale;
        if (nm > (uint64_t)INT64_MAX - fractional_nm) return false;
        nm += fractional_nm;
    }
    if (nm > (uint64_t)INT64_MAX) return false;
    *out = neg ? -(int64_t)nm : (int64_t)nm; return true;
}
static size_t count_object_members(const PJsonValue *v) {
    return v && v->kind == PROJECT_JSON_OBJECT ? v->count : 0;
}
static ProjectSettingKV *copy_kv(Arena *arena, const PJsonValue *object,
                                 size_t *out_count) {
    size_t count = count_object_members(object), i = 0;
    ProjectSettingKV *items = count ? (ProjectSettingKV *)arena_alloc_array(arena, sizeof(*items), count) : NULL;
    if (count && !items) return NULL;
    for (PJsonMember *m = object ? object->members : NULL; m; m = m->next) {
        items[i].name = m->key;
        items[i].value_json = json_raw_copy(arena, m->value);
        items[i].value_kind = m->value->kind;
        if (!items[i].value_json) return NULL;
        ++i;
    }
    *out_count = count; return items;
}
static bool fill_net_class(Arena *arena, const PJsonValue *v,
                           ProjectNetClassIR *out) {
    memset(out, 0, sizeof(*out));
    const PJsonValue *name = object_get(v, "name");
    if (!name || name->kind != PROJECT_JSON_STRING) return false;
    out->name = name->text;
#define MM_FIELD(field, key) do { const PJsonValue *n = object_get(v, key); if (n && !json_mm_to_nm(n, &out->field)) return false; } while (0)
    MM_FIELD(clearance_nm, "clearance"); MM_FIELD(track_width_nm, "track_width");
    MM_FIELD(diff_pair_width_nm, "diff_pair_width"); MM_FIELD(diff_pair_gap_nm, "diff_pair_gap");
    MM_FIELD(diff_pair_via_gap_nm, "diff_pair_via_gap"); MM_FIELD(via_diameter_nm, "via_diameter");
    MM_FIELD(via_drill_nm, "via_drill"); MM_FIELD(microvia_diameter_nm, "microvia_diameter");
    MM_FIELD(microvia_drill_nm, "microvia_drill");
#undef MM_FIELD
    const PJsonValue *bus = object_get(v, "bus_width");
    if (bus && !json_i64(bus, &out->bus_width)) return false;
    const PJsonValue *priority = object_get(v, "priority");
    if (priority && !json_i64(priority, &out->priority)) {
        /* KiCad can use a very large signed integer; preserve the raw object. */
        out->priority = 0;
    }
    out->raw_object_json = json_raw_copy(arena, v);
    return out->raw_object_json != NULL;
}

ProjectSettingsStatus project_settings_init(ProjectSettingsIR *out, Arena *arena) {
    if (!out || !arena) return PROJECT_SETTINGS_ERR_ARGUMENT;
    memset(out, 0, sizeof(*out)); out->arena = arena; return PROJECT_SETTINGS_OK;
}
ProjectSettingsStatus project_settings_parse(Arena *arena, const char *source,
                                               size_t source_length,
                                               ProjectSettingsIR *out,
                                               ProjectSettingsDiag *diag) {
    PJsonParser parser; PJsonValue *root; const PJsonValue *meta, *classes;
    if (!arena || !source || !out) { pdiag(diag, PROJECT_SETTINGS_ERR_ARGUMENT, "arena, source and output are required", 0, 0); return PROJECT_SETTINGS_ERR_ARGUMENT; }
    if (diag) memset(diag, 0, sizeof(*diag));
    project_settings_init(out, arena);
    if (source_length == SIZE_MAX) {
        pdiag(diag, PROJECT_SETTINGS_ERR_MEMORY, "project JSON is too large to retain", 1, 1);
        return PROJECT_SETTINGS_ERR_MEMORY;
    }
    char *source_copy = (char *)arena_alloc(arena, source_length + 1u);
    if (!source_copy) {
        pdiag(diag, PROJECT_SETTINGS_ERR_MEMORY, "unable to retain project JSON", 1, 1);
        return PROJECT_SETTINGS_ERR_MEMORY;
    }
    memcpy(source_copy, source, source_length);
    source_copy[source_length] = '\0';
    memset(&parser, 0, sizeof(parser)); parser.arena = arena; parser.source = source_copy; parser.length = source_length; parser.line = 1; parser.column = 1; parser.diag = diag;
    root = parse_value(&parser); skip_ws(&parser);
    if (!root || parser.position != parser.length) { pdiag(diag, PROJECT_SETTINGS_ERR_JSON, "invalid or trailing JSON content", parser.line, parser.column); return PROJECT_SETTINGS_ERR_JSON; }
    if (root->kind != PROJECT_JSON_OBJECT) { pdiag(diag, PROJECT_SETTINGS_ERR_ROOT, "KiCad project root must be a JSON object", 1, 1); return PROJECT_SETTINGS_ERR_ROOT; }
    out->source_json = source_copy;
    out->source_json_length = source_length;
    meta = object_get(root, "meta");
    const PJsonValue *version = object_get(meta, "version");
    int64_t vnum = 0; if (version && json_i64(version, &vnum) && vnum >= 0 && vnum <= UINT32_MAX) out->metadata_version = (uint32_t)vnum;
    const PJsonValue *filename = object_get(meta, "filename");
    if (filename && filename->kind == PROJECT_JSON_STRING) out->filename = filename->text;

    classes = path_get(root, "net_settings", "classes", NULL);
    if (classes) {
        if (classes->kind != PROJECT_JSON_ARRAY) { pdiag(diag, PROJECT_SETTINGS_ERR_JSON, "net_settings.classes must be an array", 1, 1); return PROJECT_SETTINGS_ERR_JSON; }
        out->net_class_count = classes->count;
        out->net_classes = classes->count ? (ProjectNetClassIR *)arena_alloc_array(arena, sizeof(*out->net_classes), classes->count) : NULL;
        if (classes->count && !out->net_classes) return PROJECT_SETTINGS_ERR_MEMORY;
        size_t i = 0;
        for (const PJsonValue *item = classes->items; item; item = item->next, ++i)
            if (item->kind != PROJECT_JSON_OBJECT || !fill_net_class(arena, item, &out->net_classes[i])) {
                pdiag(diag, PROJECT_SETTINGS_ERR_JSON, "invalid net class record", 1, 1); return PROJECT_SETTINGS_ERR_JSON;
            }
    }
    const PJsonValue *board_rules = path_get(root, "board", "design_settings", "rules");
    out->board_rules = copy_kv(arena, board_rules, &out->board_rule_count);
    if (out->board_rule_count && !out->board_rules) return PROJECT_SETTINGS_ERR_MEMORY;
    const PJsonValue *board_severity = path_get(root, "board", "design_settings", "rule_severities");
    out->board_rule_severities = copy_kv(arena, board_severity, &out->board_rule_severity_count);
    if (out->board_rule_severity_count && !out->board_rule_severities) return PROJECT_SETTINGS_ERR_MEMORY;
    const PJsonValue *erc_severity = path_get(root, "erc", "rule_severities", NULL);
    out->erc_rule_severities = copy_kv(arena, erc_severity, &out->erc_rule_severity_count);
    if (out->erc_rule_severity_count && !out->erc_rule_severities) return PROJECT_SETTINGS_ERR_MEMORY;
    pdiag(diag, PROJECT_SETTINGS_OK, "KiCad project settings parsed", 0, 0);
    return PROJECT_SETTINGS_OK;
}
ProjectSettingsStatus project_settings_load_kicad(const char *path, Arena *arena,
                                                    ProjectSettingsIR *out,
                                                    ProjectSettingsDiag *diag) {
    const unsigned char *data = NULL; size_t size = 0;
    if (!path || !arena || !out) return PROJECT_SETTINGS_ERR_ARGUMENT;
    FsStatus fs = fs_read_file_arena(arena, path, &data, &size);
    if (fs != FS_OK) { pdiag(diag, PROJECT_SETTINGS_ERR_IO, fs_status_name(fs), 0, 0); return PROJECT_SETTINGS_ERR_IO; }
    return project_settings_parse(arena, (const char *)data, size, out, diag);
}
const char *project_settings_status_name(ProjectSettingsStatus status) {
    static const char *const names[] = {"OK", "ARGUMENT", "IO", "JSON", "ROOT", "MEMORY", "RANGE"};
    return (unsigned)status < sizeof(names) / sizeof(names[0]) ? names[status] : "UNKNOWN";
}

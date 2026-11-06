/* json.c - strict JSON reader (see json.h). */
#include "json.h"

#include <stdio.h>
#include <string.h>

static const char *const NAMES[] = {"OK", "INVALID_ARGUMENT", "IO_ERROR", "PARSE_ERROR", "DUPLICATE_KEY", "TOO_DEEP",
                                    "OUT_OF_MEMORY"};

const char *json_status_name(JsonStatus s)
{
    return (unsigned)s < sizeof NAMES / sizeof *NAMES ? NAMES[s] : "UNKNOWN_STATUS";
}

typedef struct Parser {
    Arena *arena;
    const char *s;
    size_t len, pos, line, line_start;
    JsonError *err;
} Parser;

static JsonStatus fail(Parser *p, JsonStatus code, const char *message)
{
    if (p->err) {
        p->err->code = code;
        p->err->offset = p->pos;
        p->err->line = p->line;
        p->err->column = p->pos - p->line_start + 1;
        p->err->message = message;
    }
    return code;
}

static void skip_ws(Parser *p)
{
    while (p->pos < p->len) {
        char c = p->s[p->pos];
        if (c == '\n') { p->pos++; p->line++; p->line_start = p->pos; }
        else if (c == ' ' || c == '\t' || c == '\r') p->pos++;
        else break;
    }
}

static JsonValue *new_value(Parser *p, JsonKind kind)
{
    JsonValue *v = arena_alloc(p->arena, sizeof *v);
    if (!v) return NULL;
    memset(v, 0, sizeof *v);
    v->kind = kind;
    v->line = p->line;
    v->column = p->pos - p->line_start + 1;
    return v;
}

static int hex4(const char *s)
{
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return -1;
    }
    return v;
}

static size_t put_utf8(char *out, uint32_t cp)
{
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) { out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* A string starting at the opening quote; decoded into the arena. */
static JsonStatus parse_string(Parser *p, const char **out, size_t *out_len)
{
    size_t start = ++p->pos, span = 0;
    while (start + span < p->len && p->s[start + span] != '"') span += p->s[start + span] == '\\' ? 2 : 1;
    char *buf = arena_alloc(p->arena, span + 1);   /* decoded UTF-8 is never longer than its escapes */
    if (!buf) return fail(p, JSON_OUT_OF_MEMORY, "out of memory");
    size_t n = 0;
    for (;;) {
        if (p->pos >= p->len) return fail(p, JSON_PARSE_ERROR, "unterminated string");
        unsigned char c = (unsigned char)p->s[p->pos];
        if (c == '"') { p->pos++; break; }
        if (c < 0x20) return fail(p, JSON_PARSE_ERROR, "control character in a string (escape it)");
        if (c != '\\') { buf[n++] = (char)c; p->pos++; continue; }
        if (p->pos + 1 >= p->len) return fail(p, JSON_PARSE_ERROR, "unterminated escape");
        char e = p->s[p->pos + 1];
        p->pos += 2;
        switch (e) {
        case '"': buf[n++] = '"'; break;
        case '\\': buf[n++] = '\\'; break;
        case '/': buf[n++] = '/'; break;
        case 'b': buf[n++] = '\b'; break;
        case 'f': buf[n++] = '\f'; break;
        case 'n': buf[n++] = '\n'; break;
        case 'r': buf[n++] = '\r'; break;
        case 't': buf[n++] = '\t'; break;
        case 'u': {
            if (p->pos + 4 > p->len) return fail(p, JSON_PARSE_ERROR, "\\u needs four hex digits");
            int cp = hex4(p->s + p->pos);
            if (cp < 0) return fail(p, JSON_PARSE_ERROR, "\\u needs four hex digits");
            p->pos += 4;
            uint32_t code = (uint32_t)cp;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                int lo = (p->pos + 6 <= p->len && p->s[p->pos] == '\\' && p->s[p->pos + 1] == 'u') ? hex4(p->s + p->pos + 2) : -1;
                if (lo < 0xDC00 || lo > 0xDFFF) return fail(p, JSON_PARSE_ERROR, "unpaired surrogate in \\u escape");
                p->pos += 6;
                code = 0x10000 + (((uint32_t)cp - 0xD800) << 10) + ((uint32_t)lo - 0xDC00);
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                return fail(p, JSON_PARSE_ERROR, "unpaired surrogate in \\u escape");
            }
            if (code == 0) return fail(p, JSON_PARSE_ERROR, "\\u0000 is not allowed");
            n += put_utf8(buf + n, code);
            break;
        }
        default: return fail(p, JSON_PARSE_ERROR, "unknown escape");
        }
    }
    buf[n] = 0;
    *out = buf;
    *out_len = n;
    return JSON_OK;
}

static JsonStatus parse_number(Parser *p, JsonValue *v)
{
    size_t start = p->pos;
    if (p->pos < p->len && p->s[p->pos] == '-') p->pos++;
    if (p->pos >= p->len) return fail(p, JSON_PARSE_ERROR, "bad number");
    if (p->s[p->pos] == '0') p->pos++;
    else if (p->s[p->pos] >= '1' && p->s[p->pos] <= '9') while (p->pos < p->len && p->s[p->pos] >= '0' && p->s[p->pos] <= '9') p->pos++;
    else return fail(p, JSON_PARSE_ERROR, "bad number");
    if (p->pos < p->len && p->s[p->pos] == '.') {
        p->pos++;
        if (p->pos >= p->len || p->s[p->pos] < '0' || p->s[p->pos] > '9') return fail(p, JSON_PARSE_ERROR, "digits must follow '.'");
        while (p->pos < p->len && p->s[p->pos] >= '0' && p->s[p->pos] <= '9') p->pos++;
    }
    if (p->pos < p->len && (p->s[p->pos] == 'e' || p->s[p->pos] == 'E')) {
        p->pos++;
        if (p->pos < p->len && (p->s[p->pos] == '+' || p->s[p->pos] == '-')) p->pos++;
        if (p->pos >= p->len || p->s[p->pos] < '0' || p->s[p->pos] > '9') return fail(p, JSON_PARSE_ERROR, "digits must follow the exponent");
        while (p->pos < p->len && p->s[p->pos] >= '0' && p->s[p->pos] <= '9') p->pos++;
    }
    v->text = p->s + start;   /* the source buffer outlives nothing: copy it */
    char *copy = arena_alloc(p->arena, p->pos - start + 1);
    if (!copy) return fail(p, JSON_OUT_OF_MEMORY, "out of memory");
    memcpy(copy, p->s + start, p->pos - start);
    copy[p->pos - start] = 0;
    v->text = copy;
    v->text_len = p->pos - start;
    return JSON_OK;
}

static bool literal(Parser *p, const char *word)
{
    size_t n = strlen(word);
    if (p->len - p->pos < n || memcmp(p->s + p->pos, word, n)) return false;
    p->pos += n;
    return true;
}

typedef struct Members { const char **keys; JsonValue **items; size_t count, cap; } Members;

static bool push_member(Arena *a, Members *m, const char *key, JsonValue *v)
{
    if (m->count == m->cap) {
        size_t cap = m->cap ? m->cap * 2 : 8;
        JsonValue **items = arena_alloc_array(a, sizeof *items, cap);
        const char **keys = arena_alloc_array(a, sizeof *keys, cap);
        if (!items || !keys) return false;
        if (m->count) { memcpy(items, m->items, m->count * sizeof *items); memcpy(keys, m->keys, m->count * sizeof *keys); }
        m->items = items; m->keys = keys; m->cap = cap;
    }
    m->keys[m->count] = key;
    m->items[m->count++] = v;
    return true;
}

static JsonStatus parse_value(Parser *p, int depth, JsonValue **out)
{
    JsonStatus s;
    if (depth > JSON_MAX_DEPTH) return fail(p, JSON_TOO_DEEP, "nesting too deep");
    skip_ws(p);
    if (p->pos >= p->len) return fail(p, JSON_PARSE_ERROR, "a value is missing");
    char c = p->s[p->pos];
    JsonValue *v;
    if (c == '{' || c == '[') {
        bool object = c == '{';
        v = new_value(p, object ? JSON_OBJECT : JSON_ARRAY);
        if (!v) return fail(p, JSON_OUT_OF_MEMORY, "out of memory");
        Members m = {0};
        p->pos++;
        skip_ws(p);
        if (p->pos < p->len && p->s[p->pos] == (object ? '}' : ']')) { p->pos++; *out = v; return JSON_OK; }
        for (;;) {
            const char *key = NULL;
            size_t key_len;
            JsonValue *item;
            if (object) {
                skip_ws(p);
                if (p->pos >= p->len || p->s[p->pos] != '"') return fail(p, JSON_PARSE_ERROR, "an object key must be a string");
                size_t kpos = p->pos, kline = p->line, kstart = p->line_start;
                if ((s = parse_string(p, &key, &key_len))) return s;
                for (size_t i = 0; i < m.count; i++)
                    if (!strcmp(m.keys[i], key)) {
                        p->pos = kpos; p->line = kline; p->line_start = kstart;
                        return fail(p, JSON_DUPLICATE_KEY, "duplicate key in an object");
                    }
                skip_ws(p);
                if (p->pos >= p->len || p->s[p->pos] != ':') return fail(p, JSON_PARSE_ERROR, "':' expected after a key");
                p->pos++;
            }
            if ((s = parse_value(p, depth + 1, &item))) return s;
            if (!push_member(p->arena, &m, key, item)) return fail(p, JSON_OUT_OF_MEMORY, "out of memory");
            skip_ws(p);
            if (p->pos < p->len && p->s[p->pos] == ',') { p->pos++; continue; }
            if (p->pos < p->len && p->s[p->pos] == (object ? '}' : ']')) { p->pos++; break; }
            return fail(p, JSON_PARSE_ERROR, object ? "',' or '}' expected" : "',' or ']' expected");
        }
        v->count = m.count; v->items = m.items; v->keys = object ? m.keys : NULL;
        *out = v;
        return JSON_OK;
    }
    if (c == '"') {
        v = new_value(p, JSON_STRING);
        if (!v) return fail(p, JSON_OUT_OF_MEMORY, "out of memory");
        if ((s = parse_string(p, &v->text, &v->text_len))) return s;
        *out = v;
        return JSON_OK;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        v = new_value(p, JSON_NUMBER);
        if (!v) return fail(p, JSON_OUT_OF_MEMORY, "out of memory");
        if ((s = parse_number(p, v))) return s;
        *out = v;
        return JSON_OK;
    }
    JsonKind kind;
    v = new_value(p, JSON_NULL);
    if (!v) return fail(p, JSON_OUT_OF_MEMORY, "out of memory");
    if (literal(p, "true")) kind = JSON_TRUE;
    else if (literal(p, "false")) kind = JSON_FALSE;
    else if (literal(p, "null")) kind = JSON_NULL;
    else return fail(p, JSON_PARSE_ERROR, "unexpected character");
    v->kind = kind;
    *out = v;
    return JSON_OK;
}

JsonStatus json_parse(Arena *arena, const char *text, size_t len, JsonValue **out, JsonError *error)
{
    Parser p = {arena, text, len, 0, 1, 0, error};
    if (!arena || !text || !out) return fail(&p, JSON_INVALID_ARGUMENT, "NULL argument");
    if (len >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) p.pos = 3;
    JsonStatus s = parse_value(&p, 0, out);
    if (s) return s;
    skip_ws(&p);
    if (p.pos != p.len) return fail(&p, JSON_PARSE_ERROR, "text after the JSON value");
    if (error) memset(error, 0, sizeof *error);
    return JSON_OK;
}

JsonStatus json_parse_file(Arena *arena, const char *path, JsonValue **out, JsonError *error)
{
    if (!arena || !path || !out) {
        if (error) { memset(error, 0, sizeof *error); error->code = JSON_INVALID_ARGUMENT; error->message = "NULL argument"; }
        return JSON_INVALID_ARGUMENT;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (error) { memset(error, 0, sizeof *error); error->code = JSON_IO_ERROR; error->message = "cannot open file"; }
        return JSON_IO_ERROR;
    }
    size_t cap = 1 << 16, n = 0;
    char *buf = arena_alloc(arena, cap);
    while (buf) {
        size_t got = fread(buf + n, 1, cap - n, f);
        n += got;
        if (n < cap) break;
        char *bigger = arena_alloc(arena, cap * 2);
        if (bigger) memcpy(bigger, buf, n);
        buf = bigger;
        cap *= 2;
    }
    bool bad = ferror(f) != 0;
    fclose(f);
    if (!buf || bad) {
        if (error) { memset(error, 0, sizeof *error); error->code = buf ? JSON_IO_ERROR : JSON_OUT_OF_MEMORY; error->message = buf ? "read error" : "out of memory"; }
        return buf ? JSON_IO_ERROR : JSON_OUT_OF_MEMORY;
    }
    return json_parse(arena, buf, n, out, error);
}

const JsonValue *json_get(const JsonValue *o, const char *key)
{
    if (!o || o->kind != JSON_OBJECT || !key) return NULL;
    for (size_t i = 0; i < o->count; i++)
        if (!strcmp(o->keys[i], key)) return o->items[i];
    return NULL;
}

JsonNumberStatus json_to_nm(const JsonValue *v, int64_t *out)
{
    if (!v || v->kind != JSON_NUMBER || !out) return JSON_NUMBER_NOT_A_NUMBER;
    const char *s = v->text;
    bool neg = *s == '-';
    if (neg) s++;
    int64_t whole = 0, frac = 0;
    int digits = 0;
    while (*s >= '0' && *s <= '9') {
        if (whole > INT64_C(1000000000)) return JSON_NUMBER_RANGE;
        whole = whole * 10 + (*s++ - '0');
    }
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9') {
            if (++digits > 6) return JSON_NUMBER_TOO_PRECISE;
            frac = frac * 10 + (*s++ - '0');
        }
    }
    if (*s == 'e' || *s == 'E') return JSON_NUMBER_EXPONENT;
    for (; digits < 6; digits++) frac *= 10;
    if (whole > INT64_C(1000000000) || (whole == INT64_C(1000000000) && frac)) return JSON_NUMBER_RANGE;
    int64_t nm = whole * 1000000 + frac;
    *out = neg ? -nm : nm;
    return JSON_NUMBER_OK;
}

JsonNumberStatus json_to_i64(const JsonValue *v, int64_t *out)
{
    if (!v || v->kind != JSON_NUMBER || !out) return JSON_NUMBER_NOT_A_NUMBER;
    const char *s = v->text;
    bool neg = *s == '-';
    if (neg) s++;
    uint64_t x = 0;
    while (*s >= '0' && *s <= '9') {
        unsigned d = (unsigned)(*s++ - '0');
        if (x > (UINT64_C(9223372036854775807) - d) / 10) return JSON_NUMBER_RANGE;
        x = x * 10 + d;
    }
    if (*s == '.' ) return JSON_NUMBER_TOO_PRECISE;
    if (*s == 'e' || *s == 'E') return JSON_NUMBER_EXPONENT;
    *out = neg ? -(int64_t)x : (int64_t)x;
    return JSON_NUMBER_OK;
}

/* ------------------------------------------------------------------------------------------------ writer */

void json_writer_init(JsonWriter *w, Arena *arena)
{
    memset(w, 0, sizeof *w);
    w->arena = arena;
    w->depth = -1;
    if (!arena) w->failed = true;
}

static void put(JsonWriter *w, const char *s, size_t n)
{
    if (w->failed) return;
    if (w->len + n + 2 > w->cap) {
        size_t cap = w->cap ? w->cap * 2 : 1024;
        while (cap < w->len + n + 2) cap *= 2;
        char *b = arena_alloc(w->arena, cap);
        if (!b) { w->failed = true; return; }
        if (w->len) memcpy(b, w->buf, w->len);
        w->buf = b;
        w->cap = cap;
    }
    memcpy(w->buf + w->len, s, n);
    w->len += n;
    w->buf[w->len] = 0;
}

static void puts_(JsonWriter *w, const char *s) { put(w, s, strlen(s)); }

static void newline(JsonWriter *w)
{
    put(w, "\n", 1);
    for (int i = 0; i <= w->depth; i++) put(w, "  ", 2);
}

/* Before any value: a comma and a newline inside a container, and a key inside an object. */
static void before_value(JsonWriter *w)
{
    if (w->failed) return;
    if (w->depth < 0) { if (w->len) w->failed = true; return; }      /* one top-level value */
    if (w->object[w->depth]) {
        if (!w->key_pending) { w->failed = true; return; }
        w->key_pending = false;
        return;
    }
    if (w->members[w->depth]++) put(w, ",", 1);
    newline(w);
}

static void open(JsonWriter *w, bool object)
{
    before_value(w);
    if (w->failed) return;
    if (w->depth + 1 >= JSON_WRITER_MAX_DEPTH) { w->failed = true; return; }
    put(w, object ? "{" : "[", 1);
    w->depth++;
    w->object[w->depth] = object;
    w->members[w->depth] = 0;
}

static void close_(JsonWriter *w, bool object)
{
    if (w->failed) return;
    if (w->depth < 0 || w->object[w->depth] != object || w->key_pending) { w->failed = true; return; }
    size_t members = w->members[w->depth];
    w->depth--;
    if (members) newline(w);
    put(w, object ? "}" : "]", 1);
}

void json_write_begin_object(JsonWriter *w) { open(w, true); }
void json_write_end_object(JsonWriter *w) { close_(w, true); }
void json_write_begin_array(JsonWriter *w) { open(w, false); }
void json_write_end_array(JsonWriter *w) { close_(w, false); }

static void quoted(JsonWriter *w, const char *s)
{
    put(w, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        char esc[8];
        switch (*p) {
        case '"': puts_(w, "\\\""); break;
        case '\\': puts_(w, "\\\\"); break;
        case '\n': puts_(w, "\\n"); break;
        case '\r': puts_(w, "\\r"); break;
        case '\t': puts_(w, "\\t"); break;
        default:
            if (*p < 0x20) { snprintf(esc, sizeof esc, "\\u%04x", *p); puts_(w, esc); }
            else put(w, (const char *)p, 1);
        }
    }
    put(w, "\"", 1);
}
void json_write_key(JsonWriter *w, const char *key)
{
    if (w->failed) return;
    if (w->depth < 0 || !w->object[w->depth] || w->key_pending || !key) { w->failed = true; return; }
    if (w->members[w->depth]++) put(w, ",", 1);
    newline(w);
    quoted(w, key);
    put(w, ": ", 2);
    w->key_pending = true;
}

void json_write_string(JsonWriter *w, const char *s)
{
    if (!s) { json_write_null(w); return; }
    before_value(w);
    if (!w->failed) quoted(w, s);
}

void json_write_int(JsonWriter *w, int64_t v)
{
    char b[32];
    before_value(w);
    snprintf(b, sizeof b, "%lld", (long long)v);
    puts_(w, b);
}

void json_write_uint(JsonWriter *w, uint64_t v)
{
    char b[32];
    before_value(w);
    snprintf(b, sizeof b, "%llu", (unsigned long long)v);
    puts_(w, b);
}

void json_write_bool(JsonWriter *w, bool v) { before_value(w); puts_(w, v ? "true" : "false"); }
void json_write_null(JsonWriter *w) { before_value(w); puts_(w, "null"); }

const char *json_writer_finish(JsonWriter *w, size_t *len)
{
    if (w->failed || w->depth != -1 || !w->len || w->key_pending) { if (len) *len = 0; return NULL; }
    put(w, "\n", 1);
    if (w->failed) return NULL;
    if (len) *len = w->len;
    return w->buf;
}

/* schema_gbnf.c - JSON Schema (subset) to a GBNF grammar (llama.cpp syntax). See llm.h. */
#include "llm.h"

#include "json.h"

#include <stdio.h>
#include <string.h>

typedef struct Out { Arena *a; char *buf; size_t len, cap; bool oom; const char *error; } Out;

static void put(Out *o, const char *s)
{
    size_t n = strlen(s);
    if (o->oom) return;
    if (o->len + n + 1 > o->cap) {
        size_t cap = o->cap ? o->cap * 2 : 2048;
        while (cap < o->len + n + 1) cap *= 2;
        char *b = arena_alloc(o->a, cap);
        if (!b) { o->oom = true; return; }
        if (o->len) memcpy(b, o->buf, o->len);
        o->buf = b;
        o->cap = cap;
    }
    memcpy(o->buf + o->len, s, n + 1);
    o->len += n;
}

/* A GBNF string literal for the JSON text "<s>" (quotes included): "\"" s "\"" with \ and " escaped. */
static void literal_json_string(Out *o, const char *s)
{
    put(o, "\"\\\"");
    for (const char *p = s; *p; p++) {
        char c[2] = {*p, 0};
        if (*p == '"') put(o, "\\\\\\\"");          /* JSON \" inside a GBNF literal */
        else if (*p == '\\') put(o, "\\\\\\\\");    /* JSON \\ inside a GBNF literal */
        else put(o, c);
    }
    put(o, "\\\"\"");
}

static void rule_name(char *dst, size_t cap, const char *parent, const char *key)
{
    size_t n = (size_t)snprintf(dst, cap, "%s-", parent);
    for (const char *p = key; *p && n + 1 < cap; p++) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        dst[n++] = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ? c : '-';
    }
    dst[n] = 0;
}

static bool emit_rule(Out *o, const char *name, const JsonValue *schema);

/* The expression for a schema value, emitting a separate rule for objects and arrays. */
static bool value_expr(Out *o, const char *name, const JsonValue *s, char *expr, size_t cap)
{
    if (!s || s->kind != JSON_OBJECT) { o->error = "a schema must be an object"; return false; }
    const JsonValue *en = json_get(s, "enum"), *type = json_get(s, "type");
    if (en) {
        if (en->kind != JSON_ARRAY || !en->count) { o->error = "enum must be a non-empty array"; return false; }
        Out tmp = {o->a, NULL, 0, 0, false, NULL};
        put(&tmp, "(");
        for (size_t i = 0; i < en->count; i++) {
            if (en->items[i]->kind != JSON_STRING) { o->error = "only string enums are supported"; return false; }
            if (i) put(&tmp, " | ");
            literal_json_string(&tmp, en->items[i]->text);
        }
        put(&tmp, ")");
        if (tmp.oom || tmp.len + 1 > cap) { o->error = "enum too long"; return false; }
        memcpy(expr, tmp.buf, tmp.len + 1);
        return true;
    }
    if (!type || type->kind != JSON_STRING) { o->error = "every schema needs a \"type\" (or an enum)"; return false; }
    const char *t = type->text;
    if (!strcmp(t, "string") || !strcmp(t, "integer") || !strcmp(t, "number") || !strcmp(t, "boolean") || !strcmp(t, "null")) {
        snprintf(expr, cap, "%s", t);
        return true;
    }
    if (!strcmp(t, "object") || !strcmp(t, "array")) {
        if (!strcmp(t, "object") && !json_get(s, "properties")) { snprintf(expr, cap, "object"); return true; }   /* any object */
        if (!emit_rule(o, name, s)) return false;
        snprintf(expr, cap, "%s", name);
        return true;
    }
    o->error = "unsupported type (supported: object, array, string, integer, number, boolean, null)";
    return false;
}

static bool emit_rule(Out *o, const char *name, const JsonValue *s)
{
    const char *t = json_get(s, "type")->text;
    char line[8192];
    if (!strcmp(t, "array")) {
        char item[4096], child[256];
        rule_name(child, sizeof child, name, "item");
        if (!value_expr(o, child, json_get(s, "items"), item, sizeof item)) { if (!o->error) o->error = "array items"; return false; }
        snprintf(line, sizeof line, "%s ::= \"[\" ws ( %s ( ws \",\" ws %s )* )? ws \"]\"\n", name, item, item);
        put(o, line);
        return true;
    }
    const JsonValue *props = json_get(s, "properties"), *req = json_get(s, "required");
    if (!props || props->kind != JSON_OBJECT || !props->count) { o->error = "an object schema needs properties"; return false; }
    for (size_t i = 0; i < props->count; i++) {
        bool required = false;
        for (size_t k = 0; req && req->kind == JSON_ARRAY && k < req->count; k++)
            required |= req->items[k]->kind == JSON_STRING && !strcmp(req->items[k]->text, props->keys[i]);
        if (!required) { o->error = "optional properties are not supported: list every property in \"required\""; return false; }
    }
    Out body = {o->a, NULL, 0, 0, false, NULL};
    put(&body, name);
    put(&body, " ::= \"{\" ws ");
    for (size_t i = 0; i < props->count; i++) {
        char expr[4096], child[256];
        rule_name(child, sizeof child, name, props->keys[i]);
        if (!value_expr(o, child, props->items[i], expr, sizeof expr)) return false;
        if (i) put(&body, " ws \",\" ws ");
        literal_json_string(&body, props->keys[i]);
        put(&body, " ws \":\" ws ");
        put(&body, expr);
    }
    put(&body, " ws \"}\"\n");
    if (body.oom) { o->oom = true; return false; }
    put(o, body.buf);
    return true;
}

LlmStatus llm_schema_to_gbnf(Arena *a, const char *schema, size_t len, const char **gbnf, size_t *glen, const char **error)
{
    JsonValue *v;
    JsonError e;
    Out o = {a, NULL, 0, 0, false, NULL};
    if (error) *error = NULL;
    if (!a || !schema || !gbnf) return LLM_INVALID_ARGUMENT;
    if (json_parse(a, schema, len, &v, &e) != JSON_OK) { if (error) *error = "the schema is not JSON"; return LLM_INVALID_ARGUMENT; }
    char root[4096];
    if (!value_expr(&o, "root", v, root, sizeof root)) { if (error) *error = o.error ? o.error : "unsupported schema"; return o.oom ? LLM_OUT_OF_MEMORY : LLM_INVALID_ARGUMENT; }
    if (strcmp(root, "root")) {          /* a primitive or enum at the top: root names it */
        char line[4200];
        snprintf(line, sizeof line, "root ::= %s\n", root);
        put(&o, line);
    }
    /* Shared rules (llama.cpp's json grammar), always present so the output does not depend on what was used. */
    put(&o, "ws ::= [ \\t\\n]*\n");
    put(&o, "string ::= \"\\\"\" ( [^\"\\\\\\x7F\\x00-\\x1F] | \"\\\\\" ( [\"\\\\/bfnrt] | \"u\" [0-9a-fA-F] [0-9a-fA-F] [0-9a-fA-F] [0-9a-fA-F] ) )* \"\\\"\"\n");
    put(&o, "integer ::= \"-\"? ( \"0\" | [1-9] [0-9]* )\n");
    put(&o, "number ::= integer ( \".\" [0-9]+ )? ( [eE] [-+]? [0-9]+ )?\n");
    put(&o, "boolean ::= \"true\" | \"false\"\n");
    put(&o, "null ::= \"null\"\n");
    put(&o, "value ::= object | array | string | number | boolean | null\n");
    put(&o, "object ::= \"{\" ws ( string ws \":\" ws value ( ws \",\" ws string ws \":\" ws value )* )? ws \"}\"\n");
    put(&o, "array ::= \"[\" ws ( value ( ws \",\" ws value )* )? ws \"]\"\n");
    if (o.oom) return LLM_OUT_OF_MEMORY;
    *gbnf = o.buf;
    if (glen) *glen = o.len;
    return LLM_OK;
}

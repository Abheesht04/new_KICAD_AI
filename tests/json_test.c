/* The strict JSON reader (src/v2_src/json.c). */
#include "json.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static JsonStatus parse(Arena *a, const char *t, JsonValue **v, JsonError *e) { return json_parse(a, t, strlen(t), v, e); }

int main(void)
{
    Arena a;
    JsonValue *v;
    JsonError e;
    int64_t nm;
    arena_init(&a, 1 << 16);

    CHECK(parse(&a, " {\"a\": [1, -2.5, true, false, null, \"x\"], \"b\": {}} ", &v, &e) == JSON_OK);
    CHECK(v->kind == JSON_OBJECT && v->count == 2 && !strcmp(v->keys[0], "a") && !strcmp(v->keys[1], "b"));
    const JsonValue *arr = json_get(v, "a");
    CHECK(arr && arr->kind == JSON_ARRAY && arr->count == 6);
    CHECK(arr->items[0]->kind == JSON_NUMBER && !strcmp(arr->items[1]->text, "-2.5"));
    CHECK(arr->items[2]->kind == JSON_TRUE && arr->items[3]->kind == JSON_FALSE && arr->items[4]->kind == JSON_NULL);
    CHECK(arr->items[5]->kind == JSON_STRING && !strcmp(arr->items[5]->text, "x"));
    CHECK(json_get(v, "b")->kind == JSON_OBJECT && json_get(v, "b")->count == 0 && !json_get(v, "c"));

    /* Escapes, \u including a surrogate pair, decoded to UTF-8. */
    CHECK(parse(&a, "\"q\\\" b\\\\ s\\/ \\n \\u00e9 \\u20ac \\ud83d\\ude00\"", &v, &e) == JSON_OK);
    CHECK(!strcmp(v->text, "q\" b\\ s/ \n \xc3\xa9 \xe2\x82\xac \xf0\x9f\x98\x80"));

    /* Numbers to nm, exactly. */
    CHECK(parse(&a, "[0.1, 0.000001, 670, -1.25, 0.0000001, 1e-3, 1000000000.5]", &v, &e) == JSON_OK);
    CHECK(json_to_nm(v->items[0], &nm) == JSON_NUMBER_OK && nm == 100000);
    CHECK(json_to_nm(v->items[1], &nm) == JSON_NUMBER_OK && nm == 1);
    CHECK(json_to_nm(v->items[2], &nm) == JSON_NUMBER_OK && nm == 670000000);
    CHECK(json_to_nm(v->items[3], &nm) == JSON_NUMBER_OK && nm == -1250000);
    CHECK(json_to_nm(v->items[4], &nm) == JSON_NUMBER_TOO_PRECISE);
    CHECK(json_to_nm(v->items[5], &nm) == JSON_NUMBER_EXPONENT);
    CHECK(json_to_nm(v->items[6], &nm) == JSON_NUMBER_RANGE);
    int64_t i;
    CHECK(parse(&a, "[9223372036854775807, 9223372036854775808, 3.0]", &v, &e) == JSON_OK);
    CHECK(json_to_i64(v->items[0], &i) == JSON_NUMBER_OK && i == INT64_MAX);
    CHECK(json_to_i64(v->items[1], &i) == JSON_NUMBER_RANGE && json_to_i64(v->items[2], &i) != JSON_NUMBER_OK);

    /* Errors, located. */
    CHECK(parse(&a, "{\n  \"a\": 1,\n  \"a\": 2\n}", &v, &e) == JSON_DUPLICATE_KEY && e.line == 3 && e.column == 3);
    CHECK(parse(&a, "{\"a\": 1,}", &v, &e) == JSON_PARSE_ERROR);
    CHECK(parse(&a, "[1, 2", &v, &e) == JSON_PARSE_ERROR);
    CHECK(parse(&a, "[01]", &v, &e) == JSON_PARSE_ERROR);
    CHECK(parse(&a, "[1.]", &v, &e) == JSON_PARSE_ERROR);
    CHECK(parse(&a, "\"tab\there\"", &v, &e) == JSON_PARSE_ERROR);
    CHECK(parse(&a, "\"\\ud83d alone\"", &v, &e) == JSON_PARSE_ERROR);
    CHECK(parse(&a, "\"\\u0000\"", &v, &e) == JSON_PARSE_ERROR);
    CHECK(parse(&a, "{} {}", &v, &e) == JSON_PARSE_ERROR);
    CHECK(parse(&a, "\n\n   tru", &v, &e) == JSON_PARSE_ERROR && e.line == 3 && e.column == 4);
    char deep[200];
    memset(deep, '[', 100); deep[100] = 0;
    CHECK(parse(&a, deep, &v, &e) == JSON_TOO_DEEP);
    CHECK(json_parse_file(&a, "no/such/file.json", &v, &e) == JSON_IO_ERROR);
    CHECK(parse(&a, "\xef\xbb\xbf{\"bom\": 1}", &v, &e) == JSON_OK);

    arena_destroy(&a);
    if (failures) { fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
    puts("json: all checks passed");
    return 0;
}

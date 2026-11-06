#ifndef COPPER_V2_JSON_H
#define COPPER_V2_JSON_H

/*
 * json.h - a strict RFC 8259 JSON reader for data files (fab profiles, tool-server requests), Copper V2.
 *
 * The whole document is parsed into an Arena: objects keep their keys in file order (duplicate keys are an error,
 * so a profile can never carry two different values for one limit), strings are decoded to UTF-8 (\uXXXX and
 * surrogate pairs included; unpaired surrogates and NUL are refused), numbers keep their exact source text.
 * json_to_nm reads a decimal length in mm exactly into integer nanometres; no floating point anywhere.
 * Errors carry line, column and byte offset. No printf, no exit.
 */

#include "arena.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum JsonStatus {
    JSON_OK = 0,
    JSON_INVALID_ARGUMENT,
    JSON_IO_ERROR,
    JSON_PARSE_ERROR,
    JSON_DUPLICATE_KEY,
    JSON_TOO_DEEP,          /* nesting beyond JSON_MAX_DEPTH */
    JSON_OUT_OF_MEMORY
} JsonStatus;

#define JSON_MAX_DEPTH 64

const char *json_status_name(JsonStatus status);

typedef enum JsonKind { JSON_NULL = 0, JSON_FALSE, JSON_TRUE, JSON_NUMBER, JSON_STRING, JSON_ARRAY, JSON_OBJECT } JsonKind;

typedef struct JsonValue JsonValue;
struct JsonValue {
    JsonKind kind;
    const char *text;        /* STRING: decoded UTF-8; NUMBER: the source text */
    size_t text_len;
    size_t count;            /* ARRAY / OBJECT: number of members */
    const char **keys;       /* OBJECT: member names, in file order */
    JsonValue **items;       /* ARRAY / OBJECT: members, in file order */
    size_t line, column;     /* where the value starts (1-based) */
};

typedef struct JsonError {
    JsonStatus code;
    size_t line, column, offset;
    const char *message;     /* static string */
} JsonError;

JsonStatus json_parse(Arena *arena, const char *text, size_t len, JsonValue **out, JsonError *error);
JsonStatus json_parse_file(Arena *arena, const char *path, JsonValue **out, JsonError *error);

/* The member named `key` of an object, or NULL. */
const JsonValue *json_get(const JsonValue *object, const char *key);

typedef enum JsonNumberStatus {
    JSON_NUMBER_OK = 0,
    JSON_NUMBER_NOT_A_NUMBER,
    JSON_NUMBER_EXPONENT,    /* 1e-3: refused, write plain decimals */
    JSON_NUMBER_TOO_PRECISE, /* more than 6 decimals for a length in mm */
    JSON_NUMBER_RANGE
} JsonNumberStatus;

/* A decimal length in mm -> exact nm (at most 6 decimals, |value| <= 1e9 mm). */
JsonNumberStatus json_to_nm(const JsonValue *value, int64_t *out_nm);
/* An integer within int64. */
JsonNumberStatus json_to_i64(const JsonValue *value, int64_t *out);

#ifdef __cplusplus
}
#endif

#endif

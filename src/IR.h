#ifndef HW_IR_H
#define HW_IR_H

/*
 * IR.h - general-purpose S-expression tree for KiCad-oriented tooling.
 *
 * This is intentionally separate from the frozen legacy sexpr.c/sexpr.h API.
 * It preserves lexical atoms and decoded string contents; higher-level
 * modules decide which atoms represent numbers, UUIDs, nets, pins, etc.
 *
 * Memory contract:
 * - IR nodes and node text are allocated from a caller-owned Arena.
 * - Detaching/removing a node only unlinks it; arena memory is not freed.
 * - A node may have at most one parent, and siblings are linked only by the
 *   owning list. Use ir_clone() to insert a copy of an existing subtree.
 */

#include "arena.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum IRNodeKind {
    IR_NODE_ATOM = 0,
    IR_NODE_STRING,
    IR_NODE_LIST
} IRNodeKind;

typedef enum IRStatus {
    IR_STATUS_OK = 0,
    IR_STATUS_INVALID_ARGUMENT,
    IR_STATUS_OUT_OF_MEMORY,
    IR_STATUS_PARSE_ERROR,
    IR_STATUS_INVALID_NODE,
    IR_STATUS_OWNERSHIP_ERROR,
    IR_STATUS_CYCLE,
    IR_STATUS_INVALID_NUMBER,
    IR_STATUS_RANGE_ERROR,
    IR_STATUS_IO_ERROR
} IRStatus;

typedef struct IRNode IRNode;

struct IRNode {
    IRNodeKind kind;

    /* ATOM/STRING only. UTF-8 bytes, NUL-terminated, length excludes NUL. */
    const char *text;
    size_t text_len;

    /* LIST only: doubly-informed ownership + forward sibling chain. */
    IRNode *parent;
    IRNode *first_child;
    IRNode *last_child;
    IRNode *next;
};

typedef struct IRError {
    IRStatus code;
    size_t offset;       /* byte offset into source; 0 for non-parse errors */
    size_t line;         /* 1-based, meaningful for parse errors */
    size_t column;       /* 1-based byte column, meaningful for parse errors */
    const char *message; /* static string; never needs to be freed */
} IRError;

typedef struct IRWriteOptions {
    bool pretty;
    unsigned indent_width; /* tabs are not emitted; 2 is a sensible default */
    bool final_newline;
} IRWriteOptions;

/* Status/error utilities. Passing NULL for error is permitted. */
const char *ir_status_string(IRStatus status);
void ir_error_clear(IRError *error);

/* Node constructors. Text constructors accept explicit lengths for safe input. */
IRNode *ir_node_atom_n(Arena *arena, const char *text, size_t text_len);
IRNode *ir_node_atom(Arena *arena, const char *text);
IRNode *ir_node_string_n(Arena *arena, const char *text, size_t text_len);
IRNode *ir_node_string(Arena *arena, const char *text);
IRNode *ir_node_list(Arena *arena);
IRNode *ir_node_list_named(Arena *arena, const char *head_atom);
IRNode *ir_node_atom_i64(Arena *arena, int64_t value);
IRNode *ir_node_atom_u64(Arena *arena, uint64_t value);
IRNode *ir_node_atom_double(Arena *arena, double value);
IRNode *ir_node_atom_bool(Arena *arena, bool value); /* yes/no, matching KiCad */
/* Integer nanometres written as decimal mm text ("0.25", "-3.81", "1"),
 * trailing zeros trimmed, never any rounding noise. */
IRNode *ir_node_atom_nm(Arena *arena, int64_t nm);

/* Ownership-aware tree editing. child must be detached before insertion. */
IRStatus ir_list_append(IRNode *list, IRNode *child);
IRStatus ir_list_insert_before(IRNode *list, IRNode *before, IRNode *child);
IRStatus ir_node_detach(IRNode *node);
IRNode *ir_node_clone(Arena *arena, const IRNode *source, IRError *error);

/* Tree inspection. Find functions only inspect immediate list children. */
const IRNode *ir_child_at(const IRNode *list, size_t index);
const IRNode *ir_list_find(const IRNode *list, const char *head_atom);
const IRNode *ir_list_find_named(
    const IRNode *list, const char *head_atom, const char *second_value);
const IRNode *ir_list_head(const IRNode *list);
size_t ir_list_length(const IRNode *list);

/* Strict conversions: the source node remains unchanged. */
IRStatus ir_atom_to_i64(const IRNode *node, int64_t *out_value);
IRStatus ir_atom_to_u64(const IRNode *node, uint64_t *out_value);
IRStatus ir_atom_to_double(const IRNode *node, double *out_value);
IRStatus ir_atom_to_bool(const IRNode *node, bool *out_value);
/* Decimal mm text -> exact integer nm using integer arithmetic only. At most
 * 6 decimals are exact; a 7th digit rounds half away from zero. No exponent. */
IRStatus ir_atom_to_nm(const IRNode *node, int64_t *out_nm);

/* Parsing. Exactly one expression must consume the entire non-comment input. */
IRNode *ir_parse(
    Arena *arena, const char *source, size_t source_len, IRError *error);
IRNode *ir_parse_file(Arena *arena, const char *path, IRError *error);

/* Validate parent/child/sibling invariants before serialization or traversal. */
IRStatus ir_validate_tree(const IRNode *root, IRError *error);

/* Serialization. FILE output is streamed; ir_to_string() returns malloc memory. */
IRWriteOptions ir_write_options_default(void);
IRStatus ir_write(
    FILE *stream, const IRNode *root,
    const IRWriteOptions *options, IRError *error);
IRStatus ir_to_string(
    const IRNode *root, const IRWriteOptions *options,
    char **out_text, size_t *out_len, IRError *error);
void ir_string_free(char *text);

#ifdef __cplusplus
}
#endif

#endif /* HW_IR_H */

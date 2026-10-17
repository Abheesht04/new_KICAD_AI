#include "IR.h"
#include "nmath.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>

#define IR_MAX_PARSE_DEPTH 512u
#define IR_MAX_TREE_DEPTH  2048u

typedef struct IRParser {
    Arena *arena;
    const char *source;
    size_t length;
    size_t pos;
    size_t depth;
    IRError *error;
} IRParser;

typedef struct IRBuffer {
    char *data;
    size_t length;
    size_t capacity;
} IRBuffer;

typedef struct IRSink {
    FILE *stream;
    IRBuffer *buffer;
    bool failed;
} IRSink;

static void ir_set_error(IRError *error, IRStatus code, const char *message)
{
    if (!error) return;
    error->code = code;
    error->offset = 0;
    error->line = 0;
    error->column = 0;
    error->message = message;
}

static void ir_parser_set_error(IRParser *parser, IRStatus code,
                                size_t offset, const char *message)
{
    if (!parser || !parser->error) return;

    IRError *error = parser->error;
    error->code = code;
    error->offset = offset;
    error->line = 1;
    error->column = 1;
    error->message = message;

    const size_t stop = offset < parser->length ? offset : parser->length;
    for (size_t i = 0; i < stop; ++i) {
        if (parser->source[i] == '\n') {
            error->line++;
            error->column = 1;
        } else {
            error->column++;
        }
    }
}

const char *ir_status_string(IRStatus status)
{
    switch (status) {
        case IR_STATUS_OK: return "ok";
        case IR_STATUS_INVALID_ARGUMENT: return "invalid argument";
        case IR_STATUS_OUT_OF_MEMORY: return "out of memory";
        case IR_STATUS_PARSE_ERROR: return "parse error";
        case IR_STATUS_INVALID_NODE: return "invalid node";
        case IR_STATUS_OWNERSHIP_ERROR: return "node ownership error";
        case IR_STATUS_CYCLE: return "cycle detected";
        case IR_STATUS_INVALID_NUMBER: return "invalid number";
        case IR_STATUS_RANGE_ERROR: return "numeric range error";
        case IR_STATUS_IO_ERROR: return "I/O error";
        default: return "unknown IR status";
    }
}

void ir_error_clear(IRError *error)
{
    ir_set_error(error, IR_STATUS_OK, NULL);
}

static bool ir_is_ascii_space(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
           c == '\v' || c == '\f';
}

static bool ir_is_atom_delimiter(unsigned char c)
{
    return c == '(' || c == ')' || c == '"' || c == ';' || c == '#' ||
           ir_is_ascii_space(c);
}

static bool ir_text_has_nul(const char *text, size_t len)
{
    return text && memchr(text, '\0', len) != NULL;
}

static IRNode *ir_node_alloc(Arena *arena, IRNodeKind kind)
{
    if (!arena) return NULL;
    IRNode *node = (IRNode *)arena_alloc(arena, sizeof(*node));
    if (!node) return NULL;
    memset(node, 0, sizeof(*node));
    node->kind = kind;
    return node;
}

static IRNode *ir_node_text_n(Arena *arena, IRNodeKind kind,
                              const char *text, size_t text_len)
{
    if (!arena || !text || (kind != IR_NODE_ATOM && kind != IR_NODE_STRING)) {
        return NULL;
    }
    if (ir_text_has_nul(text, text_len)) return NULL;

    if (kind == IR_NODE_ATOM) {
        if (text_len == 0) return NULL;
        for (size_t i = 0; i < text_len; ++i) {
            if (ir_is_atom_delimiter((unsigned char)text[i])) return NULL;
        }
    }

    if (text_len == SIZE_MAX) return NULL;
    IRNode *node = ir_node_alloc(arena, kind);
    if (!node) return NULL;

    char *copy = (char *)arena_alloc(arena, text_len + 1);
    if (!copy) return NULL;
    if (text_len) memcpy(copy, text, text_len);
    copy[text_len] = '\0';
    node->text = copy;
    node->text_len = text_len;
    return node;
}

IRNode *ir_node_atom_n(Arena *arena, const char *text, size_t text_len)
{
    return ir_node_text_n(arena, IR_NODE_ATOM, text, text_len);
}

IRNode *ir_node_atom(Arena *arena, const char *text)
{
    return text ? ir_node_atom_n(arena, text, strlen(text)) : NULL;
}

IRNode *ir_node_string_n(Arena *arena, const char *text, size_t text_len)
{
    return ir_node_text_n(arena, IR_NODE_STRING, text, text_len);
}

IRNode *ir_node_string(Arena *arena, const char *text)
{
    return text ? ir_node_string_n(arena, text, strlen(text)) : NULL;
}

IRNode *ir_node_list(Arena *arena)
{
    return ir_node_alloc(arena, IR_NODE_LIST);
}

IRStatus ir_list_append(IRNode *list, IRNode *child)
{
    if (!list || !child) return IR_STATUS_INVALID_ARGUMENT;
    if (list->kind != IR_NODE_LIST) return IR_STATUS_INVALID_NODE;
    if (child->parent || child->next) return IR_STATUS_OWNERSHIP_ERROR;

    /* A node cannot be appended beneath itself or any of its descendants. */
    for (IRNode *ancestor = list; ancestor; ancestor = ancestor->parent) {
        if (ancestor == child) return IR_STATUS_CYCLE;
    }

    if (list->last_child) {
        if (list->last_child->next) return IR_STATUS_INVALID_NODE;
        list->last_child->next = child;
    } else {
        if (list->first_child) return IR_STATUS_INVALID_NODE;
        list->first_child = child;
    }
    list->last_child = child;
    child->parent = list;
    return IR_STATUS_OK;
}

IRStatus ir_list_insert_before(IRNode *list, IRNode *before, IRNode *child)
{
    if (!list || !before || !child) return IR_STATUS_INVALID_ARGUMENT;
    if (list->kind != IR_NODE_LIST) return IR_STATUS_INVALID_NODE;
    if (before->parent != list) return IR_STATUS_OWNERSHIP_ERROR;
    if (child->parent || child->next) return IR_STATUS_OWNERSHIP_ERROR;

    for (IRNode *ancestor = list; ancestor; ancestor = ancestor->parent) {
        if (ancestor == child) return IR_STATUS_CYCLE;
    }

    if (list->first_child == before) {
        child->next = before;
        list->first_child = child;
        child->parent = list;
        return IR_STATUS_OK;
    }

    IRNode *previous = list->first_child;
    while (previous && previous->next != before) previous = previous->next;
    if (!previous) return IR_STATUS_INVALID_NODE;

    previous->next = child;
    child->next = before;
    child->parent = list;
    return IR_STATUS_OK;
}

IRStatus ir_node_detach(IRNode *node)
{
    if (!node) return IR_STATUS_INVALID_ARGUMENT;
    if (!node->parent) {
        return node->next ? IR_STATUS_OWNERSHIP_ERROR : IR_STATUS_OK;
    }

    IRNode *parent = node->parent;
    if (parent->kind != IR_NODE_LIST) return IR_STATUS_INVALID_NODE;

    IRNode *previous = NULL;
    IRNode *current = parent->first_child;
    while (current && current != node) {
        previous = current;
        current = current->next;
    }
    if (!current) return IR_STATUS_OWNERSHIP_ERROR;

    if (previous) previous->next = node->next;
    else parent->first_child = node->next;

    if (parent->last_child == node) parent->last_child = previous;
    node->parent = NULL;
    node->next = NULL;

    if (!parent->first_child) parent->last_child = NULL;
    return IR_STATUS_OK;
}

IRNode *ir_node_list_named(Arena *arena, const char *head_atom)
{
    if (!head_atom) return NULL;
    IRNode *list = ir_node_list(arena);
    IRNode *head = ir_node_atom(arena, head_atom);
    if (!list || !head || ir_list_append(list, head) != IR_STATUS_OK) return NULL;
    return list;
}

static bool ir_format_i64(int64_t value, char *buffer, size_t size)
{
    int written = snprintf(buffer, size, "%" PRId64, value);
    return written >= 0 && (size_t)written < size;
}

static bool ir_format_u64(uint64_t value, char *buffer, size_t size)
{
    int written = snprintf(buffer, size, "%" PRIu64, value);
    return written >= 0 && (size_t)written < size;
}

/* Convert locale-dependent snprintf decimal separators to a dot. */
static bool ir_localize_to_dot(const char *input, char *output, size_t output_size)
{
    const struct lconv *lc = localeconv();
    const char *decimal = (lc && lc->decimal_point && lc->decimal_point[0])
        ? lc->decimal_point : ".";
    size_t decimal_len = strlen(decimal);
    size_t out = 0;

    for (size_t i = 0; input[i] != '\0';) {
        if (decimal_len && strncmp(input + i, decimal, decimal_len) == 0) {
            if (out + 1 >= output_size) return false;
            output[out++] = '.';
            i += decimal_len;
        } else {
            if (out + 1 >= output_size) return false;
            output[out++] = input[i++];
        }
    }
    output[out] = '\0';
    return true;
}

static bool ir_expand_exponent(const char *input, char *output, size_t output_size)
{
    const char *e = strchr(input, 'e');
    if (!e) e = strchr(input, 'E');

    char digits[64];
    size_t digit_count = 0;
    size_t digits_before_dot = 0;
    bool saw_dot = false;
    bool negative = input[0] == '-';
    const char *mantissa_begin = input + ((input[0] == '-' || input[0] == '+') ? 1 : 0);
    const char *mantissa_end = e ? e : input + strlen(input);

    for (const char *p = mantissa_begin; p < mantissa_end; ++p) {
        if (*p == '.') {
            saw_dot = true;
            continue;
        }
        if (*p < '0' || *p > '9' || digit_count >= sizeof(digits)) return false;
        digits[digit_count++] = *p;
        if (!saw_dot) digits_before_dot++;
    }
    if (digit_count == 0) return false;
    digits[digit_count] = '\0';

    int exponent = 0;
    if (e) {
        char *end = NULL;
        long parsed = strtol(e + 1, &end, 10);
        if (!end || *end != '\0' || parsed < -1000 || parsed > 1000) return false;
        exponent = (int)parsed;
    }

    long decimal_pos_long = (long)digits_before_dot + exponent;
    if (decimal_pos_long < -1000 || decimal_pos_long > 1000) return false;
    size_t out = 0;
#define IR_EMIT(ch) do { if (out + 1 >= output_size) return false; output[out++] = (ch); } while (0)
    if (negative) IR_EMIT('-');

    if (decimal_pos_long <= 0) {
        IR_EMIT('0');
        IR_EMIT('.');
        for (long i = 0; i < -decimal_pos_long; ++i) IR_EMIT('0');
        for (size_t i = 0; i < digit_count; ++i) IR_EMIT(digits[i]);
    } else if ((size_t)decimal_pos_long >= digit_count) {
        for (size_t i = 0; i < digit_count; ++i) IR_EMIT(digits[i]);
        for (long i = (long)digit_count; i < decimal_pos_long; ++i) IR_EMIT('0');
        IR_EMIT('.');
        IR_EMIT('0');
    } else {
        for (long i = 0; i < decimal_pos_long; ++i) IR_EMIT(digits[i]);
        IR_EMIT('.');
        for (size_t i = (size_t)decimal_pos_long; i < digit_count; ++i) IR_EMIT(digits[i]);
    }

    /* Each branch above emits a decimal point and at least one fractional
       digit. Do not trim zeros here: zeros before the decimal point are
       significant (e.g. 1000.0 must not become 1.0). */
    output[out] = '\0';
#undef IR_EMIT
    return true;
}

static bool ir_format_double_plain(double value, char *output, size_t output_size)
{
    if (!nm_isfinite(value) || !output || output_size == 0) return false;
    if (value == 0.0) {
        if (output_size < 4) return false;
        memcpy(output, "0.0", 4);
        return true;
    }

    char locale_text[128];
    char dot_text[128];
    int written = snprintf(locale_text, sizeof(locale_text), "%.17g", value);
    if (written < 0 || (size_t)written >= sizeof(locale_text)) return false;
    if (!ir_localize_to_dot(locale_text, dot_text, sizeof(dot_text))) return false;
    return ir_expand_exponent(dot_text, output, output_size);
}

IRNode *ir_node_atom_i64(Arena *arena, int64_t value)
{
    char buffer[64];
    return ir_format_i64(value, buffer, sizeof(buffer))
        ? ir_node_atom(arena, buffer) : NULL;
}

IRNode *ir_node_atom_u64(Arena *arena, uint64_t value)
{
    char buffer[64];
    return ir_format_u64(value, buffer, sizeof(buffer))
        ? ir_node_atom(arena, buffer) : NULL;
}

IRNode *ir_node_atom_double(Arena *arena, double value)
{
    char buffer[768];
    return ir_format_double_plain(value, buffer, sizeof(buffer))
        ? ir_node_atom(arena, buffer) : NULL;
}

IRNode *ir_node_atom_bool(Arena *arena, bool value)
{
    return ir_node_atom(arena, value ? "yes" : "no");
}

static IRNode *ir_clone_impl(Arena *arena, const IRNode *source,
                             size_t depth, IRError *error)
{
    if (!source || depth > IR_MAX_TREE_DEPTH) {
        ir_set_error(error, source ? IR_STATUS_CYCLE : IR_STATUS_INVALID_ARGUMENT,
                     source ? "tree nesting limit exceeded while cloning" : "null source node");
        return NULL;
    }

    IRNode *copy = NULL;
    if (source->kind == IR_NODE_ATOM) {
        copy = ir_node_atom_n(arena, source->text, source->text_len);
    } else if (source->kind == IR_NODE_STRING) {
        copy = ir_node_string_n(arena, source->text, source->text_len);
    } else if (source->kind == IR_NODE_LIST) {
        copy = ir_node_list(arena);
        for (const IRNode *child = source->first_child; child; child = child->next) {
            IRNode *child_copy = ir_clone_impl(arena, child, depth + 1, error);
            if (!child_copy || ir_list_append(copy, child_copy) != IR_STATUS_OK) {
                ir_set_error(error, IR_STATUS_OUT_OF_MEMORY, "failed to clone child node");
                return NULL;
            }
        }
    }

    if (!copy) {
        ir_set_error(error, IR_STATUS_OUT_OF_MEMORY, "failed to allocate cloned node");
        return NULL;
    }
    return copy;
}

IRNode *ir_node_clone(Arena *arena, const IRNode *source, IRError *error)
{
    ir_error_clear(error);
    if (!arena || !source) {
        ir_set_error(error, IR_STATUS_INVALID_ARGUMENT, "arena and source are required");
        return NULL;
    }
    return ir_clone_impl(arena, source, 0, error);
}

const IRNode *ir_child_at(const IRNode *list, size_t index)
{
    if (!list || list->kind != IR_NODE_LIST) return NULL;
    const IRNode *child = list->first_child;
    while (child && index > 0) {
        child = child->next;
        --index;
    }
    return child;
}

const IRNode *ir_list_head(const IRNode *list)
{
    if (!list || list->kind != IR_NODE_LIST) return NULL;
    return list->first_child;
}

size_t ir_list_length(const IRNode *list)
{
    if (!list || list->kind != IR_NODE_LIST) return 0;
    const IRNode *slow = list->first_child;
    const IRNode *fast = list->first_child;
    while (fast && fast->next) {
        slow = slow->next;
        fast = fast->next->next;
        if (slow == fast) return SIZE_MAX; /* malformed cyclic list */
    }
    size_t count = 0;
    for (const IRNode *child = list->first_child; child; child = child->next) {
        if (count == SIZE_MAX) return SIZE_MAX;
        ++count;
    }
    return count;
}

const IRNode *ir_list_find(const IRNode *list, const char *head_atom)
{
    if (!list || list->kind != IR_NODE_LIST || !head_atom) return NULL;
    for (const IRNode *child = list->first_child; child; child = child->next) {
        if (child->kind == IR_NODE_LIST && child->first_child &&
            child->first_child->kind == IR_NODE_ATOM &&
            strcmp(child->first_child->text, head_atom) == 0) return child;
    }
    return NULL;
}

const IRNode *ir_list_find_named(const IRNode *list, const char *head_atom,
                                 const char *second_value)
{
    if (!list || list->kind != IR_NODE_LIST || !head_atom || !second_value) return NULL;
    for (const IRNode *child = list->first_child; child; child = child->next) {
        if (child->kind != IR_NODE_LIST || !child->first_child) continue;
        const IRNode *head = child->first_child;
        const IRNode *second = head->next;
        if (head->kind == IR_NODE_ATOM && strcmp(head->text, head_atom) == 0 &&
            second && (second->kind == IR_NODE_ATOM || second->kind == IR_NODE_STRING) &&
            strcmp(second->text, second_value) == 0) return child;
    }
    return NULL;
}

static void ir_skip_space_comments(IRParser *parser)
{
    for (;;) {
        while (parser->pos < parser->length &&
               ir_is_ascii_space((unsigned char)parser->source[parser->pos])) {
            parser->pos++;
        }
        if (parser->pos >= parser->length) return;

        /* KiCad's S-expression lexer supports line comments. Accept '#' and ';'
           as comment introducers so inventory tools can also read annotated files. */
        const char c = parser->source[parser->pos];
        if (c != '#' && c != ';') return;
        while (parser->pos < parser->length && parser->source[parser->pos] != '\n') {
            parser->pos++;
        }
    }
}

static int ir_hex_value(unsigned char c)
{
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'a' && c <= 'f') return (int)(c - 'a') + 10;
    if (c >= 'A' && c <= 'F') return (int)(c - 'A') + 10;
    return -1;
}

static IRNode *ir_parse_node(IRParser *parser);

static IRNode *ir_parse_string_node(IRParser *parser)
{
    const size_t start = parser->pos;
    parser->pos++; /* opening quote */
    size_t max_len = parser->length - parser->pos;
    char *decoded = (char *)arena_alloc(parser->arena, max_len + 1);
    if (!decoded) {
        ir_parser_set_error(parser, IR_STATUS_OUT_OF_MEMORY, start, "out of memory decoding string");
        return NULL;
    }

    size_t out = 0;
    bool closed = false;
    while (parser->pos < parser->length) {
        unsigned char c = (unsigned char)parser->source[parser->pos++];
        if (c == '"') {
            closed = true;
            break;
        }
        if (c == '\0') {
            ir_parser_set_error(parser, IR_STATUS_PARSE_ERROR, parser->pos - 1,
                                "NUL byte is not valid in IR text");
            return NULL;
        }
        if (c != '\\') {
            decoded[out++] = (char)c;
            continue;
        }

        if (parser->pos >= parser->length) {
            ir_parser_set_error(parser, IR_STATUS_PARSE_ERROR, parser->pos - 1,
                                "trailing backslash in string");
            return NULL;
        }

        unsigned char escaped = (unsigned char)parser->source[parser->pos++];
        switch (escaped) {
            case '"': decoded[out++] = '"'; break;
            case '\\': decoded[out++] = '\\'; break;
            case 'n': decoded[out++] = '\n'; break;
            case 'r': decoded[out++] = '\r'; break;
            case 't': decoded[out++] = '\t'; break;
            case 'a': decoded[out++] = '\a'; break;
            case 'b': decoded[out++] = '\b'; break;
            case 'f': decoded[out++] = '\f'; break;
            case 'v': decoded[out++] = '\v'; break;
            case 'x': {
                if (parser->length - parser->pos < 2) {
                    ir_parser_set_error(parser, IR_STATUS_PARSE_ERROR, parser->pos - 2,
                                        "hex escape requires exactly two hex digits");
                    return NULL;
                }
                int hi = ir_hex_value((unsigned char)parser->source[parser->pos]);
                int lo = ir_hex_value((unsigned char)parser->source[parser->pos + 1]);
                if (hi < 0 || lo < 0) {
                    ir_parser_set_error(parser, IR_STATUS_PARSE_ERROR, parser->pos,
                                        "invalid hex escape");
                    return NULL;
                }
                unsigned char value = (unsigned char)((hi << 4) | lo);
                parser->pos += 2;
                if (value == 0) {
                    ir_parser_set_error(parser, IR_STATUS_PARSE_ERROR, parser->pos - 4,
                                        "NUL byte is not valid in IR text");
                    return NULL;
                }
                decoded[out++] = (char)value;
                break;
            }
            default:
                if (escaped >= '0' && escaped <= '7') {
                    unsigned value = (unsigned)(escaped - '0');
                    unsigned digits = 1;
                    while (digits < 3 && parser->pos < parser->length) {
                        unsigned char next = (unsigned char)parser->source[parser->pos];
                        if (next < '0' || next > '7') break;
                        value = value * 8u + (unsigned)(next - '0');
                        parser->pos++;
                        ++digits;
                    }
                    if (value > 255u) {
                        ir_parser_set_error(parser, IR_STATUS_PARSE_ERROR, parser->pos,
                                            "octal escape exceeds one byte");
                        return NULL;
                    }
                    if (value == 0) {
                        ir_parser_set_error(parser, IR_STATUS_PARSE_ERROR, parser->pos,
                                            "NUL byte is not valid in IR text");
                        return NULL;
                    }
                    decoded[out++] = (char)value;
                } else {
                    /* KiCad's lexer permits a backslash to quote the next byte. */
                    decoded[out++] = (char)escaped;
                }
                break;
        }
    }

    if (!closed) {
        ir_parser_set_error(parser, IR_STATUS_PARSE_ERROR, start, "unterminated string");
        return NULL;
    }

    decoded[out] = '\0';
    IRNode *node = ir_node_alloc(parser->arena, IR_NODE_STRING);
    if (!node) {
        ir_parser_set_error(parser, IR_STATUS_OUT_OF_MEMORY, start, "out of memory allocating string node");
        return NULL;
    }
    node->text = decoded;
    node->text_len = out;
    return node;
}

static IRNode *ir_parse_atom_node(IRParser *parser)
{
    size_t start = parser->pos;
    while (parser->pos < parser->length &&
           !ir_is_atom_delimiter((unsigned char)parser->source[parser->pos])) {
        if (parser->source[parser->pos] == '\0') {
            ir_parser_set_error(parser, IR_STATUS_PARSE_ERROR, parser->pos,
                                "NUL byte is not valid in an atom");
            return NULL;
        }
        parser->pos++;
    }

    if (parser->pos == start) {
        ir_parser_set_error(parser, IR_STATUS_PARSE_ERROR, start,
                            "unexpected character where atom was expected");
        return NULL;
    }

    IRNode *node = ir_node_atom_n(parser->arena, parser->source + start,
                                  parser->pos - start);
    if (!node) {
        ir_parser_set_error(parser, IR_STATUS_OUT_OF_MEMORY, start,
                            "out of memory allocating atom node");
    }
    return node;
}

static IRNode *ir_parse_list_node(IRParser *parser)
{
    const size_t start = parser->pos++;
    if (parser->depth >= IR_MAX_PARSE_DEPTH) {
        ir_parser_set_error(parser, IR_STATUS_PARSE_ERROR, start,
                            "maximum S-expression nesting depth exceeded");
        return NULL;
    }

    IRNode *list = ir_node_list(parser->arena);
    if (!list) {
        ir_parser_set_error(parser, IR_STATUS_OUT_OF_MEMORY, start,
                            "out of memory allocating list node");
        return NULL;
    }

    parser->depth++;
    for (;;) {
        ir_skip_space_comments(parser);
        if (parser->pos >= parser->length) {
            parser->depth--;
            ir_parser_set_error(parser, IR_STATUS_PARSE_ERROR, parser->pos,
                                "unterminated list: expected ')'");
            return NULL;
        }

        char c = parser->source[parser->pos];
        if (c == ')') {
            parser->pos++;
            parser->depth--;
            return list;
        }

        IRNode *child = ir_parse_node(parser);
        if (!child) {
            parser->depth--;
            return NULL;
        }
        IRStatus status = ir_list_append(list, child);
        if (status != IR_STATUS_OK) {
            parser->depth--;
            ir_parser_set_error(parser, status, parser->pos,
                                "failed to append parsed child");
            return NULL;
        }
    }
}

static IRNode *ir_parse_node(IRParser *parser)
{
    ir_skip_space_comments(parser);
    if (parser->pos >= parser->length) {
        ir_parser_set_error(parser, IR_STATUS_PARSE_ERROR, parser->pos,
                            "unexpected end of input");
        return NULL;
    }

    switch (parser->source[parser->pos]) {
        case '(': return ir_parse_list_node(parser);
        case '"': return ir_parse_string_node(parser);
        case ')':
            ir_parser_set_error(parser, IR_STATUS_PARSE_ERROR, parser->pos,
                                "unexpected ')' without matching '('");
            return NULL;
        default: return ir_parse_atom_node(parser);
    }
}

IRNode *ir_parse(Arena *arena, const char *source, size_t source_len, IRError *error)
{
    ir_error_clear(error);
    if (!arena || (!source && source_len != 0)) {
        ir_set_error(error, IR_STATUS_INVALID_ARGUMENT, "arena and source are required");
        return NULL;
    }

    IRParser parser = {
        .arena = arena,
        .source = source ? source : "",
        .length = source_len,
        .pos = 0,
        .depth = 0,
        .error = error
    };

    ir_skip_space_comments(&parser);
    if (parser.pos >= parser.length) {
        ir_parser_set_error(&parser, IR_STATUS_PARSE_ERROR, parser.pos, "empty input");
        return NULL;
    }

    IRNode *root = ir_parse_node(&parser);
    if (!root) return NULL;

    ir_skip_space_comments(&parser);
    if (parser.pos != parser.length) {
        ir_parser_set_error(&parser, IR_STATUS_PARSE_ERROR, parser.pos,
                            "trailing content after the first expression");
        return NULL;
    }

    ir_error_clear(error);
    return root;
}

IRNode *ir_parse_file(Arena *arena, const char *path, IRError *error)
{
    ir_error_clear(error);
    if (!arena || !path) {
        ir_set_error(error, IR_STATUS_INVALID_ARGUMENT, "arena and path are required");
        return NULL;
    }

    FILE *file = fopen(path, "rb");
    if (!file) {
        ir_set_error(error, IR_STATUS_IO_ERROR, "could not open input file");
        return NULL;
    }

    size_t length = 0;
    size_t capacity = 8192;
    char *data = (char *)malloc(capacity);
    if (!data) {
        fclose(file);
        ir_set_error(error, IR_STATUS_OUT_OF_MEMORY, "out of memory reading input file");
        return NULL;
    }

    for (;;) {
        if (length == capacity) {
            if (capacity > SIZE_MAX / 2) {
                free(data);
                fclose(file);
                ir_set_error(error, IR_STATUS_RANGE_ERROR, "input file is too large");
                return NULL;
            }
            size_t next_capacity = capacity * 2;
            char *grown = (char *)realloc(data, next_capacity);
            if (!grown) {
                free(data);
                fclose(file);
                ir_set_error(error, IR_STATUS_OUT_OF_MEMORY, "out of memory growing input buffer");
                return NULL;
            }
            data = grown;
            capacity = next_capacity;
        }

        size_t got = fread(data + length, 1, capacity - length, file);
        length += got;
        if (got == 0) {
            if (ferror(file)) {
                free(data);
                fclose(file);
                ir_set_error(error, IR_STATUS_IO_ERROR, "failed while reading input file");
                return NULL;
            }
            break;
        }
    }

    if (fclose(file) != 0) {
        free(data);
        ir_set_error(error, IR_STATUS_IO_ERROR, "failed closing input file");
        return NULL;
    }

    IRNode *root = ir_parse(arena, data, length, error);
    free(data);
    return root;
}

static IRStatus ir_validate_node(const IRNode *node, const IRNode *expected_parent,
                                 size_t depth, IRError *error)
{
    if (!node || depth > IR_MAX_TREE_DEPTH) {
        ir_set_error(error, depth > IR_MAX_TREE_DEPTH ? IR_STATUS_CYCLE : IR_STATUS_INVALID_NODE,
                     "null node or maximum tree depth exceeded");
        return depth > IR_MAX_TREE_DEPTH ? IR_STATUS_CYCLE : IR_STATUS_INVALID_NODE;
    }
    if (node->parent != expected_parent) {
        ir_set_error(error, IR_STATUS_OWNERSHIP_ERROR, "parent pointer does not match owning list");
        return IR_STATUS_OWNERSHIP_ERROR;
    }

    if (node->kind == IR_NODE_ATOM || node->kind == IR_NODE_STRING) {
        if (!node->text || node->text_len == SIZE_MAX ||
            node->text[node->text_len] != '\0' ||
            ir_text_has_nul(node->text, node->text_len)) {
            ir_set_error(error, IR_STATUS_INVALID_NODE, "invalid text node storage");
            return IR_STATUS_INVALID_NODE;
        }
        if (node->kind == IR_NODE_ATOM) {
            if (node->text_len == 0) {
                ir_set_error(error, IR_STATUS_INVALID_NODE, "atom cannot be empty");
                return IR_STATUS_INVALID_NODE;
            }
            for (size_t i = 0; i < node->text_len; ++i) {
                if (ir_is_atom_delimiter((unsigned char)node->text[i])) {
                    ir_set_error(error, IR_STATUS_INVALID_NODE, "atom contains a delimiter");
                    return IR_STATUS_INVALID_NODE;
                }
            }
        }
        if (node->first_child || node->last_child) {
            ir_set_error(error, IR_STATUS_INVALID_NODE, "text node cannot have children");
            return IR_STATUS_INVALID_NODE;
        }
        return IR_STATUS_OK;
    }

    if (node->kind != IR_NODE_LIST || node->text || node->text_len != 0) {
        ir_set_error(error, IR_STATUS_INVALID_NODE, "invalid node kind or list text storage");
        return IR_STATUS_INVALID_NODE;
    }

    const IRNode *slow = node->first_child;
    const IRNode *fast = node->first_child;
    while (fast && fast->next) {
        slow = slow->next;
        fast = fast->next->next;
        if (slow == fast) {
            ir_set_error(error, IR_STATUS_CYCLE, "cycle in sibling chain");
            return IR_STATUS_CYCLE;
        }
    }

    const IRNode *last = NULL;
    for (const IRNode *child = node->first_child; child; child = child->next) {
        IRStatus status = ir_validate_node(child, node, depth + 1, error);
        if (status != IR_STATUS_OK) return status;
        last = child;
    }
    if (last != node->last_child || (!!node->first_child != !!node->last_child)) {
        ir_set_error(error, IR_STATUS_INVALID_NODE, "list first/last child pointers are inconsistent");
        return IR_STATUS_INVALID_NODE;
    }
    return IR_STATUS_OK;
}

IRStatus ir_validate_tree(const IRNode *root, IRError *error)
{
    ir_error_clear(error);
    if (!root) {
        ir_set_error(error, IR_STATUS_INVALID_ARGUMENT, "root node is required");
        return IR_STATUS_INVALID_ARGUMENT;
    }
    if (root->next) {
        ir_set_error(error, IR_STATUS_OWNERSHIP_ERROR, "root node must not have a sibling");
        return IR_STATUS_OWNERSHIP_ERROR;
    }
    return ir_validate_node(root, NULL, 0, error);
}

static bool ir_integer_syntax(const char *text, bool allow_negative)
{
    if (!text || !*text) return false;
    const unsigned char *p = (const unsigned char *)text;
    if (*p == '+' || *p == '-') {
        if (*p == '-' && !allow_negative) return false;
        ++p;
    }
    if (!*p) return false;
    for (; *p; ++p) if (*p < '0' || *p > '9') return false;
    return true;
}

IRStatus ir_atom_to_i64(const IRNode *node, int64_t *out_value)
{
    if (!node || !out_value || node->kind != IR_NODE_ATOM || !node->text)
        return IR_STATUS_INVALID_ARGUMENT;
    if (!ir_integer_syntax(node->text, true)) return IR_STATUS_INVALID_NUMBER;

    errno = 0;
    char *end = NULL;
    intmax_t value = strtoimax(node->text, &end, 10);
    if (errno == ERANGE || value < INT64_MIN || value > INT64_MAX)
        return IR_STATUS_RANGE_ERROR;
    if (!end || *end != '\0') return IR_STATUS_INVALID_NUMBER;
    *out_value = (int64_t)value;
    return IR_STATUS_OK;
}

IRStatus ir_atom_to_u64(const IRNode *node, uint64_t *out_value)
{
    if (!node || !out_value || node->kind != IR_NODE_ATOM || !node->text)
        return IR_STATUS_INVALID_ARGUMENT;
    if (!ir_integer_syntax(node->text, false)) return IR_STATUS_INVALID_NUMBER;

    errno = 0;
    char *end = NULL;
    uintmax_t value = strtoumax(node->text, &end, 10);
    if (errno == ERANGE || value > UINT64_MAX) return IR_STATUS_RANGE_ERROR;
    if (!end || *end != '\0') return IR_STATUS_INVALID_NUMBER;
    *out_value = (uint64_t)value;
    return IR_STATUS_OK;
}

static bool ir_double_syntax(const char *text)
{
    if (!text || !*text) return false;
    const unsigned char *p = (const unsigned char *)text;
    if (*p == '+' || *p == '-') ++p;

    bool digits_before = false;
    bool digits_after = false;
    while (*p >= '0' && *p <= '9') { digits_before = true; ++p; }
    if (*p == '.') {
        ++p;
        while (*p >= '0' && *p <= '9') { digits_after = true; ++p; }
    }
    if (!digits_before && !digits_after) return false;

    if (*p == 'e' || *p == 'E') {
        ++p;
        if (*p == '+' || *p == '-') ++p;
        bool exp_digits = false;
        while (*p >= '0' && *p <= '9') { exp_digits = true; ++p; }
        if (!exp_digits) return false;
    }
    return *p == '\0';
}

IRStatus ir_atom_to_double(const IRNode *node, double *out_value)
{
    if (!node || !out_value || node->kind != IR_NODE_ATOM || !node->text)
        return IR_STATUS_INVALID_ARGUMENT;
    if (!ir_double_syntax(node->text)) return IR_STATUS_INVALID_NUMBER;

    const struct lconv *lc = localeconv();
    const char *decimal = (lc && lc->decimal_point && lc->decimal_point[0])
        ? lc->decimal_point : ".";
    size_t decimal_len = strlen(decimal);
    size_t text_len = node->text_len;
    size_t dots = 0;
    for (size_t i = 0; i < text_len; ++i) if (node->text[i] == '.') dots++;
    if (dots && decimal_len > SIZE_MAX / dots) return IR_STATUS_RANGE_ERROR;
    size_t extra = dots * (decimal_len - 1u);
    if (extra == SIZE_MAX || text_len > SIZE_MAX - extra - 1u)
        return IR_STATUS_RANGE_ERROR;
    char *locale_text = (char *)malloc(text_len + extra + 1u);
    if (!locale_text) return IR_STATUS_OUT_OF_MEMORY;

    size_t out = 0;
    for (size_t i = 0; i < text_len; ++i) {
        if (node->text[i] == '.') {
            memcpy(locale_text + out, decimal, decimal_len);
            out += decimal_len;
        } else {
            locale_text[out++] = node->text[i];
        }
    }
    locale_text[out] = '\0';

    errno = 0;
    char *end = NULL;
    double value = strtod(locale_text, &end);
    bool valid = end && *end == '\0';
    bool range_error = errno == ERANGE || !nm_isfinite(value);
    free(locale_text);

    if (!valid) return IR_STATUS_INVALID_NUMBER;
    if (range_error) return IR_STATUS_RANGE_ERROR;
    *out_value = value;
    return IR_STATUS_OK;
}

/*
 * Exact decimal-millimetre <-> integer-nanometre conversion.
 *
 * KiCad files store lengths as decimal mm with at most 6 decimals, which is
 * exactly nanometre resolution, so the text can be turned into an integer
 * without ever touching a double: no rounding noise, no locale, no libm.
 * More than 6 decimals are rounded to nearest (half away from zero) using the
 * 7th digit. Exponent notation is rejected (KiCad never writes it).
 */
IRStatus ir_atom_to_nm(const IRNode *node, int64_t *out_nm)
{
    if (!node || !out_nm || node->kind != IR_NODE_ATOM || !node->text)
        return IR_STATUS_INVALID_ARGUMENT;

    const unsigned char *p = (const unsigned char *)node->text;
    bool negative = false;
    if (*p == '+' || *p == '-') { negative = (*p == '-'); ++p; }

    uint64_t whole = 0;
    bool any_digit = false;
    const uint64_t whole_limit = (uint64_t)INT64_MAX / 1000000u;
    while (*p >= '0' && *p <= '9') {
        any_digit = true;
        whole = whole * 10u + (uint64_t)(*p - '0');
        if (whole > whole_limit) return IR_STATUS_RANGE_ERROR;
        ++p;
    }

    uint64_t frac = 0;
    unsigned frac_digits = 0;
    bool round_up = false;
    if (*p == '.') {
        ++p;
        while (*p >= '0' && *p <= '9') {
            any_digit = true;
            if (frac_digits < 6u) {
                frac = frac * 10u + (uint64_t)(*p - '0');
                ++frac_digits;
            } else if (frac_digits == 6u) {
                round_up = (*p >= '5');
                ++frac_digits;
            }
            ++p;
        }
    }
    if (!any_digit || *p != '\0') return IR_STATUS_INVALID_NUMBER;

    while (frac_digits < 6u) { frac *= 10u; ++frac_digits; }
    uint64_t magnitude = whole * 1000000u + frac + (round_up ? 1u : 0u);
    if (magnitude > (uint64_t)INT64_MAX) return IR_STATUS_RANGE_ERROR;

    *out_nm = negative ? -(int64_t)magnitude : (int64_t)magnitude;
    return IR_STATUS_OK;
}

IRNode *ir_node_atom_nm(Arena *arena, int64_t nm)
{
    /* Magnitude computed unsigned so INT64_MIN is safe. */
    const uint64_t m = (nm < 0) ? (uint64_t)0 - (uint64_t)nm : (uint64_t)nm;
    const uint64_t whole = m / 1000000u;
    uint64_t frac = m % 1000000u;

    char buffer[48];
    size_t n = 0;
    if (nm < 0) buffer[n++] = '-';

    char digits[24];
    size_t d = 0;
    uint64_t w = whole;
    do { digits[d++] = (char)('0' + (w % 10u)); w /= 10u; } while (w != 0);
    while (d > 0) buffer[n++] = digits[--d];

    if (frac != 0) {
        buffer[n++] = '.';
        char f[6];
        for (int i = 5; i >= 0; --i) { f[i] = (char)('0' + (frac % 10u)); frac /= 10u; }
        size_t len = 6;
        while (len > 0 && f[len - 1] == '0') --len; /* trim trailing zeros */
        for (size_t i = 0; i < len; ++i) buffer[n++] = f[i];
    }
    buffer[n] = '\0';
    return ir_node_atom_n(arena, buffer, n);
}

IRStatus ir_atom_to_bool(const IRNode *node, bool *out_value)
{
    if (!node || !out_value || node->kind != IR_NODE_ATOM || !node->text)
        return IR_STATUS_INVALID_ARGUMENT;
    if (strcmp(node->text, "yes") == 0 || strcmp(node->text, "true") == 0 ||
        strcmp(node->text, "1") == 0) {
        *out_value = true;
        return IR_STATUS_OK;
    }
    if (strcmp(node->text, "no") == 0 || strcmp(node->text, "false") == 0 ||
        strcmp(node->text, "0") == 0) {
        *out_value = false;
        return IR_STATUS_OK;
    }
    return IR_STATUS_INVALID_NUMBER;
}

IRWriteOptions ir_write_options_default(void)
{
    return (IRWriteOptions){ .pretty = true, .indent_width = 2, .final_newline = true };
}

static bool ir_buffer_reserve(IRBuffer *buffer, size_t extra)
{
    if (extra > SIZE_MAX - buffer->length - 1u) return false;
    size_t needed = buffer->length + extra + 1u;
    if (needed <= buffer->capacity) return true;

    size_t next = buffer->capacity ? buffer->capacity : 256u;
    while (next < needed) {
        if (next > SIZE_MAX / 2u) {
            next = needed;
            break;
        }
        next *= 2u;
    }
    char *grown = (char *)realloc(buffer->data, next);
    if (!grown) return false;
    buffer->data = grown;
    buffer->capacity = next;
    return true;
}

static bool ir_sink_bytes(IRSink *sink, const char *bytes, size_t length)
{
    if (sink->failed) return false;
    if (sink->stream) {
        if (length && fwrite(bytes, 1, length, sink->stream) != length) {
            sink->failed = true;
            return false;
        }
        return true;
    }
    if (!sink->buffer || !ir_buffer_reserve(sink->buffer, length)) {
        sink->failed = true;
        return false;
    }
    memcpy(sink->buffer->data + sink->buffer->length, bytes, length);
    sink->buffer->length += length;
    sink->buffer->data[sink->buffer->length] = '\0';
    return true;
}

static bool ir_sink_char(IRSink *sink, char c)
{
    return ir_sink_bytes(sink, &c, 1);
}

static bool ir_sink_indent(IRSink *sink, unsigned depth, unsigned indent_width)
{
    if (indent_width == 0 || depth == 0) return true;
    if (depth > SIZE_MAX / indent_width) return false;
    size_t count = (size_t)depth * (size_t)indent_width;
    static const char spaces[32] = "                                ";
    while (count) {
        size_t chunk = count < sizeof(spaces) - 1u ? count : sizeof(spaces) - 1u;
        if (!ir_sink_bytes(sink, spaces, chunk)) return false;
        count -= chunk;
    }
    return true;
}

static bool ir_write_string(IRSink *sink, const IRNode *node)
{
    if (!ir_sink_char(sink, '"')) return false;
    for (size_t i = 0; i < node->text_len; ++i) {
        unsigned char c = (unsigned char)node->text[i];
        switch (c) {
            case '"': if (!ir_sink_bytes(sink, "\\\"", 2)) return false; break;
            case '\\': if (!ir_sink_bytes(sink, "\\\\", 2)) return false; break;
            case '\n': if (!ir_sink_bytes(sink, "\\n", 2)) return false; break;
            case '\r': if (!ir_sink_bytes(sink, "\\r", 2)) return false; break;
            case '\t': if (!ir_sink_bytes(sink, "\\011", 4)) return false; break;
            default:
                if (c < 0x20u || c == 0x7fu) {
                    char octal[4] = { '\\',
                        (char)('0' + ((c >> 6) & 7u)),
                        (char)('0' + ((c >> 3) & 7u)),
                        (char)('0' + (c & 7u)) };
                    if (!ir_sink_bytes(sink, octal, sizeof(octal))) return false;
                } else {
                    if (!ir_sink_char(sink, (char)c)) return false;
                }
                break;
        }
    }
    return ir_sink_char(sink, '"');
}

static bool ir_write_node(IRSink *sink, const IRNode *node,
                          unsigned depth, const IRWriteOptions *options)
{
    if (node->kind == IR_NODE_ATOM) {
        return ir_sink_bytes(sink, node->text, node->text_len);
    }
    if (node->kind == IR_NODE_STRING) return ir_write_string(sink, node);
    if (node->kind != IR_NODE_LIST) return false;

    if (!ir_sink_char(sink, '(')) return false;
    bool first = true;
    for (const IRNode *child = node->first_child; child; child = child->next) {
        if (!first) {
            if (options->pretty && child->kind == IR_NODE_LIST) {
                if (!ir_sink_char(sink, '\n') ||
                    !ir_sink_indent(sink, depth + 1u, options->indent_width)) return false;
            } else if (!ir_sink_char(sink, ' ')) {
                return false;
            }
        }
        if (!ir_write_node(sink, child, depth + 1u, options)) return false;
        first = false;
    }
    return ir_sink_char(sink, ')');
}

static IRStatus ir_serialize_to_sink(IRSink *sink, const IRNode *root,
                                     const IRWriteOptions *options, IRError *error)
{
    IRStatus status = ir_validate_tree(root, error);
    if (status != IR_STATUS_OK) return status;
    if (!ir_write_node(sink, root, 0, options)) {
        ir_set_error(error, sink->buffer ? IR_STATUS_OUT_OF_MEMORY : IR_STATUS_IO_ERROR,
                     sink->buffer ? "could not grow serialization buffer" : "failed writing S-expression");
        return sink->buffer ? IR_STATUS_OUT_OF_MEMORY : IR_STATUS_IO_ERROR;
    }
    if (options->final_newline && !ir_sink_char(sink, '\n')) {
        ir_set_error(error, sink->buffer ? IR_STATUS_OUT_OF_MEMORY : IR_STATUS_IO_ERROR,
                     "failed writing final newline");
        return sink->buffer ? IR_STATUS_OUT_OF_MEMORY : IR_STATUS_IO_ERROR;
    }
    ir_error_clear(error);
    return IR_STATUS_OK;
}

IRStatus ir_write(FILE *stream, const IRNode *root,
                  const IRWriteOptions *options, IRError *error)
{
    ir_error_clear(error);
    if (!stream || !root) {
        ir_set_error(error, IR_STATUS_INVALID_ARGUMENT, "stream and root are required");
        return IR_STATUS_INVALID_ARGUMENT;
    }
    IRWriteOptions selected = options ? *options : ir_write_options_default();
    if (selected.indent_width > 32u) {
        ir_set_error(error, IR_STATUS_INVALID_ARGUMENT, "indent width must be <= 32");
        return IR_STATUS_INVALID_ARGUMENT;
    }
    IRSink sink = { .stream = stream, .buffer = NULL, .failed = false };
    return ir_serialize_to_sink(&sink, root, &selected, error);
}

IRStatus ir_to_string(const IRNode *root, const IRWriteOptions *options,
                      char **out_text, size_t *out_len, IRError *error)
{
    ir_error_clear(error);
    if (!root || !out_text) {
        ir_set_error(error, IR_STATUS_INVALID_ARGUMENT, "root and output-text pointer are required");
        return IR_STATUS_INVALID_ARGUMENT;
    }
    *out_text = NULL;
    if (out_len) *out_len = 0;

    IRWriteOptions selected = options ? *options : ir_write_options_default();
    if (selected.indent_width > 32u) {
        ir_set_error(error, IR_STATUS_INVALID_ARGUMENT, "indent width must be <= 32");
        return IR_STATUS_INVALID_ARGUMENT;
    }

    IRBuffer buffer = {0};
    IRSink sink = { .stream = NULL, .buffer = &buffer, .failed = false };
    IRStatus status = ir_serialize_to_sink(&sink, root, &selected, error);
    if (status != IR_STATUS_OK) {
        free(buffer.data);
        return status;
    }
    if (!buffer.data) {
        buffer.data = (char *)malloc(1);
        if (!buffer.data) {
            ir_set_error(error, IR_STATUS_OUT_OF_MEMORY, "could not allocate empty output string");
            return IR_STATUS_OUT_OF_MEMORY;
        }
        buffer.data[0] = '\0';
    }
    *out_text = buffer.data;
    if (out_len) *out_len = buffer.length;
    return IR_STATUS_OK;
}

void ir_string_free(char *text)
{
    free(text);
}

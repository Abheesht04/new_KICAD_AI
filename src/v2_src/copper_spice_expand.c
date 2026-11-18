#include "copper_spice_expand.h"

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CSPX_TOKEN_MAX 128u
#define CSPX_PARAM_MAX 256u
#define CSPX_SUBCKT_MAX 128u
#define CSPX_DEFAULT_INCLUDE_DEPTH 16u
#define CSPX_DEFAULT_SUBCKT_DEPTH 16u
#define CSPX_HARD_INCLUDE_DEPTH 31u
#define CSPX_HARD_SUBCKT_DEPTH 64u

typedef struct StrBuf {
    Arena *arena;
    char *data;
    size_t size;
    size_t capacity;
} StrBuf;

typedef struct Param {
    char *name;
    double value;
} Param;

typedef struct ParamEnv {
    Param items[CSPX_PARAM_MAX];
    size_t count;
} ParamEnv;

typedef struct SourceLine {
    char *text;
    char *file;
    size_t line;
} SourceLine;

typedef struct LineVec {
    Arena *arena;
    SourceLine *items;
    size_t count;
    size_t capacity;
} LineVec;

typedef struct Subckt {
    char *name;
    char **nodes;
    size_t node_count;
    char **params;
    char **defaults;
    size_t param_count;
    char **body;
    size_t *body_lines;
    size_t body_count;
    size_t definition_line;
} Subckt;

typedef struct SubcktTable {
    Subckt items[CSPX_SUBCKT_MAX];
    size_t count;
} SubcktTable;

typedef struct NodeMap {
    char *name;
    char *value;
} NodeMap;

typedef struct Scope {
    NodeMap *items;
    size_t count;
} Scope;

typedef struct ExpandCtx {
    Arena *scratch;
    const CspExpandOptions *options;
    const char *include_stack[CSPX_HARD_INCLUDE_DEPTH + 1u];
    unsigned max_include_depth;
    unsigned max_subcircuit_depth;
    size_t error_line;
} ExpandCtx;

typedef struct Expr {
    const char *cursor;
    const ParamEnv *parameters;
    int valid;
} Expr;

static int size_multiply(size_t a, size_t b, size_t *out)
{
    if (out == NULL || (b != 0u && a > SIZE_MAX / b))
        return 0;
    *out = a * b;
    return 1;
}

static void *scratch_alloc(Arena *arena, size_t size)
{
    return arena_alloc(arena, size == 0u ? 1u : size);
}

static void *scratch_array(Arena *arena, size_t element_size, size_t count)
{
    return arena_alloc_array(arena, element_size, count == 0u ? 1u : count);
}

static char *scratch_copy_n(Arena *arena, const char *text, size_t length)
{
    char *copy;

    if (text == NULL || length == SIZE_MAX)
        return NULL;
    copy = (char *)scratch_alloc(arena, length + 1u);
    if (copy == NULL)
        return NULL;
    memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

static char *scratch_copy(Arena *arena, const char *text)
{
    return text == NULL ? NULL : scratch_copy_n(arena, text, strlen(text));
}

static void *scratch_grow(Arena *arena, const void *old_items,
                          size_t old_count, size_t new_count,
                          size_t element_size)
{
    size_t bytes;
    void *new_items;

    if (new_count < old_count ||
        !size_multiply(new_count, element_size, &bytes))
        return NULL;
    new_items = scratch_alloc(arena, bytes);
    if (new_items == NULL)
        return NULL;
    if (old_items != NULL && old_count != 0u)
        memcpy(new_items, old_items, old_count * element_size);
    return new_items;
}

static int equals_ci(const char *left, const char *right)
{
    if (left == NULL || right == NULL)
        return 0;

    while (*left != '\0' && *right != '\0') {
        if (toupper((unsigned char)*left) !=
            toupper((unsigned char)*right))
            return 0;
        ++left;
        ++right;
    }
    return *left == '\0' && *right == '\0';
}

static void skip_space(const char **cursor)
{
    while (**cursor != '\0' && isspace((unsigned char)**cursor))
        ++(*cursor);
}

static int buffer_reserve(StrBuf *buffer, size_t additional)
{
    size_t needed;
    size_t capacity;
    char *replacement;

    if (additional > SIZE_MAX - buffer->size - 1u)
        return 0;
    needed = buffer->size + additional + 1u;
    if (needed <= buffer->capacity)
        return 1;

    capacity = buffer->capacity == 0u ? 1024u : buffer->capacity;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2u) {
            capacity = needed;
            break;
        }
        capacity *= 2u;
    }

    replacement = (char *)scratch_alloc(buffer->arena, capacity);
    if (replacement == NULL)
        return 0;
    if (buffer->data != NULL && buffer->size != 0u)
        memcpy(replacement, buffer->data, buffer->size);
    replacement[buffer->size] = '\0';
    buffer->data = replacement;
    buffer->capacity = capacity;
    return 1;
}

static int buffer_append_n(StrBuf *buffer, const char *text, size_t length)
{
    if (text == NULL || !buffer_reserve(buffer, length))
        return 0;
    memcpy(buffer->data + buffer->size, text, length);
    buffer->size += length;
    buffer->data[buffer->size] = '\0';
    return 1;
}

static int buffer_append(StrBuf *buffer, const char *text)
{
    return text != NULL &&
           buffer_append_n(buffer, text, strlen(text));
}

static int line_vector_append(LineVec *lines, const char *text,
                              const char *file, size_t line_number)
{
    SourceLine *replacement;
    SourceLine *entry;
    size_t new_capacity;

    if (lines->count == lines->capacity) {
        new_capacity = lines->capacity == 0u ? 64u : lines->capacity * 2u;
        if (new_capacity < lines->capacity)
            return 0;
        replacement = (SourceLine *)scratch_grow(
            lines->arena, lines->items, lines->count, new_capacity,
            sizeof(*replacement));
        if (replacement == NULL)
            return 0;
        lines->items = replacement;
        lines->capacity = new_capacity;
    }

    entry = &lines->items[lines->count];
    entry->text = scratch_copy(lines->arena, text);
    entry->file = scratch_copy(lines->arena, file == NULL ? "" : file);
    if (entry->text == NULL || entry->file == NULL)
        return 0;
    entry->line = line_number;
    ++lines->count;
    return 1;
}

/* SPICE scale suffixes; unit labels after a suffix are ignored. */
static int parse_scale_suffix(const char **end, double *multiplier)
{
    const char *cursor = *end;
    double scale = 1.0;

    if (*cursor != '\0' && isalpha((unsigned char)*cursor)) {
        char suffix = (char)tolower((unsigned char)*cursor++);

        switch (suffix) {
        case 't': scale = 1e12; break;
        case 'g': scale = 1e9; break;
        case 'k': scale = 1e3; break;
        case 'm':
            if (tolower((unsigned char)cursor[0]) == 'e' &&
                cursor[1] != '\0' &&
                tolower((unsigned char)cursor[1]) == 'g') {
                scale = 1e6;
                cursor += 2;
            } else if (tolower((unsigned char)cursor[0]) == 'i' &&
                       cursor[1] != '\0' &&
                       tolower((unsigned char)cursor[1]) == 'l') {
                /* One mil is one thousandth of an inch. */
                scale = 25.4e-6;
                cursor += 2;
            } else {
                scale = 1e-3;
            }
            break;
        case 'u': scale = 1e-6; break;
        case 'n': scale = 1e-9; break;
        case 'p': scale = 1e-12; break;
        case 'f': scale = 1e-15; break;
        default:
            /* SPICE ignores alphabetic unit labels such as V and Ohm. */
            scale = 1.0;
            --cursor;
            break;
        }

        while (*cursor != '\0' && isalpha((unsigned char)*cursor))
            ++cursor;
    }

    *end = cursor;
    *multiplier = scale;
    return 1;
}

static int parse_spice_number(const char *text, double *out)
{
    char *end = NULL;
    const char *suffix_end;
    double value;
    double scale = 1.0;

    if (text == NULL || out == NULL)
        return 0;
    value = strtod(text, &end);
    if (end == text || !isfinite(value))
        return 0;

    suffix_end = end;
    if (!parse_scale_suffix(&suffix_end, &scale) || *suffix_end != '\0')
        return 0;
    value *= scale;
    if (!isfinite(value))
        return 0;

    *out = value;
    return 1;
}

static int parse_expression_number(const char **cursor, double *out)
{
    char *end = NULL;
    double value = strtod(*cursor, &end);
    double scale = 1.0;
    const char *suffix_end;

    if (end == *cursor || !isfinite(value))
        return 0;
    suffix_end = end;
    if (!parse_scale_suffix(&suffix_end, &scale))
        return 0;
    value *= scale;
    if (!isfinite(value))
        return 0;

    *cursor = suffix_end;
    *out = value;
    return 1;
}

static int parameter_get(const ParamEnv *parameters, const char *name,
                         double *value)
{
    size_t index;

    for (index = parameters->count; index > 0u; --index) {
        if (equals_ci(parameters->items[index - 1u].name, name)) {
            *value = parameters->items[index - 1u].value;
            return 1;
        }
    }
    return 0;
}

static int parameter_set(Arena *scratch, ParamEnv *parameters,
                         const char *name, double value)
{
    size_t index;

    for (index = 0u; index < parameters->count; ++index) {
        if (equals_ci(parameters->items[index].name, name)) {
            parameters->items[index].value = value;
            return 1;
        }
    }

    if (parameters->count >= CSPX_PARAM_MAX)
        return 0;
    parameters->items[parameters->count].name = scratch_copy(scratch, name);
    if (parameters->items[parameters->count].name == NULL)
        return 0;
    parameters->items[parameters->count].value = value;
    ++parameters->count;
    return 1;
}

static double expression_sum(Expr *expression);

static double expression_primary(Expr *expression)
{
    const char *start;
    char name[128];
    size_t length = 0u;
    double value = 0.0;

    skip_space(&expression->cursor);
    if (*expression->cursor == '+' || *expression->cursor == '-') {
        char operation = *expression->cursor++;
        value = expression_primary(expression);
        return operation == '-' ? -value : value;
    }

    if (*expression->cursor == '(') {
        ++expression->cursor;
        value = expression_sum(expression);
        skip_space(&expression->cursor);
        if (*expression->cursor != ')')
            expression->valid = 0;
        else
            ++expression->cursor;
        return value;
    }

    if (isalpha((unsigned char)*expression->cursor) ||
        *expression->cursor == '_') {
        start = expression->cursor;
        while ((isalnum((unsigned char)*expression->cursor) ||
                *expression->cursor == '_' || *expression->cursor == '.') &&
               length + 1u < sizeof(name)) {
            name[length++] = *expression->cursor++;
        }
        if (isalnum((unsigned char)*expression->cursor) ||
            *expression->cursor == '_' || *expression->cursor == '.') {
            expression->valid = 0;
            expression->cursor = start;
            return 0.0;
        }
        name[length] = '\0';
        if (!parameter_get(expression->parameters, name, &value))
            expression->valid = 0;
        return value;
    }

    if (!parse_expression_number(&expression->cursor, &value)) {
        expression->valid = 0;
        if (*expression->cursor != '\0')
            ++expression->cursor;
    }
    return value;
}

static double expression_power(Expr *expression)
{
    double value = expression_primary(expression);

    skip_space(&expression->cursor);
    if (*expression->cursor == '^') {
        double exponent;
        ++expression->cursor;
        exponent = expression_power(expression);
        value = pow(value, exponent);
        if (!isfinite(value))
            expression->valid = 0;
    }
    return value;
}

static double expression_product(Expr *expression)
{
    double value = expression_power(expression);

    for (;;) {
        char operation;
        double right;

        skip_space(&expression->cursor);
        operation = *expression->cursor;
        if (operation != '*' && operation != '/')
            break;
        ++expression->cursor;
        right = expression_power(expression);
        if (operation == '*') {
            value *= right;
        } else if (right == 0.0) {
            expression->valid = 0;
        } else {
            value /= right;
        }
        if (!isfinite(value))
            expression->valid = 0;
    }
    return value;
}

static double expression_sum(Expr *expression)
{
    double value = expression_product(expression);

    for (;;) {
        char operation;
        double right;

        skip_space(&expression->cursor);
        operation = *expression->cursor;
        if (operation != '+' && operation != '-')
            break;
        ++expression->cursor;
        right = expression_product(expression);
        value = operation == '+' ? value + right : value - right;
        if (!isfinite(value))
            expression->valid = 0;
    }
    return value;
}

static int evaluate_expression(const char *text, const ParamEnv *parameters,
                               double *out)
{
    Expr expression;
    double value;

    expression.cursor = text;
    expression.parameters = parameters;
    expression.valid = 1;
    value = expression_sum(&expression);
    skip_space(&expression.cursor);
    if (*expression.cursor != '\0' || !expression.valid || !isfinite(value))
        return 0;
    *out = value;
    return 1;
}

/* Tokenizer preserves text inside braces and quoted strings. */
static size_t split_tokens(char *line, char **tokens, size_t capacity)
{
    size_t count = 0u;
    char *cursor = line;

    while (*cursor != '\0') {
        int in_braces = 0;
        int in_quotes = 0;

        while (*cursor != '\0' &&
               (isspace((unsigned char)*cursor) || *cursor == ',' ||
                *cursor == '(' || *cursor == ')'))
            ++cursor;
        if (*cursor == '\0')
            break;
        if (count == capacity)
            return capacity + 1u;

        tokens[count++] = cursor;
        while (*cursor != '\0') {
            if (*cursor == '"' && !in_braces)
                in_quotes = !in_quotes;
            else if (*cursor == '{' && !in_quotes)
                ++in_braces;
            else if (*cursor == '}' && !in_quotes && in_braces > 0)
                --in_braces;

            if (!in_quotes && in_braces == 0 &&
                (isspace((unsigned char)*cursor) || *cursor == ',' ||
                 *cursor == '(' || *cursor == ')'))
                break;
            ++cursor;
        }
        if (*cursor != '\0')
            *cursor++ = '\0';
    }
    return count;
}

/* The assignment helper needs scratch-owned expression text. */
static int evaluate_assignment(Arena *scratch, const char *text,
                               const ParamEnv *parameters, double *value)
{
    size_t length;
    char *expression;

    if (text == NULL || value == NULL)
        return 0;
    length = strlen(text);
    if (length >= 2u && text[0] == '{' && text[length - 1u] == '}') {
        expression = scratch_copy_n(scratch, text + 1u, length - 2u);
        return expression != NULL &&
               evaluate_expression(expression, parameters, value);
    }
    return evaluate_expression(text, parameters, value) ||
           parse_spice_number(text, value);
}

static int apply_assignments(Arena *scratch, ParamEnv *parameters,
                             char **tokens, size_t first, size_t count)
{
    size_t index;

    for (index = first; index < count; ++index) {
        char *equals = strchr(tokens[index], '=');
        double value;

        if (equals == NULL)
            return 0;
        *equals = '\0';
        if (!evaluate_assignment(scratch, equals + 1, parameters, &value) ||
            !parameter_set(scratch, parameters, tokens[index], value))
            return 0;
    }
    return 1;
}

static int include_is_active(const ExpandCtx *context, unsigned depth,
                             const char *resolved_path)
{
    unsigned index;

    for (index = 0u; index <= depth; ++index) {
        if (context->include_stack[index] != NULL &&
            strcmp(context->include_stack[index], resolved_path) == 0)
            return 1;
    }
    return 0;
}

static int expand_includes(ExpandCtx *context, LineVec *lines,
                           const char *source_name, const char *text,
                           size_t text_size, unsigned depth, int is_root)
{
    size_t offset = 0u;
    size_t line_number = 1u;
    int first_line = 1;

    if (depth > context->max_include_depth) {
        context->error_line = 1u;
        return 0;
    }

    while (offset < text_size) {
        size_t start = offset;
        size_t length;
        char *raw;
        char *trimmed;
        char *token_line;
        char *tokens[CSPX_TOKEN_MAX];
        size_t token_count;

        while (offset < text_size && text[offset] != '\n')
            ++offset;
        length = offset - start;
        if (offset < text_size)
            ++offset;
        if (length != 0u && text[start + length - 1u] == '\r')
            --length;

        raw = scratch_copy_n(context->scratch, text + start, length);
        if (raw == NULL)
            return 0;
        trimmed = raw;
        while (isspace((unsigned char)*trimmed))
            ++trimmed;
        token_line = scratch_copy(context->scratch, trimmed);
        if (token_line == NULL)
            return 0;
        token_count = split_tokens(token_line, tokens, CSPX_TOKEN_MAX);

        /* Only the root deck has a title line. Include files are inserted
         * literally at the directive location, beginning with their line 1. */
        if (is_root && first_line) {
            if (!line_vector_append(lines, raw, source_name, line_number))
                return 0;
        } else if (token_count >= 2u && equals_ci(tokens[0], ".INCLUDE")) {
            const char *requested = tokens[1];
            const char *resolved = NULL;
            const char *included_text = NULL;
            size_t included_size = 0u;
            size_t requested_length = strlen(requested);

            if (token_count != 2u || context->options == NULL ||
                context->options->read_include == NULL) {
                context->error_line = line_number;
                return 0;
            }
            if (requested_length >= 2u &&
                ((requested[0] == '"' &&
                  requested[requested_length - 1u] == '"') ||
                 (requested[0] == '\'' &&
                  requested[requested_length - 1u] == '\''))) {
                requested = scratch_copy_n(context->scratch, requested + 1u,
                                           requested_length - 2u);
                if (requested == NULL)
                    return 0;
            }

            if (!context->options->read_include(
                    context->options->read_context, context->scratch,
                    source_name, requested, &resolved, &included_text,
                    &included_size) ||
                resolved == NULL || included_text == NULL ||
                include_is_active(context, depth, resolved)) {
                context->error_line = line_number;
                return 0;
            }
            if (depth >= context->max_include_depth) {
                context->error_line = line_number;
                return 0;
            }
            context->include_stack[depth + 1u] = resolved;
            if (!expand_includes(context, lines, resolved, included_text,
                                 included_size, depth + 1u, 0)) {
                if (context->error_line == 0u)
                    context->error_line = line_number;
                return 0;
            }
            context->include_stack[depth + 1u] = NULL;
        } else {
            if (!line_vector_append(lines, trimmed, source_name, line_number))
                return 0;
        }

        first_line = 0;
        ++line_number;
    }
    return 1;
}

static Subckt *find_subcircuit(SubcktTable *table, const char *name)
{
    size_t index;

    for (index = 0u; index < table->count; ++index) {
        if (equals_ci(table->items[index].name, name))
            return &table->items[index];
    }
    return NULL;
}

static int subcircuit_add_string(Arena *scratch, char ***items,
                                 size_t count, const char *value)
{
    char **replacement = (char **)scratch_grow(
        scratch, *items, count, count + 1u, sizeof(**items));
    if (replacement == NULL)
        return 0;
    replacement[count] = scratch_copy(scratch, value);
    if (replacement[count] == NULL)
        return 0;
    *items = replacement;
    return 1;
}

static int subcircuit_add_body(Arena *scratch, Subckt *subcircuit,
                               const char *line, size_t line_number)
{
    char **new_body;
    size_t *new_lines;

    new_body = (char **)scratch_grow(
        scratch, subcircuit->body, subcircuit->body_count,
        subcircuit->body_count + 1u, sizeof(*new_body));
    if (new_body == NULL)
        return 0;
    new_lines = (size_t *)scratch_grow(
        scratch, subcircuit->body_lines, subcircuit->body_count,
        subcircuit->body_count + 1u, sizeof(*new_lines));
    if (new_lines == NULL)
        return 0;
    new_body[subcircuit->body_count] = scratch_copy(scratch, line);
    if (new_body[subcircuit->body_count] == NULL)
        return 0;
    new_lines[subcircuit->body_count] = line_number;
    subcircuit->body = new_body;
    subcircuit->body_lines = new_lines;
    ++subcircuit->body_count;
    return 1;
}

static int collect_subcircuits(ExpandCtx *context, const LineVec *lines,
                               SubcktTable *table)
{
    size_t index;

    for (index = 0u; index < lines->count; ++index) {
        char *copy = scratch_copy(context->scratch, lines->items[index].text);
        char *tokens[CSPX_TOKEN_MAX];
        size_t token_count;
        size_t cursor;
        Subckt *subcircuit;
        size_t body_line;

        if (copy == NULL)
            return 0;
        token_count = split_tokens(copy, tokens, CSPX_TOKEN_MAX);
        if (token_count == 0u || !equals_ci(tokens[0], ".SUBCKT"))
            continue;
        if (token_count < 2u || table->count >= CSPX_SUBCKT_MAX ||
            find_subcircuit(table, tokens[1]) != NULL) {
            context->error_line = lines->items[index].line;
            return 0;
        }

        subcircuit = &table->items[table->count++];
        memset(subcircuit, 0, sizeof(*subcircuit));
        subcircuit->name = scratch_copy(context->scratch, tokens[1]);
        subcircuit->definition_line = lines->items[index].line;
        if (subcircuit->name == NULL)
            return 0;

        cursor = 2u;
        while (cursor < token_count && !equals_ci(tokens[cursor], "PARAMS:")) {
            if (!subcircuit_add_string(context->scratch,
                                       &subcircuit->nodes,
                                       subcircuit->node_count,
                                       tokens[cursor]))
                return 0;
            ++subcircuit->node_count;
            ++cursor;
        }
        if (cursor < token_count && equals_ci(tokens[cursor], "PARAMS:"))
            ++cursor;
        for (; cursor < token_count; ++cursor) {
            char *equals = strchr(tokens[cursor], '=');
            char *name;
            char *default_value;
            char **new_params;
            char **new_defaults;
            size_t current = subcircuit->param_count;

            if (equals == NULL || equals == tokens[cursor] || equals[1] == '\0') {
                context->error_line = lines->items[index].line;
                return 0;
            }
            *equals = '\0';
            name = scratch_copy(context->scratch, tokens[cursor]);
            default_value = scratch_copy(context->scratch, equals + 1);
            new_params = (char **)scratch_grow(
                context->scratch, subcircuit->params, current, current + 1u,
                sizeof(*new_params));
            new_defaults = (char **)scratch_grow(
                context->scratch, subcircuit->defaults, current, current + 1u,
                sizeof(*new_defaults));
            if (name == NULL || default_value == NULL ||
                new_params == NULL || new_defaults == NULL)
                return 0;
            new_params[current] = name;
            new_defaults[current] = default_value;
            subcircuit->params = new_params;
            subcircuit->defaults = new_defaults;
            ++subcircuit->param_count;
        }

        body_line = index + 1u;
        for (; body_line < lines->count; ++body_line) {
            char *body_copy = scratch_copy(context->scratch,
                                           lines->items[body_line].text);
            char *body_tokens[CSPX_TOKEN_MAX];
            size_t body_token_count;

            if (body_copy == NULL)
                return 0;
            body_token_count = split_tokens(body_copy, body_tokens,
                                            CSPX_TOKEN_MAX);
            if (body_token_count != 0u &&
                equals_ci(body_tokens[0], ".ENDS"))
                break;
            if (body_token_count != 0u &&
                equals_ci(body_tokens[0], ".SUBCKT")) {
                context->error_line = lines->items[body_line].line;
                return 0;
            }
            if (!subcircuit_add_body(context->scratch, subcircuit,
                                     lines->items[body_line].text,
                                     lines->items[body_line].line))
                return 0;
        }
        if (body_line == lines->count) {
            context->error_line = lines->items[index].line;
            return 0;
        }
        index = body_line;
    }
    return 1;
}

static int parameter_line(ExpandCtx *context, ParamEnv *parameters,
                          const char *line)
{
    char *copy = scratch_copy(context->scratch, line);
    char *tokens[CSPX_TOKEN_MAX];
    size_t token_count;

    if (copy == NULL)
        return 0;
    token_count = split_tokens(copy, tokens, CSPX_TOKEN_MAX);
    return token_count >= 2u && equals_ci(tokens[0], ".PARAM") &&
           apply_assignments(context->scratch, parameters, tokens, 1u,
                             token_count);
}

static char *map_node(ExpandCtx *context, const Scope *scope,
                      const char *node, const char *prefix)
{
    size_t index;

    if (equals_ci(node, "0"))
        return scratch_copy(context->scratch, "0");
    for (index = 0u; index < scope->count; ++index) {
        if (equals_ci(scope->items[index].name, node))
            return scratch_copy(context->scratch, scope->items[index].value);
    }
    if (prefix == NULL || *prefix == '\0')
        return scratch_copy(context->scratch, node);

    {
        size_t required = strlen(prefix) + strlen(node) + 2u;
        char *mapped = (char *)scratch_alloc(context->scratch, required);
        if (mapped != NULL)
            (void)snprintf(mapped, required, "%s_%s", prefix, node);
        return mapped;
    }
}

static char *scoped_name(ExpandCtx *context, char lead,
                         const char *prefix, const char *name)
{
    const char *tail = name;
    size_t required;
    char *result;

    if (*tail != '\0' && toupper((unsigned char)*tail) == lead)
        ++tail;
    required = strlen(prefix) + strlen(tail) + 4u;
    result = (char *)scratch_alloc(context->scratch, required);
    if (result != NULL)
        (void)snprintf(result, required, "%c_%s_%s", lead, prefix, tail);
    return result;
}

static int append_tokens(StrBuf *output, char **tokens, size_t count)
{
    size_t index;

    for (index = 0u; index < count; ++index) {
        if (index != 0u && !buffer_append(output, " "))
            return 0;
        if (!buffer_append(output, tokens[index]))
            return 0;
    }
    return buffer_append(output, "\n");
}

static int evaluate_token(ExpandCtx *context, const char *token,
                          const ParamEnv *parameters, char **expanded)
{
    const char *open;
    const char *close;
    char *expression;
    double value;
    char number[64];
    int number_length;
    size_t prefix_length;
    size_t required;

    *expanded = NULL;
    open = strchr(token, '{');
    if (open == NULL)
        return 1;
    close = strrchr(open, '}');
    if (close == NULL || close[1] != '\0')
        return 0;

    expression = scratch_copy_n(context->scratch, open + 1u,
                                (size_t)(close - open - 1u));
    if (expression == NULL ||
        !evaluate_expression(expression, parameters, &value))
        return 0;

    number_length = snprintf(number, sizeof(number), "%.17g", value);
    if (number_length < 0 || (size_t)number_length >= sizeof(number))
        return 0;
    prefix_length = (size_t)(open - token);
    if (prefix_length > SIZE_MAX - (size_t)number_length - 1u)
        return 0;
    required = prefix_length + (size_t)number_length + 1u;
    *expanded = (char *)scratch_alloc(context->scratch, required);
    if (*expanded == NULL)
        return 0;
    memcpy(*expanded, token, prefix_length);
    memcpy(*expanded + prefix_length, number, (size_t)number_length + 1u);
    return 1;
}

static int evaluate_and_emit(ExpandCtx *context, StrBuf *output,
                             const char *line, const ParamEnv *parameters,
                             const Scope *scope, const char *prefix,
                             const Subckt *current)
{
    char *copy = scratch_copy(context->scratch, line);
    char *tokens[CSPX_TOKEN_MAX];
    char *owned[CSPX_TOKEN_MAX] = {0};
    size_t token_count;
    size_t index;

    if (copy == NULL)
        return 0;
    token_count = split_tokens(copy, tokens, CSPX_TOKEN_MAX);
    if (token_count == 0u)
        return buffer_append(output, "\n");
    if (token_count > CSPX_TOKEN_MAX)
        return 0;

    for (index = 0u; index < token_count; ++index) {
        if (!evaluate_token(context, tokens[index], parameters,
                            &owned[index]))
            return 0;
        if (owned[index] != NULL)
            tokens[index] = owned[index];
    }

    if (tokens[0][0] == '.' && equals_ci(tokens[0], ".MODEL")) {
        if (token_count < 3u)
            return 0;
        if (prefix != NULL && *prefix != '\0') {
            owned[1] = scoped_name(context, 'M', prefix, tokens[1]);
            if (owned[1] == NULL)
                return 0;
            tokens[1] = owned[1];
        }
    } else if (tokens[0][0] == '.' &&
               (equals_ci(tokens[0], ".IC") ||
                equals_ci(tokens[0], ".NODESET"))) {
        if (token_count >= 4u && equals_ci(tokens[1], "V")) {
            owned[2] = map_node(context, scope, tokens[2], prefix);
            if (owned[2] == NULL)
                return 0;
            tokens[2] = owned[2];
        }
    } else if (tokens[0][0] != '.') {
        char lead = (char)toupper((unsigned char)tokens[0][0]);
        size_t terminal_count = 2u;
        size_t model_index = SIZE_MAX;

        if (prefix != NULL && *prefix != '\0')
            owned[0] = scoped_name(context, lead, prefix, tokens[0]);
        else
            owned[0] = scratch_copy(context->scratch, tokens[0]);
        if (owned[0] == NULL)
            return 0;
        tokens[0] = owned[0];

        switch (lead) {
        case 'Q':
        case 'J': terminal_count = 3u; model_index = 4u; break;
        case 'M': terminal_count = 4u; model_index = 5u; break;
        case 'E':
        case 'G': terminal_count = 4u; break;
        case 'D': model_index = 3u; break;
        default: break;
        }

        if ((lead == 'F' || lead == 'H') && token_count > 3u &&
            prefix != NULL && *prefix != '\0') {
            owned[3] = scoped_name(context,
                                   (char)toupper((unsigned char)tokens[3][0]),
                                   prefix, tokens[3]);
            if (owned[3] == NULL)
                return 0;
            tokens[3] = owned[3];
        }

        for (index = 0u; index < terminal_count && index + 1u < token_count;
             ++index) {
            owned[index + 1u] = map_node(context, scope, tokens[index + 1u],
                                         prefix);
            if (owned[index + 1u] == NULL)
                return 0;
            tokens[index + 1u] = owned[index + 1u];
        }

        if (model_index < token_count && prefix != NULL && *prefix != '\0' &&
            current != NULL) {
            size_t body_index;
            int is_local_model = 0;

            for (body_index = 0u; body_index < current->body_count;
                 ++body_index) {
                char *model_line = scratch_copy(
                    context->scratch, current->body[body_index]);
                char *model_tokens[CSPX_TOKEN_MAX];
                size_t model_count;
                if (model_line == NULL)
                    return 0;
                model_count = split_tokens(model_line, model_tokens,
                                           CSPX_TOKEN_MAX);
                if (model_count > 2u &&
                    equals_ci(model_tokens[0], ".MODEL") &&
                    equals_ci(model_tokens[1], tokens[model_index])) {
                    is_local_model = 1;
                    break;
                }
            }
            if (is_local_model) {
                owned[model_index] = scoped_name(
                    context, 'M', prefix, tokens[model_index]);
                if (owned[model_index] == NULL)
                    return 0;
                tokens[model_index] = owned[model_index];
            }
        }
    }

    return append_tokens(output, tokens, token_count);
}

static int flatten_lines(ExpandCtx *context, const LineVec *lines,
                         SubcktTable *subcircuits, StrBuf *output,
                         size_t start, size_t end, ParamEnv *parameters,
                         const Scope *scope, const char *prefix,
                         unsigned depth, const Subckt *current);

static int expand_instance(ExpandCtx *context, SubcktTable *subcircuits,
                           StrBuf *output, char **tokens, size_t token_count,
                           const ParamEnv *parent_parameters,
                           const Scope *parent_scope, const char *parent_prefix,
                           unsigned depth, size_t line_number)
{
    Subckt *subcircuit = NULL;
    size_t model_position = 0u;
    size_t index;
    unsigned maximum_depth = context->max_subcircuit_depth;
    char *instance_prefix;
    Scope child_scope;
    ParamEnv local_parameters;
    LineVec body_lines;
    int success = 1;

    memset(&child_scope, 0, sizeof(child_scope));
    memset(&local_parameters, 0, sizeof(local_parameters));
    memset(&body_lines, 0, sizeof(body_lines));
    body_lines.arena = context->scratch;

    for (index = 1u; index < token_count; ++index) {
        Subckt *candidate = find_subcircuit(subcircuits, tokens[index]);
        if (candidate != NULL && index - 1u == candidate->node_count) {
            subcircuit = candidate;
            model_position = index;
            break;
        }
    }

    if (subcircuit == NULL || depth >= maximum_depth) {
        context->error_line = line_number;
        return 0;
    }

    if (parent_prefix != NULL && *parent_prefix != '\0') {
        size_t required = strlen(parent_prefix) + strlen(tokens[0]) + 2u;
        instance_prefix = (char *)scratch_alloc(context->scratch, required);
        if (instance_prefix != NULL)
            (void)snprintf(instance_prefix, required, "%s_%s", parent_prefix,
                           tokens[0]);
    } else {
        instance_prefix = scratch_copy(context->scratch, tokens[0]);
    }
    if (instance_prefix == NULL)
        return 0;

    child_scope.count = subcircuit->node_count;
    child_scope.items = (NodeMap *)scratch_array(
        context->scratch, sizeof(*child_scope.items), child_scope.count);
    if (child_scope.items == NULL)
        return 0;
    memset(child_scope.items, 0,
           child_scope.count * sizeof(*child_scope.items));

    for (index = 0u; index < subcircuit->node_count; ++index) {
        child_scope.items[index].name =
            scratch_copy(context->scratch, subcircuit->nodes[index]);
        child_scope.items[index].value = map_node(
            context, parent_scope, tokens[index + 1u], parent_prefix);
        if (child_scope.items[index].name == NULL ||
            child_scope.items[index].value == NULL)
            success = 0;
    }

    for (index = 0u; success && index < parent_parameters->count; ++index) {
        success = parameter_set(context->scratch, &local_parameters,
                                parent_parameters->items[index].name,
                                parent_parameters->items[index].value);
    }

    for (index = 0u; success && index < subcircuit->param_count; ++index) {
        double value;
        if (!evaluate_assignment(context->scratch,
                                 subcircuit->defaults[index],
                                 &local_parameters, &value) ||
            !parameter_set(context->scratch, &local_parameters,
                           subcircuit->params[index], value))
            success = 0;
    }

    index = model_position + 1u;
    if (index < token_count && equals_ci(tokens[index], "PARAMS:"))
        ++index;
    if (success && index < token_count) {
        success = apply_assignments(context->scratch, &local_parameters,
                                    tokens, index, token_count);
    }

    for (index = 0u; success && index < subcircuit->body_count; ++index) {
        success = line_vector_append(
            &body_lines, subcircuit->body[index], "<subckt>",
            subcircuit->body_lines[index]);
    }

    if (success) {
        success = flatten_lines(context, &body_lines, subcircuits, output, 0u,
                                body_lines.count, &local_parameters,
                                &child_scope, instance_prefix, depth + 1u,
                                subcircuit);
    }
    if (!success && context->error_line == 0u)
        context->error_line = line_number;
    return success;
}

static int flatten_lines(ExpandCtx *context, const LineVec *lines,
                         SubcktTable *subcircuits, StrBuf *output,
                         size_t start, size_t end, ParamEnv *parameters,
                         const Scope *scope, const char *prefix,
                         unsigned depth, const Subckt *current)
{
    size_t index;
    size_t limit = end == 0u ? lines->count : end;

    for (index = start; index < limit; ++index) {
        char *copy = scratch_copy(context->scratch, lines->items[index].text);
        char *tokens[CSPX_TOKEN_MAX];
        size_t token_count;

        if (copy == NULL)
            return 0;
        token_count = split_tokens(copy, tokens, CSPX_TOKEN_MAX);
        if (token_count == 0u)
            continue;
        if (token_count > CSPX_TOKEN_MAX) {
            context->error_line = lines->items[index].line;
            return 0;
        }

        if (equals_ci(tokens[0], ".SUBCKT")) {
            size_t end_index = index + 1u;
            for (; end_index < limit; ++end_index) {
                char *end_copy = scratch_copy(
                    context->scratch, lines->items[end_index].text);
                char *end_tokens[CSPX_TOKEN_MAX];
                size_t end_count;
                if (end_copy == NULL)
                    return 0;
                end_count = split_tokens(end_copy, end_tokens,
                                         CSPX_TOKEN_MAX);
                if (end_count != 0u && equals_ci(end_tokens[0], ".ENDS"))
                    break;
            }
            if (end_index == limit) {
                context->error_line = lines->items[index].line;
                return 0;
            }
            index = end_index;
            continue;
        }
        if (equals_ci(tokens[0], ".ENDS"))
            continue;
        if (equals_ci(tokens[0], ".PARAM")) {
            if (!parameter_line(context, parameters,
                                lines->items[index].text)) {
                context->error_line = lines->items[index].line;
                return 0;
            }
            continue;
        }
        if (toupper((unsigned char)tokens[0][0]) == 'X') {
            if (!expand_instance(context, subcircuits, output, tokens,
                                 token_count, parameters, scope, prefix,
                                 depth, lines->items[index].line))
                return 0;
            continue;
        }
        if (!evaluate_and_emit(context, output, lines->items[index].text,
                               parameters, scope, prefix, current)) {
            context->error_line = lines->items[index].line;
            return 0;
        }
    }
    return 1;
}

int csp_expand_deck(Arena *arena, const char *source_name, const char *text,
                    size_t text_size, const CspExpandOptions *options,
                    char **out_text, size_t *out_size, size_t *error_line)
{
    Arena scratch;
    ExpandCtx context;
    LineVec lines;
    SubcktTable subcircuits;
    ParamEnv global_parameters;
    Scope global_scope;
    StrBuf flattened;
    char *result;
    size_t error = 0u;
    int success = 0;

    if (arena == NULL || text == NULL || out_text == NULL)
        return 0;
    *out_text = NULL;
    if (out_size != NULL)
        *out_size = 0u;
    if (error_line != NULL)
        *error_line = 0u;

    arena_init(&scratch, 16u * 1024u);
    memset(&context, 0, sizeof(context));
    memset(&lines, 0, sizeof(lines));
    memset(&subcircuits, 0, sizeof(subcircuits));
    memset(&global_parameters, 0, sizeof(global_parameters));
    memset(&global_scope, 0, sizeof(global_scope));
    memset(&flattened, 0, sizeof(flattened));

    context.scratch = &scratch;
    context.options = options;
    context.max_include_depth =
        options != NULL && options->max_include_depth != 0u
            ? options->max_include_depth : CSPX_DEFAULT_INCLUDE_DEPTH;
    context.max_subcircuit_depth =
        options != NULL && options->max_subcircuit_depth != 0u
            ? options->max_subcircuit_depth : CSPX_DEFAULT_SUBCKT_DEPTH;
    if (context.max_include_depth > CSPX_HARD_INCLUDE_DEPTH)
        context.max_include_depth = CSPX_HARD_INCLUDE_DEPTH;
    if (context.max_subcircuit_depth > CSPX_HARD_SUBCKT_DEPTH)
        context.max_subcircuit_depth = CSPX_HARD_SUBCKT_DEPTH;

    lines.arena = &scratch;
    flattened.arena = &scratch;
    context.include_stack[0] = source_name == NULL ? "<memory>" : source_name;

    if (!expand_includes(&context, &lines, context.include_stack[0], text,
                         text_size, 0u, 1))
        goto cleanup;
    if (!collect_subcircuits(&context, &lines, &subcircuits))
        goto cleanup;

    if (lines.count != 0u) {
        if (!buffer_append(&flattened, lines.items[0].text) ||
            !buffer_append(&flattened, "\n"))
            goto cleanup;
    }
    if (!flatten_lines(&context, &lines, &subcircuits, &flattened, 1u,
                       lines.count, &global_parameters, &global_scope, "", 0u,
                       NULL))
        goto cleanup;

    result = (char *)arena_alloc(arena, flattened.size + 1u);
    if (result == NULL)
        goto cleanup;
    if (flattened.size != 0u)
        memcpy(result, flattened.data, flattened.size);
    result[flattened.size] = '\0';
    *out_text = result;
    if (out_size != NULL)
        *out_size = flattened.size;
    success = 1;

cleanup:
    error = context.error_line;
    arena_destroy(&scratch);
    if (!success && error_line != NULL)
        *error_line = error;
    return success;
}

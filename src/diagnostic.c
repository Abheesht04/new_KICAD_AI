#include "diagnostic.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void diagnostic_list_init(DiagnosticList *list)
{
    if (!list) {
        return;
    }

    memset(list, 0, sizeof(*list));
}

void diagnostic_list_free(DiagnosticList *list)
{
    if (!list) {
        return;
    }

    vec_free(list->items);
}

int diagnostic_add(
    DiagnosticList *list,
    DiagnosticSeverity severity,
    DiagnosticTargetKind target_kind,
    uint32_t target_id,
    const char *rule_name,
    const char *format,
    ...
)
{
    if (!list || !format) {
        return -1;
    }

    Diagnostic diagnostic;
    memset(&diagnostic, 0, sizeof(diagnostic));

    diagnostic.severity = severity;
    diagnostic.target_kind = target_kind;
    diagnostic.target_id = target_id;
    diagnostic.rule_name = rule_name;

    va_list args;
    va_start(args, format);
    int written = vsnprintf(
        diagnostic.message,
        sizeof(diagnostic.message),
        format,
        args
    );
    va_end(args);

    if (written < 0) {
        return -1;
    }

    if (vec_push(list->items, diagnostic) != 0) {
        return -1;
    }

    return 0;
}

size_t diagnostic_count(const DiagnosticList *list)
{
    if (!list) {
        return 0;
    }

    return vec_len(list->items);
}

const Diagnostic *diagnostic_get(const DiagnosticList *list, size_t index)
{
    if (!list || index >= vec_len(list->items)) {
        return NULL;
    }

    return &list->items[index];
}

const char *diagnostic_severity_name(DiagnosticSeverity severity)
{
    switch (severity) {
        case DIAGNOSTIC_INFO:
            return "INFO";
        case DIAGNOSTIC_WARNING:
            return "WARNING";
        case DIAGNOSTIC_ERROR:
            return "ERROR";
        default:
            return "UNKNOWN";
    }
}

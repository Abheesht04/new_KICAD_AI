#ifndef DIAGNOSTIC_H
#define DIAGNOSTIC_H

#include <stddef.h>
#include <stdint.h>

#include "vec.h"

#define DIAGNOSTIC_MESSAGE_MAX 256
#define DIAGNOSTIC_INVALID_ID UINT32_MAX

typedef enum {
    DIAGNOSTIC_INFO,
    DIAGNOSTIC_WARNING,
    DIAGNOSTIC_ERROR
} DiagnosticSeverity;

typedef enum {
    DIAG_TARGET_DESIGN,
    DIAG_TARGET_COMPONENT,
    DIAG_TARGET_NET,
    DIAG_TARGET_CONSTRAINT
} DiagnosticTargetKind;

typedef struct {
    DiagnosticSeverity severity;
    DiagnosticTargetKind target_kind;
    uint32_t target_id;
    const char *rule_name;
    char message[DIAGNOSTIC_MESSAGE_MAX];
} Diagnostic;

typedef struct {
    Diagnostic *items;
} DiagnosticList;

void diagnostic_list_init(DiagnosticList *list);
void diagnostic_list_free(DiagnosticList *list);

int diagnostic_add(
    DiagnosticList *list,
    DiagnosticSeverity severity,
    DiagnosticTargetKind target_kind,
    uint32_t target_id,
    const char *rule_name,
    const char *format,
    ...
);

size_t diagnostic_count(const DiagnosticList *list);
const Diagnostic *diagnostic_get(const DiagnosticList *list, size_t index);

const char *diagnostic_severity_name(DiagnosticSeverity severity);

#endif

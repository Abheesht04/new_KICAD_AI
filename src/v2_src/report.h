#ifndef COPPER_REPORT_H
#define COPPER_REPORT_H
#include "arena.h"
#include "g2_types.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { REPORT_PASS, REPORT_FAIL, REPORT_UNVERIFIED } ReportStatus;
typedef struct {
  ReportStatus status;
  const char *rule_id;
  const char *kind;
  const char *object;
  size_t object_index;
  G2Point location;
  bool has_location;
  bool has_measurement;
  G2Nm limit_nm;
  G2Nm measured_nm;
  const char *detail;
  const char *source_url;
  const char *retrieved;
} ReportFinding;
typedef struct {
  ReportFinding *findings;
  size_t count;
  size_t capacity;
  size_t pass_count;
  size_t fail_count;
  size_t unverified_count;
} CopperReport;
void report_init(CopperReport *report, Arena *arena, size_t capacity);
bool report_add(CopperReport *report, const ReportFinding *finding);
ReportStatus report_overall(const CopperReport *report);
const char *report_status_name(ReportStatus status);
size_t report_format_text(const CopperReport *report, const char *board_path,
                          char *out, size_t capacity);

/* General project/import diagnostics. These are deliberately separate from
 * DFM findings: a parser warning is not a manufacturing-rule result. */
typedef enum {
  DIAG_INFO = 0,
  DIAG_WARNING,
  DIAG_ERROR,
  DIAG_UNVERIFIED
} DiagnosticSeverity;

typedef struct {
  DiagnosticSeverity severity;
  const char *code;
  const char *file;
  const char *object;
  const char *message;
  size_t line;
  size_t column;
  size_t offset;
} CopperDiagnostic;

typedef struct {
  CopperDiagnostic *items;
  size_t count;
  size_t capacity;
  size_t info_count;
  size_t warning_count;
  size_t error_count;
  size_t unverified_count;
} CopperDiagnosticReport;

void diagnostic_report_init(CopperDiagnosticReport *report, Arena *arena,
                            size_t capacity);
bool diagnostic_report_add(CopperDiagnosticReport *report,
                           const CopperDiagnostic *diagnostic);
const char *diagnostic_severity_name(DiagnosticSeverity severity);
/* Returned buffers are malloc-owned and must be released with free(). */
char *diagnostic_report_to_json(const CopperDiagnosticReport *report,
                                size_t *out_length);
char *diagnostic_report_to_text(const CopperDiagnosticReport *report,
                                size_t *out_length);
#endif

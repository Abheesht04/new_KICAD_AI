#include "report.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>

void report_init(CopperReport *r, Arena *arena, size_t capacity) {
  if (!r) return;
  memset(r, 0, sizeof(*r));
  r->capacity = capacity;
  if (arena && capacity) r->findings = (ReportFinding *)arena_alloc_array(arena, sizeof(ReportFinding), capacity);
  if (!r->findings) r->capacity = 0;
}
bool report_add(CopperReport *r, const ReportFinding *f) {
  if (!r || !f || r->count >= r->capacity) return false;
  r->findings[r->count++] = *f;
  if (f->status == REPORT_PASS) ++r->pass_count;
  else if (f->status == REPORT_FAIL) ++r->fail_count;
  else ++r->unverified_count;
  return true;
}
ReportStatus report_overall(const CopperReport *r) {
  if (!r) return REPORT_UNVERIFIED;
  /* A known violation must never be hidden by an unrelated unknown check. */
  if (r->fail_count) return REPORT_FAIL;
  if (r->unverified_count) return REPORT_UNVERIFIED;
  return REPORT_PASS;
}
const char *report_status_name(ReportStatus s) {
  switch (s) {
  case REPORT_PASS: return "PASS";
  case REPORT_FAIL: return "FAIL";
  default: return "UNVERIFIED";
  }
}
static size_t append(char *out, size_t cap, size_t used, const char *fmt, ...) {
  if (used >= cap) return used;
  va_list ap; va_start(ap, fmt);
  int n = vsnprintf(out + used, cap - used, fmt, ap);
  va_end(ap);
  if (n < 0) return used;
  return used + (size_t)n;
}
size_t report_format_text(const CopperReport *r, const char *board_path,
                          char *out, size_t cap) {
  if (!r || !out || cap == 0) return 0;
  size_t n = 0; out[0] = '\0';
  n = append(out, cap, n, "COPPER DFM REPORT\nBoard: %s\nVerdict: %s\nPASS: %zu  FAIL: %zu  UNVERIFIED: %zu\n\n",
             board_path ? board_path : "(unknown)", report_status_name(report_overall(r)),
             r->pass_count, r->fail_count, r->unverified_count);
  for (size_t i = 0; i < r->count && n < cap; ++i) {
    const ReportFinding *f = &r->findings[i];
    n = append(out, cap, n, "[%s] %s / %s", report_status_name(f->status),
               f->rule_id ? f->rule_id : "NO_RULE", f->object ? f->object : "board");
    if (f->object_index != SIZE_MAX) n = append(out, cap, n, " #%zu", f->object_index);
    if (f->has_location) n = append(out, cap, n, " @ (%.6f, %.6f) mm",
                                     (double)f->location.x / 1000000.0,
                                     (double)f->location.y / 1000000.0);
    if (f->has_measurement)
      n = append(out, cap, n, " measured %.6f mm / limit %.6f mm",
                 (double)f->measured_nm / 1000000.0,
                 (double)f->limit_nm / 1000000.0);
    if (f->detail) n = append(out, cap, n, "\n  %s", f->detail);
    if (f->source_url) n = append(out, cap, n, "\n  Source: %s", f->source_url);
    if (f->retrieved) n = append(out, cap, n, " (retrieved %s)", f->retrieved);
    n = append(out, cap, n, "\n\n");
  }
  if (n >= cap) out[cap - 1] = '\0';
  return n;
}

typedef struct ReportBuffer {
  char *data;
  size_t length;
  size_t capacity;
  bool failed;
} ReportBuffer;
static bool rb_reserve(ReportBuffer *b, size_t extra) {
  if (extra > SIZE_MAX - b->length - 1u) return false;
  size_t need = b->length + extra + 1u;
  if (need <= b->capacity) return true;
  size_t cap = b->capacity ? b->capacity : 512u;
  while (cap < need) {
    if (cap > SIZE_MAX / 2u) { cap = need; break; }
    cap *= 2u;
  }
  char *grown = (char *)realloc(b->data, cap);
  if (!grown) return false;
  b->data = grown; b->capacity = cap;
  return true;
}
static void rb_bytes(ReportBuffer *b, const char *s, size_t n) {
  if (!b || !s || b->failed) return;
  if (!rb_reserve(b, n)) { b->failed = true; return; }
  memcpy(b->data + b->length, s, n); b->length += n; b->data[b->length] = '\0';
}
static void rb_text(ReportBuffer *b, const char *s) {
  if (!s) s = "";
  rb_bytes(b, s, strlen(s));
}
static void rb_fmt(ReportBuffer *b, const char *fmt, ...) {
  va_list ap, copy;
  va_start(ap, fmt); va_copy(copy, ap);
  int n = vsnprintf(NULL, 0, fmt, copy); va_end(copy);
  if (n < 0 || !rb_reserve(b, (size_t)n)) { b->failed = true; va_end(ap); return; }
  (void)vsnprintf(b->data + b->length, b->capacity - b->length, fmt, ap);
  va_end(ap); b->length += (size_t)n;
}
static void rb_json_string(ReportBuffer *b, const char *s) {
  if (!s) s = "";
  rb_text(b, "\"");
  for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
    char temp[7];
    switch (*p) {
      case '"': rb_text(b, "\\\""); break;
      case '\\': rb_text(b, "\\\\"); break;
      case '\b': rb_text(b, "\\b"); break;
      case '\f': rb_text(b, "\\f"); break;
      case '\n': rb_text(b, "\\n"); break;
      case '\r': rb_text(b, "\\r"); break;
      case '\t': rb_text(b, "\\t"); break;
      default:
        if (*p < 0x20) { (void)snprintf(temp, sizeof(temp), "\\u%04x", (unsigned)*p); rb_text(b, temp); }
        else rb_bytes(b, (const char *)p, 1);
        break;
    }
  }
  rb_text(b, "\"");
}
void diagnostic_report_init(CopperDiagnosticReport *r, Arena *arena,
                            size_t capacity) {
  if (!r) return;
  memset(r, 0, sizeof(*r)); r->capacity = capacity;
  if (arena && capacity)
    r->items = (CopperDiagnostic *)arena_alloc_array(arena, sizeof(*r->items), capacity);
  if (capacity && !r->items) r->capacity = 0;
}
bool diagnostic_report_add(CopperDiagnosticReport *r,
                           const CopperDiagnostic *d) {
  if (!r || !d || r->count >= r->capacity) return false;
  r->items[r->count++] = *d;
  switch (d->severity) {
    case DIAG_INFO: ++r->info_count; break;
    case DIAG_WARNING: ++r->warning_count; break;
    case DIAG_ERROR: ++r->error_count; break;
    case DIAG_UNVERIFIED: ++r->unverified_count; break;
  }
  return true;
}
const char *diagnostic_severity_name(DiagnosticSeverity s) {
  switch (s) {
    case DIAG_INFO: return "INFO";
    case DIAG_WARNING: return "WARNING";
    case DIAG_ERROR: return "ERROR";
    case DIAG_UNVERIFIED: return "UNVERIFIED";
    default: return "UNKNOWN";
  }
}
char *diagnostic_report_to_json(const CopperDiagnosticReport *r,
                                size_t *out_length) {
  ReportBuffer b = {0};
  if (out_length) *out_length = 0;
  if (!r) return NULL;
  rb_text(&b, "{\"schema\":\"copper.diagnostics\",\"schema_version\":1,\"summary\":{");
  rb_fmt(&b, "\"info\":%zu,\"warning\":%zu,\"error\":%zu,\"unverified\":%zu},\"diagnostics\":[",
         r->info_count, r->warning_count, r->error_count, r->unverified_count);
  for (size_t i = 0; i < r->count; ++i) {
    const CopperDiagnostic *d = &r->items[i];
    if (i) rb_text(&b, ",");
    rb_text(&b, "{\"severity\":"); rb_json_string(&b, diagnostic_severity_name(d->severity));
    rb_text(&b, ",\"code\":"); rb_json_string(&b, d->code);
    rb_text(&b, ",\"file\":"); rb_json_string(&b, d->file);
    rb_text(&b, ",\"object\":"); rb_json_string(&b, d->object);
    rb_text(&b, ",\"message\":"); rb_json_string(&b, d->message);
    rb_fmt(&b, ",\"line\":%zu,\"column\":%zu,\"offset\":%zu}", d->line, d->column, d->offset);
  }
  rb_text(&b, "]}\n");
  if (b.failed) { free(b.data); return NULL; }
  if (!b.data) { b.data = (char *)malloc(1); if (!b.data) return NULL; b.data[0] = '\0'; }
  if (out_length) *out_length = b.length;
  return b.data;
}
char *diagnostic_report_to_text(const CopperDiagnosticReport *r,
                                size_t *out_length) {
  ReportBuffer b = {0};
  if (out_length) *out_length = 0;
  if (!r) return NULL;
  rb_fmt(&b, "COPPER PROJECT IMPORT REPORT\nINFO: %zu  WARNING: %zu  ERROR: %zu  UNVERIFIED: %zu\n\n",
         r->info_count, r->warning_count, r->error_count, r->unverified_count);
  for (size_t i = 0; i < r->count; ++i) {
    const CopperDiagnostic *d = &r->items[i];
    rb_fmt(&b, "[%s] %s", diagnostic_severity_name(d->severity), d->code ? d->code : "GENERAL");
    if (d->file) rb_fmt(&b, " (%s)", d->file);
    if (d->object) rb_fmt(&b, " — %s", d->object);
    rb_fmt(&b, ": %s", d->message ? d->message : "");
    if (d->line) rb_fmt(&b, " [line %zu, column %zu]", d->line, d->column);
    rb_text(&b, "\n");
  }
  if (b.failed) { free(b.data); return NULL; }
  if (!b.data) { b.data = (char *)malloc(1); if (!b.data) return NULL; b.data[0] = '\0'; }
  if (out_length) *out_length = b.length;
  return b.data;
}

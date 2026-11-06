#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arena.h"
#include "g2_types.h" /* G2Nm, G2Point: nm integers, the single unit in V2 */

#define EXC_MAX_TOOLS 100  /* T00..T99 */
#define EXC_MAX_ISSUES 256 /* further issues are counted, not stored */

/* Result of the call. Content problems are in file->issues. */
typedef enum {
  EXC_OK = 0,           /* parsed completely, zero ERROR issues */
  EXC_ERR_CONTENT = -1, /* parsed, but the file has ERROR issues: UNVERIFIED */
  EXC_ERR_ARGUMENT = -2,
  EXC_ERR_IO = -3,
  EXC_ERR_MEMORY = -4,
  EXC_ERR_LIMIT = -5 /* more hits/slots than the reader supports */
} ExcStatus;

typedef enum {
  EXC_UNITS_UNKNOWN = 0,
  EXC_UNITS_METRIC,
  EXC_UNITS_INCH
} ExcUnits;

/* METRIC / INCH header may carry ",LZ" or ",TZ"; KiCad decimal files carry
 * none. */
typedef enum {
  EXC_ZEROS_DECIMAL =
      0, /* explicit decimal point: the only mode supported first */
  EXC_ZEROS_LEADING_OMITTED,
  EXC_ZEROS_TRAILING_OMITTED
} ExcZeroSuppress;

typedef enum {
  EXC_PLATING_UNKNOWN = 0,
  EXC_PLATING_PLATED,
  EXC_PLATING_NONPLATED,
  EXC_PLATING_MIXED /* file level only: "MixedPlating" */
} ExcPlating;

/* From "; #@! TA.AperFunction,Plated,PTH,ComponentDrill" */
typedef enum {
  EXC_DRILL_UNKNOWN = 0,
  EXC_DRILL_COMPONENT,
  EXC_DRILL_VIA,
  EXC_DRILL_MECHANICAL,
  EXC_DRILL_OTHER
} ExcDrillFunction;

typedef struct {
  uint32_t number;    /* the Tn number */
  G2Nm diameter;      /* nm */
  ExcPlating plating; /* from the tool's TA attribute, else the file's */
  ExcDrillFunction function;
  bool defined;  /* true once this tool has a valid definition */
  uint32_t line; /* line of the definition */
  size_t hit_count;
  size_t slot_count;
} ExcTool;

typedef struct {
  G2Point pos;   /* nm, absolute, file origin */
  uint32_t tool; /* index into ExcFile.tools (not the Tn number) */
  uint32_t line;
} ExcHit;

/* Reserved for routed-slot support; the current reader does not populate it. */
typedef struct {
  G2Point a;
  G2Point b;
  uint32_t tool;
  uint32_t line;
} ExcSlot;

typedef enum { EXC_SEV_INFO = 0, EXC_SEV_WARNING, EXC_SEV_ERROR } ExcSeverity;

typedef enum {
  EXC_ISSUE_NONE = 0,
  EXC_ISSUE_MISSING_HEADER,     /* no M48 */
  EXC_ISSUE_MISSING_UNITS,      /* no METRIC / INCH before the first tool */
  EXC_ISSUE_MISSING_END,        /* no M30 */
  EXC_ISSUE_UNKNOWN_COMMAND,    /* ignored, line recorded */
  EXC_ISSUE_UNSUPPORTED_FORMAT, /* non-decimal coordinates, incremental mode */
  EXC_ISSUE_UNSUPPORTED_SLOT,
  EXC_ISSUE_BAD_NUMBER,  /* text is not a number */
  EXC_ISSUE_COORD_RANGE, /* outside +-G2_COORD_MAX_NM */
  EXC_ISSUE_ROUNDED,     /* inch value not exact in nm (WARNING) */
  EXC_ISSUE_DUPLICATE_TOOL,
  EXC_ISSUE_UNDEFINED_TOOL,  /* Tn selected but never defined */
  EXC_ISSUE_HIT_BEFORE_TOOL, /* coordinates with no tool selected */
  EXC_ISSUE_BAD_DIAMETER,    /* zero or negative */
  EXC_ISSUE_MISSING_HEADER_END /* no '%' or M95 */
} ExcIssueCode;

typedef struct {
  ExcIssueCode code;
  ExcSeverity severity;
  uint32_t line;
  uint32_t tool; /* Tn number, 0 if not relevant */
} ExcIssue;

typedef struct {
  const char *source_name; /* not owned: file name for reports */

  /* header */
  ExcUnits units;
  ExcZeroSuppress zeros;
  bool absolute;            /* G90 */
  uint32_t fmat;            /* FMAT,n; 0 if absent */
  ExcPlating file_plating;  /* TF.FileFunction first token */
  int layer_from, layer_to; /* from "MixedPlating,1,2"; 0 if absent */
  bool saw_header_end;      /* the '%' line */
  bool saw_end;             /* M30 */

  /* Hits are allocated from the arena. Slot storage is reserved but unused. */
  ExcTool tools[EXC_MAX_TOOLS];
  size_t tool_count;
  ExcHit *hits;
  size_t hit_count;
  ExcSlot *slots;       /* reserved for routed-slot support; currently unused */
  size_t slot_count;    /* reserved; remains zero in this reader version */

  /* diagnostics */
  ExcIssue issues[EXC_MAX_ISSUES];
  size_t issue_count;
  size_t issues_dropped;
  size_t error_count;
  size_t warning_count;
} ExcFile;

/* Parse an Excellon drill file from memory. `out` is caller-owned and is
 * fully initialised by the call. Returns EXC_OK only with zero ERROR issues. */
ExcStatus exc_parse(Arena *arena, const char *text, size_t length,
                    const char *source_name, ExcFile *out);

/* Read the whole file, then exc_parse. */
ExcStatus exc_parse_path(Arena *arena, const char *path, ExcFile *out);

/* Pure helper, public so it can be unit tested on its own: parse decimal text
 * such as "-14.54" to nm with integer arithmetic only. No strtod.
 * Returns EXC_ISSUE_NONE, EXC_ISSUE_BAD_NUMBER, or EXC_ISSUE_ROUNDED (value is
 * still written). Range checking is the caller's job. */
ExcIssueCode exc_decimal_to_nm(const char *text, size_t length, ExcUnits units,
                               G2Nm *out_nm);

const ExcTool *exc_find_tool(const ExcFile *file, uint32_t number);
const char *exc_status_name(ExcStatus status);
const char *exc_issue_text(ExcIssueCode code);

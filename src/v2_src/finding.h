#ifndef COPPER_V2_FINDING_H
#define COPPER_V2_FINDING_H

/*
 * finding.h - one Finding for every checker, and a sorted JSON report (Copper V2 track E).
 *
 * Each source maps its own results into Finding:
 *   dfm       every DfmViolation -> FAIL; every rule that is UNVERIFIED or NOT_IMPLEMENTED -> one finding with the
 *             reason (passing rules are counted in the report, not listed);
 *   excellon  every ExcIssue: ERROR -> INVALID_INPUT, WARNING -> UNVERIFIED, INFO -> INFO;
 *   gerber    every GerberIssue: ERROR -> INVALID_INPUT, WARNING -> UNVERIFIED;
 *   board     every BoardIssue -> UNVERIFIED (a part of the board read but not modelled exactly).
 * Statuses are the project's honest set (CLAUDE.md): FAIL, UNVERIFIED, NOT_IMPLEMENTED, INVALID_INPUT, INFO.
 * The report's verdict is FAIL if anything fails, else INVALID_INPUT if an input could not be read correctly, else
 * UNVERIFIED if anything is unverified or not implemented, else PASS.
 *
 * Findings are sorted by a total order (source, file, rule, line, y, x, items, message), so a report depends only
 * on its inputs, never on the order they were added. Lengths are integer nm. Memory: the caller's Arena; strings
 * are copied. No printf, no exit.
 */

#include "arena.h"
#include "board.h"
#include "dfm.h"
#include "excellon.h"
#include "g2_types.h"
#include "gerber.h"
#include "json.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum FindingStatus {
    FINDING_FAIL = 0,
    FINDING_INVALID_INPUT,
    FINDING_UNVERIFIED,
    FINDING_NOT_IMPLEMENTED,
    FINDING_INFO
} FindingStatus;

typedef enum FindingSeverity { FINDING_SEV_ERROR = 0, FINDING_SEV_WARNING, FINDING_SEV_INFO } FindingSeverity;

typedef enum FindingVerdict { FINDING_VERDICT_PASS = 0, FINDING_VERDICT_FAIL, FINDING_VERDICT_INVALID_INPUT, FINDING_VERDICT_UNVERIFIED } FindingVerdict;

const char *finding_status_name(FindingStatus s);
const char *finding_severity_name(FindingSeverity s);
const char *finding_verdict_name(FindingVerdict v);

typedef struct Finding {
    const char *source;          /* "dfm", "excellon", "gerber", "board" */
    const char *rule;            /* e.g. "clearance/pth_to_track_min_mm", "excellon/undefined_tool" */
    FindingStatus status;
    FindingSeverity severity;
    const char *file;            /* the input's label (as given by the caller), or NULL */
    size_t line;                 /* 1-based line in `file`, 0 when not from a line */
    bool has_point;
    G2Point at;                  /* nm, in the source's coordinates */
    uint64_t layers;             /* board layer bits, 0 when not applicable */
    bool has_limit, has_measured;
    G2Nm limit_nm, measured_nm;
    const char *items[2];        /* e.g. "pad R1.2", "track 12" */
    const char *message;
    const char *quote;           /* the fabricator's wording for a DFM limit, or NULL */
} Finding;

typedef struct FindingSourceSummary {
    const char *source, *file;
    FindingVerdict verdict;
    size_t checks_passed;        /* dfm: rules that passed; others: 0 */
    size_t findings;
} FindingSourceSummary;

typedef struct FindingList {
    Arena *arena;
    Finding *items;
    size_t count, cap;
    FindingSourceSummary *sources;
    size_t source_count, source_cap;
    bool failed;                 /* out of memory at some point: the list is incomplete */
} FindingList;

void finding_list_init(FindingList *list, Arena *arena);
/* Add a copy of `f` (strings copied). */
bool finding_add(FindingList *list, const Finding *f);

/* Mappings. `label` names the input in the report (keep it relative, e.g. "drill/design.drl"). */
bool finding_from_dfm(FindingList *list, const char *label, const Board *board, const DfmProfile *profile, const DfmReport *report);
bool finding_from_excellon(FindingList *list, const char *label, const ExcFile *file);
bool finding_from_gerber(FindingList *list, const char *label, const GerberFile *file);
bool finding_from_board(FindingList *list, const char *label, const Board *board);

/* Sort (total order) and compute the verdict. */
void finding_sort(FindingList *list);
FindingVerdict finding_verdict(const FindingList *list);

/* The report as JSON ("copper.findings/1"): verdict, counts by status, one summary per source, and every finding. */
const char *finding_report_json(FindingList *list, Arena *arena, size_t *len);

#ifdef __cplusplus
}
#endif

#endif

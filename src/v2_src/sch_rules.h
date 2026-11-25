#ifndef COPPER_SCH_RULES_H
#define COPPER_SCH_RULES_H

/*
 * sch_rules.h - schematic and project rules S1 and S3 (docs/engineering/LAYOUT_RULES.md section 4, Step 3d).
 *
 *   sch_power_flags: one KiCad PWR_FLAG on each net that layout_hints.json lists under "power_flags" (the nets a
 *     block port with "external_supply" feeds: power that enters the board there). The PWR_FLAG symbol is copied
 *     verbatim from KiCad's own power library (symbols_root/power.kicad_sym) into lib_symbols, as KiCad does; each
 *     flag is a symbol instance in KiCad 9's own form (as in src/v2_src/tests/fixtures/sample_resistor_lol) with a
 *     label of the net at its pin (the pin is at the symbol's origin, length 0). Nowhere else: a power net that no
 *     output drives stays an ERC error. A schematic that already has a PWR_FLAG is left as it is (idempotent).
 *   sch_lib_tables: the project's sym-lib-table and fp-lib-table, one entry per library the schematic and board use,
 *     in KiCad 9's standard form (${KICAD9_SYMBOL_DIR}/NAME.kicad_sym, ${KICAD9_FOOTPRINT_DIR}/NAME.pretty), each
 *     marked with SCH_LIB_TABLE_DESCR so a table Copper did not write is never overwritten.
 *
 * Text in, text out (arena-owned); string-aware bracket scanning, no floating point; deterministic UUIDs from
 * SHA-256 of the sheet's UUID and the item.
 */

#include "arena.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum SchRulesStatus {
    SCH_RULES_OK = 0,
    SCH_RULES_INVALID_INPUT,   /* message says what */
    SCH_RULES_ERR_ARGUMENT,
    SCH_RULES_ERR_MEMORY
} SchRulesStatus;

typedef struct SchRulesReport {
    SchRulesStatus status;
    char message[300];
    size_t flags_added;        /* PWR_FLAGs written by this call */
    size_t flags_present;      /* PWR_FLAGs the schematic already had (then nothing is added) */
    size_t sym_libs, fp_libs;  /* entries in the tables */
} SchRulesReport;

#define SCH_LIB_TABLE_DESCR "Copper: KiCad 9 standard library"

SchRulesStatus sch_power_flags(Arena *arena, const char *sch, size_t sch_len, const char *hints, size_t hints_len,
                               const char *power_lib, size_t power_lib_len, const char **out, size_t *out_len,
                               SchRulesReport *report);

SchRulesStatus sch_lib_tables(Arena *arena, const char *sch, size_t sch_len, const char *pcb, size_t pcb_len,
                              const char **sym_table, size_t *sym_len, const char **fp_table, size_t *fp_len,
                              SchRulesReport *report);

/* True when every library entry of an existing table carries SCH_LIB_TABLE_DESCR (Copper wrote it). */
bool sch_lib_table_is_copper(const char *text, size_t len);

const char *sch_rules_status_name(SchRulesStatus status);

#ifdef __cplusplus
}
#endif

#endif

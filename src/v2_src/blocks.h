/*
 * blocks.h - circuit block definitions, format "copper.block/1" (JSON; one block per file, blocks_library/).
 *
 * A block is a small circuit with a fixed topology (an LED with its resistor, an LDO with its capacitors, an ESP32
 * module with its reset circuit...). Blocks are data: this module loads and checks a definition and never designs
 * anything. The composer (Phase 2c) joins checked blocks into a design. PIPELINE_HARD_RULES.md H2: every default value
 * and every formula names a source.
 *
 *   {
 *     "format": "copper.block/1",
 *     "name": "led_indicator",                          identifier [a-z][a-z0-9_]*
 *     "summary": "...",
 *     "sources": [ {"id": "s1", "document": "...", "url": "...", "standard": "...", "revision": "...", "page": 2} ],
 *                                                       document and one of url / standard are required
 *     "parameters": [ {"name": "led_current", "unit": "uA", "required": false, "default": 2000, "source": "s1",
 *                      "min": 1, "max": 20000} ],       a parameter that is not required has a default and its source
 *     "ports": [ {"name": "drive", "kind": "gpio"},
 *                {"name": "vin", "kind": "power_in", "voltage_min": 4500000, "voltage_max": 6000000, "source": "s2"},
 *                {"name": "vout", "kind": "power_out", "voltage_parameter": "rail"},
 *                {"name": "gnd", "kind": "ground"} ],
 *     "nets": ["led_a"],                                internal nets
 *     "parts": [ {"role": "led", "prefix": "D", "select": {"part_id": "led_0603_red_kingbright_apt1608ec"},
 *                 "pins": {"anode": "led_a", "cathode": "gnd"}},
 *                {"role": "resistor", "prefix": "R", "select": {"kind": "R", "package": "0603", "value": {"formula": "r_led"}},
 *                 "pins": {"terminal_1": "drive", "terminal_2": "led_a"}} ],
 *     "formulas": [ {"name": "r_led", "function": "led_series_resistor", "source": "s1",
 *                    "inputs": {"v_drive": "drive_voltage", "v_forward": "led_vf", "current": "led_current"}} ],
 *     "notes": [ {"text": "...", "source": "s1"} ]
 *   }
 *
 * Numbers are integers in the unit named: uV, uA, uW, uohm, pF, nH, mC (milli-degrees C), ppm, ns. Port kinds:
 * power_in, power_out, ground, gpio, i2c_scl, i2c_sda, uart_tx, uart_rx, analog, signal. A pin connects to a port, an
 * internal net, or "nc". Every port and every terminal of a part is connected; an internal net joins at least two pins.
 * Formula functions are a fixed C table (blocks.c), each with named, unit-typed inputs and an output unit; a part value
 * from a formula or parameter must have the part kind's unit (R uohm, C pF, L nH).
 *
 * With part catalogues (parts_library), each "part_id" must exist, its kind must match the prefix, and the pin keys
 * must be exactly the part's pin-map terminals. A kind/package selection must match at least one catalogue part.
 * Without catalogues those checks are NOT_RUN and the verdict is at best UNVERIFIED.
 */
#ifndef COPPER_BLOCKS_H
#define COPPER_BLOCKS_H

#include "arena.h"
#include "part_catalogue.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum BlockStatus {
    BLOCK_VALID = 0,        /* every check ran and passed */
    BLOCK_UNVERIFIED,       /* valid, but the catalogue checks did not run */
    BLOCK_INVALID,          /* one or more findings */
    BLOCK_ERR_ARGUMENT,
    BLOCK_ERR_MEMORY
} BlockStatus;

typedef struct BlockFinding {
    const char *code;       /* static, e.g. "UNKNOWN_KEY" */
    const char *path;       /* Arena-owned, e.g. "parts[1].pins.anode" */
    const char *message;    /* Arena-owned */
} BlockFinding;

typedef struct BlockReport {
    BlockStatus verdict;
    const char *source;     /* the file, or "text" */
    const char *name;       /* the block name when readable, else NULL */
    bool catalogue_checked;
    size_t parameters, ports, nets, parts, formulas;
    BlockFinding *findings;
    size_t finding_count;
} BlockReport;

/* Check one block definition. `catalogues` may be NULL (count 0): the catalogue checks are then NOT_RUN. */
BlockStatus block_check_text(Arena *arena, const char *json, size_t length, const PartCatalogue *const *catalogues,
                             size_t catalogue_count, BlockReport *out);
BlockStatus block_check_file(Arena *arena, const char *path, const PartCatalogue *const *catalogues,
                             size_t catalogue_count, BlockReport *out);

/* The report as JSON ("copper.block-check/1"), Arena-owned. */
const char *block_report_json(Arena *arena, const BlockReport *report, size_t *length);

const char *block_status_name(BlockStatus status);

/* The formula table, for documentation and the composer: function name, output unit and input names/units. */
typedef struct BlockFunctionInput { const char *name; const char *unit; } BlockFunctionInput;
typedef struct BlockFunction {
    const char *name;
    const char *output_unit;
    const char *what;       /* one line: the formula and its standard source */
    BlockFunctionInput inputs[6];
    size_t input_count;
} BlockFunction;
const BlockFunction *block_functions(size_t *count);

#ifdef __cplusplus
}
#endif

#endif

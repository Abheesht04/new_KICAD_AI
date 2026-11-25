/*
 * compose.h - join checked circuit blocks (copper.block/1) into a design intent (copper.design-intent v1).
 *
 * Input, "copper.compose/1":
 *   {
 *     "format": "copper.compose/1",
 *     "project": {"name": "Esp32S3Led", "board": {"width_mm": 60, "height_mm": 70}},
 *     "blocks": [ {"id": "power", "block": "power_input_header", "parameters": {"supply_voltage": 5000000}},
 *                 {"id": "ldo", "block": "ldo_3v3"}, {"id": "mcu", "block": "esp32s3_core"}, ... ],
 *     "nets":   [ {"name": "+5V", "ports": ["power.vout", "ldo.vin"]},
 *                 {"name": "GND", "ports": ["power.gnd", "ldo.gnd", "mcu.gnd"]},
 *                 {"ports": ["mcu.io4", "led.drive"]} ]           a name is optional
 *   }
 *
 * The composer decides nothing a block or the request does not state, and guesses nothing (PIPELINE_HARD_RULES.md H1-H3):
 *   - every block must be VALID against the part catalogues (blocks.c);
 *   - nets: a port is in at most one net; ground only with ground; at most one power source (power_out) per net;
 *     every power_in has a source whose voltage range lies inside the input's range; one I2C pull-up pair per bus;
 *     SCL never with SDA; two ports of one block are never joined; every power_in and ground port is connected;
 *   - current budget: per source, the sum of the declared demands (a regulator's input follows its output) against
 *     the source's current_max; a demand that is not declared makes the budget a lower bound (UNVERIFIED);
 *   - parameters: given in the request, a block default with its source, or derived ("from") from what a port is
 *     connected to; a value nobody gives is a question (NEEDS_INPUT), never a guess;
 *   - formulas (led_series_resistor, i2c_pullup, ldo_dissipation, rc_time_constant) are computed in integers; a part
 *     chosen by value must exist in a catalogue (else NOT_IMPLEMENTED, naming the value to add).
 *
 * Output: the intent JSON (every symbol pin with its library name, pads, nets; derived KiCad symbols are written as
 * their base symbol, which has the same pins) and a report of every finding and computed value with its source.
 * The intent is meant to be checked again by the grounding gate (intent_check.c), which is independent code (H6).
 * Board positions are not chosen here (placement is a later step); the compiler's default grid applies.
 */
#ifndef COPPER_COMPOSE_H
#define COPPER_COMPOSE_H

#include "arena.h"
#include "part_catalogue.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ComposeStatus {
    COMPOSE_COMPOSED = 0,       /* an intent was written; there may be UNVERIFIED or INFO findings */
    COMPOSE_NEEDS_INPUT,        /* questions for the user; nothing written */
    COMPOSE_NOT_IMPLEMENTED,    /* a part the design needs is not in the catalogues; nothing written */
    COMPOSE_REFUSED,            /* an engineering check failed; nothing written */
    COMPOSE_INVALID_INPUT,      /* the compose file or a block is malformed or invalid */
    COMPOSE_ERR_ARGUMENT,
    COMPOSE_ERR_MEMORY
} ComposeStatus;

typedef enum ComposeLevel {
    COMPOSE_FAIL = 0,
    COMPOSE_INVALID,
    COMPOSE_NOT_IMPL,
    COMPOSE_QUESTION,
    COMPOSE_UNVERIFIED,
    COMPOSE_INFO
} ComposeLevel;

typedef struct ComposeFinding {
    ComposeLevel level;
    const char *code;           /* static */
    const char *where;          /* Arena-owned: "ldo.vin", "nets[2]", "led1.r_led"... */
    const char *message;        /* Arena-owned */
} ComposeFinding;

typedef struct ComposeValue {
    const char *where;          /* "led1.led_current", "led1.r_led"... */
    const char *value;          /* "2000 uA", "680 ohm"... */
    const char *origin;         /* "request", "default", "derived", "computed", "chosen" */
    const char *source;         /* the source document, or how it was derived */
} ComposeValue;

typedef struct ComposeReport {
    ComposeStatus status;
    ComposeFinding *findings;
    size_t finding_count;
    ComposeValue *values;
    size_t value_count;
    size_t blocks, nets, components;
    const char *intent_json;    /* set when status == COMPOSE_COMPOSED */
    size_t intent_len;
    /* Layout hints for placement and routing ("copper.layout-hints/1"), set with the intent:
     *   {"keepouts": [{"reference", "rect_um": [x1, y1, x2, y2] in the footprint frame, "why"}],
     *    "decoupling": [{"capacitor", "net", "ic"}]}   a block capacitor between a power port and ground, and the
     *                                                    block's IC on that net: the placer pulls them together */
    const char *hints_json;
    size_t hints_len;
} ComposeReport;

typedef struct ComposeInputs {
    const char *blocks_dir;                 /* <dir>/<block>.block.json */
    const PartCatalogue *const *catalogues;
    size_t catalogue_count;
    const char *symbols_root;               /* KiCad symbol libraries: <root>/<Lib>.kicad_sym */
    const char *prompt;                     /* the user's request, or NULL: then evidence is not checked */
} ComposeInputs;

/* Evidence (Step 2d): with a prompt, every value the compose file states (a block parameter "<id>.<name>", or
 * "project.board.width_mm" / "project.board.height_mm") needs an "evidence" item {"field", "quote"} whose quote occurs
 * in the prompt (case and white space ignored) and contains the value, directly or as a k/M/m/u multiple (the rule of
 * template_gen.c). A value without one is IGNORED and reported, and then comes from the block's default or becomes a
 * question. The board size is required: without it the result is NEEDS_INPUT (placement may size it later). */

ComposeStatus compose_design(Arena *arena, const char *compose_json, size_t length, const ComposeInputs *in,
                             ComposeReport *out);

/* The report as JSON ("copper.compose-report/1"), Arena-owned. */
const char *compose_report_json(Arena *arena, const ComposeReport *report, size_t *length);

const char *compose_status_name(ComposeStatus status);
const char *compose_level_name(ComposeLevel level);

#ifdef __cplusplus
}
#endif

#endif

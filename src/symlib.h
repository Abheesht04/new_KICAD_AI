#ifndef SYMLIB_H
#define SYMLIB_H
/*
 * symlib.h   Owner: azure   Status: kernel (new)
 *
 * Reads KiCad .kicad_sym libraries (the user's own KiCad install, read at
 * run time; nothing from them is copied into this repo) into plain C data.
 *
 * What a symbol DOES give us: pin numbers, pin names, pin electrical types,
 * symbol-local pin positions, the Reference prefix, keywords, in_bom/on_board,
 * a Footprint name and ki_fp_filters (candidate footprint globs, e.g. "R_*").
 * What it does NOT give us: pad geometry (that is the .kicad_mod footprint)
 * nor any electrical value/model or rating. The Description text may mention
 * "100V 0.15A"; that is free text and is NEVER parsed into a number here.
 * Ratings come from the part layer (structured, user/vendor supplied).
 *
 * COORDINATES: symbol-local, integer nm (decimal text parsed exactly, no
 * double), Y axis UP as stored in .kicad_sym. Flip Y when placing on a
 * .kicad_sch sheet (Y down).
 *
 * PIN GEOMETRY: (at X Y A) is the pin's CONNECTION point (where a wire
 * attaches). The pin line runs from there in direction A (0=right, 90=up,
 * 180=left, 270=down, y-up) for `length`, ending at the body.
 *
 * EXTENDS: in real libraries most symbols are stubs, e.g. Diode:1N4148 is
 * just (extends "1N4001") + new properties; it has no pins of its own.
 * symlib_load_file() resolves this: after load, every SymbolDef carries the
 * inherited pins/units/flags and the merged properties (child overrides
 * parent). A missing parent or a cycle is an ERROR, never a silent empty
 * symbol.
 *
 * Memory: everything is allocated from the caller's Arena; nothing to free.
 * Strings in SymLib/SymbolDef/SymPin live as long as that arena.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arena.h"
#include "geom.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SYM_PIN_INPUT = 0, SYM_PIN_OUTPUT, SYM_PIN_BIDIRECTIONAL, SYM_PIN_TRISTATE,
    SYM_PIN_PASSIVE, SYM_PIN_FREE, SYM_PIN_UNSPECIFIED,
    SYM_PIN_POWER_IN, SYM_PIN_POWER_OUT,
    SYM_PIN_OPEN_COLLECTOR, SYM_PIN_OPEN_EMITTER, SYM_PIN_NO_CONNECT
} SymPinType;

typedef struct {
    const char *number;           /* "1", "A3", "VCC"... a string, not an int */
    const char *name;             /* "~" means unnamed in KiCad */
    SymPinType type;
    Pt pos;                       /* connection point, nm, y-up, symbol-local */
    int16_t angle_deg;            /* 0 / 90 / 180 / 270 only */
    Nm length;
    uint8_t unit;                 /* 0 = common to all units, else 1.. */
    uint8_t body_style;           /* 0 = common, else 1.. (De Morgan etc.) */
    bool hidden;
} SymPin;

typedef struct {
    const char *name;             /* "Reference", "Value", "Footprint", ... */
    const char *value;
} SymProp;

/* Bits of SymbolDef.set_mask: which flags the symbol itself declared.
 * Internal to resolve (a child inherits what it did not declare). */
enum {
    SYM_SET_IN_BOM = 1u << 0, SYM_SET_ON_BOARD = 1u << 1,
    SYM_SET_EXCL_SIM = 1u << 2, SYM_SET_POWER = 1u << 3,
    SYM_SET_PIN_NUMBERS = 1u << 4, SYM_SET_PIN_NAMES = 1u << 5,
    SYM_SET_PIN_OFFSET = 1u << 6
};

typedef struct {
    const char *name;             /* "R", "1N4148" */
    const char *extends;          /* parent name as written, or NULL */
    SymProp *props;   size_t prop_count;   /* merged after resolve */
    SymPin  *pins;    size_t pin_count;    /* inherited after resolve */
    uint8_t unit_count;           /* max unit number (1 for single-unit) */
    bool pin_numbers_hidden;
    bool pin_names_hidden;
    Nm   pin_name_offset;
    bool in_bom, on_board, exclude_from_sim, is_power;
    /* Graphics bounding box, nm, y-up. Built from rectangle, polyline,
     * circle, bezier control points and arc start/mid/end points. The arc
     * contribution is APPROXIMATE (mid point is not the extremum): treat the
     * box as a placement hint, not a collision proof. */
    Pt   body_min, body_max;
    bool has_body;                /* false: no graphics, box is 0,0 */
    uint32_t set_mask;
} SymbolDef;

typedef struct {
    const char *source_path;
    SymbolDef *symbols;           /* sorted by name (strcmp) -> deterministic */
    size_t symbol_count;
} SymLib;

typedef enum {
    SYM_OK = 0,
    SYM_ERR_ARG,
    SYM_ERR_IO,
    SYM_ERR_PARSE,        /* not valid S-expression */
    SYM_ERR_FORMAT,       /* valid S-expr, not a valid symbol library */
    SYM_ERR_NUMBER,       /* bad/out-of-range coordinate, angle or unit */
    SYM_ERR_DUPLICATE,    /* two top-level symbols with one name */
    SYM_ERR_EXTENDS,      /* parent missing, or extends cycle/too deep */
    SYM_ERR_OOM
} SymStatus;

typedef struct {
    SymStatus code;
    size_t line, column;          /* parse errors only */
    const char *message;          /* static string */
    const char *symbol;           /* offending symbol name (arena) or NULL */
} SymError;

/* Load one .kicad_sym and resolve extends. Returns SYM_OK or an error code
 * (also stored in err when non-NULL). On error `out` is zeroed. */
SymStatus symlib_load_file(Arena *arena, const char *path, SymLib *out,
                           SymError *err);
/* Same, from memory (tests). `source_name` is only stored for messages. */
SymStatus symlib_load_text(Arena *arena, const char *text, size_t len,
                           const char *source_name, SymLib *out, SymError *err);

const char *symstatus_string(SymStatus s);

/* Binary search by exact name; NULL if absent. */
const SymbolDef *symlib_find(const SymLib *lib, const char *name);
/* Property value or NULL. */
const char *symdef_prop(const SymbolDef *s, const char *prop_name);
/* First pin with this number in `unit` (or a common unit-0 pin); NULL if none. */
const SymPin *symdef_pin_by_number(const SymbolDef *s, const char *number,
                                   uint8_t unit);
/* Pin line end at the body, from pos/angle/length; table lookup, no trig. */
Pt sympin_body_end(const SymPin *p);

/* Glob with '*' and '?' only, ASCII case-insensitive. (KiCad's footprint
 * filter appears case-insensitive; UNVERIFIED against KiCad source.) */
bool symlib_glob_match(const char *pattern, const char *text);
/* ki_fp_filters is a space-separated glob list. True if `footprint_name`
 * (library prefix already stripped) matches any glob. A symbol with no
 * filters returns false: "no filter" is not "everything fits". */
bool symdef_fp_filter_match(const SymbolDef *s, const char *footprint_name);

#ifdef __cplusplus
}
#endif

#endif

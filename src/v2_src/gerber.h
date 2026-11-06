#ifndef COPPER_V2_GERBER_H
#define COPPER_V2_GERBER_H

/*
 * gerber.h - a Gerber RS-274X / X2 reader for KiCad 9 output, in exact integer nanometres (Copper V2 track A).
 *
 * Supported: FS (leading-zero omission, absolute, n.m digits), MO (mm exact; inch rounded to the nearest nm and
 * flagged), AD with standard apertures C, R, O, P and macro apertures, AM macros (primitives 0 comment, 1 circle,
 * 4 outline, 20 vector line, 21 centre line; $n variables, $n=... definitions and + - x / expressions), D01 / D02 /
 * D03, Dnn aperture selection, G01, G36 / G37 regions, LPD / LPC polarity, G04 comments, M02, and the X2
 * attributes TF (file), TA (aperture), TO (object: .N net, .P pin, .C component) and TD.
 * Reported, not guessed: G02 / G03 circular interpolation (the draw is recorded as unsupported with its line),
 * incremental coordinates and trailing-zero format (errors), unknown commands, macro primitives 5 and 7.
 *
 * Output: flashes (an aperture at a point), draws (a circular aperture swept along a line: a G2 capsule) and
 * region contours (G2 polygons), each with its polarity, line number and object attributes. Aperture shapes are
 * built at the origin (G2 circles, rounded rectangles, quads, polygons): exact for mm data, axis-aligned rectangles
 * and multiples of 90 degrees; otherwise marked inexact. Coordinates are the file's (y up). Errors and issues carry
 * line numbers. Memory: the caller's Arena. No floating point, no printf, no exit.
 */

#include "arena.h"
#include "g2_types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum GerberStatus {
    GERBER_OK = 0,          /* parsed, no ERROR issue */
    GERBER_INVALID_ARGUMENT,
    GERBER_IO_ERROR,
    GERBER_OUT_OF_MEMORY,
    GERBER_CONTENT_ERROR    /* parsed to the end, but with ERROR issues (see issues) */
} GerberStatus;

const char *gerber_status_name(GerberStatus status);

typedef enum GerberSeverity { GERBER_WARNING = 0, GERBER_ERROR } GerberSeverity;

typedef enum GerberIssueCode {
    GERBER_ISSUE_SYNTAX = 0,             /* ERROR: a word without '*', an unterminated '%', a bad number */
    GERBER_ISSUE_NO_FORMAT,              /* ERROR: coordinates before FS / MO */
    GERBER_ISSUE_UNSUPPORTED_FORMAT,     /* ERROR: incremental or trailing-zero FS */
    GERBER_ISSUE_UNDEFINED_APERTURE,     /* ERROR: D01/D03 with no (defined) aperture selected */
    GERBER_ISSUE_D03_IN_REGION,          /* ERROR */
    GERBER_ISSUE_BAD_APERTURE,           /* ERROR: an AD the reader cannot build */
    GERBER_ISSUE_BAD_MACRO,              /* ERROR: an AM that does not parse or evaluate */
    GERBER_ISSUE_COORD_RANGE,            /* ERROR: beyond +-1 m */
    GERBER_ISSUE_ARC_UNSUPPORTED,        /* WARNING: G02/G03 draw, recorded as unsupported */
    GERBER_ISSUE_UNSUPPORTED_COMMAND,    /* WARNING: ignored, with its line */
    GERBER_ISSUE_UNSUPPORTED_PRIMITIVE,  /* WARNING: macro primitive 5 or 7: that aperture is incomplete */
    GERBER_ISSUE_NON_CIRCULAR_DRAW,      /* WARNING: D01 with a non-circular aperture: recorded, no shape */
    GERBER_ISSUE_ROUNDED,                /* WARNING: inch data or > 6 decimals, rounded to the nearest nm */
    GERBER_ISSUE_INEXACT_ROTATION,       /* WARNING: an aperture or primitive rotation not a multiple of 90 */
    GERBER_ISSUE_MISSING_M02,            /* WARNING */
    GERBER_ISSUE_REGION_NOT_CLOSED       /* ERROR: a contour whose last point is not its first */
} GerberIssueCode;

const char *gerber_issue_name(GerberIssueCode code);

typedef struct GerberIssue {
    GerberIssueCode code;
    GerberSeverity severity;
    size_t line;
    const char *detail;          /* e.g. the command text; may be NULL */
} GerberIssue;

typedef struct GerberAttribute {
    const char *name;            /* e.g. ".FileFunction", ".AperFunction", ".N" */
    const char **values;
    size_t value_count;
} GerberAttribute;

typedef enum GerberApertureType {
    GERBER_AP_CIRCLE = 0, GERBER_AP_RECT, GERBER_AP_OBROUND, GERBER_AP_POLYGON, GERBER_AP_MACRO
} GerberApertureType;

typedef struct GerberAperture {
    int number;                  /* Dnn, nn >= 10 */
    GerberApertureType type;
    const char *macro;           /* macro name for GERBER_AP_MACRO */
    G2Nm params[16];             /* as given, in nm (counts and degrees x 1e6 also as plain scaled numbers) */
    size_t param_count;
    G2Shape *shapes;             /* the aperture at the origin: the union of these dark shapes */
    size_t shape_count;
    bool exact;
    G2Aabb bounds;               /* of the shapes */
    GerberAttribute *attributes; /* TA attributes in force at the AD */
    size_t attribute_count;
    size_t line;
} GerberAperture;

typedef enum GerberObjectKind { GERBER_FLASH = 0, GERBER_DRAW, GERBER_REGION, GERBER_ARC } GerberObjectKind;

typedef struct GerberObject {
    GerberObjectKind kind;
    bool dark;                   /* LPD (true) or LPC */
    size_t aperture;             /* index into apertures; SIZE_MAX for regions */
    G2Point at;                  /* flash point; draw/arc start */
    G2Point end;                 /* draw/arc end */
    G2Point centre_offset;       /* arcs: I, J as given */
    const G2Point *contour;      /* regions: vertices, the last equal to the first */
    size_t contour_count;
    bool has_capsule;            /* draws with a circular aperture */
    G2Capsule capsule;
    const char *net;             /* TO.N (first value), or NULL */
    const char *pin_ref, *pin_number;   /* TO.P */
    const char *component;       /* TO.C */
    size_t line;
} GerberObject;

typedef enum GerberUnits { GERBER_UNITS_UNKNOWN = 0, GERBER_UNITS_MM, GERBER_UNITS_INCH } GerberUnits;

typedef struct GerberFile {
    GerberUnits units;
    int int_digits, dec_digits;
    GerberAttribute *attributes; size_t attribute_count;   /* TF */
    GerberAperture *apertures;   size_t aperture_count;
    GerberObject *objects;       size_t object_count;
    GerberIssue *issues;         size_t issue_count;
    size_t error_count, warning_count;
    bool saw_m02;
    size_t flash_count, draw_count, region_count, arc_count;
} GerberFile;

GerberStatus gerber_parse(Arena *arena, const char *text, size_t len, GerberFile *out);
GerberStatus gerber_parse_path(Arena *arena, const char *path, GerberFile *out);

/* The TF attribute `name` (e.g. ".FileFunction"), or NULL. */
const GerberAttribute *gerber_file_attribute(const GerberFile *file, const char *name);
/* The aperture with D-code `number`, or NULL. */
const GerberAperture *gerber_find_aperture(const GerberFile *file, int number);
/* Shape k of a flash, moved to the flash point. */
G2Status gerber_flash_shape(const GerberFile *file, const GerberObject *flash, size_t k, G2Shape *out);

/* Profile outline: the centrelines of all draws chained into loops by exact end points. */
typedef struct GerberOutline {
    size_t segments, loops, open_loops;
    bool closed;                 /* at least one segment and every loop closed */
    G2Aabb bounds;               /* of the centrelines */
} GerberOutline;

GerberStatus gerber_outline(Arena *arena, const GerberFile *file, GerberOutline *out);

#ifdef __cplusplus
}
#endif

#endif

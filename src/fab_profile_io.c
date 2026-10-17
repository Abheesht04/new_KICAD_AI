#include "fab_profile_io.h"

#include <ctype.h>
#include <math.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static char *trim(char *text)
{
    while (*text && isspace((unsigned char)*text)) {
        ++text;
    }

    char *end = text + strlen(text);

    while (end > text && isspace((unsigned char)end[-1])) {
        --end;
    }

    *end = '\0';
    return text;
}

static int parse_double_value(
    const char *value,
    double *out
)
{
    if (!value || !out) {
        return 0;
    }

    errno = 0;
    char *end = NULL;
    double parsed = strtod(value, &end);

    if (end == value || errno == ERANGE || !isfinite(parsed) || parsed < 0) {
        return 0;
    }

    end = trim(end);

    if (*end != '\0') {
        return 0;
    }

    *out = parsed;
    return 1;
}

static int parse_uint_value(
    const char *value,
    unsigned long *out
)
{
    if (!value || !out) {
        return 0;
    }

    errno = 0;
    char *end = NULL;
    unsigned long parsed = strtoul(value, &end, 10);

    if (end == value || errno == ERANGE || value[0] == '-') {
        return 0;
    }

    end = trim(end);

    if (*end != '\0') {
        return 0;
    }

    *out = parsed;
    return 1;
}

static int assign_double(
    const char *key,
    const char *value,
    FabProfile *profile
)
{
#define MATCH_DOUBLE(field) \
    if (strcmp(key, #field) == 0) return parse_double_value(value, &profile->field)

    MATCH_DOUBLE(min_board_width_mm);
    MATCH_DOUBLE(max_board_width_mm);
    MATCH_DOUBLE(min_board_height_mm);
    MATCH_DOUBLE(max_board_height_mm);
    MATCH_DOUBLE(min_trace_width_mm);
    MATCH_DOUBLE(min_trace_clearance_mm);
    MATCH_DOUBLE(min_via_diameter_mm);
    MATCH_DOUBLE(min_via_drill_mm);
    MATCH_DOUBLE(min_via_annular_ring_mm);
    MATCH_DOUBLE(min_pad_annular_ring_mm);
    MATCH_DOUBLE(min_hole_clearance_mm);
    MATCH_DOUBLE(min_copper_to_edge_mm);
    MATCH_DOUBLE(max_component_height_mm);

#undef MATCH_DOUBLE
    return -1;
}

int fab_profile_write(FILE *out, const FabProfile *profile)
{
    if (!out || !profile) {
        return -1;
    }

    if (fprintf(
            out,
            "# Deterministic fab capability profile\n"
            "schema_version=1\n"
            "name=%s\n"
            "max_layer_count=%u\n"
            "min_board_width_mm=%.9g\n"
            "max_board_width_mm=%.9g\n"
            "min_board_height_mm=%.9g\n"
            "max_board_height_mm=%.9g\n"
            "min_trace_width_mm=%.9g\n"
            "min_trace_clearance_mm=%.9g\n"
            "min_via_diameter_mm=%.9g\n"
            "min_via_drill_mm=%.9g\n"
            "min_via_annular_ring_mm=%.9g\n"
            "min_pad_annular_ring_mm=%.9g\n"
            "min_hole_clearance_mm=%.9g\n"
            "min_copper_to_edge_mm=%.9g\n"
            "max_component_height_mm=%.9g\n",
            profile->name,
            (unsigned)profile->max_layer_count,
            profile->min_board_width_mm,
            profile->max_board_width_mm,
            profile->min_board_height_mm,
            profile->max_board_height_mm,
            profile->min_trace_width_mm,
            profile->min_trace_clearance_mm,
            profile->min_via_diameter_mm,
            profile->min_via_drill_mm,
            profile->min_via_annular_ring_mm,
            profile->min_pad_annular_ring_mm,
            profile->min_hole_clearance_mm,
            profile->min_copper_to_edge_mm,
            profile->max_component_height_mm) < 0) {
        return -1;
    }

    return ferror(out) ? -1 : 0;
}

int fab_profile_write_file(const char *path, const FabProfile *profile)
{
    if (!path) {
        return -1;
    }

    FILE *out = fopen(path, "wb");

    if (!out) {
        return -1;
    }

    int result = fab_profile_write(out, profile);

    if (fclose(out) != 0) {
        return -1;
    }

    return result;
}

int fab_profile_read(FILE *in, FabProfile *profile)
{
    if (!in || !profile) {
        return -1;
    }

    *profile = (FabProfile){0};

    char line[512];
    unsigned long schema_version = 0;
    int saw_schema = 0;
    int saw_name = 0;

    while (fgets(line, sizeof(line), in)) {
        char *text = trim(line);

        if (*text == '\0' || *text == '#') {
            continue;
        }

        char *equals = strchr(text, '=');

        if (!equals) {
            return -1;
        }

        *equals = '\0';

        const char *key = trim(text);
        const char *value = trim(equals + 1);

        if (strcmp(key, "schema_version") == 0) {
            if (!parse_uint_value(value, &schema_version)) {
                return -1;
            }

            saw_schema = 1;
            continue;
        }

        if (strcmp(key, "name") == 0) {
            size_t length = strlen(value);

            if (length >= FAB_PROFILE_NAME_MAX) {
                return -1;
            }

            memcpy(profile->name, value, length + 1);
            saw_name = 1;
            continue;
        }

        if (strcmp(key, "max_layer_count") == 0) {
            unsigned long value_uint = 0;

            if (!parse_uint_value(value, &value_uint) ||
                value_uint > 255UL) {
                return -1;
            }

            profile->max_layer_count = (uint8_t)value_uint;
            continue;
        }

        int double_result = assign_double(key, value, profile);

        if (double_result >= 0) {
            if (!double_result) {
                return -1;
            }
            continue;
        }

        return -1;
    }

    if (!saw_schema || schema_version != 1UL || !saw_name) {
        return -1;
    }

    return ferror(in) ? -1 : 0;
}

int fab_profile_read_file(const char *path, FabProfile *profile)
{
    if (!path) {
        return -1;
    }

    FILE *in = fopen(path, "rb");

    if (!in) {
        return -1;
    }

    int result = fab_profile_read(in, profile);
    fclose(in);
    return result;
}

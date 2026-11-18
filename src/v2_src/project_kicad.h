#ifndef COPPER_PROJECT_KICAD_H
#define COPPER_PROJECT_KICAD_H

#include "arena.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ProjectSettingsStatus {
    PROJECT_SETTINGS_OK = 0,
    PROJECT_SETTINGS_ERR_ARGUMENT,
    PROJECT_SETTINGS_ERR_IO,
    PROJECT_SETTINGS_ERR_JSON,
    PROJECT_SETTINGS_ERR_ROOT,
    PROJECT_SETTINGS_ERR_MEMORY,
    PROJECT_SETTINGS_ERR_RANGE
} ProjectSettingsStatus;

typedef enum ProjectJsonValueKind {
    PROJECT_JSON_NULL = 0,
    PROJECT_JSON_BOOLEAN,
    PROJECT_JSON_NUMBER,
    PROJECT_JSON_STRING,
    PROJECT_JSON_OBJECT,
    PROJECT_JSON_ARRAY
} ProjectJsonValueKind;

typedef struct ProjectSettingKV {
    const char *name;
    const char *value_json; /* original JSON literal, validated by the parser */
    ProjectJsonValueKind value_kind;
} ProjectSettingKV;

typedef struct ProjectNetClassIR {
    const char *name;
    int64_t clearance_nm;
    int64_t track_width_nm;
    int64_t diff_pair_width_nm;
    int64_t diff_pair_gap_nm;
    int64_t diff_pair_via_gap_nm;
    int64_t via_diameter_nm;
    int64_t via_drill_nm;
    int64_t microvia_diameter_nm;
    int64_t microvia_drill_nm;
    int64_t bus_width;
    int64_t priority;
    const char *raw_object_json;
} ProjectNetClassIR;

typedef struct ProjectSettingsIR {
    Arena *arena;
    uint32_t metadata_version;
    const char *filename;
    ProjectNetClassIR *net_classes;
    size_t net_class_count;
    ProjectSettingKV *board_rules;
    size_t board_rule_count;
    ProjectSettingKV *board_rule_severities;
    size_t board_rule_severity_count;
    ProjectSettingKV *erc_rule_severities;
    size_t erc_rule_severity_count;
    const char *source_json; /* full source retained in arena */
    size_t source_json_length;
} ProjectSettingsIR;

typedef struct ProjectSettingsDiag {
    ProjectSettingsStatus status;
    char message[192];
    size_t line;
    size_t column;
} ProjectSettingsDiag;

ProjectSettingsStatus project_settings_init(ProjectSettingsIR *out, Arena *arena);
ProjectSettingsStatus project_settings_parse(Arena *arena, const char *source,
                                               size_t source_length,
                                               ProjectSettingsIR *out,
                                               ProjectSettingsDiag *diag);
ProjectSettingsStatus project_settings_load_kicad(const char *path, Arena *arena,
                                                    ProjectSettingsIR *out,
                                                    ProjectSettingsDiag *diag);
const char *project_settings_status_name(ProjectSettingsStatus status);

#ifdef __cplusplus
}
#endif
#endif

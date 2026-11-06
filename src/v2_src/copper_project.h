#ifndef COPPER_NATIVE_KICAD_PROJECT_H
#define COPPER_NATIVE_KICAD_PROJECT_H

#include "arena.h"
#include "filesystem.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COPPER_PROJECT_NAME_MAX 128

typedef enum CopperProjectStatus {
  COPPER_PROJECT_OK = 0,
  COPPER_PROJECT_ERR_ARGUMENT,
  COPPER_PROJECT_ERR_INVALID_NAME,
  COPPER_PROJECT_ERR_ALREADY_EXISTS,
  COPPER_PROJECT_ERR_NOT_FOUND,
  COPPER_PROJECT_ERR_INVALID_PROJECT,
  COPPER_PROJECT_ERR_FILESYSTEM,
  COPPER_PROJECT_ERR_MEMORY,
  COPPER_PROJECT_ERR_UUID,
  COPPER_PROJECT_ERR_CLI_NOT_FOUND
} CopperProjectStatus;

typedef struct CopperProject {
  const char *name;
  const char *root_dir;
  const char *project_file;
  const char *schematic_file;
  const char *board_file;
  const char *copper_dir;
  const char *cache_dir;
  const char *logs_dir;
  const char *symbol_table_file;
  const char *footprint_table_file;
  const char *kicad_cli_path;
  bool has_schematic;
  bool has_board;
  bool has_symbol_table;
  bool has_footprint_table;
} CopperProject;

typedef struct CopperProjectCreateOptions {
  /* Parent directory. The project directory is created beneath this path. */
  const char *parent_directory;
  const char *project_name;

  /* Creates .copper/cache and .copper/logs when true. */
  bool create_copper_workspace;

  /* Optional explicit path to kicad-cli[.exe]. NULL triggers discovery. */
  const char *kicad_cli_path;
} CopperProjectCreateOptions;

typedef struct CopperProjectError {
  CopperProjectStatus code;
  FsStatus filesystem_code;
  const char *message;
} CopperProjectError;

/*
 * Creates a new native KiCad 9 project inside parent_directory/project_name.
 * All returned strings are allocated from arena and remain valid until reset.
 * Existing destinations are never overwritten.
 */
CopperProjectStatus copper_project_create(
    Arena *arena, const CopperProjectCreateOptions *options,
    CopperProject *out, CopperProjectError *error);

/* Opens an existing project directory. The .kicad_pro file is required. */
CopperProjectStatus copper_project_open(Arena *arena,
                                        const char *project_directory,
                                        CopperProject *out,
                                        CopperProjectError *error);

bool copper_project_name_is_valid(const char *name);

/*
 * Resolves an explicit path, KICAD_CLI_PATH, PATH, then common KiCad 9
 * Windows install locations. The resulting path is arena-owned.
 */
CopperProjectStatus copper_project_find_kicad_cli(
    Arena *arena, const char *explicit_path, const char **out_path);

const char *copper_project_status_name(CopperProjectStatus status);
const char *copper_project_status_message(CopperProjectStatus status);

#ifdef __cplusplus
}
#endif

#endif /* COPPER_NATIVE_KICAD_PROJECT_H */

#include "copper_project.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#pragma comment(lib, "ole32.lib")
#else
#include <errno.h>
#endif

#define COPPER_STAGING_SUFFIX ".copper-staging-"

static void set_error(CopperProjectError *error, CopperProjectStatus code,
                      FsStatus fs_code) {
  if (error == NULL) return;
  error->code = code;
  error->filesystem_code = fs_code;
  error->message = copper_project_status_message(code);
}

static CopperProjectStatus map_fs_status(FsStatus status) {
  switch (status) {
    case FS_OK:
      return COPPER_PROJECT_OK;
    case FS_ERR_ALREADY_EXISTS:
      return COPPER_PROJECT_ERR_ALREADY_EXISTS;
    case FS_ERR_NOT_FOUND:
      return COPPER_PROJECT_ERR_NOT_FOUND;
    case FS_ERR_OUT_OF_MEMORY:
      return COPPER_PROJECT_ERR_MEMORY;
    default:
      return COPPER_PROJECT_ERR_FILESYSTEM;
  }
}

static char *arena_strdup(Arena *arena, const char *text) {
  size_t length;
  char *copy;
  if (arena == NULL || text == NULL) return NULL;
  length = strlen(text);
  if (length == SIZE_MAX) return NULL;
  copy = (char *)arena_alloc(arena, length + 1);
  if (copy == NULL) return NULL;
  memcpy(copy, text, length + 1);
  return copy;
}

static bool make_project_path(Arena *arena, const char *root,
                              const char *filename, const char **out) {
  char buffer[FS_PATH_MAX];
  if (out == NULL) return false;
  *out = NULL;
  if (fs_path_join(buffer, sizeof(buffer), root, filename) != FS_OK)
    return false;
  *out = arena_strdup(arena, buffer);
  return *out != NULL;
}

bool copper_project_name_is_valid(const char *name) {
  size_t length;
  if (name == NULL) return false;
  length = strlen(name);
  if (length == 0 || length > COPPER_PROJECT_NAME_MAX) return false;
  if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return false;
  if (name[length - 1] == '.' || name[length - 1] == ' ') return false;

  for (size_t i = 0; i < length; ++i) {
    unsigned char c = (unsigned char)name[i];
    if (c < 32 || c == '/' || c == '\\' || c == ':' || c == '*' ||
        c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
      return false;
    }
  }

#ifdef _WIN32
  /* Windows reserves these device names, even when followed by an extension. */
  char base[16];
  size_t base_length = 0;
  while (base_length < length && name[base_length] != '.' &&
         base_length < sizeof(base) - 1) {
    base[base_length] = name[base_length];
    ++base_length;
  }
  base[base_length] = '\0';
  for (size_t i = 0; i < base_length; ++i) {
    if (base[i] >= 'a' && base[i] <= 'z') base[i] = (char)(base[i] - 'a' + 'A');
  }
  if (strcmp(base, "CON") == 0 || strcmp(base, "PRN") == 0 ||
      strcmp(base, "AUX") == 0 || strcmp(base, "NUL") == 0 ||
      (base_length == 4 &&
       (memcmp(base, "COM", 3) == 0 || memcmp(base, "LPT", 3) == 0) &&
       base[3] >= '1' && base[3] <= '9')) {
    return false;
  }
#endif
  return true;
}

static bool generate_uuid(char out[37]) {
  unsigned char bytes[16];
#ifdef _WIN32
  GUID guid;
  if (CoCreateGuid(&guid) != S_OK) return false;
  bytes[0] = (unsigned char)(guid.Data1 >> 24);
  bytes[1] = (unsigned char)(guid.Data1 >> 16);
  bytes[2] = (unsigned char)(guid.Data1 >> 8);
  bytes[3] = (unsigned char)guid.Data1;
  bytes[4] = (unsigned char)(guid.Data2 >> 8);
  bytes[5] = (unsigned char)guid.Data2;
  bytes[6] = (unsigned char)(guid.Data3 >> 8);
  bytes[7] = (unsigned char)guid.Data3;
  memcpy(bytes + 8, guid.Data4, 8);
#else
  FILE *random_file = fopen("/dev/urandom", "rb");
  if (random_file == NULL) return false;
  size_t got = fread(bytes, 1, sizeof(bytes), random_file);
  bool close_ok = fclose(random_file) == 0;
  if (got != sizeof(bytes) || !close_ok) return false;
#endif
  /* RFC 4122 version 4 / variant bits. UUIDs are identifiers, not secrets. */
  bytes[6] = (unsigned char)((bytes[6] & 0x0F) | 0x40);
  bytes[8] = (unsigned char)((bytes[8] & 0x3F) | 0x80);
  int written = snprintf(
      out, 37,
      "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
      bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6],
      bytes[7], bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13],
      bytes[14], bytes[15]);
  return written == 36;
}

static CopperProjectStatus build_project_paths(Arena *arena,
                                               const char *root_directory,
                                               const char *name,
                                               CopperProject *out) {
  out->name = arena_strdup(arena, name);
  out->root_dir = arena_strdup(arena, root_directory);
  if (out->name == NULL || out->root_dir == NULL) {
    return COPPER_PROJECT_ERR_MEMORY;
  }

  char filename[COPPER_PROJECT_NAME_MAX + 32];
  int n = snprintf(filename, sizeof(filename), "%s.kicad_pro", name);
  if (n < 0 || (size_t)n >= sizeof(filename) ||
      !make_project_path(arena, root_directory, filename, &out->project_file))
    return COPPER_PROJECT_ERR_MEMORY;
  n = snprintf(filename, sizeof(filename), "%s.kicad_sch", name);
  if (n < 0 || (size_t)n >= sizeof(filename) ||
      !make_project_path(arena, root_directory, filename, &out->schematic_file))
    return COPPER_PROJECT_ERR_MEMORY;
  n = snprintf(filename, sizeof(filename), "%s.kicad_pcb", name);
  if (n < 0 || (size_t)n >= sizeof(filename) ||
      !make_project_path(arena, root_directory, filename, &out->board_file))
    return COPPER_PROJECT_ERR_MEMORY;

  if (!make_project_path(arena, root_directory, "sym-lib-table",
                         &out->symbol_table_file) ||
      !make_project_path(arena, root_directory, "fp-lib-table",
                         &out->footprint_table_file)) {
    return COPPER_PROJECT_ERR_MEMORY;
  }

  char path_buffer[FS_PATH_MAX];
  if (fs_path_join(path_buffer, sizeof(path_buffer), root_directory, ".copper") !=
      FS_OK) {
    return COPPER_PROJECT_ERR_FILESYSTEM;
  }
  out->copper_dir = arena_strdup(arena, path_buffer);
  if (out->copper_dir == NULL ||
      !make_project_path(arena, out->copper_dir, "cache", &out->cache_dir) ||
      !make_project_path(arena, out->copper_dir, "logs", &out->logs_dir)) {
    return COPPER_PROJECT_ERR_MEMORY;
  }
  return COPPER_PROJECT_OK;
}

CopperProjectStatus copper_project_find_kicad_cli(
    Arena *arena, const char *explicit_path, const char **out_path) {
  char candidate[FS_PATH_MAX];
  const char *configured;
  FsStatus fs_status;
  if (out_path == NULL || arena == NULL) return COPPER_PROJECT_ERR_ARGUMENT;
  *out_path = NULL;

  if (explicit_path != NULL && explicit_path[0] != '\0') {
    if (!fs_is_executable(explicit_path))
      return COPPER_PROJECT_ERR_CLI_NOT_FOUND;
    fs_status = fs_path_absolute(candidate, sizeof(candidate), explicit_path);
    if (fs_status != FS_OK) return COPPER_PROJECT_ERR_FILESYSTEM;
    *out_path = arena_strdup(arena, candidate);
    return *out_path != NULL ? COPPER_PROJECT_OK : COPPER_PROJECT_ERR_MEMORY;
  }

  configured = getenv("KICAD_CLI_PATH");
  if (configured != NULL && configured[0] != '\0' &&
      fs_is_executable(configured)) {
    fs_status = fs_path_absolute(candidate, sizeof(candidate), configured);
    if (fs_status == FS_OK) {
      *out_path = arena_strdup(arena, candidate);
      return *out_path != NULL ? COPPER_PROJECT_OK : COPPER_PROJECT_ERR_MEMORY;
    }
  }

  if (fs_find_executable_on_path("kicad-cli", candidate, sizeof(candidate)) ==
      FS_OK) {
    char absolute_candidate[FS_PATH_MAX];
    fs_status = fs_path_absolute(absolute_candidate, sizeof(absolute_candidate),
                                 candidate);
    if (fs_status == FS_OK) {
      *out_path = arena_strdup(arena, absolute_candidate);
      return *out_path != NULL ? COPPER_PROJECT_OK : COPPER_PROJECT_ERR_MEMORY;
    }
  }

#ifdef _WIN32
  {
    const char *program_files = getenv("ProgramFiles");
    const char *program_files_x86 = getenv("ProgramFiles(x86)");
    const char *roots[3] = {program_files, program_files_x86, "C:\\Program Files"};
    const char *versions[] = {"9.0", "9.0\\bin"};
    for (size_t r = 0; r < 3; ++r) {
      if (roots[r] == NULL || roots[r][0] == '\0') continue;
      for (size_t v = 0; v < 2; ++v) {
        char install_dir[FS_PATH_MAX];
        char bin_dir[FS_PATH_MAX];
        if (fs_path_join(install_dir, sizeof(install_dir), roots[r], "KiCad") !=
                FS_OK ||
            fs_path_join(bin_dir, sizeof(bin_dir), install_dir, versions[v]) !=
                FS_OK ||
            fs_path_join(candidate, sizeof(candidate), bin_dir,
                         "kicad-cli.exe") != FS_OK) {
          continue;
        }
        if (fs_is_executable(candidate)) {
          *out_path = arena_strdup(arena, candidate);
          return *out_path != NULL ? COPPER_PROJECT_OK
                                   : COPPER_PROJECT_ERR_MEMORY;
        }
      }
    }
  }
#endif
  return COPPER_PROJECT_ERR_CLI_NOT_FOUND;
}

CopperProjectStatus copper_project_open(Arena *arena,
                                        const char *project_directory,
                                        CopperProject *out,
                                        CopperProjectError *error) {
  char absolute_root[FS_PATH_MAX];
  char name[COPPER_PROJECT_NAME_MAX + 1];
  CopperProjectStatus status;
  FsStatus fs_status;

  if (out == NULL || arena == NULL || project_directory == NULL ||
      project_directory[0] == '\0') {
    set_error(error, COPPER_PROJECT_ERR_ARGUMENT, FS_ERR_ARGUMENT);
    return COPPER_PROJECT_ERR_ARGUMENT;
  }
  memset(out, 0, sizeof(*out));
  fs_status = fs_path_absolute(absolute_root, sizeof(absolute_root),
                               project_directory);
  if (fs_status != FS_OK) {
    status = map_fs_status(fs_status);
    set_error(error, status, fs_status);
    return status;
  }
  if (!fs_directory_exists(absolute_root)) {
    set_error(error, COPPER_PROJECT_ERR_NOT_FOUND, FS_ERR_NOT_FOUND);
    return COPPER_PROJECT_ERR_NOT_FOUND;
  }
  fs_status = fs_path_basename(name, sizeof(name), absolute_root);
  if (fs_status != FS_OK || !copper_project_name_is_valid(name)) {
    set_error(error, COPPER_PROJECT_ERR_INVALID_NAME, fs_status);
    return COPPER_PROJECT_ERR_INVALID_NAME;
  }

  status = build_project_paths(arena, absolute_root, name, out);
  if (status != COPPER_PROJECT_OK) {
    set_error(error, status, FS_OK);
    return status;
  }
  if (!fs_file_exists(out->project_file)) {
    memset(out, 0, sizeof(*out));
    set_error(error, COPPER_PROJECT_ERR_INVALID_PROJECT, FS_ERR_NOT_FOUND);
    return COPPER_PROJECT_ERR_INVALID_PROJECT;
  }

  out->has_schematic = fs_file_exists(out->schematic_file);
  out->has_board = fs_file_exists(out->board_file);
  out->has_symbol_table = fs_file_exists(out->symbol_table_file);
  out->has_footprint_table = fs_file_exists(out->footprint_table_file);
  (void)copper_project_find_kicad_cli(arena, NULL, &out->kicad_cli_path);
  set_error(error, COPPER_PROJECT_OK, FS_OK);
  return COPPER_PROJECT_OK;
}

CopperProjectStatus copper_project_create(
    Arena *arena, const CopperProjectCreateOptions *options,
    CopperProject *out, CopperProjectError *error) {
  char parent[FS_PATH_MAX];
  char final_root[FS_PATH_MAX];
  char uuid[37];
  char stage_path[FS_PATH_MAX];
  char project_path[FS_PATH_MAX];
  char schematic_path[FS_PATH_MAX];
  char board_path[FS_PATH_MAX];
  char copper_path[FS_PATH_MAX];
  char cache_path[FS_PATH_MAX];
  char logs_path[FS_PATH_MAX];
  char project_text[512];
  char schematic_text[512];
  static const char board_text[] =
      "(kicad_pcb\n"
      "  (version 20241229)\n"
      "  (generator \"pcbnew\")\n"
      "  (general (thickness 1.6))\n"
      "  (paper \"A4\")\n"
      "  (layers\n"
      "    (0 \"F.Cu\" signal)\n"
      "    (2 \"B.Cu\" signal)\n"
      "    (3 \"B.Adhes\" user)\n"
      "    (5 \"F.SilkS\" user)\n"
      "    (7 \"B.SilkS\" user)\n"
      "    (9 \"F.Mask\" user)\n"
      "    (11 \"B.Mask\" user)\n"
      "    (13 \"Dwgs.User\" user)\n"
      "    (25 \"Edge.Cuts\" user)\n"
      "    (31 \"F.CrtYd\" user)\n"
      "    (33 \"B.CrtYd\" user)\n"
      "    (35 \"F.Fab\" user)\n"
      "    (37 \"B.Fab\" user)\n"
      "    (1 \"F.Adhes\" user)\n"
      "    (19 \"F.Paste\" user)\n"
      "    (21 \"B.Paste\" user)\n"
      "  )\n"
      "  (setup (pad_to_mask_clearance 0.05))\n"
      "  (net 0 \"\")\n"
      "  (embedded_fonts no)\n"
      ")\n";
  size_t project_text_length;
  size_t schematic_text_length;
  FsStatus fs_status;
  CopperProjectStatus status;

  if (out == NULL || arena == NULL || options == NULL ||
      options->project_name == NULL) {
    set_error(error, COPPER_PROJECT_ERR_ARGUMENT, FS_ERR_ARGUMENT);
    return COPPER_PROJECT_ERR_ARGUMENT;
  }
  if (!copper_project_name_is_valid(options->project_name)) {
    set_error(error, COPPER_PROJECT_ERR_INVALID_NAME, FS_ERR_ARGUMENT);
    return COPPER_PROJECT_ERR_INVALID_NAME;
  }
  memset(out, 0, sizeof(*out));

  const char *parent_input =
      options->parent_directory != NULL && options->parent_directory[0] != '\0'
          ? options->parent_directory
          : ".";
  fs_status = fs_path_absolute(parent, sizeof(parent), parent_input);
  if (fs_status != FS_OK) {
    status = map_fs_status(fs_status);
    set_error(error, status, fs_status);
    return status;
  }
  fs_status = fs_create_directories(parent);
  if (fs_status != FS_OK && fs_status != FS_ERR_ALREADY_EXISTS) {
    status = map_fs_status(fs_status);
    set_error(error, status, fs_status);
    return status;
  }
  fs_status = fs_path_join(final_root, sizeof(final_root), parent,
                           options->project_name);
  if (fs_status != FS_OK) {
    set_error(error, COPPER_PROJECT_ERR_FILESYSTEM, fs_status);
    return COPPER_PROJECT_ERR_FILESYSTEM;
  }
  if (fs_directory_exists(final_root) || fs_file_exists(final_root)) {
    set_error(error, COPPER_PROJECT_ERR_ALREADY_EXISTS,
              FS_ERR_ALREADY_EXISTS);
    return COPPER_PROJECT_ERR_ALREADY_EXISTS;
  }

  if (!generate_uuid(uuid)) {
    set_error(error, COPPER_PROJECT_ERR_UUID, FS_ERR_IO);
    return COPPER_PROJECT_ERR_UUID;
  }
  int n = snprintf(stage_path, sizeof(stage_path), "%s%s%.8s", final_root,
                   COPPER_STAGING_SUFFIX, uuid);
  if (n < 0 || (size_t)n >= sizeof(stage_path)) {
    set_error(error, COPPER_PROJECT_ERR_FILESYSTEM, FS_ERR_RANGE);
    return COPPER_PROJECT_ERR_FILESYSTEM;
  }
  fs_status = fs_create_directory(stage_path);
  if (fs_status != FS_OK) {
    status = map_fs_status(fs_status);
    set_error(error, status, fs_status);
    return status;
  }

  char filename[COPPER_PROJECT_NAME_MAX + 32];
  n = snprintf(filename, sizeof(filename), "%s.kicad_pro", options->project_name);
  if (n < 0 || (size_t)n >= sizeof(filename)) fs_status = FS_ERR_RANGE;
  if (fs_status == FS_OK)
    fs_status = fs_path_join(project_path, sizeof(project_path), stage_path,
                             filename);
  n = snprintf(filename, sizeof(filename), "%s.kicad_sch", options->project_name);
  if (n < 0 || (size_t)n >= sizeof(filename)) fs_status = FS_ERR_RANGE;
  if (fs_status == FS_OK)
    fs_status = fs_path_join(schematic_path, sizeof(schematic_path), stage_path,
                             filename);
  n = snprintf(filename, sizeof(filename), "%s.kicad_pcb", options->project_name);
  if (n < 0 || (size_t)n >= sizeof(filename)) fs_status = FS_ERR_RANGE;
  if (fs_status == FS_OK)
    fs_status = fs_path_join(board_path, sizeof(board_path), stage_path,
                             filename);
  if (fs_status != FS_OK) {
    (void)fs_remove_tree(stage_path);
    set_error(error, COPPER_PROJECT_ERR_FILESYSTEM, fs_status);
    return COPPER_PROJECT_ERR_FILESYSTEM;
  }

  n = snprintf(project_text, sizeof(project_text),
               "{\n  \"meta\": {\n    \"filename\": \"%s.kicad_pro\",\n"
               "    \"version\": 1\n  }\n}\n",
               options->project_name);
  if (n < 0 || (size_t)n >= sizeof(project_text)) {
    (void)fs_remove_tree(stage_path);
    set_error(error, COPPER_PROJECT_ERR_FILESYSTEM, FS_ERR_RANGE);
    return COPPER_PROJECT_ERR_FILESYSTEM;
  }
  project_text_length = (size_t)n;

  if (!generate_uuid(uuid)) {
    (void)fs_remove_tree(stage_path);
    set_error(error, COPPER_PROJECT_ERR_UUID, FS_ERR_IO);
    return COPPER_PROJECT_ERR_UUID;
  }
  n = snprintf(schematic_text, sizeof(schematic_text),
               "(kicad_sch\n"
               "  (version 20250114)\n"
               "  (generator \"eeschema\")\n"
               "  (uuid \"%s\")\n"
               "  (paper \"A4\")\n"
               "  (lib_symbols)\n"
               "  (sheet_instances (path \"/\" (page \"1\")))\n"
               "  (embedded_fonts no)\n"
               ")\n",
               uuid);
  if (n < 0 || (size_t)n >= sizeof(schematic_text)) {
    (void)fs_remove_tree(stage_path);
    set_error(error, COPPER_PROJECT_ERR_FILESYSTEM, FS_ERR_RANGE);
    return COPPER_PROJECT_ERR_FILESYSTEM;
  }
  schematic_text_length = (size_t)n;

  fs_status = fs_write_file_exclusive(project_path, project_text,
                                      project_text_length);
  if (fs_status == FS_OK)
    fs_status = fs_write_file_exclusive(schematic_path, schematic_text,
                                        schematic_text_length);
  if (fs_status == FS_OK)
    fs_status = fs_write_file_exclusive(board_path, board_text,
                                        sizeof(board_text) - 1);

  if (fs_status == FS_OK && options->create_copper_workspace) {
    if (fs_path_join(copper_path, sizeof(copper_path), stage_path, ".copper") !=
            FS_OK ||
        fs_path_join(cache_path, sizeof(cache_path), copper_path, "cache") !=
            FS_OK ||
        fs_path_join(logs_path, sizeof(logs_path), copper_path, "logs") !=
            FS_OK) {
      fs_status = FS_ERR_RANGE;
    } else {
      fs_status = fs_create_directories(cache_path);
      if (fs_status == FS_OK) fs_status = fs_create_directories(logs_path);
    }
  }

  if (fs_status != FS_OK) {
    (void)fs_remove_tree(stage_path);
    status = map_fs_status(fs_status);
    set_error(error, status, fs_status);
    return status;
  }

  fs_status = fs_rename_path(stage_path, final_root);
  if (fs_status != FS_OK) {
    (void)fs_remove_tree(stage_path);
    status = map_fs_status(fs_status);
    set_error(error, status, fs_status);
    return status;
  }

  status = copper_project_open(arena, final_root, out, error);
  if (status != COPPER_PROJECT_OK) return status;
  if (options->kicad_cli_path != NULL && options->kicad_cli_path[0] != '\0') {
    const char *cli_path = NULL;
    if (copper_project_find_kicad_cli(arena, options->kicad_cli_path,
                                      &cli_path) == COPPER_PROJECT_OK) {
      out->kicad_cli_path = cli_path;
    }
  }
  return COPPER_PROJECT_OK;
}

const char *copper_project_status_name(CopperProjectStatus status) {
  static const char *const names[] = {
      "OK", "ARGUMENT_ERROR", "INVALID_NAME", "ALREADY_EXISTS",
      "NOT_FOUND", "INVALID_PROJECT", "FILESYSTEM_ERROR", "MEMORY_ERROR",
      "UUID_ERROR", "KICAD_CLI_NOT_FOUND"};
  if ((unsigned)status >= sizeof(names) / sizeof(names[0]))
    return "UNKNOWN_STATUS";
  return names[status];
}

const char *copper_project_status_message(CopperProjectStatus status) {
  static const char *const messages[] = {
      "Operation completed.",
      "Invalid argument supplied.",
      "Project name is invalid for a filesystem directory.",
      "A file or directory already exists at the project destination.",
      "The requested project directory was not found.",
      "The directory does not contain a valid Copper/KiCad project file.",
      "A filesystem operation failed.",
      "Memory allocation failed.",
      "The operating system could not generate a project UUID.",
      "KiCad CLI was not found; project creation can still continue."};
  if ((unsigned)status >= sizeof(messages) / sizeof(messages[0]))
    return "Unknown project status.";
  return messages[status];
}

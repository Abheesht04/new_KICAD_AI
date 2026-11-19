#include "copper_spice_fs.h"

#include "copper_spice_expand.h"
#include "filesystem.h"

#include <string.h>

typedef struct CspFsIncludeContext {
  const char *allowed_root;
} CspFsIncludeContext;

static CspStatus csp_fs_status(FsStatus status) {
  if (status == FS_ERR_OUT_OF_MEMORY) return CSP_ERR_MEMORY;
  if (status == FS_ERR_ARGUMENT || status == FS_ERR_INVALID_PATH)
    return CSP_ERR_ARGUMENT;
  if (status == FS_ERR_RANGE) return CSP_ERR_RANGE;
  return CSP_ERR_IO;
}

static int csp_get_parent_directory(const char *path, char *parent,
                                    size_t parent_capacity) {
  size_t length;
  size_t separator;
  if (path == NULL || parent == NULL || parent_capacity == 0u) return 0;
  length = strlen(path);
  separator = length;
  while (separator > 0u && path[separator - 1u] != '/' &&
         path[separator - 1u] != '\\')
    --separator;
  if (separator == 0u) return 0;
#ifdef _WIN32
  if (separator == 3u && path[1] == ':')
    length = 3u;
  else
    length = separator - 1u;
#else
  length = separator == 1u ? 1u : separator - 1u;
#endif
  if (length >= parent_capacity) return 0;
  memcpy(parent, path, length);
  parent[length] = '\0';
  return 1;
}

static int csp_fs_read_include(void *context, Arena *scratch_arena,
                               const char *including_path,
                               const char *requested_path,
                               const char **resolved_path, const char **data,
                               size_t *data_size) {
  CspFsIncludeContext *include_context = (CspFsIncludeContext *)context;
  char including_real[FS_PATH_MAX];
  char parent[FS_PATH_MAX];
  char requested_absolute[FS_PATH_MAX];
  char resolved[FS_PATH_MAX];
  bool within_root = false;
  const unsigned char *file_data = NULL;
  size_t file_size = 0u;
  char *path_copy;
  FsStatus fs_status;

  if (include_context == NULL || include_context->allowed_root == NULL ||
      scratch_arena == NULL || including_path == NULL ||
      requested_path == NULL || resolved_path == NULL || data == NULL ||
      data_size == NULL)
    return 0;

  *resolved_path = NULL;
  *data = NULL;
  *data_size = 0u;

  fs_status = fs_path_resolve_existing(including_real, sizeof(including_real),
                                       including_path);
  if (fs_status != FS_OK ||
      !csp_get_parent_directory(including_real, parent, sizeof(parent)))
    return 0;

  if (fs_path_is_absolute(requested_path)) {
    fs_status = fs_path_absolute(requested_absolute,
                                 sizeof(requested_absolute), requested_path);
  } else {
    fs_status = fs_path_join(requested_absolute, sizeof(requested_absolute),
                             parent, requested_path);
    if (fs_status == FS_OK) {
      char normalised[FS_PATH_MAX];
      fs_status = fs_path_absolute(normalised, sizeof(normalised),
                                   requested_absolute);
      if (fs_status == FS_OK)
        memcpy(requested_absolute, normalised, strlen(normalised) + 1u);
    }
  }
  if (fs_status != FS_OK) return 0;

  fs_status = fs_path_resolve_existing(resolved, sizeof(resolved),
                                       requested_absolute);
  if (fs_status != FS_OK) return 0;
  fs_status = fs_path_is_within(resolved, include_context->allowed_root,
                                &within_root);
  if (fs_status != FS_OK || !within_root) return 0;

  fs_status = fs_read_file_arena(scratch_arena, resolved, &file_data,
                                 &file_size);
  if (fs_status != FS_OK || memchr(file_data, '\0', file_size) != NULL)
    return 0;

  path_copy = (char *)arena_alloc(scratch_arena, strlen(resolved) + 1u);
  if (path_copy == NULL) return 0;
  memcpy(path_copy, resolved, strlen(resolved) + 1u);

  *resolved_path = path_copy;
  *data = (const char *)file_data;
  *data_size = file_size;
  return 1;
}

CspStatus csp_parse_file_in_root(Arena *arena, const char *path,
                                 const char *allowed_root, CspNetlist *out,
                                 size_t *error_line) {
  char resolved_deck[FS_PATH_MAX];
  char resolved_root[FS_PATH_MAX];
  char default_root[FS_PATH_MAX];
  bool within_root = false;
  const unsigned char *file_data = NULL;
  size_t file_size = 0u;
  CspExpandOptions expand_options;
  CspFsIncludeContext include_context;
  CspStatus parse_status;
  FsStatus fs_status;

  if (arena == NULL || path == NULL || out == NULL)
    return CSP_ERR_ARGUMENT;

  fs_status = fs_path_resolve_existing(resolved_deck, sizeof(resolved_deck),
                                       path);
  if (fs_status != FS_OK) return csp_fs_status(fs_status);

  if (allowed_root != NULL) {
    fs_status = fs_path_resolve_existing(resolved_root, sizeof(resolved_root),
                                         allowed_root);
    if (fs_status != FS_OK) return csp_fs_status(fs_status);
    if (!fs_directory_exists(resolved_root)) return CSP_ERR_ARGUMENT;
  } else {
    if (!csp_get_parent_directory(resolved_deck, default_root,
                                  sizeof(default_root)))
      return CSP_ERR_IO;
    memcpy(resolved_root, default_root, strlen(default_root) + 1u);
  }

  fs_status = fs_path_is_within(resolved_deck, resolved_root, &within_root);
  if (fs_status != FS_OK) return csp_fs_status(fs_status);
  if (!within_root) return CSP_ERR_IO;

  fs_status = fs_read_file_arena(arena, resolved_deck, &file_data, &file_size);
  if (fs_status != FS_OK) return csp_fs_status(fs_status);
  if (memchr(file_data, '\0', file_size) != NULL) return CSP_ERR_SYNTAX;

  include_context.allowed_root = resolved_root;
  expand_options.read_include = csp_fs_read_include;
  expand_options.read_context = &include_context;
  expand_options.max_include_depth = 16u;
  expand_options.max_subcircuit_depth = 16u;

  parse_status = csp_parse_with_includes(
      arena, resolved_deck, (const char *)file_data, file_size,
      &expand_options, out, error_line);
  return parse_status;
}

CspStatus csp_parse_file(Arena *arena, const char *path, CspNetlist *out,
                         size_t *error_line) {
  return csp_parse_file_in_root(arena, path, NULL, out, error_line);
}

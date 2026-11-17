#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#else
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#endif

#include "filesystem.h"
#include "sha256.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <direct.h>
#include <wchar.h>
#include <wctype.h>
#include <io.h>
#define FS_NATIVE_SEP '\\'
#else
#include <dirent.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif
#define FS_NATIVE_SEP '/'
#endif

static bool fs_is_sep(char value) {
  return value == '/' || value == '\\';
}

static FsStatus fs_copy_string(char *out, size_t capacity, const char *text) {
  size_t length;
  if (out == NULL || capacity == 0 || text == NULL)
    return FS_ERR_ARGUMENT;
  length = strlen(text);
  if (length >= capacity) return FS_ERR_RANGE;
  memcpy(out, text, length + 1u);
  return FS_OK;
}

#ifdef _WIN32
static FsStatus fs_win_error_status(DWORD error) {
  switch (error) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_DRIVE:
      return FS_ERR_NOT_FOUND;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
      return FS_ERR_ALREADY_EXISTS;
    case ERROR_ACCESS_DENIED:
    case ERROR_PRIVILEGE_NOT_HELD:
    case ERROR_WRITE_PROTECT:
      return FS_ERR_PERMISSION;
    case ERROR_DIRECTORY:
      return FS_ERR_NOT_DIRECTORY;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
      return FS_ERR_OUT_OF_MEMORY;
    case ERROR_FILENAME_EXCED_RANGE:
    case ERROR_BUFFER_OVERFLOW:
      return FS_ERR_RANGE;
    case ERROR_INVALID_NAME:
    case ERROR_BAD_PATHNAME:
      return FS_ERR_INVALID_PATH;
    default:
      return FS_ERR_IO;
  }
}

static wchar_t *fs_utf8_to_wide(const char *text, FsStatus *status) {
  int required;
  wchar_t *wide;
  if (text == NULL) {
    if (status) *status = FS_ERR_ARGUMENT;
    return NULL;
  }
  required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1,
                                 NULL, 0);
  if (required <= 0) {
    if (status) *status = FS_ERR_INVALID_PATH;
    return NULL;
  }
  if ((size_t)required > SIZE_MAX / sizeof(*wide)) {
    if (status) *status = FS_ERR_RANGE;
    return NULL;
  }
  wide = (wchar_t *)malloc((size_t)required * sizeof(*wide));
  if (wide == NULL) {
    if (status) *status = FS_ERR_OUT_OF_MEMORY;
    return NULL;
  }
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide,
                          required) != required) {
    free(wide);
    if (status) *status = FS_ERR_INVALID_PATH;
    return NULL;
  }
  if (status) *status = FS_OK;
  return wide;
}

static FsStatus fs_wide_to_utf8(const wchar_t *wide, char *out,
                                size_t out_capacity) {
  int required;
  if (wide == NULL || out == NULL || out_capacity == 0)
    return FS_ERR_ARGUMENT;
  required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1,
                                 NULL, 0, NULL, NULL);
  if (required <= 0) return FS_ERR_INVALID_PATH;
  if ((size_t)required > out_capacity) return FS_ERR_RANGE;
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, out,
                          required, NULL, NULL) != required)
    return FS_ERR_INVALID_PATH;
  return FS_OK;
}

static DWORD fs_win_attributes(const char *path, FsStatus *status) {
  FsStatus conversion_status;
  wchar_t *wide = fs_utf8_to_wide(path, &conversion_status);
  DWORD attributes;
  if (wide == NULL) {
    if (status) *status = conversion_status;
    return INVALID_FILE_ATTRIBUTES;
  }
  attributes = GetFileAttributesW(wide);
  if (attributes == INVALID_FILE_ATTRIBUTES && status)
    *status = fs_win_error_status(GetLastError());
  else if (status)
    *status = FS_OK;
  free(wide);
  return attributes;
}
#else
static FsStatus fs_errno_status(int error) {
  switch (error) {
    case 0: return FS_OK;
    case ENOENT: return FS_ERR_NOT_FOUND;
    case EEXIST: return FS_ERR_ALREADY_EXISTS;
    case ENOTDIR: return FS_ERR_NOT_DIRECTORY;
    case EISDIR: return FS_ERR_NOT_FILE;
    case EACCES:
    case EPERM:
#ifdef EROFS
    case EROFS:
#endif
      return FS_ERR_PERMISSION;
    case ENOMEM: return FS_ERR_OUT_OF_MEMORY;
    case ENAMETOOLONG:
    case EOVERFLOW: return FS_ERR_RANGE;
    case EINVAL: return FS_ERR_INVALID_PATH;
    default: return FS_ERR_IO;
  }
}
#endif

bool fs_path_is_absolute(const char *path) {
  if (path == NULL || path[0] == '\0') return false;
#ifdef _WIN32
  /* A single leading separator is current-drive rooted and must not be joined
   * beneath the caller's directory. UNC and drive-rooted paths are absolute. */
  if (fs_is_sep(path[0])) return true;
  return (((path[0] >= 'A' && path[0] <= 'Z') ||
           (path[0] >= 'a' && path[0] <= 'z')) &&
          path[1] == ':' && fs_is_sep(path[2]));
#else
  return path[0] == '/';
#endif
}

FsStatus fs_path_join(char *out, size_t out_capacity, const char *left,
                      const char *right) {
  size_t left_length;
  size_t right_start = 0;
  size_t right_length;
  bool need_separator;
  if (out == NULL || out_capacity == 0 || left == NULL || right == NULL)
    return FS_ERR_ARGUMENT;
  if (fs_path_is_absolute(right))
    return fs_copy_string(out, out_capacity, right);
  left_length = strlen(left);
  right_length = strlen(right);
  while (right_start < right_length && fs_is_sep(right[right_start]))
    ++right_start;
  while (left_length > 0 && fs_is_sep(left[left_length - 1u])) {
#ifdef _WIN32
    if (left_length == 3u && left[1] == ':') break;
#else
    if (left_length == 1u) break;
#endif
    --left_length;
  }
  need_separator = left_length > 0 && right_start < right_length &&
                   !fs_is_sep(left[left_length - 1u]);
  if (left_length > SIZE_MAX - right_length - 2u) return FS_ERR_RANGE;
  if (left_length + (need_separator ? 1u : 0u) +
          (right_length - right_start) + 1u > out_capacity)
    return FS_ERR_RANGE;
  if (left_length != 0) memcpy(out, left, left_length);
  size_t position = left_length;
  if (need_separator) out[position++] = FS_NATIVE_SEP;
  if (right_length > right_start) {
    memcpy(out + position, right + right_start, right_length - right_start);
    position += right_length - right_start;
  }
  out[position] = '\0';
  return FS_OK;
}

static FsStatus fs_normalize_absolute_utf8(char *out, size_t out_capacity,
                                           const char *absolute_path) {
  char source[FS_PATH_MAX];
  size_t source_length;
  size_t root_length = 0;
  size_t output_length;
  size_t cursor;
  size_t restore_lengths[FS_PATH_MAX / 2u];
  size_t segment_count = 0;

  if (out == NULL || out_capacity == 0 || absolute_path == NULL)
    return FS_ERR_ARGUMENT;
  source_length = strlen(absolute_path);
  if (source_length == 0 || source_length >= sizeof(source))
    return FS_ERR_RANGE;
  memcpy(source, absolute_path, source_length + 1u);
  for (cursor = 0; cursor < source_length; ++cursor)
    if (source[cursor] == '\\') source[cursor] = '/';

#ifdef _WIN32
  if (source_length >= 3u &&
      ((source[0] >= 'A' && source[0] <= 'Z') ||
       (source[0] >= 'a' && source[0] <= 'z')) &&
      source[1] == ':' && source[2] == '/') {
    source[0] = (char)((source[0] >= 'a' && source[0] <= 'z')
                           ? source[0] - ('a' - 'A') : source[0]);
    out[0] = source[0]; out[1] = ':'; out[2] = '/';
    root_length = 3u;
  } else if (source_length >= 5u && source[0] == '/' && source[1] == '/') {
    size_t server_end = 2u;
    size_t share_end;
    while (server_end < source_length && source[server_end] != '/')
      ++server_end;
    if (server_end == 2u || server_end == source_length)
      return FS_ERR_INVALID_PATH;
    share_end = server_end + 1u;
    while (share_end < source_length && source[share_end] != '/')
      ++share_end;
    if (share_end == server_end + 1u) return FS_ERR_INVALID_PATH;
    root_length = share_end;
    if (root_length + 1u < out_capacity) {
      memcpy(out, source, root_length);
      out[root_length] = '\0';
    } else {
      return FS_ERR_RANGE;
    }
  } else {
    return FS_ERR_INVALID_PATH;
  }
#else
  if (source[0] != '/') return FS_ERR_INVALID_PATH;
  out[0] = '/';
  root_length = 1u;
#endif

  if (root_length >= out_capacity) return FS_ERR_RANGE;
  output_length = root_length;
  cursor = root_length;
  while (cursor < source_length) {
    size_t segment_start;
    size_t segment_length;
    while (cursor < source_length && source[cursor] == '/') ++cursor;
    if (cursor == source_length) break;
    segment_start = cursor;
    while (cursor < source_length && source[cursor] != '/') ++cursor;
    segment_length = cursor - segment_start;
    if (segment_length == 1u && source[segment_start] == '.') continue;
    if (segment_length == 2u && source[segment_start] == '.' &&
        source[segment_start + 1u] == '.') {
      if (segment_count != 0) {
        output_length = restore_lengths[--segment_count];
        out[output_length] = '\0';
      }
      continue;
    }
    if (segment_count >= sizeof(restore_lengths) / sizeof(restore_lengths[0]))
      return FS_ERR_RANGE;
    size_t separator_length =
        (output_length != 0 && out[output_length - 1u] != '/') ? 1u : 0u;
    if (separator_length + segment_length >= out_capacity - output_length)
      return FS_ERR_RANGE;
    restore_lengths[segment_count++] = output_length;
    if (separator_length) out[output_length++] = '/';
    memcpy(out + output_length, source + segment_start, segment_length);
    output_length += segment_length;
    out[output_length] = '\0';
  }
  out[output_length] = '\0';
  return FS_OK;
}

FsStatus fs_get_current_directory(char *out, size_t out_capacity) {
  if (out == NULL || out_capacity == 0) return FS_ERR_ARGUMENT;
#ifdef _WIN32
  DWORD needed = GetCurrentDirectoryW(0, NULL);
  wchar_t *wide;
  DWORD written;
  if (needed == 0) return fs_win_error_status(GetLastError());
  wide = (wchar_t *)malloc((size_t)needed * sizeof(*wide));
  if (wide == NULL) return FS_ERR_OUT_OF_MEMORY;
  written = GetCurrentDirectoryW(needed, wide);
  if (written == 0 || written >= needed) {
    FsStatus status = fs_win_error_status(GetLastError());
    free(wide);
    return status;
  }
  FsStatus status = fs_wide_to_utf8(wide, out, out_capacity);
  free(wide);
  if (status != FS_OK) return status;
  return fs_normalize_absolute_utf8(out, out_capacity, out);
#else
  if (getcwd(out, out_capacity) == NULL) return fs_errno_status(errno);
  return fs_normalize_absolute_utf8(out, out_capacity, out);
#endif
}

FsStatus fs_path_absolute(char *out, size_t out_capacity, const char *path) {
  if (out == NULL || out_capacity == 0 || path == NULL || path[0] == '\0')
    return FS_ERR_ARGUMENT;
#ifdef _WIN32
  FsStatus conversion_status;
  wchar_t *input = fs_utf8_to_wide(path, &conversion_status);
  DWORD needed;
  wchar_t *absolute;
  if (input == NULL) return conversion_status;
  needed = GetFullPathNameW(input, 0, NULL, NULL);
  if (needed == 0) {
    FsStatus status = fs_win_error_status(GetLastError());
    free(input);
    return status;
  }
  absolute = (wchar_t *)malloc(((size_t)needed + 1u) * sizeof(*absolute));
  if (absolute == NULL) {
    free(input);
    return FS_ERR_OUT_OF_MEMORY;
  }
  DWORD written = GetFullPathNameW(input, needed + 1u, absolute, NULL);
  free(input);
  if (written == 0 || written > needed) {
    FsStatus status = fs_win_error_status(GetLastError());
    free(absolute);
    return status;
  }
  char utf8[FS_PATH_MAX];
  FsStatus status = fs_wide_to_utf8(absolute, utf8, sizeof(utf8));
  free(absolute);
  if (status != FS_OK) return status;
  return fs_normalize_absolute_utf8(out, out_capacity, utf8);
#else
  char absolute[FS_PATH_MAX];
  FsStatus status;
  if (path[0] == '/') {
    if (strlen(path) >= sizeof(absolute)) return FS_ERR_RANGE;
    memcpy(absolute, path, strlen(path) + 1u);
  } else {
    char cwd[FS_PATH_MAX];
    status = fs_get_current_directory(cwd, sizeof(cwd));
    if (status != FS_OK) return status;
    status = fs_path_join(absolute, sizeof(absolute), cwd, path);
    if (status != FS_OK) return status;
  }
  return fs_normalize_absolute_utf8(out, out_capacity, absolute);
#endif
}

FsStatus fs_path_normalize(char *out, size_t out_capacity, const char *path) {
  return fs_path_absolute(out, out_capacity, path);
}

FsStatus fs_path_basename(char *out, size_t out_capacity, const char *path) {
  size_t end;
  size_t start;
  size_t length;
  if (out == NULL || out_capacity == 0 || path == NULL || path[0] == '\0')
    return FS_ERR_ARGUMENT;
  end = strlen(path);
  while (end > 0 && fs_is_sep(path[end - 1u])) --end;
  start = end;
  while (start > 0 && !fs_is_sep(path[start - 1u])) --start;
  length = end - start;
  if (length >= out_capacity) return FS_ERR_RANGE;
  memcpy(out, path + start, length);
  out[length] = '\0';
  return FS_OK;
}

FsStatus fs_path_resolve_existing(char *out, size_t out_capacity,
                                  const char *path) {
  if (out == NULL || out_capacity == 0 || path == NULL || path[0] == '\0')
    return FS_ERR_ARGUMENT;
#ifdef _WIN32
  FsStatus conversion_status;
  wchar_t *wide = fs_utf8_to_wide(path, &conversion_status);
  HANDLE handle;
  wchar_t *final_path;
  DWORD needed;
  if (wide == NULL) return conversion_status;
  handle = CreateFileW(wide, FILE_READ_ATTRIBUTES,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
  free(wide);
  if (handle == INVALID_HANDLE_VALUE)
    return fs_win_error_status(GetLastError());
  needed = GetFinalPathNameByHandleW(handle, NULL, 0,
                                     FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (needed == 0) {
    FsStatus status = fs_win_error_status(GetLastError());
    CloseHandle(handle);
    return status;
  }
  final_path = (wchar_t *)malloc(((size_t)needed + 1u) * sizeof(*final_path));
  if (final_path == NULL) {
    CloseHandle(handle);
    return FS_ERR_OUT_OF_MEMORY;
  }
  DWORD written = GetFinalPathNameByHandleW(
      handle, final_path, needed + 1u,
      FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  CloseHandle(handle);
  if (written == 0 || written > needed) {
    FsStatus status = fs_win_error_status(GetLastError());
    free(final_path);
    return status;
  }
  /* Remove Win32's extended-length prefix for normal public UTF-8 paths. */
  const wchar_t *normal = final_path;
  if (wcsncmp(normal, L"\\\\?\\UNC\\", 8) == 0) {
    normal += 8;
    size_t normal_length = wcslen(normal);
    if (normal_length > SIZE_MAX / sizeof(*normal) - 3u) {
      free(final_path);
      return FS_ERR_RANGE;
    }
    wchar_t *unc = (wchar_t *)malloc((normal_length + 3u) * sizeof(*unc));
    if (unc == NULL) {
      free(final_path);
      return FS_ERR_OUT_OF_MEMORY;
    }
    unc[0] = L'\\'; unc[1] = L'\\';
    memcpy(unc + 2, normal, (normal_length + 1u) * sizeof(*unc));
    FsStatus status = fs_wide_to_utf8(unc, out, out_capacity);
    free(unc);
    free(final_path);
    return status;
  }
  if (wcsncmp(normal, L"\\\\?\\", 4) == 0) normal += 4;
  FsStatus status = fs_wide_to_utf8(normal, out, out_capacity);
  free(final_path);
  if (status != FS_OK) return status;
  char normalized[FS_PATH_MAX];
  status = fs_normalize_absolute_utf8(normalized, sizeof(normalized), out);
  if (status != FS_OK) return status;
  return fs_copy_string(out, out_capacity, normalized);
#else
  char absolute[FS_PATH_MAX];
  char *resolved;
  FsStatus status = fs_path_absolute(absolute, sizeof(absolute), path);
  if (status != FS_OK) return status;
  resolved = realpath(absolute, NULL);
  if (resolved == NULL) return fs_errno_status(errno);
  status = fs_copy_string(out, out_capacity, resolved);
  free(resolved);
  return status;
#endif
}

FsStatus fs_path_is_within(const char *candidate, const char *allowed_root,
                           bool *out_within) {
  char candidate_real[FS_PATH_MAX];
  char root_real[FS_PATH_MAX];
  FsStatus status;
  if (candidate == NULL || allowed_root == NULL || out_within == NULL)
    return FS_ERR_ARGUMENT;
  *out_within = false;
  status = fs_path_resolve_existing(candidate_real, sizeof(candidate_real),
                                    candidate);
  if (status != FS_OK) return status;
  status = fs_path_resolve_existing(root_real, sizeof(root_real), allowed_root);
  if (status != FS_OK) return status;
#ifdef _WIN32
  FsStatus conversion_status;
  wchar_t *candidate_wide = fs_utf8_to_wide(candidate_real, &conversion_status);
  wchar_t *root_wide;
  if (candidate_wide == NULL) return conversion_status;
  root_wide = fs_utf8_to_wide(root_real, &conversion_status);
  if (root_wide == NULL) {
    free(candidate_wide);
    return conversion_status;
  }
  size_t root_length = wcslen(root_wide);
  size_t candidate_length = wcslen(candidate_wide);
  if (candidate_length >= root_length) {
    int equal = CompareStringOrdinal(candidate_wide, (int)root_length,
                                     root_wide, (int)root_length, TRUE);
    if (equal == CSTR_EQUAL &&
        (candidate_length == root_length ||
         (root_length != 0u &&
          (root_wide[root_length - 1u] == L'\\' ||
           root_wide[root_length - 1u] == L'/')) ||
         candidate_wide[root_length] == L'\\' ||
         candidate_wide[root_length] == L'/'))
      *out_within = true;
  }
  free(candidate_wide);
  free(root_wide);
#else
  size_t root_length = strlen(root_real);
  for (size_t i = 0; i < root_length; ++i) {
    if (candidate_real[i] == '\0' || candidate_real[i] != root_real[i])
      return FS_OK;
  }
  if (candidate_real[root_length] == '\0' ||
      (root_length != 0u && fs_is_sep(root_real[root_length - 1u])) ||
      fs_is_sep(candidate_real[root_length]))
    *out_within = true;
#endif
  return FS_OK;
}

#ifdef _WIN32
static bool fs_is_regular_attributes(DWORD attributes) {
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}
bool fs_file_exists(const char *path) {
  FsStatus status;
  DWORD attributes = fs_win_attributes(path, &status);
  return fs_is_regular_attributes(attributes);
}
bool fs_directory_exists(const char *path) {
  FsStatus status;
  DWORD attributes = fs_win_attributes(path, &status);
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}
static FsStatus fs_win_require_regular_file(const char *path) {
  FsStatus status;
  DWORD attributes = fs_win_attributes(path, &status);
  if (attributes == INVALID_FILE_ATTRIBUTES) return status;
  return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ? FS_ERR_NOT_FILE : FS_OK;
}
#else
bool fs_file_exists(const char *path) {
  struct stat info;
  return path != NULL && stat(path, &info) == 0 && S_ISREG(info.st_mode);
}
bool fs_directory_exists(const char *path) {
  struct stat info;
  return path != NULL && stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}
#endif

#ifdef _WIN32
static bool fs_extension_in_pathext(const wchar_t *extension) {
  DWORD needed = GetEnvironmentVariableW(L"PATHEXT", NULL, 0);
  wchar_t *value;
  if (needed == 0) {
    static const wchar_t defaults[] = L".COM;.EXE;.BAT;.CMD";
    value = (wchar_t *)malloc(sizeof(defaults));
    if (value == NULL) return false;
    memcpy(value, defaults, sizeof(defaults));
  } else {
    value = (wchar_t *)malloc((size_t)needed * sizeof(*value));
    if (value == NULL) return false;
    if (GetEnvironmentVariableW(L"PATHEXT", value, needed) == 0) {
      free(value);
      return false;
    }
  }
  bool found = false;
  wchar_t *cursor = value;
  while (*cursor != L'\0') {
    wchar_t *end = wcschr(cursor, L';');
    size_t length = end ? (size_t)(end - cursor) : wcslen(cursor);
    if (wcslen(extension) == length) {
      size_t i;
      for (i = 0; i < length; ++i)
        if (towlower(extension[i]) != towlower(cursor[i])) break;
      if (i == length) { found = true; break; }
    }
    if (end == NULL) break;
    cursor = end + 1;
  }
  free(value);
  return found;
}
#endif

bool fs_is_executable(const char *path) {
  if (!fs_file_exists(path)) return false;
#ifdef _WIN32
  FsStatus conversion_status;
  wchar_t *wide = fs_utf8_to_wide(path, &conversion_status);
  const wchar_t *base;
  const wchar_t *dot;
  bool result;
  if (wide == NULL) return false;
  base = wcsrchr(wide, L'\\');
  if (base == NULL) base = wcsrchr(wide, L'/');
  base = base ? base + 1 : wide;
  dot = wcsrchr(base, L'.');
  result = dot != NULL && fs_extension_in_pathext(dot);
  free(wide);
  return result;
#else
  return access(path, X_OK) == 0;
#endif
}

FsStatus fs_create_directory(const char *path) {
  if (path == NULL || path[0] == '\0') return FS_ERR_ARGUMENT;
#ifdef _WIN32
  FsStatus conversion_status;
  wchar_t *wide = fs_utf8_to_wide(path, &conversion_status);
  if (wide == NULL) return conversion_status;
  if (CreateDirectoryW(wide, NULL)) {
    free(wide);
    return FS_OK;
  }
  DWORD error = GetLastError();
  free(wide);
  if (error == ERROR_ALREADY_EXISTS)
    return fs_directory_exists(path) ? FS_ERR_ALREADY_EXISTS
                                     : FS_ERR_NOT_DIRECTORY;
  return fs_win_error_status(error);
#else
  if (mkdir(path, 0777) == 0) return FS_OK;
  if (errno == EEXIST)
    return fs_directory_exists(path) ? FS_ERR_ALREADY_EXISTS
                                     : FS_ERR_NOT_DIRECTORY;
  return fs_errno_status(errno);
#endif
}

static size_t fs_root_length(const char *path) {
  if (path == NULL || path[0] == '\0') return 0;
#ifdef _WIN32
  if (path[0] == '/' && path[1] == '/') {
    size_t i = 2;
    while (path[i] != '\0' && path[i] != '/') ++i;
    if (path[i] == '\0') return i;
    ++i;
    while (path[i] != '\0' && path[i] != '/') ++i;
    return i;
  }
  if (path[1] == ':' && path[2] == '/') return 3;
  return 0;
#else
  return path[0] == '/' ? 1u : 0u;
#endif
}

FsStatus fs_create_directories(const char *path) {
  char normalized[FS_PATH_MAX];
  size_t length;
  size_t root_length;
  FsStatus status;
  if (path == NULL || path[0] == '\0') return FS_ERR_ARGUMENT;
  status = fs_path_absolute(normalized, sizeof(normalized), path);
  if (status != FS_OK) return status;
  length = strlen(normalized);
  root_length = fs_root_length(normalized);
  for (size_t i = root_length; i <= length; ++i) {
    if (normalized[i] == '\0' || fs_is_sep(normalized[i])) {
      char saved = normalized[i];
      normalized[i] = '\0';
      if (normalized[0] != '\0' && !fs_directory_exists(normalized)) {
        status = fs_create_directory(normalized);
        if (status != FS_OK && status != FS_ERR_ALREADY_EXISTS) {
          normalized[i] = saved;
          return status;
        }
      }
      normalized[i] = saved;
    }
  }
  return fs_directory_exists(normalized) ? FS_OK : FS_ERR_IO;
}

static FsStatus fs_write_all_fd(int descriptor, const void *data, size_t size) {
  const unsigned char *bytes = (const unsigned char *)data;
  while (size != 0) {
#ifdef _WIN32
    unsigned int request = size > INT_MAX ? INT_MAX : (unsigned int)size;
    int written = _write(descriptor, bytes, request);
#else
    size_t request = size > (size_t)SSIZE_MAX ? (size_t)SSIZE_MAX : size;
    ssize_t written = write(descriptor, bytes, request);
#endif
    if (written < 0) {
#ifndef _WIN32
      if (errno == EINTR) continue;
#endif
      return
#ifdef _WIN32
          FS_ERR_IO;
#else
          fs_errno_status(errno);
#endif
    }
    if (written == 0) return FS_ERR_IO;
    bytes += (size_t)written;
    size -= (size_t)written;
  }
  return FS_OK;
}

FsStatus fs_write_file_exclusive(const char *path, const void *data,
                                 size_t size) {
  if (path == NULL || path[0] == '\0' || (data == NULL && size != 0))
    return FS_ERR_ARGUMENT;
#ifdef _WIN32
  FsStatus conversion_status;
  wchar_t *wide = fs_utf8_to_wide(path, &conversion_status);
  HANDLE file;
  if (wide == NULL) return conversion_status;
  file = CreateFileW(wide, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                     FILE_ATTRIBUTE_NORMAL, NULL);
  if (file == INVALID_HANDLE_VALUE) {
    FsStatus status = fs_win_error_status(GetLastError());
    free(wide);
    return status;
  }
  const unsigned char *bytes = (const unsigned char *)data;
  size_t remaining = size;
  FsStatus status = FS_OK;
  while (remaining != 0) {
    DWORD request = remaining > 1024u * 1024u
                        ? 1024u * 1024u : (DWORD)remaining;
    DWORD written = 0;
    if (!WriteFile(file, bytes, request, &written, NULL) || written == 0) {
      status = fs_win_error_status(GetLastError());
      if (status == FS_OK) status = FS_ERR_IO;
      break;
    }
    bytes += written;
    remaining -= written;
  }
  if (status == FS_OK && !FlushFileBuffers(file))
    status = fs_win_error_status(GetLastError());
  if (!CloseHandle(file) && status == FS_OK)
    status = fs_win_error_status(GetLastError());
  if (status != FS_OK) DeleteFileW(wide);
  free(wide);
  return status;
#else
  int descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666);
  if (descriptor < 0) return fs_errno_status(errno);
  FsStatus status = fs_write_all_fd(descriptor, data, size);
  if (status == FS_OK && fsync(descriptor) != 0) status = fs_errno_status(errno);
  if (close(descriptor) != 0 && status == FS_OK) status = fs_errno_status(errno);
  if (status != FS_OK) unlink(path);
  return status;
#endif
}

#ifdef _WIN32
static volatile LONG fs_temp_counter = 0;
static unsigned long long fs_process_id(void) {
  return (unsigned long long)GetCurrentProcessId();
}
static unsigned long long fs_next_temp_id(void) {
  return (unsigned long long)InterlockedIncrement(&fs_temp_counter);
}
#else
static atomic_uint_fast64_t fs_temp_counter = 0;
static unsigned long long fs_process_id(void) {
  return (unsigned long long)getpid();
}
static unsigned long long fs_next_temp_id(void) {
  return (unsigned long long)atomic_fetch_add(&fs_temp_counter, 1u) + 1u;
}
#endif

static FsStatus fs_make_temp_path(const char *path, char *out,
                                  size_t out_capacity) {
  int written = snprintf(out, out_capacity, "%s.copper-tmp-%llu-%llu",
                          path, fs_process_id(), fs_next_temp_id());
  if (written < 0) return FS_ERR_IO;
  if ((size_t)written >= out_capacity) return FS_ERR_RANGE;
  return FS_OK;
}

#ifndef _WIN32
static void fs_sync_parent_directory(const char *path) {
  char parent[FS_PATH_MAX];
  size_t length = strlen(path);
  size_t slash = length;
  while (slash > 0 && path[slash - 1u] != '/') --slash;
  if (slash == 0) {
    memcpy(parent, ".", 2u);
  } else if (slash == 1u) {
    memcpy(parent, "/", 2u);
  } else {
    if (slash - 1u >= sizeof(parent)) return;
    memcpy(parent, path, slash - 1u);
    parent[slash - 1u] = '\0';
  }
  int descriptor = open(parent, O_RDONLY | O_DIRECTORY);
  if (descriptor >= 0) {
    (void)fsync(descriptor);
    (void)close(descriptor);
  }
}
#endif

static FsStatus fs_write_file_atomic_impl(const char *path, const void *data,
                                          size_t size, bool replace) {
  if (path == NULL || path[0] == '\0' || (data == NULL && size != 0))
    return FS_ERR_ARGUMENT;
  for (unsigned attempt = 0; attempt < 128u; ++attempt) {
    char temporary[FS_PATH_MAX];
    FsStatus status = fs_make_temp_path(path, temporary, sizeof(temporary));
    if (status != FS_OK) return status;
#ifdef _WIN32
    FsStatus conversion_status;
    /* Convert before creating the temporary file so every cleanup path can
     * use DeleteFileW, even when a later allocation/conversion fails. */
    wchar_t *temp_wide = fs_utf8_to_wide(temporary, &conversion_status);
    if (temp_wide == NULL) return conversion_status;
#endif
    status = fs_write_file_exclusive(temporary, data, size);
    if (status == FS_ERR_ALREADY_EXISTS) {
#ifdef _WIN32
      free(temp_wide);
#endif
      continue;
    }
    if (status != FS_OK) {
#ifdef _WIN32
      free(temp_wide);
#endif
      return status;
    }
#ifdef _WIN32
    wchar_t *target_wide = fs_utf8_to_wide(path, &conversion_status);
    if (target_wide == NULL) {
      DeleteFileW(temp_wide);
      free(temp_wide);
      return conversion_status;
    }
    BOOL moved = replace
                     ? MoveFileExW(temp_wide, target_wide,
                                   MOVEFILE_REPLACE_EXISTING |
                                       MOVEFILE_WRITE_THROUGH)
                     : MoveFileExW(temp_wide, target_wide,
                                   MOVEFILE_WRITE_THROUGH);
    DWORD error = moved ? ERROR_SUCCESS : GetLastError();
    if (!moved) DeleteFileW(temp_wide);
    free(temp_wide);
    free(target_wide);
    return moved ? FS_OK : fs_win_error_status(error);
#else
    if (replace) {
      if (rename(temporary, path) == 0) {
        fs_sync_parent_directory(path);
        return FS_OK;
      }
      status = fs_errno_status(errno);
      (void)unlink(temporary);
      return status;
    }
    /* link() is an atomic no-replace commit for a sibling temp file. */
    if (link(temporary, path) == 0) {
      (void)unlink(temporary);
      fs_sync_parent_directory(path);
      return FS_OK;
    }
    status = fs_errno_status(errno);
    (void)unlink(temporary);
    return status;
#endif
  }
  return FS_ERR_ALREADY_EXISTS;
}

FsStatus fs_write_file_atomic_exclusive(const char *path, const void *data,
                                        size_t size) {
  return fs_write_file_atomic_impl(path, data, size, false);
}

FsStatus fs_write_file_atomic_replace(const char *path, const void *data,
                                      size_t size) {
  return fs_write_file_atomic_impl(path, data, size, true);
}

FsStatus fs_rename_path(const char *old_path, const char *new_path) {
  if (old_path == NULL || new_path == NULL || old_path[0] == '\0' ||
      new_path[0] == '\0') return FS_ERR_ARGUMENT;
#ifdef _WIN32
  FsStatus conversion_status;
  wchar_t *old_wide = fs_utf8_to_wide(old_path, &conversion_status);
  wchar_t *new_wide;
  if (old_wide == NULL) return conversion_status;
  new_wide = fs_utf8_to_wide(new_path, &conversion_status);
  if (new_wide == NULL) {
    free(old_wide);
    return conversion_status;
  }
  BOOL ok = MoveFileW(old_wide, new_wide);
  DWORD error = ok ? ERROR_SUCCESS : GetLastError();
  free(old_wide); free(new_wide);
  return ok ? FS_OK : fs_win_error_status(error);
#else
  if (rename(old_path, new_path) == 0) return FS_OK;
  return fs_errno_status(errno);
#endif
}

FsStatus fs_rename_path_replace(const char *old_path, const char *new_path) {
  if (old_path == NULL || new_path == NULL || old_path[0] == '\0' ||
      new_path[0] == '\0') return FS_ERR_ARGUMENT;
#ifdef _WIN32
  FsStatus conversion_status;
  wchar_t *old_wide = fs_utf8_to_wide(old_path, &conversion_status);
  wchar_t *new_wide;
  if (old_wide == NULL) return conversion_status;
  new_wide = fs_utf8_to_wide(new_path, &conversion_status);
  if (new_wide == NULL) {
    free(old_wide);
    return conversion_status;
  }
  BOOL ok = MoveFileExW(old_wide, new_wide,
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
  DWORD error = ok ? ERROR_SUCCESS : GetLastError();
  free(old_wide); free(new_wide);
  return ok ? FS_OK : fs_win_error_status(error);
#else
  if (rename(old_path, new_path) == 0) {
    fs_sync_parent_directory(new_path);
    return FS_OK;
  }
  return fs_errno_status(errno);
#endif
}

static int fs_compare_entries(const void *left, const void *right) {
  const FsDirectoryEntry *a = (const FsDirectoryEntry *)left;
  const FsDirectoryEntry *b = (const FsDirectoryEntry *)right;
  return strcmp(a->name, b->name);
}

static char *fs_strdup(const char *text) {
  size_t length;
  char *copy;
  if (text == NULL) return NULL;
  length = strlen(text);
  if (length == SIZE_MAX) return NULL;
  copy = (char *)malloc(length + 1u);
  if (copy == NULL) return NULL;
  memcpy(copy, text, length + 1u);
  return copy;
}

void fs_free_directory_listing(FsDirectoryListing *listing) {
  if (listing == NULL) return;
  for (size_t i = 0; i < listing->count; ++i)
    free(listing->entries[i].name);
  free(listing->entries);
  listing->entries = NULL;
  listing->count = 0;
}

static FsStatus fs_append_directory_entry(FsDirectoryListing *listing,
                                          size_t *capacity, const char *name,
                                          bool is_directory,
                                          bool is_regular_file) {
  if (listing->count == *capacity) {
    size_t next = *capacity == 0 ? 32u : *capacity * 2u;
    FsDirectoryEntry *grown;
    if (next < *capacity || next > SIZE_MAX / sizeof(*grown))
      return FS_ERR_RANGE;
    grown = (FsDirectoryEntry *)realloc(listing->entries,
                                        next * sizeof(*grown));
    if (grown == NULL) return FS_ERR_OUT_OF_MEMORY;
    listing->entries = grown;
    *capacity = next;
  }
  char *copy = fs_strdup(name);
  if (copy == NULL) return FS_ERR_OUT_OF_MEMORY;
  listing->entries[listing->count].name = copy;
  listing->entries[listing->count].is_directory = is_directory;
  listing->entries[listing->count].is_regular_file = is_regular_file;
  ++listing->count;
  return FS_OK;
}

FsStatus fs_list_directory(const char *path, FsDirectoryListing *out) {
  if (path == NULL || path[0] == '\0' || out == NULL)
    return FS_ERR_ARGUMENT;
  out->entries = NULL;
  out->count = 0;
  size_t capacity = 0;
#ifdef _WIN32
  char pattern[FS_PATH_MAX];
  FsStatus status = fs_path_join(pattern, sizeof(pattern), path, "*");
  if (status != FS_OK) return status;
  FsStatus conversion_status;
  wchar_t *pattern_wide = fs_utf8_to_wide(pattern, &conversion_status);
  if (pattern_wide == NULL) return conversion_status;
  WIN32_FIND_DATAW data;
  HANDLE handle = FindFirstFileW(pattern_wide, &data);
  free(pattern_wide);
  if (handle == INVALID_HANDLE_VALUE)
    return fs_win_error_status(GetLastError());
  do {
    if (wcscmp(data.cFileName, L".") == 0 ||
        wcscmp(data.cFileName, L"..") == 0) continue;
    char name[FS_PATH_MAX];
    status = fs_wide_to_utf8(data.cFileName, name, sizeof(name));
    if (status != FS_OK) break;
    bool is_directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    bool is_reparse = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    status = fs_append_directory_entry(out, &capacity, name, is_directory,
                                       !is_directory && !is_reparse);
    if (status != FS_OK) break;
  } while (FindNextFileW(handle, &data));
  DWORD error = GetLastError();
  FindClose(handle);
  if (status == FS_OK && error != ERROR_NO_MORE_FILES)
    status = fs_win_error_status(error);
  if (status != FS_OK) {
    fs_free_directory_listing(out);
    return status;
  }
#else
  DIR *directory = opendir(path);
  if (directory == NULL) return fs_errno_status(errno);
  FsStatus status = FS_OK;
  struct dirent *item;
  while ((item = readdir(directory)) != NULL) {
    if (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0)
      continue;
    char child[FS_PATH_MAX];
    struct stat info;
    status = fs_path_join(child, sizeof(child), path, item->d_name);
    if (status != FS_OK) break;
    if (lstat(child, &info) != 0) {
      status = fs_errno_status(errno);
      break;
    }
    bool is_directory = S_ISDIR(info.st_mode);
    bool is_regular = S_ISREG(info.st_mode);
    status = fs_append_directory_entry(out, &capacity, item->d_name,
                                       is_directory, is_regular);
    if (status != FS_OK) break;
  }
  if (closedir(directory) != 0 && status == FS_OK)
    status = fs_errno_status(errno);
  if (status != FS_OK) {
    fs_free_directory_listing(out);
    return status;
  }
#endif
  if (out->count > 1u)
    qsort(out->entries, out->count, sizeof(out->entries[0]), fs_compare_entries);
  return FS_OK;
}

FsStatus fs_read_file(const char *path, void **out_data, size_t *out_size) {
  if (path == NULL || out_data == NULL || out_size == NULL)
    return FS_ERR_ARGUMENT;
  *out_data = NULL;
  *out_size = 0;
#ifdef _WIN32
  FsStatus regular_status = fs_win_require_regular_file(path);
  if (regular_status != FS_OK) return regular_status;
  FsStatus conversion_status;
  wchar_t *wide = fs_utf8_to_wide(path, &conversion_status);
  HANDLE file;
  LARGE_INTEGER file_size;
  unsigned char *data;
  if (wide == NULL) return conversion_status;
  file = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE |
                                          FILE_SHARE_DELETE,
                     NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  free(wide);
  if (file == INVALID_HANDLE_VALUE) return fs_win_error_status(GetLastError());
  if (!GetFileSizeEx(file, &file_size)) {
    FsStatus status = fs_win_error_status(GetLastError());
    CloseHandle(file);
    return status;
  }
  if (file_size.QuadPart < 0 ||
      (unsigned long long)file_size.QuadPart > SIZE_MAX - 1u) {
    CloseHandle(file);
    return FS_ERR_RANGE;
  }
  data = (unsigned char *)malloc((size_t)file_size.QuadPart + 1u);
  if (data == NULL) {
    CloseHandle(file);
    return FS_ERR_OUT_OF_MEMORY;
  }
  size_t remaining = (size_t)file_size.QuadPart;
  size_t offset = 0;
  while (remaining != 0) {
    DWORD request = remaining > 1024u * 1024u
                        ? 1024u * 1024u : (DWORD)remaining;
    DWORD received = 0;
    if (!ReadFile(file, data + offset, request, &received, NULL) || received == 0) {
      FsStatus status = fs_win_error_status(GetLastError());
      if (status == FS_OK) status = FS_ERR_IO;
      free(data); CloseHandle(file); return status;
    }
    offset += received; remaining -= received;
  }
  CloseHandle(file);
  data[offset] = 0;
  *out_data = data;
  *out_size = offset;
  return FS_OK;
#else
  int descriptor = open(path, O_RDONLY);
  struct stat info;
  unsigned char *data;
  size_t capacity = 16384u;
  size_t length = 0;
  if (descriptor < 0) return fs_errno_status(errno);
  if (fstat(descriptor, &info) != 0) {
    FsStatus status = fs_errno_status(errno);
    close(descriptor);
    return status;
  }
  if (!S_ISREG(info.st_mode)) {
    close(descriptor);
    return FS_ERR_NOT_FILE;
  }
  if (info.st_size >= 0) {
    if ((uintmax_t)info.st_size > SIZE_MAX - 1u) {
      close(descriptor);
      return FS_ERR_RANGE;
    }
    capacity = (size_t)info.st_size + 1u;
    if (capacity < 16384u) capacity = 16384u;
  }
  data = (unsigned char *)malloc(capacity);
  if (data == NULL) {
    close(descriptor);
    return FS_ERR_OUT_OF_MEMORY;
  }
  for (;;) {
    if (length == capacity - 1u) {
      if (capacity > SIZE_MAX / 2u) {
        free(data); close(descriptor); return FS_ERR_RANGE;
      }
      size_t next_capacity = capacity * 2u;
      unsigned char *grown = (unsigned char *)realloc(data, next_capacity);
      if (grown == NULL) {
        free(data); close(descriptor); return FS_ERR_OUT_OF_MEMORY;
      }
      data = grown; capacity = next_capacity;
    }
    ssize_t received = read(descriptor, data + length, capacity - length - 1u);
    if (received < 0) {
      if (errno == EINTR) continue;
      FsStatus status = fs_errno_status(errno);
      free(data); close(descriptor); return status;
    }
    if (received == 0) break;
    length += (size_t)received;
  }
  if (close(descriptor) != 0) {
    FsStatus status = fs_errno_status(errno);
    free(data);
    return status;
  }
  data[length] = 0;
  *out_data = data;
  *out_size = length;
  return FS_OK;
#endif
}

FsStatus fs_read_file_arena(Arena *arena, const char *path,
                            const unsigned char **out_data, size_t *out_size) {
  if (arena == NULL || path == NULL || out_data == NULL || out_size == NULL)
    return FS_ERR_ARGUMENT;
  *out_data = NULL;
  *out_size = 0;
#ifdef _WIN32
  FsStatus regular_status = fs_win_require_regular_file(path);
  if (regular_status != FS_OK) return regular_status;
  FsStatus conversion_status;
  wchar_t *wide = fs_utf8_to_wide(path, &conversion_status);
  HANDLE file;
  LARGE_INTEGER file_size;
  unsigned char *buffer;
  if (wide == NULL) return conversion_status;
  file = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE |
                                          FILE_SHARE_DELETE,
                     NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  free(wide);
  if (file == INVALID_HANDLE_VALUE) return fs_win_error_status(GetLastError());
  if (!GetFileSizeEx(file, &file_size)) {
    FsStatus status = fs_win_error_status(GetLastError()); CloseHandle(file); return status;
  }
  if (file_size.QuadPart < 0 ||
      (unsigned long long)file_size.QuadPart > SIZE_MAX - 1u) {
    CloseHandle(file); return FS_ERR_RANGE;
  }
  size_t size = (size_t)file_size.QuadPart;
  buffer = (unsigned char *)arena_alloc(arena, size + 1u);
  if (buffer == NULL) { CloseHandle(file); return FS_ERR_OUT_OF_MEMORY; }
  size_t offset = 0;
  while (offset < size) {
    DWORD request = size - offset > 1024u * 1024u
                        ? 1024u * 1024u : (DWORD)(size - offset);
    DWORD received = 0;
    if (!ReadFile(file, buffer + offset, request, &received, NULL) || received == 0) {
      FsStatus status = fs_win_error_status(GetLastError());
      if (status == FS_OK) status = FS_ERR_IO;
      CloseHandle(file); return status;
    }
    offset += received;
  }
  {
    unsigned char extra_byte;
    DWORD received = 0;
    if (!ReadFile(file, &extra_byte, 1u, &received, NULL)) {
      FsStatus status = fs_win_error_status(GetLastError());
      CloseHandle(file);
      return status;
    }
    if (received != 0u) {
      CloseHandle(file);
      return FS_ERR_IO; /* File grew after its initial size was observed. */
    }
  }
  CloseHandle(file);
  buffer[size] = 0;
  *out_data = buffer; *out_size = size;
  return FS_OK;
#else
  int descriptor = open(path, O_RDONLY);
  struct stat info;
  unsigned char *buffer;
  if (descriptor < 0) return fs_errno_status(errno);
  if (fstat(descriptor, &info) != 0) {
    FsStatus status = fs_errno_status(errno); close(descriptor); return status;
  }
  if (!S_ISREG(info.st_mode)) { close(descriptor); return FS_ERR_NOT_FILE; }
  if (info.st_size < 0 || (uintmax_t)info.st_size > SIZE_MAX - 1u) {
    close(descriptor); return FS_ERR_RANGE;
  }
  size_t size = (size_t)info.st_size;
  buffer = (unsigned char *)arena_alloc(arena, size + 1u);
  if (buffer == NULL) { close(descriptor); return FS_ERR_OUT_OF_MEMORY; }
  size_t offset = 0;
  while (offset < size) {
    ssize_t received = read(descriptor, buffer + offset, size - offset);
    if (received < 0) {
      if (errno == EINTR) continue;
      FsStatus status = fs_errno_status(errno); close(descriptor); return status;
    }
    if (received == 0) { close(descriptor); return FS_ERR_IO; }
    offset += (size_t)received;
  }
  {
    unsigned char extra_byte;
    ssize_t received;
    do {
      received = read(descriptor, &extra_byte, 1u);
    } while (received < 0 && errno == EINTR);
    if (received < 0) {
      FsStatus status = fs_errno_status(errno);
      close(descriptor);
      return status;
    }
    if (received != 0) {
      close(descriptor);
      return FS_ERR_IO; /* File grew after its initial size was observed. */
    }
  }
  close(descriptor);
  buffer[size] = 0;
  *out_data = buffer; *out_size = size;
  return FS_OK;
#endif
}

FsStatus fs_copy_file_exclusive(const char *source, const char *destination) {
  void *data = NULL;
  size_t size = 0;
  FsStatus status = fs_read_file(source, &data, &size);
  if (status != FS_OK) return status;
  status = fs_write_file_atomic_exclusive(destination, data, size);
  free(data);
  return status;
}

FsStatus fs_sha256_file(const char *path,
                        unsigned char out_digest[FS_SHA256_SIZE]) {
  FsSha256 context;
  unsigned char buffer[64u * 1024u];
  if (path == NULL || out_digest == NULL) return FS_ERR_ARGUMENT;
#ifdef _WIN32
  FsStatus regular_status = fs_win_require_regular_file(path);
  if (regular_status != FS_OK) return regular_status;
#endif
  fs_sha256_init(&context);
#ifdef _WIN32
  FsStatus conversion_status;
  wchar_t *wide = fs_utf8_to_wide(path, &conversion_status);
  HANDLE file;
  if (wide == NULL) return conversion_status;
  file = CreateFileW(wide, GENERIC_READ,
                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                     NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  free(wide);
  if (file == INVALID_HANDLE_VALUE) return fs_win_error_status(GetLastError());
  for (;;) {
    DWORD received = 0;
    if (!ReadFile(file, buffer, (DWORD)sizeof(buffer), &received, NULL)) {
      FsStatus status = fs_win_error_status(GetLastError());
      CloseHandle(file);
      return status;
    }
    if (received == 0) break;
    fs_sha256_update(&context, buffer, (size_t)received);
  }
  if (!CloseHandle(file)) return fs_win_error_status(GetLastError());
#else
  int descriptor = open(path, O_RDONLY);
  if (descriptor < 0) return fs_errno_status(errno);
  for (;;) {
    ssize_t received = read(descriptor, buffer, sizeof(buffer));
    if (received < 0) {
      if (errno == EINTR) continue;
      FsStatus status = fs_errno_status(errno);
      close(descriptor);
      return status;
    }
    if (received == 0) break;
    fs_sha256_update(&context, buffer, (size_t)received);
  }
  if (close(descriptor) != 0) return fs_errno_status(errno);
#endif
  fs_sha256_final(&context, out_digest);
  return FS_OK;
}

void fs_sha256_bytes(const void *data, size_t size,
                     unsigned char out_digest[FS_SHA256_SIZE]) {
  fs_sha256_digest(data, size, out_digest);
}

void fs_sha256_hex(const unsigned char digest[FS_SHA256_SIZE],
                   char out_hex[FS_SHA256_HEX_SIZE]) {
  static const char alphabet[] = "0123456789abcdef";
  if (digest == NULL || out_hex == NULL) return;
  for (size_t i = 0; i < FS_SHA256_SIZE; ++i) {
    out_hex[i * 2u] = alphabet[digest[i] >> 4];
    out_hex[i * 2u + 1u] = alphabet[digest[i] & 0x0fu];
  }
  out_hex[64] = '\0';
}

FsStatus fs_sha256_file_hex(const char *path,
                            char out_hex[FS_SHA256_HEX_SIZE]) {
  unsigned char digest[FS_SHA256_SIZE];
  FsStatus status;
  if (out_hex == NULL) return FS_ERR_ARGUMENT;
  status = fs_sha256_file(path, digest);
  if (status != FS_OK) return status;
  fs_sha256_hex(digest, out_hex);
  return FS_OK;
}

#ifdef _WIN32
static FsStatus fs_remove_tree_internal(const wchar_t *path) {
  DWORD attributes = GetFileAttributesW(path);
  if (attributes == INVALID_FILE_ATTRIBUTES)
    return fs_win_error_status(GetLastError());
  if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
      (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    BOOL ok = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0
                  ? RemoveDirectoryW(path) : DeleteFileW(path);
    return ok ? FS_OK : fs_win_error_status(GetLastError());
  }
  size_t length = wcslen(path);
  if (length > SIZE_MAX / sizeof(*path) - 3u) return FS_ERR_RANGE;
  wchar_t *pattern = (wchar_t *)malloc((length + 3u) * sizeof(*pattern));
  if (pattern == NULL) return FS_ERR_OUT_OF_MEMORY;
  memcpy(pattern, path, (length + 1u) * sizeof(*pattern));
  if (length != 0 && pattern[length - 1u] != L'\\' &&
      pattern[length - 1u] != L'/') pattern[length++] = L'\\';
  pattern[length++] = L'*'; pattern[length] = L'\0';
  WIN32_FIND_DATAW item;
  HANDLE find = FindFirstFileW(pattern, &item);
  free(pattern);
  if (find == INVALID_HANDLE_VALUE) return fs_win_error_status(GetLastError());
  FsStatus status = FS_OK;
  do {
    if (wcscmp(item.cFileName, L".") == 0 ||
        wcscmp(item.cFileName, L"..") == 0) continue;
    size_t child_length = wcslen(path) + wcslen(item.cFileName) + 2u;
    wchar_t *child = (wchar_t *)malloc(child_length * sizeof(*child));
    if (child == NULL) { status = FS_ERR_OUT_OF_MEMORY; break; }
    swprintf(child, child_length, L"%ls\\%ls", path, item.cFileName);
    DWORD child_attributes = item.dwFileAttributes;
    if ((child_attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
        (child_attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0)
      status = fs_remove_tree_internal(child);
    else if ((child_attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
      status = RemoveDirectoryW(child) ? FS_OK : fs_win_error_status(GetLastError());
    else
      status = DeleteFileW(child) ? FS_OK : fs_win_error_status(GetLastError());
    free(child);
    if (status != FS_OK) break;
  } while (FindNextFileW(find, &item));
  DWORD error = GetLastError();
  FindClose(find);
  if (status == FS_OK && error != ERROR_NO_MORE_FILES)
    status = fs_win_error_status(error);
  if (status != FS_OK) return status;
  return RemoveDirectoryW(path) ? FS_OK : fs_win_error_status(GetLastError());
}
#endif

FsStatus fs_remove_tree(const char *path) {
  if (path == NULL || path[0] == '\0') return FS_ERR_ARGUMENT;
#ifdef _WIN32
  FsStatus conversion_status;
  wchar_t *wide = fs_utf8_to_wide(path, &conversion_status);
  if (wide == NULL) return conversion_status;
  FsStatus status = fs_remove_tree_internal(wide);
  free(wide);
  return status;
#else
  struct stat info;
  if (lstat(path, &info) != 0) return fs_errno_status(errno);
  if (!S_ISDIR(info.st_mode))
    return unlink(path) == 0 ? FS_OK : fs_errno_status(errno);
  DIR *directory = opendir(path);
  if (directory == NULL) return fs_errno_status(errno);
  FsStatus status = FS_OK;
  struct dirent *item;
  while ((item = readdir(directory)) != NULL) {
    if (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0)
      continue;
    char child[FS_PATH_MAX];
    status = fs_path_join(child, sizeof(child), path, item->d_name);
    if (status != FS_OK) break;
    status = fs_remove_tree(child);
    if (status != FS_OK) break;
  }
  if (closedir(directory) != 0 && status == FS_OK)
    status = fs_errno_status(errno);
  if (status != FS_OK) return status;
  return rmdir(path) == 0 ? FS_OK : fs_errno_status(errno);
#endif
}

FsStatus fs_find_executable_on_path(const char *executable_name, char *out,
                                    size_t out_capacity) {
  if (executable_name == NULL || executable_name[0] == '\0' || out == NULL ||
      out_capacity == 0 || strchr(executable_name, '/') != NULL ||
      strchr(executable_name, '\\') != NULL)
    return FS_ERR_ARGUMENT;
#ifdef _WIN32
  FsStatus conversion_status;
  wchar_t *name = fs_utf8_to_wide(executable_name, &conversion_status);
  DWORD path_needed;
  wchar_t *path_value;
  wchar_t *extensions;
  bool has_extension;
  if (name == NULL) return conversion_status;

  path_needed = GetEnvironmentVariableW(L"PATH", NULL, 0);
  if (path_needed == 0) {
    free(name);
    return FS_ERR_NOT_FOUND;
  }
  path_value = (wchar_t *)malloc((size_t)path_needed * sizeof(*path_value));
  if (path_value == NULL) {
    free(name);
    return FS_ERR_OUT_OF_MEMORY;
  }
  if (GetEnvironmentVariableW(L"PATH", path_value, path_needed) == 0) {
    FsStatus status = fs_win_error_status(GetLastError());
    free(name);
    free(path_value);
    return status;
  }

  DWORD extension_needed = GetEnvironmentVariableW(L"PATHEXT", NULL, 0);
  if (extension_needed == 0) {
    static const wchar_t default_extensions[] = L".COM;.EXE;.BAT;.CMD";
    extensions = (wchar_t *)malloc(sizeof(default_extensions));
    if (extensions != NULL)
      memcpy(extensions, default_extensions, sizeof(default_extensions));
  } else {
    extensions = (wchar_t *)malloc((size_t)extension_needed * sizeof(*extensions));
    if (extensions != NULL &&
        GetEnvironmentVariableW(L"PATHEXT", extensions, extension_needed) == 0) {
      free(extensions);
      extensions = NULL;
    }
  }
  if (extensions == NULL) {
    free(name);
    free(path_value);
    return FS_ERR_OUT_OF_MEMORY;
  }

  const wchar_t *name_base = wcsrchr(name, L'\\');
  if (name_base == NULL) name_base = wcsrchr(name, L'/');
  name_base = name_base ? name_base + 1 : name;
  has_extension = wcsrchr(name_base, L'.') != NULL;
  if (has_extension && !fs_extension_in_pathext(wcsrchr(name_base, L'.'))) {
    free(extensions);
    free(path_value);
    free(name);
    return FS_ERR_NOT_FOUND;
  }
  wchar_t *path_cursor = path_value;
  FsStatus result = FS_ERR_NOT_FOUND;

  while (*path_cursor != L'\0') {
    wchar_t *path_end = wcschr(path_cursor, L';');
    size_t directory_length = path_end
                                  ? (size_t)(path_end - path_cursor)
                                  : wcslen(path_cursor);
    wchar_t *directory = NULL;

    if (directory_length == 0) {
      DWORD cwd_needed = GetCurrentDirectoryW(0, NULL);
      if (cwd_needed != 0) {
        directory = (wchar_t *)malloc((size_t)cwd_needed * sizeof(*directory));
        if (directory != NULL &&
            GetCurrentDirectoryW(cwd_needed, directory) == 0) {
          free(directory);
          directory = NULL;
        }
      }
    } else {
      const wchar_t *segment = path_cursor;
      if (directory_length >= 2u && segment[0] == L'"' &&
          segment[directory_length - 1u] == L'"') {
        ++segment;
        directory_length -= 2u;
      }
      directory = (wchar_t *)malloc((directory_length + 1u) * sizeof(*directory));
      if (directory != NULL) {
        memcpy(directory, segment, directory_length * sizeof(*directory));
        directory[directory_length] = L'\0';
      }
    }

    if (directory == NULL) {
      result = FS_ERR_OUT_OF_MEMORY;
      break;
    }

    wchar_t *extension_cursor = has_extension ? NULL : extensions;
    do {
      wchar_t *extension_end = extension_cursor
                                   ? wcschr(extension_cursor, L';') : NULL;
      size_t extension_length = extension_cursor
                                    ? (extension_end
                                           ? (size_t)(extension_end - extension_cursor)
                                           : wcslen(extension_cursor))
                                    : 0u;
      size_t candidate_capacity = wcslen(directory) + wcslen(name) +
                                  extension_length + 3u;
      wchar_t *candidate = (wchar_t *)malloc(candidate_capacity * sizeof(*candidate));
      if (candidate == NULL) {
        result = FS_ERR_OUT_OF_MEMORY;
        break;
      }
      if (has_extension) {
        swprintf(candidate, candidate_capacity, L"%ls\\%ls", directory, name);
      } else {
        swprintf(candidate, candidate_capacity, L"%ls\\%ls%.*ls", directory,
                 name, (int)extension_length, extension_cursor);
      }

      DWORD attributes = GetFileAttributesW(candidate);
      if (attributes != INVALID_FILE_ATTRIBUTES &&
          (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        result = fs_wide_to_utf8(candidate, out, out_capacity);
        free(candidate);
        break;
      }
      free(candidate);

      if (has_extension || extension_end == NULL) break;
      extension_cursor = extension_end + 1;
    } while (true);

    free(directory);
    if (result != FS_ERR_NOT_FOUND) break;
    if (path_end == NULL) break;
    path_cursor = path_end + 1;
  }

  free(extensions);
  free(path_value);
  free(name);
  return result;
#else
  const char *path_value = getenv("PATH");
  if (path_value == NULL) return FS_ERR_NOT_FOUND;
  const char *cursor = path_value;
  while (*cursor != '\0') {
    const char *end = strchr(cursor, ':');
    size_t directory_length = end ? (size_t)(end - cursor) : strlen(cursor);
    char directory[FS_PATH_MAX];
    char candidate[FS_PATH_MAX];
    if (directory_length == 0) {
      FsStatus status = fs_get_current_directory(directory, sizeof(directory));
      if (status != FS_OK) directory[0] = '\0';
    } else if (directory_length < sizeof(directory)) {
      memcpy(directory, cursor, directory_length);
      directory[directory_length] = '\0';
    } else {
      directory[0] = '\0';
    }
    if (directory[0] != '\0' &&
        fs_path_join(candidate, sizeof(candidate), directory,
                     executable_name) == FS_OK && fs_is_executable(candidate))
      return fs_copy_string(out, out_capacity, candidate);
    if (end == NULL) break;
    cursor = end + 1;
  }
  return FS_ERR_NOT_FOUND;
#endif
}

const char *fs_status_name(FsStatus status) {
  static const char *const names[] = {
      "OK", "ARGUMENT_ERROR", "RANGE_ERROR", "NOT_FOUND", "ALREADY_EXISTS",
      "NOT_DIRECTORY", "NOT_FILE", "PERMISSION_ERROR", "IO_ERROR",
      "OUT_OF_MEMORY", "INVALID_PATH", "NOT_SUPPORTED"};
  if ((unsigned)status >= sizeof(names) / sizeof(names[0]))
    return "UNKNOWN_STATUS";
  return names[(unsigned)status];
}

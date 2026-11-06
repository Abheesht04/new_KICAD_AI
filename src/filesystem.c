#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "filesystem.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#define FS_NATIVE_SEP '\\'
#define fs_stat _stat
#define fs_mkdir(path) _mkdir(path)
#define fs_getcwd(buf, size) _getcwd((buf), (int)(size))
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define FS_NATIVE_SEP '/'
#define fs_stat stat
#define fs_mkdir(path) mkdir((path), 0777)
#define fs_getcwd(buf, size) getcwd((buf), (size))
#endif

static bool is_separator(char c) { return c == '/' || c == '\\'; }

static FsStatus copy_string(char *out, size_t capacity, const char *value) {
  size_t length;
  if (out == NULL || capacity == 0 || value == NULL) {
    return FS_ERR_ARGUMENT;
  }
  length = strlen(value);
  if (length >= capacity) {
    return FS_ERR_RANGE;
  }
  memcpy(out, value, length + 1);
  return FS_OK;
}

bool fs_path_is_absolute(const char *path) {
  if (path == NULL || path[0] == '\0') {
    return false;
  }
#ifdef _WIN32
  if (is_separator(path[0])) {
    return true; /* rooted path or UNC path */
  }
  return ((path[0] >= 'A' && path[0] <= 'Z') ||
          (path[0] >= 'a' && path[0] <= 'z')) &&
         path[1] == ':' && is_separator(path[2]);
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

  if (out == NULL || out_capacity == 0 || left == NULL || right == NULL) {
    return FS_ERR_ARGUMENT;
  }
  if (fs_path_is_absolute(right)) {
    return copy_string(out, out_capacity, right);
  }

  left_length = strlen(left);
  right_length = strlen(right);
  while (right_start < right_length && is_separator(right[right_start])) {
    ++right_start;
  }
  while (left_length > 0 && is_separator(left[left_length - 1])) {
    /* Preserve a filesystem root such as / or C:\\ . */
#ifdef _WIN32
    if (left_length == 3 && left[1] == ':') {
      break;
    }
#else
    if (left_length == 1) {
      break;
    }
#endif
    --left_length;
  }

  need_separator = left_length > 0 && right_start < right_length &&
                   !is_separator(left[left_length - 1]);
  if (left_length > SIZE_MAX - right_length - 2) {
    return FS_ERR_RANGE;
  }
  if (left_length + (need_separator ? 1u : 0u) +
          (right_length - right_start) + 1 >
      out_capacity) {
    return FS_ERR_RANGE;
  }

  if (left_length > 0) {
    memcpy(out, left, left_length);
  }
  size_t pos = left_length;
  if (need_separator) {
    out[pos++] = FS_NATIVE_SEP;
  }
  if (right_length > right_start) {
    memcpy(out + pos, right + right_start, right_length - right_start);
    pos += right_length - right_start;
  }
  out[pos] = '\0';
  return FS_OK;
}

FsStatus fs_get_current_directory(char *out, size_t out_capacity) {
  if (out == NULL || out_capacity == 0) {
    return FS_ERR_ARGUMENT;
  }
  if (fs_getcwd(out, out_capacity) == NULL) {
    return errno == ERANGE ? FS_ERR_RANGE : FS_ERR_IO;
  }
  return FS_OK;
}

FsStatus fs_path_absolute(char *out, size_t out_capacity, const char *path) {
  char cwd[FS_PATH_MAX];
  if (out == NULL || out_capacity == 0 || path == NULL || path[0] == '\0') {
    return FS_ERR_ARGUMENT;
  }
#ifdef _WIN32
  if (_fullpath(out, path, out_capacity) == NULL) {
    return errno == ERANGE ? FS_ERR_RANGE : FS_ERR_IO;
  }
  return FS_OK;
#else
  if (path[0] == '/') {
    return copy_string(out, out_capacity, path);
  }
  FsStatus status = fs_get_current_directory(cwd, sizeof(cwd));
  if (status != FS_OK) {
    return status;
  }
  return fs_path_join(out, out_capacity, cwd, path);
#endif
}

FsStatus fs_path_basename(char *out, size_t out_capacity, const char *path) {
  size_t length;
  size_t start;
  if (out == NULL || out_capacity == 0 || path == NULL || path[0] == '\0') {
    return FS_ERR_ARGUMENT;
  }
  length = strlen(path);
  while (length > 0 && is_separator(path[length - 1])) {
    --length;
  }
  start = length;
  while (start > 0 && !is_separator(path[start - 1])) {
    --start;
  }
  if (length - start >= out_capacity) {
    return FS_ERR_RANGE;
  }
  memcpy(out, path + start, length - start);
  out[length - start] = '\0';
  return FS_OK;
}

#ifdef _WIN32
static bool stat_path(const char *path, struct _stat *st) {
  return path != NULL && _stat(path, st) == 0;
}
#else
static bool stat_path(const char *path, struct stat *st) {
  return path != NULL && stat(path, st) == 0;
}
#endif

bool fs_file_exists(const char *path) {
#ifdef _WIN32
  struct _stat st;
#else
  struct stat st;
#endif
#ifdef _WIN32
  return stat_path(path, &st) && (st.st_mode & _S_IFMT) == _S_IFREG;
#else
  return stat_path(path, &st) && S_ISREG(st.st_mode);
#endif
}

bool fs_directory_exists(const char *path) {
#ifdef _WIN32
  struct _stat st;
#else
  struct stat st;
#endif
#ifdef _WIN32
  return stat_path(path, &st) && (st.st_mode & _S_IFMT) == _S_IFDIR;
#else
  return stat_path(path, &st) && S_ISDIR(st.st_mode);
#endif
}

bool fs_is_executable(const char *path) {
  if (!fs_file_exists(path)) {
    return false;
  }
#ifdef _WIN32
  return true; /* Windows uses executable file extensions and CreateProcess. */
#else
  return access(path, X_OK) == 0;
#endif
}

FsStatus fs_create_directory(const char *path) {
  if (path == NULL || path[0] == '\0') {
    return FS_ERR_ARGUMENT;
  }
  if (fs_directory_exists(path)) {
    return FS_ERR_ALREADY_EXISTS;
  }
  if (fs_file_exists(path)) {
    return FS_ERR_NOT_DIRECTORY;
  }
  if (fs_mkdir(path) == 0) {
    return FS_OK;
  }
  if (errno == EEXIST) {
    return fs_directory_exists(path) ? FS_ERR_ALREADY_EXISTS
                                     : FS_ERR_NOT_DIRECTORY;
  }
  if (errno == EACCES || errno == EPERM) {
    return FS_ERR_PERMISSION;
  }
  return FS_ERR_IO;
}

static size_t path_root_length(const char *path) {
  if (path == NULL || path[0] == '\0') {
    return 0;
  }
#ifdef _WIN32
  if (is_separator(path[0]) && is_separator(path[1])) {
    /* UNC: \\server\share\ is the root. */
    size_t i = 2;
    while (path[i] != '\0' && !is_separator(path[i])) ++i;
    if (path[i] == '\0') return i;
    ++i;
    while (path[i] != '\0' && !is_separator(path[i])) ++i;
    return i;
  }
  if (((path[0] >= 'A' && path[0] <= 'Z') ||
       (path[0] >= 'a' && path[0] <= 'z')) &&
      path[1] == ':' && is_separator(path[2])) {
    return 3;
  }
  return is_separator(path[0]) ? 1 : 0;
#else
  return path[0] == '/' ? 1 : 0;
#endif
}

FsStatus fs_create_directories(const char *path) {
  char buffer[FS_PATH_MAX];
  size_t length;
  size_t root_length;
  FsStatus status;

  if (path == NULL || path[0] == '\0') {
    return FS_ERR_ARGUMENT;
  }
  length = strlen(path);
  if (length >= sizeof(buffer)) {
    return FS_ERR_RANGE;
  }
  memcpy(buffer, path, length + 1);
  root_length = path_root_length(buffer);
  if (length == root_length && fs_directory_exists(buffer)) {
    return FS_OK;
  }

  for (size_t i = root_length; i <= length; ++i) {
    if (buffer[i] == '\0' || is_separator(buffer[i])) {
      char saved = buffer[i];
      buffer[i] = '\0';
      if (buffer[0] != '\0' && !(i == 2 && buffer[1] == ':')) {
        if (!fs_directory_exists(buffer)) {
          status = fs_create_directory(buffer);
          if (status != FS_OK && status != FS_ERR_ALREADY_EXISTS) {
            buffer[i] = saved;
            return status;
          }
        }
      }
      buffer[i] = saved;
    }
  }
  return fs_directory_exists(path) ? FS_OK : FS_ERR_IO;
}

FsStatus fs_write_file_exclusive(const char *path, const void *data,
                                 size_t size) {
  if (path == NULL || path[0] == '\0' || (data == NULL && size != 0)) {
    return FS_ERR_ARGUMENT;
  }
#ifdef _WIN32
  HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL, NULL);
  if (file == INVALID_HANDLE_VALUE) {
    DWORD error = GetLastError();
    if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS)
      return FS_ERR_ALREADY_EXISTS;
    if (error == ERROR_ACCESS_DENIED) return FS_ERR_PERMISSION;
    return FS_ERR_IO;
  }
  const unsigned char *bytes = (const unsigned char *)data;
  size_t remaining = size;
  bool ok = true;
  while (remaining > 0) {
    DWORD chunk = remaining > 1024u * 1024u ? 1024u * 1024u : (DWORD)remaining;
    DWORD written = 0;
    if (!WriteFile(file, bytes, chunk, &written, NULL) || written == 0) {
      ok = false;
      break;
    }
    bytes += written;
    remaining -= written;
  }
  if (!CloseHandle(file)) ok = false;
  if (!ok) {
    DeleteFileA(path);
    return FS_ERR_IO;
  }
  return FS_OK;
#else
  int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666);
  if (fd < 0) {
    if (errno == EEXIST) return FS_ERR_ALREADY_EXISTS;
    if (errno == EACCES || errno == EPERM) return FS_ERR_PERMISSION;
    return FS_ERR_IO;
  }
  const unsigned char *bytes = (const unsigned char *)data;
  size_t remaining = size;
  bool ok = true;
  while (remaining > 0) {
    ssize_t written = write(fd, bytes, remaining > (size_t)SSIZE_MAX
                                                ? SSIZE_MAX
                                                : (size_t)remaining);
    if (written < 0) {
      if (errno == EINTR) continue;
      ok = false;
      break;
    }
    if (written == 0) {
      ok = false;
      break;
    }
    bytes += (size_t)written;
    remaining -= (size_t)written;
  }
  if (close(fd) != 0) ok = false;
  if (!ok) {
    unlink(path);
    return FS_ERR_IO;
  }
  return FS_OK;
#endif
}

FsStatus fs_rename_path(const char *old_path, const char *new_path) {
  if (old_path == NULL || new_path == NULL || old_path[0] == '\0' ||
      new_path[0] == '\0') {
    return FS_ERR_ARGUMENT;
  }
#ifdef _WIN32
  if (MoveFileA(old_path, new_path)) return FS_OK;
  DWORD error = GetLastError();
  if (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS)
    return FS_ERR_ALREADY_EXISTS;
  if (error == ERROR_ACCESS_DENIED) return FS_ERR_PERMISSION;
  return FS_ERR_IO;
#else
  if (rename(old_path, new_path) == 0) return FS_OK;
  if (errno == EEXIST || errno == ENOTEMPTY) return FS_ERR_ALREADY_EXISTS;
  if (errno == EACCES || errno == EPERM) return FS_ERR_PERMISSION;
  return FS_ERR_IO;
#endif
}

FsStatus fs_remove_tree(const char *path) {
  if (path == NULL || path[0] == '\0') return FS_ERR_ARGUMENT;
#ifdef _WIN32
  if (!fs_directory_exists(path)) {
    if (DeleteFileA(path)) return FS_OK;
    if (GetLastError() == ERROR_FILE_NOT_FOUND) return FS_ERR_NOT_FOUND;
    if (RemoveDirectoryA(path)) return FS_OK;
  }
#else
  struct stat root_stat;
  if (lstat(path, &root_stat) != 0)
    return errno == ENOENT ? FS_ERR_NOT_FOUND : FS_ERR_IO;
  if (!S_ISDIR(root_stat.st_mode))
    return unlink(path) == 0 ? FS_OK : FS_ERR_IO;
#endif
#ifdef _WIN32
  char pattern[FS_PATH_MAX];
  if (fs_path_join(pattern, sizeof(pattern), path, "*") != FS_OK)
    return FS_ERR_RANGE;
  WIN32_FIND_DATAA item;
  HANDLE find = FindFirstFileA(pattern, &item);
  if (find == INVALID_HANDLE_VALUE) return FS_ERR_IO;
  FsStatus result = FS_OK;
  do {
    if (strcmp(item.cFileName, ".") == 0 ||
        strcmp(item.cFileName, "..") == 0) continue;
    char child[FS_PATH_MAX];
    if (fs_path_join(child, sizeof(child), path, item.cFileName) != FS_OK) {
      result = FS_ERR_RANGE;
      break;
    }
    if ((item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
        (item.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
      result = fs_remove_tree(child);
    } else if ((item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
      result = RemoveDirectoryA(child) ? FS_OK : FS_ERR_IO;
    } else {
      result = DeleteFileA(child) ? FS_OK : FS_ERR_IO;
    }
    if (result != FS_OK) break;
  } while (FindNextFileA(find, &item));
  FindClose(find);
  if (result != FS_OK) return result;
  return RemoveDirectoryA(path) ? FS_OK : FS_ERR_IO;
#else
  DIR *directory = opendir(path);
  if (directory == NULL) return FS_ERR_IO;
  FsStatus result = FS_OK;
  struct dirent *item;
  while ((item = readdir(directory)) != NULL) {
    if (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0)
      continue;
    char child[FS_PATH_MAX];
    if (fs_path_join(child, sizeof(child), path, item->d_name) != FS_OK) {
      result = FS_ERR_RANGE;
      break;
    }
    struct stat st;
    if (lstat(child, &st) != 0) {
      result = FS_ERR_IO;
      break;
    }
    if (S_ISDIR(st.st_mode)) {
      result = fs_remove_tree(child);
    } else {
      result = unlink(child) == 0 ? FS_OK : FS_ERR_IO;
    }
    if (result != FS_OK) break;
  }
  closedir(directory);
  if (result != FS_OK) return result;
  return rmdir(path) == 0 ? FS_OK : FS_ERR_IO;
#endif
}

FsStatus fs_find_executable_on_path(const char *executable_name, char *out,
                                    size_t out_capacity) {
  const char *path_env;
  size_t name_length;
  if (executable_name == NULL || executable_name[0] == '\0' || out == NULL ||
      out_capacity == 0 || strchr(executable_name, '/') != NULL ||
      strchr(executable_name, '\\') != NULL) {
    return FS_ERR_ARGUMENT;
  }
  name_length = strlen(executable_name);
  path_env = getenv("PATH");
  if (path_env == NULL) return FS_ERR_NOT_FOUND;

  const char *cursor = path_env;
#ifdef _WIN32
  const char delimiter = ';';
#else
  const char delimiter = ':';
#endif
  while (*cursor != '\0') {
    const char *end = strchr(cursor, delimiter);
    size_t directory_length = end ? (size_t)(end - cursor) : strlen(cursor);
    char directory[FS_PATH_MAX];
    char candidate[FS_PATH_MAX];
    if (directory_length == 0) {
      if (fs_get_current_directory(directory, sizeof(directory)) != FS_OK)
        directory[0] = '\0';
    } else if (directory_length < sizeof(directory)) {
      memcpy(directory, cursor, directory_length);
      directory[directory_length] = '\0';
    } else {
      directory[0] = '\0';
    }
    if (directory[0] != '\0' &&
        fs_path_join(candidate, sizeof(candidate), directory,
                     executable_name) == FS_OK &&
        fs_is_executable(candidate)) {
      return copy_string(out, out_capacity, candidate);
    }
#ifdef _WIN32
    if (directory[0] != '\0' && strchr(executable_name, '.') == NULL) {
      char exe_name[FS_PATH_MAX];
      if (name_length + 5 < sizeof(exe_name)) {
        memcpy(exe_name, executable_name, name_length);
        memcpy(exe_name + name_length, ".exe", 5);
        if (fs_path_join(candidate, sizeof(candidate), directory, exe_name) ==
                FS_OK &&
            fs_is_executable(candidate)) {
          return copy_string(out, out_capacity, candidate);
        }
      }
    }
#endif
    if (end == NULL) break;
    cursor = end + 1;
  }
  return FS_ERR_NOT_FOUND;
}

const char *fs_status_name(FsStatus status) {
  static const char *const names[] = {
      "OK",          "ARGUMENT_ERROR", "RANGE_ERROR", "NOT_FOUND",
      "ALREADY_EXISTS", "NOT_DIRECTORY", "NOT_FILE", "PERMISSION_ERROR",
      "IO_ERROR",    "OUT_OF_MEMORY"};
  if ((unsigned)status >= sizeof(names) / sizeof(names[0]))
    return "UNKNOWN_STATUS";
  return names[status];
}

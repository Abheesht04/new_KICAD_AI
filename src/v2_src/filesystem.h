#ifndef COPPER_FILESYSTEM_H
#define COPPER_FILESYSTEM_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum FsStatus {
  FS_OK = 0,
  FS_ERR_ARGUMENT,
  FS_ERR_RANGE,
  FS_ERR_NOT_FOUND,
  FS_ERR_ALREADY_EXISTS,
  FS_ERR_NOT_DIRECTORY,
  FS_ERR_NOT_FILE,
  FS_ERR_PERMISSION,
  FS_ERR_IO,
  FS_ERR_OUT_OF_MEMORY
} FsStatus;

/* Path helpers use caller-owned output buffers. Paths are not canonicalized. */
#define FS_PATH_MAX 4096

bool fs_path_is_absolute(const char *path);
FsStatus fs_path_join(char *out, size_t out_capacity, const char *left,
                      const char *right);
FsStatus fs_path_absolute(char *out, size_t out_capacity, const char *path);
FsStatus fs_path_basename(char *out, size_t out_capacity, const char *path);

bool fs_file_exists(const char *path);
bool fs_directory_exists(const char *path);
bool fs_is_executable(const char *path);

FsStatus fs_get_current_directory(char *out, size_t out_capacity);
FsStatus fs_create_directory(const char *path);
FsStatus fs_create_directories(const char *path);

/* Creates a new file only. It never overwrites an existing file. */
FsStatus fs_write_file_exclusive(const char *path, const void *data,
                                 size_t size);
/* Rename fails rather than replacing an existing destination on Windows. */
FsStatus fs_rename_path(const char *old_path, const char *new_path);
/* Recursively removes path. Use only for directories owned by Copper. */
FsStatus fs_remove_tree(const char *path);

/* Searches PATH for an executable. executable_name should be a bare name. */
FsStatus fs_find_executable_on_path(const char *executable_name, char *out,
                                    size_t out_capacity);

const char *fs_status_name(FsStatus status);

#ifdef __cplusplus
}
#endif

#endif /* COPPER_FILESYSTEM_H */

#ifndef COPPER_FILESYSTEM_H
#define COPPER_FILESYSTEM_H

#include "arena.h"

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
  FS_ERR_OUT_OF_MEMORY,
  FS_ERR_INVALID_PATH,
  FS_ERR_NOT_SUPPORTED
} FsStatus;

/*
 * All public paths are UTF-8. On Windows they are converted to UTF-16 at the
 * filesystem boundary. FS_PATH_MAX is a byte capacity for UTF-8 buffers.
 */
#define FS_PATH_MAX 4096
#define FS_SHA256_SIZE 32
#define FS_SHA256_HEX_SIZE 65

bool fs_path_is_absolute(const char *path);
FsStatus fs_path_join(char *out, size_t out_capacity, const char *left,
                      const char *right);
/* Returns an absolute, lexically normalised path (mixed separators, . and ..). */
FsStatus fs_path_absolute(char *out, size_t out_capacity, const char *path);
FsStatus fs_path_normalize(char *out, size_t out_capacity, const char *path);
/* Resolves symlinks/junctions and requires the target to exist. */
FsStatus fs_path_resolve_existing(char *out, size_t out_capacity,
                                  const char *path);
/* Physical containment check; both paths must exist. */
FsStatus fs_path_is_within(const char *candidate, const char *allowed_root,
                           bool *out_within);
FsStatus fs_path_basename(char *out, size_t out_capacity, const char *path);

bool fs_file_exists(const char *path);
bool fs_directory_exists(const char *path);
bool fs_is_executable(const char *path);

FsStatus fs_get_current_directory(char *out, size_t out_capacity);
FsStatus fs_create_directory(const char *path);
FsStatus fs_create_directories(const char *path);

/* Direct exclusive write. The final path becomes visible while being written. */
FsStatus fs_write_file_exclusive(const char *path, const void *data,
                                 size_t size);
/*
 * Atomic create: writes a sibling temporary file, flushes it, then commits
 * without replacing an existing destination.
 */
FsStatus fs_write_file_atomic_exclusive(const char *path, const void *data,
                                        size_t size);
/* Atomic replacement. Existing destination contents are replaced on commit. */
FsStatus fs_write_file_atomic_replace(const char *path, const void *data,
                                      size_t size);
/* POSIX follows rename() replacement rules; Windows MoveFileW does not replace.
 * Prefer the explicit atomic write functions for generated files.
 */
FsStatus fs_rename_path(const char *old_path, const char *new_path);
FsStatus fs_rename_path_replace(const char *old_path, const char *new_path);
FsStatus fs_remove_tree(const char *path);

/* PATH lookup. Windows honours PATHEXT; input and result are UTF-8. */
FsStatus fs_find_executable_on_path(const char *executable_name, char *out,
                                    size_t out_capacity);

typedef struct FsDirectoryEntry {
  char *name; /* malloc-owned UTF-8 */
  bool is_directory;
  bool is_regular_file;
} FsDirectoryEntry;

typedef struct FsDirectoryListing {
  FsDirectoryEntry *entries; /* malloc-owned */
  size_t count;
} FsDirectoryListing;

/* Entries are sorted by UTF-8 byte order for deterministic processing. */
FsStatus fs_list_directory(const char *path, FsDirectoryListing *out);
void fs_free_directory_listing(FsDirectoryListing *listing);

/* malloc-owned bytes; caller releases with free(). */
FsStatus fs_read_file(const char *path, void **out_data, size_t *out_size);
/* Arena-owned bytes plus a trailing NUL; binary size excludes the terminator. */
FsStatus fs_read_file_arena(Arena *arena, const char *path,
                            const unsigned char **out_data, size_t *out_size);
FsStatus fs_copy_file_exclusive(const char *source, const char *destination);

/* SHA-256 helpers. Digest bytes are in standard network-order representation. */
void fs_sha256_bytes(const void *data, size_t size,
                     unsigned char out_digest[FS_SHA256_SIZE]);
void fs_sha256_hex(const unsigned char digest[FS_SHA256_SIZE],
                   char out_hex[FS_SHA256_HEX_SIZE]);
FsStatus fs_sha256_file(const char *path,
                        unsigned char out_digest[FS_SHA256_SIZE]);
FsStatus fs_sha256_file_hex(const char *path,
                            char out_hex[FS_SHA256_HEX_SIZE]);

const char *fs_status_name(FsStatus status);

#ifdef __cplusplus
}
#endif

#endif /* COPPER_FILESYSTEM_H */

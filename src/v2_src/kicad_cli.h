#ifndef COPPER_V2_KICAD_CLI_H
#define COPPER_V2_KICAD_CLI_H

/*
 * kicad_cli.h - run kicad-cli without a shell, with a timeout, never over existing files (Copper V2 track D).
 *
 * Jobs: version, DRC (JSON report), Gerbers, drill (Excellon, metric, decimal, absolute origin) and the
 * component position file (CSV, mm, both sides).
 *
 * How a job runs:
 *   1. The output path must not exist (KCLI_EXISTS otherwise: nothing is ever overwritten or merged into).
 *   2. A fresh staging folder "<output>.copper-staging-<pid>-<n>" is created next to it; kicad-cli writes there,
 *      with its stdout and stderr redirected to files in that folder (no pipes, so no deadlock on large output).
 *   3. The process gets argv directly: CreateProcessW with each argument quoted by the Windows rules (UTF-8 paths
 *      converted to UTF-16), posix_spawn elsewhere. It runs in its own Windows job object / POSIX process group;
 *      when the timeout expires the whole tree is killed and the result is KCLI_TIMEOUT.
 *   4. On success the output is moved into place with a rename that refuses to replace (MoveFileExW without
 *      REPLACE_EXISTING; link + unlink for files, a checked rename for folders on POSIX), and the staging folder is
 *      removed. On any failure the staging folder is removed and the output path is left untouched.
 * The result records the exact argv, exit code, elapsed time, captured stdout/stderr, the files produced and the
 * kicad-cli version. KiCad 9.x is the only version accepted as native evidence (CLAUDE.md): `require_9x` makes any
 * other version KCLI_WRONG_VERSION before a job runs; without it the result says which version ran.
 *
 * Memory: the caller's Arena. No printf, no exit. Paths are UTF-8.
 */

#include "arena.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum KicadCliStatus {
    KCLI_OK = 0,
    KCLI_INVALID_ARGUMENT,
    KCLI_NOT_FOUND,        /* no kicad-cli at the given path, KICAD_CLI_PATH, PATH or the KiCad 9 install folders */
    KCLI_WRONG_VERSION,    /* require_9x and the CLI is not 9.x, or its version could not be read */
    KCLI_EXISTS,           /* the output path already exists */
    KCLI_LAUNCH_FAILED,
    KCLI_TIMEOUT,          /* killed after timeout_ms */
    KCLI_TOOL_FAILED,      /* an exit code the job does not accept */
    KCLI_NO_OUTPUT,        /* exit code accepted, but the expected output is missing */
    KCLI_IO_ERROR,
    KCLI_OUT_OF_MEMORY
} KicadCliStatus;

const char *kicad_cli_status_name(KicadCliStatus status);

typedef enum KicadCliJob { KCLI_JOB_VERSION = 0, KCLI_JOB_DRC, KCLI_JOB_GERBERS, KCLI_JOB_DRILL, KCLI_JOB_POS } KicadCliJob;

const char *kicad_cli_job_name(KicadCliJob job);

typedef struct KicadCli {
    const char *path;          /* absolute path of the executable */
    const char *version;       /* e.g. "9.0.6" */
    int major, minor, patch;   /* -1 when unknown */
    uint32_t timeout_ms;       /* per job; 0 means no limit */
    bool require_9x;
} KicadCli;

typedef struct KicadCliResult {
    KicadCliStatus status;
    KicadCliJob job;
    const char **argv;         /* exactly what was passed, argv[0] = the executable */
    size_t argc;
    bool launched, timed_out;
    int exit_code;             /* -1 when not launched or killed */
    uint64_t elapsed_ms;
    const char *stdout_text, *stderr_text;   /* captured, NUL-terminated (never NULL after a launch) */
    size_t stdout_len, stderr_len;
    const char *output;        /* the final output path (file or folder), when KCLI_OK */
    const char **files;        /* files produced, relative to `output` for folder jobs */
    size_t file_count;
    bool drc_violations;       /* DRC: exit code 5 under --exit-code-violations (the report is still written) */
    const char *cli_version;   /* the version of the CLI that ran */
    bool cli_is_9x;
    const char *message;       /* static explanation when status != KCLI_OK */
} KicadCliResult;

/* Find kicad-cli (explicit path, KICAD_CLI_PATH, PATH, KiCad 9 install folders) and read its version. */
KicadCliStatus kicad_cli_open(Arena *arena, const char *explicit_path, uint32_t timeout_ms, bool require_9x,
                              KicadCli *out, KicadCliResult *version_result);

typedef struct KicadDrcOptions {
    bool schematic_parity;     /* --schematic-parity (needs the schematic next to the board) */
    bool all_track_errors;     /* --all-track-errors */
} KicadDrcOptions;

/* DRC as JSON into `out_json` (a file that must not exist). KCLI_OK for exit 0 and for exit 5 (violations). */
KicadCliStatus kicad_cli_drc(Arena *arena, const KicadCli *cli, const char *board, const char *out_json,
                             const KicadDrcOptions *options, KicadCliResult *result);
/* Gerbers (and the job file) into a new folder `out_dir`. */
KicadCliStatus kicad_cli_gerbers(Arena *arena, const KicadCli *cli, const char *board, const char *out_dir,
                                 KicadCliResult *result);
/* Excellon drill files into a new folder: metric, decimal coordinates, absolute origin (what excellon.c reads). */
KicadCliStatus kicad_cli_drill(Arena *arena, const KicadCli *cli, const char *board, const char *out_dir,
                               KicadCliResult *result);
/* Component positions as CSV in mm, both sides, into `out_file`. */
KicadCliStatus kicad_cli_pos(Arena *arena, const KicadCli *cli, const char *board, const char *out_file,
                             KicadCliResult *result);

/* Windows command-line quoting of one argument (CommandLineToArgvW rules), exposed for tests. Returns the length,
 * or (size_t)-1 when `cap` is too small. */
size_t kicad_cli_quote_arg(const char *arg, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif

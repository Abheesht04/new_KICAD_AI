#ifndef PCBA_CLI_H
#define PCBA_CLI_H
/*
 * pcba: the single user-facing PCBAI command.
 *
 * pcba owns the command tree, argument validation, help, colour policy, the
 * JSON envelope and the exit-code contract. Engineering work is delegated:
 * tools/copper.py orchestrates (via its --result-json protocol) and
 * copper-core/hwcore own physics, geometry and DFM. No numerical, KiCad or
 * DFM implementation lives in these files.
 */
#include <stddef.h>
#include <stdio.h>

#include "cli_os.h"

/* Exit-code contract; documented in `pcba help` and docs/engineering/CLI.md. */
enum {
    PCBA_EXIT_PASS = 0,
    PCBA_EXIT_FAIL = 1,
    PCBA_EXIT_INVALID_INPUT = 2,
    PCBA_EXIT_UNVERIFIED = 3,
    PCBA_EXIT_NOT_IMPLEMENTED = 4,
    PCBA_EXIT_USAGE = 64,
    PCBA_EXIT_INTERNAL = 70
};

typedef struct {
    const char *status;
    int exit_code;
    const char *meaning;
} PcbaStatusInfo;

const PcbaStatusInfo *pcba_statuses(size_t *count);
int pcba_exit_for_status(const char *status); /* unknown statuses map to INTERNAL */
const char *pcba_status_for_exit(int exit_code);

typedef struct {
    int json;           /* --json: exactly one JSON document on stdout */
    int color;          /* ANSI styling enabled for stdout */
    char exe_dir[1024]; /* directory holding pcba (and copper-core) */
} PcbaContext;

/* ---- command table and argument parsing ---- */
#define PCBA_MAX_OPTIONS 16
#define PCBA_MAX_POSITIONAL 4

typedef struct {
    const char *name;    /* "--out" */
    const char *metavar; /* "DIR"; NULL for a flag */
    int required;
    const char *help;
} PcbaOption;

typedef struct {
    const char *positional[PCBA_MAX_POSITIONAL];
    size_t positional_count;
    const char *values[PCBA_MAX_OPTIONS]; /* value, or "" for a present flag; NULL when absent */
} PcbaArgs;

typedef struct PcbaCommand PcbaCommand;
typedef int (*PcbaRunFn)(PcbaContext *ctx, const PcbaCommand *cmd, const PcbaArgs *args);

struct PcbaCommand {
    const char *group;      /* "project", or NULL for a top-level command */
    const char *name;       /* "check" */
    const char *positional; /* "<file.copper>", or "" */
    size_t min_positional, max_positional;
    const char *summary;    /* one line */
    const char *description;
    const char *examples;   /* newline-separated */
    const PcbaOption *options;
    size_t option_count;
    const char *planned;    /* non-NULL: NOT_IMPLEMENTED; names the stage that adds it */
    PcbaRunFn run;
};

const PcbaCommand *pcba_commands(size_t *count);
int pcba_is_group(const char *word);
/* Finds the command named by argv[0] (and argv[1] for a group); *consumed = words used. */
const PcbaCommand *pcba_find_command(int argc, char **argv, int *consumed);
void pcba_command_path(const PcbaCommand *cmd, char *buf, size_t cap);   /* "project check" */
void pcba_command_usage(const PcbaCommand *cmd, char *buf, size_t cap);  /* "pcba project check <file.copper>" */
int pcba_parse_args(const PcbaCommand *cmd, int argc, char **argv, PcbaArgs *out, char *err, size_t cap);
const char *pcba_opt(const PcbaCommand *cmd, const PcbaArgs *args, const char *name);
int pcba_parse_double(const char *text, double *out);
int pcba_parse_count(const char *text, long min, long max, long *out);

/* ---- terminal output ---- */
typedef enum { PCBA_STYLE_RESET, PCBA_STYLE_BOLD, PCBA_STYLE_DIM, PCBA_STYLE_PASS, PCBA_STYLE_FAIL,
               PCBA_STYLE_WARN, PCBA_STYLE_INFO, PCBA_STYLE_ACCENT } PcbaStyle;
void pcba_term_init(PcbaContext *ctx, int no_color_flag);
const char *pcba_style(const PcbaContext *ctx, PcbaStyle style);
const char *pcba_status_style(const PcbaContext *ctx, const char *status);
void pcba_banner(const PcbaContext *ctx);
void pcba_json_string(FILE *f, const char *s);

/* Growable text buffer for building small JSON results. */
typedef struct {
    char *data;
    size_t len, cap;
} PcbaBuf;
void pcba_buf_add(PcbaBuf *b, const char *text);
void pcba_buf_json(PcbaBuf *b, const char *text); /* quoted, escaped */
void pcba_buf_free(PcbaBuf *b);
void pcba_print_check(const PcbaContext *ctx, const char *status, const char *name, const char *detail, const char *hint);

/* Reports the outcome and returns its exit code. Human: one status line.
 * JSON: one envelope on stdout; result_json is embedded verbatim (or null). */
int pcba_finish(const PcbaContext *ctx, const char *command, const char *status, const char *message,
                const char *result_json);
int pcba_usage_error(const PcbaContext *ctx, const PcbaCommand *cmd, const char *fmt, ...);

/* ---- help / version ---- */
int pcba_help(PcbaContext *ctx, int argc, char **argv);
int pcba_command_help(const PcbaContext *ctx, const PcbaCommand *cmd);
int pcba_version(const PcbaContext *ctx);
const char *pcba_version_string(void);

/* ---- backend (tools/copper.py --result-json) ---- */
typedef struct {
    int found;
    const char *argv[3]; /* launcher and optional "-3", NULL-terminated */
    char program[1024];
    char version[32];
} PcbaPython;

int pcba_find_python(PcbaPython *py);
/* Runs copper.py with bargs (NULL-terminated) and reports the outcome. */
int pcba_run_backend(PcbaContext *ctx, const char *command, const char *const *bargs);
/* Same, but returns the backend status/raw result to the caller instead of reporting. */
int pcba_call_backend(PcbaContext *ctx, const char *const *bargs, char *status, size_t status_cap,
                      char **raw_result, char *message, size_t message_cap);

int pcba_cmd_doctor(PcbaContext *ctx, const PcbaCommand *cmd, const PcbaArgs *args);
int pcba_cmd_test(PcbaContext *ctx, const PcbaCommand *cmd, const PcbaArgs *args);

#endif

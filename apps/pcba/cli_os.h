#ifndef PCBA_CLI_OS_H
#define PCBA_CLI_OS_H
/*
 * Platform layer for the command-line front ends (pcba, hwc).
 *
 * All strings are UTF-8. Child processes are started directly from an argv
 * array; no shell is ever involved, so arguments containing spaces, quotes,
 * %VAR%, &, ; or $(...) reach the child unchanged. Nothing here knows about
 * circuits, KiCad or DFM.
 */
#include <stddef.h>

typedef enum {
    CLI_STDOUT_INHERIT = 0,  /* child writes to our stdout/stderr */
    CLI_STDOUT_TO_STDERR,    /* child stdout goes to our stderr (keeps our stdout machine-readable) */
    CLI_STDOUT_CAPTURE,      /* child stdout captured; child stderr inherited */
    CLI_STDOUT_CAPTURE_ALL   /* child stdout and stderr captured together */
} CliStdout;

typedef struct {
    int launched;      /* 0: the program could not be started (not found, not executable) */
    int exit_code;     /* valid when launched; 128+N for POSIX signal N */
    char *output;      /* captured bytes, NUL-terminated, or NULL */
    size_t output_len;
} CliProcResult;

/* argv is NULL-terminated; argv[0] is searched on PATH. Returns result->launched. */
int cli_run(const char *const *argv, CliStdout mode, CliProcResult *result);
void cli_proc_result_free(CliProcResult *result);

/* Quote one argument for a Windows command line (CommandLineToArgvW rules).
 * Returns the length written (excluding NUL), or (size_t)-1 when cap is too small.
 * Platform-independent so it can be unit tested everywhere. */
size_t cli_quote_windows_arg(const char *arg, char *out, size_t cap);

int cli_getenv(const char *name, char *buf, size_t cap); /* 1 when set and non-empty */
int cli_setenv(const char *name, const char *value);     /* 1 on success; also seen by children */
int cli_is_file(const char *path);
int cli_is_dir(const char *path);
int cli_self_path(char *buf, size_t cap);                 /* absolute path of this executable */
/* Path of an executable built next to this one (".exe" added on Windows); 1 if it exists. */
int cli_sibling_executable(const char *name, char *buf, size_t cap);
/* Point COPPER_CORE at the copper-core built beside this executable unless it is already set. */
void cli_default_copper_core(void);
int cli_temp_path(const char *stem, const char *ext, char *buf, size_t cap);
char *cli_read_file(const char *path, size_t *len);       /* malloc'd, NUL-terminated; NULL on error */
int cli_remove(const char *path);
int cli_stdout_is_terminal(void);
int cli_console_init(void);                               /* UTF-8 console; 1 if ANSI sequences render */

/* Source tree holding tools/copper.py: PCBA_ROOT if set, else the configured source dir. */
int cli_tool_root(char *buf, size_t cap);

/* On Windows, replace the ANSI argv with UTF-8 copies of the wide command line. */
char **cli_utf8_argv(int argc, char **argv);

#endif

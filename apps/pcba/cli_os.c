#include "cli_os.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PCBA_SOURCE_DIR
#define PCBA_SOURCE_DIR "."
#endif

size_t cli_quote_windows_arg(const char *arg, char *out, size_t cap) {
    size_t n = 0;
    int needs = arg[0] == '\0' || strpbrk(arg, " \t\n\v\"") != NULL;
#define PUT(c) do { if (n + 1 >= cap) return (size_t)-1; out[n++] = (c); } while (0)
    if (!needs) {
        for (const char *p = arg; *p; ++p) PUT(*p);
        out[n] = '\0';
        return n;
    }
    PUT('"');
    for (const char *p = arg;; ++p) {
        size_t slashes = 0;
        while (*p == '\\') { ++slashes; ++p; }
        if (*p == '\0') {          /* backslashes before the closing quote are doubled */
            for (size_t i = 0; i < slashes * 2; ++i) PUT('\\');
            break;
        }
        if (*p == '"') {           /* escape the quote and every backslash before it */
            for (size_t i = 0; i < slashes * 2 + 1; ++i) PUT('\\');
        } else {
            for (size_t i = 0; i < slashes; ++i) PUT('\\');
        }
        PUT(*p);
    }
    PUT('"');
#undef PUT
    out[n] = '\0';
    return n;
}

void cli_proc_result_free(CliProcResult *result) {
    if (!result) return;
    free(result->output);
    memset(result, 0, sizeof *result);
}

static int append(CliProcResult *r, size_t *cap, const char *data, size_t len) {
    if (r->output_len + len + 1 > *cap) {
        size_t next = *cap ? *cap * 2 : 4096;
        while (next < r->output_len + len + 1) next *= 2;
        char *grown = realloc(r->output, next);
        if (!grown) return 0;
        r->output = grown;
        *cap = next;
    }
    memcpy(r->output + r->output_len, data, len);
    r->output_len += len;
    r->output[r->output_len] = '\0';
    return 1;
}

int cli_sibling_executable(const char *name, char *buf, size_t cap) {
    char self[1024];
    if (!cli_self_path(self, sizeof self)) return 0;
    char *slash = strrchr(self, '/'), *back = strrchr(self, '\\');
    if (back > slash) slash = back;
    if (!slash) return 0;
    *slash = '\0';
#ifdef _WIN32
    int n = snprintf(buf, cap, "%s/%s.exe", self, name);
#else
    int n = snprintf(buf, cap, "%s/%s", self, name);
#endif
    return n > 0 && (size_t)n < cap && cli_is_file(buf);
}

void cli_default_copper_core(void) {
    char path[1200];
    if (!cli_getenv("COPPER_CORE", path, sizeof path) && cli_sibling_executable("copper-core", path, sizeof path))
        cli_setenv("COPPER_CORE", path);
}

int cli_tool_root(char *buf, size_t cap) {
    if (cli_getenv("PCBA_ROOT", buf, cap)) return 1;
    int n = snprintf(buf, cap, "%s", PCBA_SOURCE_DIR);
    return n > 0 && (size_t)n < cap;
}

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <io.h>

static wchar_t *widen(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = malloc((size_t)n * sizeof *w);
    if (w && MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0) { free(w); return NULL; }
    return w;
}

static int narrow(const wchar_t *w, char *buf, size_t cap) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, (int)cap, NULL, NULL);
    return n > 0;
}

static char *narrow_alloc(const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    char *s = malloc((size_t)n);
    if (s && WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL) <= 0) { free(s); return NULL; }
    return s;
}

/* Std handles of the parent are not necessarily inheritable; hand the child duplicates. */
static HANDLE inheritable(DWORD which) {
    HANDLE h = GetStdHandle(which), dup = NULL;
    if (h == NULL || h == INVALID_HANDLE_VALUE) return NULL;
    if (!DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &dup, 0, TRUE, DUPLICATE_SAME_ACCESS)) return NULL;
    return dup;
}

int cli_run(const char *const *argv, CliStdout mode, CliProcResult *result) {
    memset(result, 0, sizeof *result);
    size_t total = 1;
    for (size_t i = 0; argv[i]; ++i) total += strlen(argv[i]) * 2 + 3;
    char *line = malloc(total), *quoted = malloc(total);
    if (!line || !quoted) { free(line); free(quoted); return 0; }
    line[0] = '\0';
    for (size_t i = 0; argv[i]; ++i) {
        if (cli_quote_windows_arg(argv[i], quoted, total) == (size_t)-1) { free(line); free(quoted); return 0; }
        if (i) strcat(line, " ");
        strcat(line, quoted);
    }
    free(quoted);
    wchar_t *wline = widen(line);
    free(line);
    if (!wline) return 0;

    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE read_end = NULL, write_end = NULL;
    int capture = mode == CLI_STDOUT_CAPTURE || mode == CLI_STDOUT_CAPTURE_ALL;
    if (capture) {
        if (!CreatePipe(&read_end, &write_end, &sa, 0)) { free(wline); return 0; }
        SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
    }
    HANDLE in = inheritable(STD_INPUT_HANDLE);
    HANDLE out = capture ? NULL : inheritable(mode == CLI_STDOUT_TO_STDERR ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    HANDLE err = mode == CLI_STDOUT_CAPTURE_ALL ? NULL : inheritable(STD_ERROR_HANDLE);
    STARTUPINFOW si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in;
    si.hStdOutput = capture ? write_end : out;
    si.hStdError = mode == CLI_STDOUT_CAPTURE_ALL ? write_end : err;
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof pi);
    fflush(stdout);
    fflush(stderr);
    BOOL ok = CreateProcessW(NULL, wline, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    free(wline);
    if (in) CloseHandle(in);
    if (out) CloseHandle(out);
    if (err) CloseHandle(err);
    if (write_end) CloseHandle(write_end);
    if (!ok) {
        if (read_end) CloseHandle(read_end);
        return 0;
    }
    result->launched = 1;
    if (capture) {
        size_t cap = 0;
        char chunk[4096];
        DWORD got = 0;
        while (ReadFile(read_end, chunk, sizeof chunk, &got, NULL) && got) append(result, &cap, chunk, got);
        CloseHandle(read_end);
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    result->exit_code = (int)code;
    return 1;
}

int cli_getenv(const char *name, char *buf, size_t cap) {
    wchar_t *wname = widen(name), value[4096];
    if (!wname) return 0;
    DWORD n = GetEnvironmentVariableW(wname, value, (DWORD)(sizeof value / sizeof value[0]));
    free(wname);
    if (n == 0 || n >= sizeof value / sizeof value[0]) return 0;
    return narrow(value, buf, cap);
}

int cli_setenv(const char *name, const char *value) {
    wchar_t *wname = widen(name), *wvalue = widen(value);
    int ok = wname && wvalue && SetEnvironmentVariableW(wname, wvalue);
    free(wname);
    free(wvalue);
    return ok;
}

static DWORD attributes(const char *path) {
    wchar_t *w = widen(path);
    if (!w) return INVALID_FILE_ATTRIBUTES;
    DWORD a = GetFileAttributesW(w);
    free(w);
    return a;
}

int cli_is_file(const char *path) {
    DWORD a = attributes(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

int cli_is_dir(const char *path) {
    DWORD a = attributes(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

int cli_self_path(char *buf, size_t cap) {
    wchar_t path[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(NULL, path, (DWORD)(sizeof path / sizeof path[0]));
    return n > 0 && n < sizeof path / sizeof path[0] && narrow(path, buf, cap);
}

int cli_temp_path(const char *stem, const char *ext, char *buf, size_t cap) {
    static unsigned counter;
    wchar_t dir[MAX_PATH + 1];
    char dir8[MAX_PATH * 4];
    DWORD n = GetTempPathW((DWORD)(sizeof dir / sizeof dir[0]), dir);
    if (n == 0 || n > MAX_PATH || !narrow(dir, dir8, sizeof dir8)) return 0;
    int m = snprintf(buf, cap, "%s%s-%lu-%u-%lu%s", dir8, stem, (unsigned long)GetCurrentProcessId(), ++counter,
                     (unsigned long)GetTickCount(), ext);
    return m > 0 && (size_t)m < cap;
}

char *cli_read_file(const char *path, size_t *len) {
    wchar_t *w = widen(path);
    FILE *f = w ? _wfopen(w, L"rb") : NULL;
    free(w);
    if (!f) return NULL;
    CliProcResult buf = {0};
    size_t cap = 0;
    char chunk[4096];
    size_t got;
    while ((got = fread(chunk, 1, sizeof chunk, f)) > 0) {
        if (!append(&buf, &cap, chunk, got)) { fclose(f); free(buf.output); return NULL; }
    }
    fclose(f);
    if (!buf.output) buf.output = calloc(1, 1);
    if (len) *len = buf.output_len;
    return buf.output;
}

int cli_remove(const char *path) {
    wchar_t *w = widen(path);
    int ok = w && DeleteFileW(w);
    free(w);
    return ok;
}

int cli_stdout_is_terminal(void) { return _isatty(_fileno(stdout)); }

int cli_console_init(void) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (h == NULL || h == INVALID_HANDLE_VALUE || !GetConsoleMode(h, &mode)) return 1; /* not a console: bytes pass through */
    SetConsoleOutputCP(CP_UTF8);
    return SetConsoleMode(h, mode | 0x0004 /* ENABLE_VIRTUAL_TERMINAL_PROCESSING */) != 0;
}

char **cli_utf8_argv(int argc, char **argv) {
    int wargc = 0;
    wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (!wargv || wargc != argc) {
        if (wargv) LocalFree(wargv);
        return argv;
    }
    char **out = calloc((size_t)argc + 1, sizeof *out);
    for (int i = 0; out && i < argc; ++i) {
        out[i] = narrow_alloc(wargv[i]);
        if (!out[i]) { LocalFree(wargv); return argv; } /* lifetime: whole program */
    }
    LocalFree(wargv);
    return out ? out : argv;
}

#else /* POSIX */
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

int cli_run(const char *const *argv, CliStdout mode, CliProcResult *result) {
    memset(result, 0, sizeof *result);
    int capture = mode == CLI_STDOUT_CAPTURE || mode == CLI_STDOUT_CAPTURE_ALL;
    int out_pipe[2] = {-1, -1}, err_pipe[2] = {-1, -1};
    if (capture && pipe(out_pipe)) return 0;
    /* exec failures are reported over a close-on-exec pipe, so 127 from the child stays a real exit code */
    if (pipe(err_pipe)) { if (capture) { close(out_pipe[0]); close(out_pipe[1]); } return 0; }
    fcntl(err_pipe[1], F_SETFD, FD_CLOEXEC);
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid == 0) {
        close(err_pipe[0]);
        if (capture) {
            dup2(out_pipe[1], STDOUT_FILENO);
            if (mode == CLI_STDOUT_CAPTURE_ALL) dup2(out_pipe[1], STDERR_FILENO);
            close(out_pipe[0]);
            close(out_pipe[1]);
        } else if (mode == CLI_STDOUT_TO_STDERR) {
            dup2(STDERR_FILENO, STDOUT_FILENO);
        }
        execvp(argv[0], (char *const *)argv);
        int e = errno;
        ssize_t ignored = write(err_pipe[1], &e, sizeof e);
        (void)ignored;
        _exit(127);
    }
    close(err_pipe[1]);
    if (capture) close(out_pipe[1]);
    if (pid < 0) {
        close(err_pipe[0]);
        if (capture) close(out_pipe[0]);
        return 0;
    }
    if (capture) {
        size_t cap = 0;
        char chunk[4096];
        ssize_t got;
        while ((got = read(out_pipe[0], chunk, sizeof chunk)) > 0) append(result, &cap, chunk, (size_t)got);
        close(out_pipe[0]);
    }
    int exec_errno = 0;
    ssize_t failed = read(err_pipe[0], &exec_errno, sizeof exec_errno);
    close(err_pipe[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (failed > 0) return 0;
    result->launched = 1;
    result->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1;
    return 1;
}

int cli_getenv(const char *name, char *buf, size_t cap) {
    const char *v = getenv(name);
    if (!v || !*v) return 0;
    int n = snprintf(buf, cap, "%s", v);
    return n > 0 && (size_t)n < cap;
}

int cli_setenv(const char *name, const char *value) { return setenv(name, value, 1) == 0; }

int cli_is_file(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

int cli_is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int cli_self_path(char *buf, size_t cap) {
    ssize_t n = readlink("/proc/self/exe", buf, cap - 1);
    if (n <= 0 || (size_t)n >= cap - 1) return 0;
    buf[n] = '\0';
    return 1;
}

int cli_temp_path(const char *stem, const char *ext, char *buf, size_t cap) {
    static unsigned counter;
    const char *dir = getenv("TMPDIR");
    if (!dir || !*dir) dir = "/tmp";
    int n = snprintf(buf, cap, "%s/%s-%ld-%u%s", dir, stem, (long)getpid(), ++counter, ext);
    return n > 0 && (size_t)n < cap;
}

char *cli_read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    CliProcResult buf = {0};
    size_t cap = 0, got;
    char chunk[4096];
    while ((got = fread(chunk, 1, sizeof chunk, f)) > 0) {
        if (!append(&buf, &cap, chunk, got)) { fclose(f); free(buf.output); return NULL; }
    }
    fclose(f);
    if (!buf.output) buf.output = calloc(1, 1);
    if (len) *len = buf.output_len;
    return buf.output;
}

int cli_remove(const char *path) { return remove(path) == 0; }
int cli_stdout_is_terminal(void) { return isatty(STDOUT_FILENO); }
int cli_console_init(void) { return 1; }
char **cli_utf8_argv(int argc, char **argv) { (void)argc; return argv; }
#endif

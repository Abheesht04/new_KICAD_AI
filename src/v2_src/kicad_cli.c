/* kicad_cli.c - kicad-cli runner (see kicad_cli.h). */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "kicad_cli.h"

#include "copper_project.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#define SEP "\\"
#else
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#define SEP "/"
extern char **environ;
#endif

static const char *const STATUS_NAMES[] = {"OK", "INVALID_ARGUMENT", "NOT_FOUND", "WRONG_VERSION", "EXISTS",
                                           "LAUNCH_FAILED", "TIMEOUT", "TOOL_FAILED", "NO_OUTPUT", "IO_ERROR",
                                           "OUT_OF_MEMORY"};
static const char *const JOB_NAMES[] = {"version", "drc", "gerbers", "drill", "pos"};

const char *kicad_cli_status_name(KicadCliStatus s)
{
    return (unsigned)s < sizeof STATUS_NAMES / sizeof *STATUS_NAMES ? STATUS_NAMES[s] : "UNKNOWN_STATUS";
}

const char *kicad_cli_job_name(KicadCliJob j)
{
    return (unsigned)j < sizeof JOB_NAMES / sizeof *JOB_NAMES ? JOB_NAMES[j] : "unknown";
}

size_t kicad_cli_quote_arg(const char *arg, char *out, size_t cap)
{
    size_t n = 0;
    bool needs = arg[0] == '\0' || strpbrk(arg, " \t\n\v\"") != NULL;
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
        if (*p == '\0') {                       /* backslashes before the closing quote are doubled */
            for (size_t i = 0; i < slashes * 2; ++i) PUT('\\');
            break;
        }
        if (*p == '"') {                        /* escape the quote and every backslash before it */
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

static char *dup(Arena *a, const char *s)
{
    size_t n = strlen(s);
    char *d = arena_alloc(a, n + 1);
    if (d) memcpy(d, s, n + 1);
    return d;
}

static char *join(Arena *a, const char *x, const char *sep, const char *y)
{
    size_t nx = strlen(x), ns = strlen(sep), ny = strlen(y);
    char *d = arena_alloc(a, nx + ns + ny + 1);
    if (!d) return NULL;
    memcpy(d, x, nx);
    memcpy(d + nx, sep, ns);
    memcpy(d + nx + ns, y, ny + 1);
    return d;
}

/* ------------------------------------------------------------------------------------------------ platform */

typedef struct Spawned { bool launched, timed_out; int exit_code; uint64_t elapsed_ms; } Spawned;

#ifdef _WIN32
static wchar_t *wide(Arena *a, const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = arena_alloc(a, (size_t)n * sizeof *w);
    if (w && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, n) != n) return NULL;
    return w;
}

static char *narrow(Arena *a, const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    char *s = arena_alloc(a, (size_t)n);
    if (s && WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL) != n) return NULL;
    return s;
}

static int path_kind(Arena *a, const char *p)   /* 0 absent, 1 file, 2 directory, -1 error */
{
    wchar_t *w = wide(a, p);
    if (!w) return -1;
    DWORD at = GetFileAttributesW(w);
    if (at == INVALID_FILE_ATTRIBUTES) return GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND ? 0 : -1;
    return (at & FILE_ATTRIBUTE_DIRECTORY) ? 2 : 1;
}

static bool make_dir(Arena *a, const char *p)
{
    wchar_t *w = wide(a, p);
    return w && CreateDirectoryW(w, NULL);
}

static bool move_noreplace(Arena *a, const char *from, const char *to)
{
    wchar_t *f = wide(a, from), *t = wide(a, to);
    return f && t && MoveFileExW(f, t, 0);   /* no MOVEFILE_REPLACE_EXISTING: fails if `to` exists */
}

static void remove_tree(Arena *a, const char *p)
{
    int k = path_kind(a, p);
    wchar_t *w = wide(a, p);
    if (!w || k <= 0) return;
    if (k == 1) { SetFileAttributesW(w, FILE_ATTRIBUTE_NORMAL); DeleteFileW(w); return; }
    char *pattern = join(a, p, SEP, "*");
    wchar_t *wp = pattern ? wide(a, pattern) : NULL;
    WIN32_FIND_DATAW fd;
    HANDLE h = wp ? FindFirstFileW(wp, &fd) : INVALID_HANDLE_VALUE;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            char *name = narrow(a, fd.cFileName);
            if (name) remove_tree(a, join(a, p, SEP, name));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(w);
}

static bool list_dir(Arena *a, const char *p, const char ***out, size_t *count)
{
    size_t cap = 16, n = 0;
    const char **names = arena_alloc_array(a, sizeof *names, cap);
    char *pattern = join(a, p, SEP, "*");
    wchar_t *wp = pattern ? wide(a, pattern) : NULL;
    if (!names || !wp) return false;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wp, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..") || (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (n == cap) {
                const char **more = arena_alloc_array(a, sizeof *more, cap * 2);
                if (!more) { FindClose(h); return false; }
                memcpy(more, names, n * sizeof *names);
                names = more;
                cap *= 2;
            }
            names[n] = narrow(a, fd.cFileName);
            if (!names[n]) { FindClose(h); return false; }
            n++;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    *out = names;
    *count = n;
    return true;
}

static char *read_all(Arena *a, const char *p, size_t *len)
{
    wchar_t *w = wide(a, p);
    *len = 0;
    if (!w) return NULL;
    HANDLE h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER size;
    char *buf = NULL;
    if (GetFileSizeEx(h, &size) && size.QuadPart < (LONGLONG)0x40000000) {
        buf = arena_alloc(a, (size_t)size.QuadPart + 1);
        DWORD got = 0;
        if (buf && (size.QuadPart == 0 || ReadFile(h, buf, (DWORD)size.QuadPart, &got, NULL))) {
            buf[got] = 0;
            *len = got;
        } else {
            buf = NULL;
        }
    }
    CloseHandle(h);
    return buf;
}

static unsigned long process_id(void) { return (unsigned long)GetCurrentProcessId(); }

static const char *temp_dir(Arena *a)
{
    wchar_t buf[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH + 1, buf);
    if (!n || n > MAX_PATH) return NULL;
    if (buf[n - 1] == L'\\') buf[n - 1] = 0;
    return narrow(a, buf);
}

static HANDLE inheritable_file(Arena *a, const char *p, bool write)
{
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    wchar_t *w = wide(a, p);
    if (!w) return INVALID_HANDLE_VALUE;
    return CreateFileW(w, write ? GENERIC_WRITE : GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                       write ? CREATE_NEW : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
}

static bool spawn(Arena *a, const char *const *argv, const char *out_file, const char *err_file, uint32_t timeout_ms, Spawned *s)
{
    size_t total = 1;
    for (size_t i = 0; argv[i]; i++) total += strlen(argv[i]) * 2 + 3;
    char *line = arena_alloc(a, total), *q = arena_alloc(a, total);
    if (!line || !q) return false;
    line[0] = 0;
    for (size_t i = 0; argv[i]; i++) {
        if (kicad_cli_quote_arg(argv[i], q, total) == (size_t)-1) return false;
        if (i) strcat(line, " ");
        strcat(line, q);
    }
    wchar_t *wline = wide(a, line), *wapp = wide(a, argv[0]);
    if (!wline || !wapp) return false;
    HANDLE in = inheritable_file(a, "NUL", false), out = inheritable_file(a, out_file, true), err = inheritable_file(a, err_file, true);
    HANDLE job = CreateJobObjectW(NULL, NULL);
    bool ok = in != INVALID_HANDLE_VALUE && out != INVALID_HANDLE_VALUE && err != INVALID_HANDLE_VALUE && job;
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof pi);
    if (ok) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
        memset(&li, 0, sizeof li);
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
        STARTUPINFOW si;
        memset(&si, 0, sizeof si);
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = in; si.hStdOutput = out; si.hStdError = err;
        ok = CreateProcessW(wapp, wline, NULL, NULL, TRUE, CREATE_SUSPENDED | CREATE_NO_WINDOW, NULL, NULL, &si, &pi) != 0;
    }
    if (in != INVALID_HANDLE_VALUE) CloseHandle(in);
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (err != INVALID_HANDLE_VALUE) CloseHandle(err);
    if (!ok) { if (job) CloseHandle(job); return false; }
    ULONGLONG t0 = GetTickCount64();
    AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    s->launched = true;
    DWORD w = WaitForSingleObject(pi.hProcess, timeout_ms ? timeout_ms : INFINITE);
    if (w == WAIT_TIMEOUT) {
        TerminateJobObject(job, 1);
        WaitForSingleObject(pi.hProcess, INFINITE);
        s->timed_out = true;
    }
    DWORD code = (DWORD)-1;
    GetExitCodeProcess(pi.hProcess, &code);
    s->exit_code = s->timed_out ? -1 : (int)code;
    s->elapsed_ms = (uint64_t)(GetTickCount64() - t0);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(job);
    return true;
}
#else
static int path_kind(Arena *a, const char *p)
{
    struct stat st;
    (void)a;
    if (lstat(p, &st)) return errno == ENOENT ? 0 : -1;
    return S_ISDIR(st.st_mode) ? 2 : 1;
}

static bool make_dir(Arena *a, const char *p) { (void)a; return mkdir(p, 0700) == 0; }

static bool move_noreplace(Arena *a, const char *from, const char *to)
{
    int k = path_kind(a, from);
    if (k == 1) {                                     /* link() fails if `to` exists: never replaces */
        if (link(from, to)) return false;
        unlink(from);
        return true;
    }
    if (path_kind(a, to) != 0) return false;          /* rename(2) would replace an empty folder: refuse first */
    return rename(from, to) == 0;
}

static void remove_tree(Arena *a, const char *p)
{
    int k = path_kind(a, p);
    if (k == 1) { unlink(p); return; }
    if (k != 2) return;
    DIR *d = opendir(p);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char *child = join(a, p, SEP, e->d_name);
            if (child) remove_tree(a, child);
        }
        closedir(d);
    }
    rmdir(p);
}

static bool list_dir(Arena *a, const char *p, const char ***out, size_t *count)
{
    size_t cap = 16, n = 0;
    const char **names = arena_alloc_array(a, sizeof *names, cap);
    DIR *d = opendir(p);
    if (!names || !d) { if (d) closedir(d); return false; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char *child = join(a, p, SEP, e->d_name);
        if (!child || path_kind(a, child) != 1) continue;
        if (n == cap) {
            const char **more = arena_alloc_array(a, sizeof *more, cap * 2);
            if (!more) { closedir(d); return false; }
            memcpy(more, names, n * sizeof *names);
            names = more;
            cap *= 2;
        }
        names[n++] = dup(a, e->d_name);
    }
    closedir(d);
    *out = names;
    *count = n;
    return true;
}

static char *read_all(Arena *a, const char *p, size_t *len)
{
    FILE *f = fopen(p, "rb");
    *len = 0;
    if (!f) return NULL;
    size_t cap = 4096, n = 0;
    char *buf = arena_alloc(a, cap);
    while (buf) {
        n += fread(buf + n, 1, cap - n - 1, f);
        if (n < cap - 1) break;
        char *more = arena_alloc(a, cap * 2);
        if (more) memcpy(more, buf, n);
        buf = more;
        cap *= 2;
    }
    fclose(f);
    if (!buf) return NULL;
    buf[n] = 0;
    *len = n;
    return buf;
}

static unsigned long process_id(void) { return (unsigned long)getpid(); }

static const char *temp_dir(Arena *a)
{
    const char *t = getenv("TMPDIR");
    return dup(a, t && *t ? t : "/tmp");
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static bool spawn(Arena *a, const char *const *argv, const char *out_file, const char *err_file, uint32_t timeout_ms, Spawned *s)
{
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t at;
    pid_t pid;
    (void)a;
    if (posix_spawn_file_actions_init(&fa)) return false;
    if (posix_spawnattr_init(&at)) { posix_spawn_file_actions_destroy(&fa); return false; }
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, out_file, O_WRONLY | O_CREAT | O_EXCL, 0600);
    posix_spawn_file_actions_addopen(&fa, 2, err_file, O_WRONLY | O_CREAT | O_EXCL, 0600);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&at, 0);                   /* its own group, so the whole tree can be killed */
    uint64_t t0 = now_ms();
    int rc = posix_spawn(&pid, argv[0], &fa, &at, (char *const *)argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    if (rc) return false;
    s->launched = true;
    int st = 0;
    for (;;) {
        pid_t w = waitpid(pid, &st, WNOHANG);
        if (w == pid) break;
        if (w < 0 && errno != EINTR) { st = -1; break; }
        if (timeout_ms && now_ms() - t0 >= timeout_ms) {
            kill(-pid, SIGKILL);
            waitpid(pid, &st, 0);
            s->timed_out = true;
            break;
        }
        struct timespec nap = {0, 5000000};
        nanosleep(&nap, NULL);
    }
    s->exit_code = s->timed_out || st < 0 ? -1 : WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    s->elapsed_ms = now_ms() - t0;
    return true;
}
#endif

/* ------------------------------------------------------------------------------------------------ jobs */

static KicadCliStatus finish(KicadCliResult *r, KicadCliStatus s, const char *message)
{
    r->status = s;
    if (s != KCLI_OK && !r->message) r->message = message;
    return s;
}

static int cmp_name(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

static unsigned long g_counter;   /* staging folder names: process id + counter (one process may run jobs in turn) */

/* Run `tail` (with "{OUT}" replaced by the staged output) for `job`. out_path NULL: no output (version). */
static KicadCliStatus run_job(Arena *a, const KicadCli *cli, KicadCliJob job, const char *const *tail, const char *board,
                              const char *out_path, bool out_is_dir, KicadCliResult *r)
{
    memset(r, 0, sizeof *r);
    r->job = job;
    r->exit_code = -1;
    if (!a || !cli || !cli->path) return finish(r, KCLI_INVALID_ARGUMENT, "NULL argument");
    r->cli_version = cli->version;
    r->cli_is_9x = cli->major == 9;
    if (job != KCLI_JOB_VERSION && cli->require_9x && cli->major != 9)
        return finish(r, KCLI_WRONG_VERSION, "kicad-cli is not KiCad 9.x (native validation needs 9.x)");
    if (board && path_kind(a, board) != 1) return finish(r, KCLI_INVALID_ARGUMENT, "the board file does not exist");
    if (out_path && path_kind(a, out_path) != 0) return finish(r, KCLI_EXISTS, "the output path already exists: nothing is overwritten");

    /* A fresh staging folder next to the output (or in the temp folder for jobs without output). */
    const char *base = out_path ? out_path : NULL;
    if (!base) {
        const char *t = temp_dir(a);
        if (!t) return finish(r, KCLI_IO_ERROR, "no temporary folder");
        base = join(a, t, SEP, "copper-kicad-cli");
    }
    char *staging = NULL;
    for (int attempt = 0; attempt < 100 && !staging; attempt++) {
        char suffix[64];
        snprintf(suffix, sizeof suffix, ".copper-staging-%lu-%lu", process_id(), ++g_counter);
        char *candidate = join(a, base, "", suffix);
        if (!candidate) return finish(r, KCLI_OUT_OF_MEMORY, "out of memory");
        if (path_kind(a, candidate) == 0 && make_dir(a, candidate)) staging = candidate;
    }
    if (!staging) return finish(r, KCLI_IO_ERROR, "cannot create a staging folder next to the output");
    const char *out_name = NULL, *staged = NULL;
    if (out_path) {
        const char *slash = strrchr(out_path, '/');
#ifdef _WIN32
        const char *bs = strrchr(out_path, '\\');
        if (!slash || (bs && bs > slash)) slash = bs;
#endif
        out_name = slash ? slash + 1 : out_path;
        if (!*out_name) { remove_tree(a, staging); return finish(r, KCLI_INVALID_ARGUMENT, "the output path ends in a separator"); }
        staged = join(a, staging, SEP, out_name);
        if (!staged || (out_is_dir && !make_dir(a, staged))) { remove_tree(a, staging); return finish(r, KCLI_IO_ERROR, "cannot create the staged output folder"); }
    }
    char *out_log = join(a, staging, SEP, ".stdout"), *err_log = join(a, staging, SEP, ".stderr");
    const char *out_arg = staged ? (out_is_dir ? join(a, staged, SEP, "") : staged) : NULL;

    size_t n = 0;
    while (tail[n]) n++;
    const char **argv = arena_alloc_array(a, sizeof *argv, n + 3);
    if (!argv || !out_log || !err_log) { remove_tree(a, staging); return finish(r, KCLI_OUT_OF_MEMORY, "out of memory"); }
    argv[0] = cli->path;
    size_t k = 1;
    for (size_t i = 0; i < n; i++) argv[k++] = !strcmp(tail[i], "{OUT}") ? out_arg : !strcmp(tail[i], "{BOARD}") ? board : tail[i];
    argv[k] = NULL;
    r->argv = argv;
    r->argc = k;

    Spawned sp = {false, false, -1, 0};
    bool started = spawn(a, argv, out_log, err_log, cli->timeout_ms, &sp);
    r->launched = sp.launched;
    r->timed_out = sp.timed_out;
    r->exit_code = sp.exit_code;
    r->elapsed_ms = sp.elapsed_ms;
    if (started) {
        char *o = read_all(a, out_log, &r->stdout_len), *e = read_all(a, err_log, &r->stderr_len);
        r->stdout_text = o ? o : "";
        r->stderr_text = e ? e : "";
    }
    KicadCliStatus status = KCLI_OK;
    const char *why = NULL;
    if (!started) { status = KCLI_LAUNCH_FAILED; why = "kicad-cli could not be started"; }
    else if (sp.timed_out) { status = KCLI_TIMEOUT; why = "kicad-cli did not finish in time and was killed"; }
    else if (job == KCLI_JOB_DRC && sp.exit_code == 5) r->drc_violations = true;
    else if (sp.exit_code != 0) { status = KCLI_TOOL_FAILED; why = "kicad-cli exited with an error"; }

    if (status == KCLI_OK && staged) {
        int kind = path_kind(a, staged);
        if (out_is_dir) {
            if (kind != 2 || !list_dir(a, staged, &r->files, &r->file_count) || !r->file_count) { status = KCLI_NO_OUTPUT; why = "kicad-cli wrote no files"; }
            else qsort(r->files, r->file_count, sizeof *r->files, cmp_name);
        } else if (kind != 1) {
            status = KCLI_NO_OUTPUT; why = "kicad-cli did not write the output file";
        } else {
            r->files = arena_alloc_array(a, sizeof *r->files, 1);
            if (r->files) { r->files[0] = dup(a, out_name); r->file_count = 1; }
        }
        if (status == KCLI_OK) {
            if (!move_noreplace(a, staged, out_path)) { status = KCLI_EXISTS; why = "the output path appeared while kicad-cli ran: nothing is overwritten"; }
            else r->output = dup(a, out_path);
        }
    }
    remove_tree(a, staging);
    if (status != KCLI_OK) { r->files = NULL; r->file_count = 0; }
    return finish(r, status, why);
}

static void parse_version(const char *text, KicadCli *cli, Arena *a)
{
    int part[3] = {-1, -1, -1}, i = 0;
    const char *p = text;
    while (*p && !(*p >= '0' && *p <= '9')) p++;
    const char *start = p;
    while (i < 3 && *p >= '0' && *p <= '9') {
        int v = 0;
        while (*p >= '0' && *p <= '9' && v < 100000) v = v * 10 + (*p++ - '0');
        part[i++] = v;
        if (*p != '.') break;
        p++;
    }
    cli->major = part[0]; cli->minor = part[1]; cli->patch = part[2];
    size_t n = 0;
    while (start[n] && start[n] != '\n' && start[n] != '\r' && start[n] != ' ') n++;
    char *v = arena_alloc(a, n + 1);
    if (v) { memcpy(v, start, n); v[n] = 0; }
    cli->version = v;
}

KicadCliStatus kicad_cli_open(Arena *arena, const char *explicit_path, uint32_t timeout_ms, bool require_9x,
                              KicadCli *out, KicadCliResult *vr)
{
    KicadCliResult local;
    if (!vr) vr = &local;
    memset(vr, 0, sizeof *vr);
    if (!arena || !out) return finish(vr, KCLI_INVALID_ARGUMENT, "NULL argument");
    memset(out, 0, sizeof *out);
    out->major = out->minor = out->patch = -1;
    out->timeout_ms = timeout_ms;
    const char *path = NULL;
    if (copper_project_find_kicad_cli(arena, explicit_path, &path) != COPPER_PROJECT_OK || !path)
        return finish(vr, KCLI_NOT_FOUND, "kicad-cli not found (explicit path, KICAD_CLI_PATH, PATH, KiCad 9 install folders)");
    out->path = path;
    static const char *const TAIL[] = {"version", NULL};
    KicadCliStatus s = run_job(arena, out, KCLI_JOB_VERSION, TAIL, NULL, NULL, false, vr);
    if (s) return s;
    parse_version(vr->stdout_text, out, arena);
    vr->cli_version = out->version;
    vr->cli_is_9x = out->major == 9;
    out->require_9x = require_9x;
    if (out->major < 0) return finish(vr, KCLI_WRONG_VERSION, "kicad-cli printed no version number");
    if (require_9x && out->major != 9) return finish(vr, KCLI_WRONG_VERSION, "kicad-cli is not KiCad 9.x (native validation needs 9.x)");
    return KCLI_OK;
}

KicadCliStatus kicad_cli_drc(Arena *a, const KicadCli *cli, const char *board, const char *out_json,
                             const KicadDrcOptions *o, KicadCliResult *r)
{
    const char *tail[16];
    size_t n = 0;
    tail[n++] = "pcb"; tail[n++] = "drc"; tail[n++] = "--format"; tail[n++] = "json"; tail[n++] = "--exit-code-violations";
    if (o && o->schematic_parity) tail[n++] = "--schematic-parity";
    if (o && o->all_track_errors) tail[n++] = "--all-track-errors";
    tail[n++] = "-o"; tail[n++] = "{OUT}"; tail[n++] = "{BOARD}"; tail[n] = NULL;
    if (!board || !out_json) { memset(r, 0, sizeof *r); return finish(r, KCLI_INVALID_ARGUMENT, "NULL argument"); }
    return run_job(a, cli, KCLI_JOB_DRC, tail, board, out_json, false, r);
}

KicadCliStatus kicad_cli_gerbers(Arena *a, const KicadCli *cli, const char *board, const char *out_dir, KicadCliResult *r)
{
    static const char *const TAIL[] = {"pcb", "export", "gerbers", "-o", "{OUT}", "{BOARD}", NULL};
    if (!board || !out_dir) { memset(r, 0, sizeof *r); return finish(r, KCLI_INVALID_ARGUMENT, "NULL argument"); }
    return run_job(a, cli, KCLI_JOB_GERBERS, TAIL, board, out_dir, true, r);
}

KicadCliStatus kicad_cli_drill(Arena *a, const KicadCli *cli, const char *board, const char *out_dir, KicadCliResult *r)
{
    static const char *const TAIL[] = {"pcb", "export", "drill", "--format", "excellon", "--drill-origin", "absolute",
                                       "--excellon-zeros-format", "decimal", "--excellon-units", "mm",
                                       "-o", "{OUT}", "{BOARD}", NULL};
    if (!board || !out_dir) { memset(r, 0, sizeof *r); return finish(r, KCLI_INVALID_ARGUMENT, "NULL argument"); }
    return run_job(a, cli, KCLI_JOB_DRILL, TAIL, board, out_dir, true, r);
}

KicadCliStatus kicad_cli_pos(Arena *a, const KicadCli *cli, const char *board, const char *out_file, KicadCliResult *r)
{
    static const char *const TAIL[] = {"pcb", "export", "pos", "--format", "csv", "--units", "mm", "--side", "both",
                                       "-o", "{OUT}", "{BOARD}", NULL};
    if (!board || !out_file) { memset(r, 0, sizeof *r); return finish(r, KCLI_INVALID_ARGUMENT, "NULL argument"); }
    return run_job(a, cli, KCLI_JOB_POS, TAIL, board, out_file, false, r);
}

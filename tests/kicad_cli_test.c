/* The kicad-cli runner (src/v2_src/kicad_cli.c).
 *
 *   kicad_cli_test          against tests/fake_kicad_cli.c: argv passed exactly (spaces, quotes, UTF-8), every job,
 *                           never overwriting, exit codes, missing output, timeout kill, large output, version gate.
 *   kicad_cli_test --real   against a real kicad-cli (KICAD_CLI): all four jobs on the Copper-generated r0603x5 board;
 *                           the drill file is read back by excellon.c and must hold the board model's J1 holes; the DRC
 *                           report must be JSON. Exit 77 (UNVERIFIED) without KICAD_CLI. A KiCad 10 CLI is a
 *                           development check, not KiCad 9 evidence; the output says which version ran. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "board.h"
#include "excellon.h"
#include "json.h"
#include "kicad_cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* ------------------------------------------------------------ test-side file helpers (UTF-8 paths) */
#ifdef _WIN32
static void wpath(const char *p, wchar_t *w) { MultiByteToWideChar(CP_UTF8, 0, p, -1, w, 4096); }
static int kind(const char *p) { wchar_t w[4096]; wpath(p, w); DWORD a = GetFileAttributesW(w); return a == INVALID_FILE_ATTRIBUTES ? 0 : (a & FILE_ATTRIBUTE_DIRECTORY) ? 2 : 1; }
static void mkdir_utf8(const char *p) { wchar_t w[4096]; wpath(p, w); CreateDirectoryW(w, NULL); }
static void setenv_utf8(const char *k, const char *v) { SetEnvironmentVariableA(k, v); }
static char *slurp(const char *p)
{
    wchar_t w[4096]; wpath(p, w);
    HANDLE h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    DWORD n = GetFileSize(h, NULL), got = 0;
    char *b = malloc(n + 1);
    ReadFile(h, b, n, &got, NULL); b[got] = 0; CloseHandle(h);
    return b;
}
static void rm_tree(const char *p)
{
    wchar_t w[4096], pat[4100];
    wpath(p, w);
    swprintf(pat, 4100, L"%ls\\*", w);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            wchar_t c[4096]; swprintf(c, 4096, L"%ls\\%ls", w, fd.cFileName);
            char cc[4096]; WideCharToMultiByte(CP_UTF8, 0, c, -1, cc, 4096, NULL, NULL);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) rm_tree(cc); else DeleteFileW(c);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(w);
}
static int count_staging(const char *dir)
{
    wchar_t w[4096], pat[4100]; wpath(dir, w); swprintf(pat, 4100, L"%ls\\*copper-staging*", w);
    WIN32_FIND_DATAW fd; int n = 0;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) { do n++; while (FindNextFileW(h, &fd)); FindClose(h); }
    return n;
}
#define SEPS "\\"
#else
static int kind(const char *p) { struct stat st; return stat(p, &st) ? 0 : S_ISDIR(st.st_mode) ? 2 : 1; }
static void mkdir_utf8(const char *p) { mkdir(p, 0700); }
static void setenv_utf8(const char *k, const char *v) { if (v) setenv(k, v, 1); else unsetenv(k); }
static char *slurp(const char *p)
{
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1); size_t got = fread(b, 1, (size_t)n, f); b[got] = 0; fclose(f); return b;
}
static void rm_tree(const char *p)
{
    DIR *d = opendir(p);
    if (!d) { unlink(p); return; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char c[4096]; snprintf(c, sizeof c, "%s/%s", p, e->d_name);
        if (kind(c) == 2) rm_tree(c); else unlink(c);
    }
    closedir(d);
    rmdir(p);
}
static int count_staging(const char *dir)
{
    DIR *d = opendir(dir); int n = 0; struct dirent *e;
    if (!d) return 0;
    while ((e = readdir(d))) n += strstr(e->d_name, "copper-staging") != NULL;
    closedir(d);
    return n;
}
#define SEPS "/"
#endif

static char ROOTDIR[4096];
static const char *at(const char *name)
{
    static char buf[8][4096];
    static int k;
    k = (k + 1) % 8;
    snprintf(buf[k], sizeof buf[k], "%s" SEPS "%s", ROOTDIR, name);
    return buf[k];
}

/* ------------------------------------------------------------ fake */

static void test_quote(void)
{
    char out[256];
    CHECK(kicad_cli_quote_arg("plain", out, sizeof out) == 5 && !strcmp(out, "plain"));
    CHECK(kicad_cli_quote_arg("", out, sizeof out) == 2 && !strcmp(out, "\"\""));
    CHECK(kicad_cli_quote_arg("a b", out, sizeof out) && !strcmp(out, "\"a b\""));
    CHECK(kicad_cli_quote_arg("say \"hi\"", out, sizeof out) && !strcmp(out, "\"say \\\"hi\\\"\""));
    CHECK(kicad_cli_quote_arg("C:\\dir with space\\", out, sizeof out) && !strcmp(out, "\"C:\\dir with space\\\\\""));
    CHECK(kicad_cli_quote_arg("a\\\\b", out, sizeof out) && !strcmp(out, "a\\\\b"));   /* no quoting: unchanged */
    CHECK(kicad_cli_quote_arg("long argument", out, 5) == (size_t)-1);
}

static void test_fake(void)
{
    Arena a;
    KicadCli cli;
    KicadCliResult r;
    arena_init(&a, 1 << 16);
    rm_tree(ROOTDIR);
    mkdir_utf8(ROOTDIR);
    setenv_utf8("FAKE_KICAD_MODE", "");
    setenv_utf8("FAKE_KICAD_VERSION", NULL);

    CHECK(kicad_cli_open(&a, FAKE_KICAD_CLI, 20000, true, &cli, &r) == KCLI_OK);
    CHECK(cli.major == 9 && cli.minor == 0 && cli.patch == 6 && !strcmp(cli.version, "9.0.6") && r.cli_is_9x);
    CHECK(r.launched && r.exit_code == 0 && r.argc == 2 && !strcmp(r.argv[1], "version"));
    CHECK(kicad_cli_open(&a, "no/such/kicad-cli.exe", 1000, true, &cli, &r) == KCLI_NOT_FOUND);

    /* A board and outputs whose paths have spaces, a quote-free UTF-8 name, and a folder that does not exist yet. */
    const char *dir = at("out dir \xc3\xa9");
    mkdir_utf8(dir);
    char board[4096], report[4096], gerbers[4096], drill[4096], pos[4096];
    snprintf(board, sizeof board, "%s" SEPS "my board.kicad_pcb", dir);
    snprintf(report, sizeof report, "%s" SEPS "drc report.json", dir);
    snprintf(gerbers, sizeof gerbers, "%s" SEPS "gerbers", dir);
    snprintf(drill, sizeof drill, "%s" SEPS "drill", dir);
    snprintf(pos, sizeof pos, "%s" SEPS "pos.csv", dir);
#ifdef _WIN32
    { wchar_t w[4096]; wpath(board, w); HANDLE h = CreateFileW(w, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL); DWORD n; WriteFile(h, "(kicad_pcb)", 11, &n, NULL); CloseHandle(h); }
#else
    { FILE *f = fopen(board, "wb"); fputs("(kicad_pcb)", f); fclose(f); }
#endif
    CHECK(kicad_cli_open(&a, FAKE_KICAD_CLI, 20000, true, &cli, &r) == KCLI_OK);

    KicadDrcOptions o = {false, true};
    CHECK(kicad_cli_drc(&a, &cli, board, report, &o, &r) == KCLI_OK && !r.drc_violations && r.exit_code == 0);
    CHECK(r.output && !strcmp(r.output, report) && r.file_count == 1 && kind(report) == 1);
    char *text = slurp(report);
    /* The fake echoes its argv after the JSON: each argument arrived whole, the board path included. */
    char expect[8192];
    snprintf(expect, sizeof expect, "pcb\ndrc\n--format\njson\n--exit-code-violations\n--all-track-errors\n-o\n");
    CHECK(text && strstr(text, expect) && strstr(text, board));
    CHECK(text && strstr(text, "drc report.json\n"));
    free(text);
    /* Never overwrite: the same output again is refused and the first report is untouched. */
    char *before = slurp(report);
    CHECK(kicad_cli_drc(&a, &cli, board, report, &o, &r) == KCLI_EXISTS && !r.launched);
    char *after = slurp(report);
    CHECK(before && after && !strcmp(before, after));
    free(before); free(after);

    CHECK(kicad_cli_gerbers(&a, &cli, board, gerbers, &r) == KCLI_OK && kind(gerbers) == 2);
    CHECK(r.file_count == 2 && !strcmp(r.files[0], "args.txt") && !strcmp(r.files[1], "design-F_Cu.gtl"));
    CHECK(kicad_cli_drill(&a, &cli, board, drill, &r) == KCLI_OK && r.file_count == 2);
    char *args = slurp(strcat(strcpy(expect, drill), SEPS "args.txt"));
    CHECK(args && strstr(args, "--drill-origin\nabsolute\n--excellon-zeros-format\ndecimal\n--excellon-units\nmm\n"));
    free(args);
    ExcFile f;
    /* Read through memory: exc_parse_path uses fopen, which cannot open non-ASCII paths on Windows (a limitation of
     * the protected excellon module, noted in docs/engineering/KICAD_CLI.md). */
    char *drl = slurp(strcat(strcpy(expect, drill), SEPS "design.drl"));
    CHECK(drl && exc_parse(&a, drl, strlen(drl), "design.drl", &f) == EXC_OK && f.hit_count == 2);
    free(drl);
    CHECK(kicad_cli_pos(&a, &cli, board, pos, &r) == KCLI_OK && kind(pos) == 1);
    CHECK(count_staging(dir) == 0);

    /* Exit codes. */
    setenv_utf8("FAKE_KICAD_MODE", "violations");
    CHECK(kicad_cli_drc(&a, &cli, board, at("v.json"), NULL, &r) == KCLI_OK && r.drc_violations && r.exit_code == 5 && kind(at("v.json")) == 1);
    setenv_utf8("FAKE_KICAD_MODE", "fail");
    CHECK(kicad_cli_gerbers(&a, &cli, board, at("g-fail"), &r) == KCLI_TOOL_FAILED && r.exit_code == 3);
    CHECK(strstr(r.stderr_text, "boom: the board is broken") && kind(at("g-fail")) == 0 && !r.output);
    setenv_utf8("FAKE_KICAD_MODE", "nooutput");
    CHECK(kicad_cli_drill(&a, &cli, board, at("d-none"), &r) == KCLI_NO_OUTPUT && kind(at("d-none")) == 0);
    CHECK(kicad_cli_pos(&a, &cli, board, at("p-none.csv"), &r) == KCLI_NO_OUTPUT && kind(at("p-none.csv")) == 0);
    CHECK(strstr(r.stdout_text, "fake kicad-cli: pcb export"));

    /* Timeout: the fake hangs for 60 s; it is killed after 300 ms. */
    setenv_utf8("FAKE_KICAD_MODE", "sleep");
    KicadCli quick = cli;
    quick.timeout_ms = 300;
    CHECK(kicad_cli_drc(&a, &quick, board, at("t.json"), NULL, &r) == KCLI_TIMEOUT && r.timed_out && r.exit_code == -1);
    CHECK(r.elapsed_ms >= 250 && r.elapsed_ms < 10000 && kind(at("t.json")) == 0);

    /* 4 MiB on each of stdout and stderr: captured whole, no deadlock. */
    setenv_utf8("FAKE_KICAD_MODE", "flood");
    CHECK(kicad_cli_drc(&a, &cli, board, at("f.json"), NULL, &r) == KCLI_OK);
    CHECK(r.stdout_len >= 4u * 1024 * 1024 && r.stderr_len >= 4u * 1024 * 1024);   /* >=: Windows text mode adds 
 */

    /* Version gate: a KiCad 10 CLI is refused when 9.x is required, accepted and labelled otherwise. */
    setenv_utf8("FAKE_KICAD_MODE", "");
    setenv_utf8("FAKE_KICAD_VERSION", "10.0.5");
    CHECK(kicad_cli_open(&a, FAKE_KICAD_CLI, 20000, true, &cli, &r) == KCLI_WRONG_VERSION && cli.major == 10);
    CHECK(kicad_cli_open(&a, FAKE_KICAD_CLI, 20000, false, &cli, &r) == KCLI_OK && !r.cli_is_9x);
    CHECK(kicad_cli_pos(&a, &cli, board, at("p10.csv"), &r) == KCLI_OK && !r.cli_is_9x && !strcmp(r.cli_version, "10.0.5"));
    cli.require_9x = true;
    CHECK(kicad_cli_pos(&a, &cli, board, at("p10b.csv"), &r) == KCLI_WRONG_VERSION && !r.launched);
    setenv_utf8("FAKE_KICAD_VERSION", "garbage");
    CHECK(kicad_cli_open(&a, FAKE_KICAD_CLI, 20000, false, &cli, &r) == KCLI_WRONG_VERSION);
    setenv_utf8("FAKE_KICAD_VERSION", NULL);

    /* Inputs. */
    CHECK(kicad_cli_open(&a, FAKE_KICAD_CLI, 20000, true, &cli, &r) == KCLI_OK);
    CHECK(kicad_cli_drc(&a, &cli, at("missing.kicad_pcb"), at("m.json"), NULL, &r) == KCLI_INVALID_ARGUMENT && !r.launched);
    CHECK(count_staging(ROOTDIR) == 0);
    arena_destroy(&a);
}

/* ------------------------------------------------------------ real kicad-cli */

static int test_real(void)
{
    const char *path = getenv("KICAD_CLI");
    if (!path || !*path) { puts("kicad_cli --real: UNVERIFIED (set KICAD_CLI)"); return 77; }
    Arena a;
    KicadCli cli;
    KicadCliResult r;
    arena_init(&a, 1 << 20);
    rm_tree(ROOTDIR);
    mkdir_utf8(ROOTDIR);
    KicadCliStatus s = kicad_cli_open(&a, path, 300000, false, &cli, &r);
    CHECK(s == KCLI_OK);
    if (s) { fprintf(stderr, "open: %s %s\n", kicad_cli_status_name(s), r.message ? r.message : ""); return 1; }
    printf("kicad_cli --real: kicad-cli %s%s\n", cli.version, cli.major == 9 ? "" : " (development check, not KiCad 9 evidence)");
    /* Work on a copy: kicad-cli writes project-local settings (.kicad_prl) next to the board it opens. */
    char *src = slurp(BOARD_GOLDEN_DIR "/r0603x5.kicad_pcb");
    const char *board = at("r0603x5.kicad_pcb");
    FILE *copy = src ? fopen(board, "wb") : NULL;
    CHECK(copy != NULL);
    if (!copy) return 1;
    fputs(src, copy);
    fclose(copy);
    free(src);

    s = kicad_cli_drc(&a, &cli, board, at("drc.json"), NULL, &r);
    CHECK(s == KCLI_OK);
    if (s) fprintf(stderr, "drc: %s exit %d\n%s\n", kicad_cli_status_name(s), r.exit_code, r.stderr_text ? r.stderr_text : "");
    JsonValue *v;
    JsonError je;
    CHECK(s || json_parse_file(&a, at("drc.json"), &v, &je) == JSON_OK);
    printf("  drc: exit %d, %s, %llu ms\n", r.exit_code, r.drc_violations ? "violations reported" : "no violations", (unsigned long long)r.elapsed_ms);

    s = kicad_cli_gerbers(&a, &cli, board, at("gerbers"), &r);
    CHECK(s == KCLI_OK && r.file_count >= 3);
    bool job = false;
    for (size_t i = 0; i < r.file_count; i++) job |= strstr(r.files[i], ".gbrjob") != NULL;
    CHECK(job);
    printf("  gerbers: %zu files, %llu ms\n", r.file_count, (unsigned long long)r.elapsed_ms);

    s = kicad_cli_drill(&a, &cli, board, at("drill"), &r);
    CHECK(s == KCLI_OK);
    Board b;
    BoardError be;
    CHECK(board_load(&a, board, &b, &be) == BOARD_OK);
    size_t holes = 0, matched = 0;
    for (size_t i = 0; s == KCLI_OK && i < r.file_count; i++) {
        if (!strstr(r.files[i], ".drl")) continue;
        char p[4096];
        snprintf(p, sizeof p, "%s" SEPS "%s", at("drill"), r.files[i]);
        ExcFile f;
        CHECK(exc_parse_path(&a, p, &f) == EXC_OK);
        for (size_t h = 0; h < f.hit_count; h++, holes++)
            for (size_t k = 0; k < b.pad_count; k++)
                if (b.pads[k].has_drill && b.pads[k].at.x == f.hits[h].pos.x && b.pads[k].at.y == -f.hits[h].pos.y &&
                    b.pads[k].drill_x == f.tools[f.hits[h].tool].diameter) { matched++; break; }
    }
    CHECK(holes == 3 && matched == 3);
    printf("  drill: %zu files, %zu holes, all at the board model's J1 pads\n", r.file_count, holes);

    s = kicad_cli_pos(&a, &cli, board, at("pos.csv"), &r);
    CHECK(s == KCLI_OK);
    char *csv = slurp(at("pos.csv"));
    CHECK(csv && strstr(csv, "\"R1\"") && strstr(csv, "\"R5\"") && strstr(csv, "\"J1\""));
    free(csv);
    CHECK(count_staging(ROOTDIR) == 0);
    arena_destroy(&a);
    return failures ? 1 : 0;
}

int main(int argc, char **argv)
{
    snprintf(ROOTDIR, sizeof ROOTDIR, "%s", KCLI_TEST_DIR);
    if (argc > 1 && !strcmp(argv[1], "--real")) {
        strcat(ROOTDIR, "-real");
        int rc = test_real();
        if (rc == 1 || failures) { fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
        if (rc == 0) puts("kicad_cli --real: all checks passed");
        return rc;
    }
    test_quote();
    test_fake();
    if (failures) { fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
    puts("kicad_cli: all checks passed");
    return 0;
}

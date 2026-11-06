/* The tool server (src/v2_src/tool_server.c, track F), driven by a scripted transcript.
 *
 * tests/golden/tool_server/requests.jsonl is run through tool_server_handle with root = the repository and
 * kicad-cli = tests/fake_kicad_cli.c ("${WORK}" stands for this test's scratch folder). The responses must equal
 * tests/golden/tool_server/responses.jsonl line for line ("${WORK}" substituted back); notifications must get none;
 * every response must be one line of JSON-RPC 2.0; and the session log must hold every message in and out.
 * `tool_server_test --write-golden` rewrites responses.jsonl for review. */
#include "tool_server.h"

#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
static void rm_tree(const char *p)
{
    char pat[4096];
    snprintf(pat, sizeof pat, "%s\\*", p);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            char c[4096];
            snprintf(c, sizeof c, "%s\\%s", p, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) rm_tree(c); else DeleteFileA(c);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(p);
}
static void make_dir(const char *p) { CreateDirectoryA(p, NULL); }
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
static void rm_tree(const char *p)
{
    DIR *d = opendir(p);
    if (!d) { unlink(p); return; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char c[4096];
        struct stat st;
        snprintf(c, sizeof c, "%s/%s", p, e->d_name);
        if (!lstat(c, &st) && S_ISDIR(st.st_mode)) rm_tree(c); else unlink(c);
    }
    closedir(d);
    rmdir(p);
}
static void make_dir(const char *p) { mkdir(p, 0700); }
#endif

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    size_t got = fread(b, 1, (size_t)n, f), k = 0;
    fclose(f);
    for (size_t i = 0; i < got; i++) if (b[i] != '\r') b[k++] = b[i];
    b[k] = 0;
    return b;
}

/* Replace every `from` in `s` by `to` (malloc'd result). */
static char *replace(const char *s, const char *from, const char *to)
{
    size_t nf = strlen(from), nt = strlen(to), count = 0;
    for (const char *p = strstr(s, from); p; p = strstr(p + nf, from)) count++;
    char *out = malloc(strlen(s) + count * (nt > nf ? nt - nf : 0) + 1), *o = out;
    while (*s) {
        if (!strncmp(s, from, nf)) { memcpy(o, to, nt); o += nt; s += nf; }
        else *o++ = *s++;
    }
    *o = 0;
    return out;
}

typedef struct Log { size_t lines, ins, outs; bool valid; Arena arena; } Log;

static void on_log(void *ctx, const char *line, size_t len)
{
    Log *l = ctx;
    JsonValue *v;
    JsonError e;
    l->lines++;
    if (memchr(line, '\n', len) || json_parse(&l->arena, line, len, &v, &e) != JSON_OK || !json_get(v, "seq")) { l->valid = false; return; }
    l->ins += json_get(v, "in") != NULL;
    l->outs += json_get(v, "out") != NULL;
}

int main(int argc, char **argv)
{
    bool write = argc > 1 && !strcmp(argv[1], "--write-golden");
    rm_tree(TS_WORK);
    make_dir(TS_WORK);
    Log log;
    memset(&log, 0, sizeof log);
    log.valid = true;
    arena_init(&log.arena, 1 << 16);
    ToolServerConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.root = TS_ROOT;
    cfg.kicad_cli = FAKE_KICAD_CLI;
    cfg.kicad_timeout_ms = 60000;
    cfg.version = "test";
    cfg.log = on_log;
    cfg.log_ctx = &log;
    ToolServer s;
    tool_server_init(&s, &cfg);

    char *requests = slurp(TS_ROOT "/tests/golden/tool_server/requests.jsonl");
    CHECK(requests != NULL);
    if (!requests) return 1;
    size_t cap = 1 << 20, n = 0, sent = 0, answered = 0;
    char *responses = malloc(cap);
    responses[0] = 0;
    for (char *line = requests, *next; *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0; else next = line + strlen(line);
        if (!*line) continue;
        char *req = replace(line, "${WORK}", TS_WORK);
        size_t rl;
        const char *resp = tool_server_handle(&s, req, strlen(req), &rl);
        sent++;
        free(req);
        if (!resp) continue;
        answered++;
        CHECK(!memchr(resp, '\n', rl));
        JsonValue *v;
        JsonError e;
        CHECK(json_parse(&log.arena, resp, rl, &v, &e) == JSON_OK && !strcmp(json_get(v, "jsonrpc")->text, "2.0") &&
              (json_get(v, "result") != NULL) != (json_get(v, "error") != NULL));
        char *back = replace(resp, TS_WORK, "${WORK}");
        size_t nb = strlen(back);
        while (n + nb + 2 > cap) { cap *= 2; responses = realloc(responses, cap); }
        memcpy(responses + n, back, nb);
        n += nb;
        responses[n++] = '\n';
        responses[n] = 0;
        free(back);
    }
    CHECK(s.exited && s.shutdown);
    CHECK(sent == 25 && answered == 23);                      /* two notifications: tools/list without id, exit */
    CHECK(log.valid && log.ins == sent && log.outs == answered && log.lines == sent + answered);

    if (write) {
        FILE *f = fopen(TS_ROOT "/tests/golden/tool_server/responses.jsonl", "wb");
        if (!f) return 1;
        fwrite(responses, 1, n, f);
        fclose(f);
        puts("tool_server: responses.jsonl written; review it");
    } else {
        char *golden = slurp(TS_ROOT "/tests/golden/tool_server/responses.jsonl");
        CHECK(golden != NULL);
        if (golden && strcmp(golden, responses)) {
            size_t i = 0, lineno = 1;
            while (golden[i] && golden[i] == responses[i]) { if (golden[i] == '\n') lineno++; i++; }
            fprintf(stderr, "responses differ from tests/golden/tool_server/responses.jsonl at response %zu:\n  golden: %.120s\n  now:    %.120s\n",
                    lineno, golden + i, responses + i);
            failures++;
        }
        free(golden);
    }
    free(requests);
    free(responses);
    tool_server_destroy(&s);
    arena_destroy(&log.arena);
    if (failures) { fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
    if (!write) puts("tool_server: all checks passed");
    return 0;
}

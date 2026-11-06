#include "cli.h"

#include <stdlib.h>
#include <string.h>

static const char *PROBE =
    "import sys;print('%d.%d.%d'%sys.version_info[:3]);raise SystemExit(0 if sys.version_info>=(3,10) else 3)";

static int probe(PcbaPython *py, const char *program, const char *flag) {
    const char *argv[6] = {program, flag ? flag : "-c", flag ? "-c" : PROBE, flag ? PROBE : NULL, NULL, NULL};
    CliProcResult r;
    int ok = cli_run(argv, CLI_STDOUT_CAPTURE, &r) && r.exit_code == 0;
    if (r.launched && r.output) {
        snprintf(py->version, sizeof py->version, "%s", r.output);
        py->version[strcspn(py->version, "\r\n")] = '\0';
    }
    cli_proc_result_free(&r);
    if (!ok) return 0;
    snprintf(py->program, sizeof py->program, "%s", program);
    py->argv[0] = py->program;
    py->argv[1] = flag;
    py->argv[2] = NULL;
    py->found = 1;
    return 1;
}

/* PCBA_PYTHON / HWC_PYTHON are honoured strictly; otherwise try the usual launchers. */
int pcba_find_python(PcbaPython *out) {
    static PcbaPython cached;
    static int done;
    if (!done) {
        char explicit_python[1024];
        memset(&cached, 0, sizeof cached);
        if (cli_getenv("PCBA_PYTHON", explicit_python, sizeof explicit_python) ||
            cli_getenv("HWC_PYTHON", explicit_python, sizeof explicit_python)) {
            probe(&cached, explicit_python, NULL);
        } else {
#ifdef _WIN32
            if (!probe(&cached, "py", "-3") && !probe(&cached, "python", NULL)) probe(&cached, "python3", NULL);
#else
            if (!probe(&cached, "python3", NULL)) probe(&cached, "python", NULL);
#endif
        }
        done = 1;
    }
    *out = cached;
    if (out->found) out->argv[0] = out->program;
    return out->found;
}

static int hex4(const char *p, unsigned *out) {
    *out = 0;
    for (int i = 0; i < 4; ++i) {
        char c = p[i];
        int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (v < 0) return 0;
        *out = *out * 16 + (unsigned)v;
    }
    return 1;
}

/* Appends code point cp as UTF-8 if it fits (leaving room for the terminator). */
static void put_utf8(char *s, size_t *n, size_t cap, unsigned cp) {
    char b[4];
    size_t k = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
    if (k == 1) b[0] = (char)cp;
    else if (k == 2) { b[0] = (char)(0xC0 | cp >> 6); b[1] = (char)(0x80 | (cp & 0x3F)); }
    else if (k == 3) { b[0] = (char)(0xE0 | cp >> 12); b[1] = (char)(0x80 | (cp >> 6 & 0x3F)); b[2] = (char)(0x80 | (cp & 0x3F)); }
    else { b[0] = (char)(0xF0 | cp >> 18); b[1] = (char)(0x80 | (cp >> 12 & 0x3F)); b[2] = (char)(0x80 | (cp >> 6 & 0x3F)); b[3] = (char)(0x80 | (cp & 0x3F)); }
    if (*n + k + 1 > cap) return;
    memcpy(s + *n, b, k);
    *n += k;
}

/* The backend's "message" as UTF-8: JSON escapes (including \uXXXX and surrogate pairs) are decoded,
 * so non-ASCII text such as units reaches the user unchanged. */
static void extract_message(const char *raw, char *message, size_t cap) {
    const char *p = strstr(raw, "\"message\": \"");
    size_t n = 0;
    if (cap) message[0] = '\0';
    if (!p || !cap) return;
    for (p += 12; *p && *p != '"' && n + 1 < cap; ++p) {
        if (*p != '\\' || !p[1]) { message[n++] = *p; continue; }
        unsigned cp, low;
        switch (*++p) {
        case 'n': message[n++] = '\n'; break;
        case 't': message[n++] = '\t'; break;
        case 'r': message[n++] = '\r'; break;
        case 'b': message[n++] = '\b'; break;
        case 'f': message[n++] = '\f'; break;
        case 'u':
            if (!hex4(p + 1, &cp)) { message[n++] = 'u'; break; }
            p += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && p[1] == '\\' && p[2] == 'u' && hex4(p + 3, &low) && low >= 0xDC00 && low < 0xE000) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                p += 6;
            }
            put_utf8(message, &n, cap, cp);
            break;
        default: message[n++] = *p; /* \" \\ \/ */
        }
    }
    message[n] = '\0';
}

int pcba_call_backend(PcbaContext *ctx, const char *const *bargs, char *status, size_t status_cap,
                      char **raw_result, char *message, size_t message_cap) {
    PcbaPython py;
    char root[1024], script[1200], result_path[1200];
    *raw_result = NULL;
    snprintf(status, status_cap, "ERROR");
    message[0] = '\0';
    if (!pcba_find_python(&py)) {
        snprintf(status, status_cap, "UNVERIFIED");
        snprintf(message, message_cap, "Python 3.10+ not found%s%s; install it or set PCBA_PYTHON",
                 *py.version ? " (found " : "", *py.version ? py.version : "");
        if (*py.version) strncat(message, ")", message_cap - strlen(message) - 1);
        return 0;
    }
    if (!cli_tool_root(root, sizeof root) || snprintf(script, sizeof script, "%s/tools/copper.py", root) <= 0 ||
        !cli_is_file(script)) {
        snprintf(status, status_cap, "UNVERIFIED");
        snprintf(message, message_cap, "tools/copper.py not found under '%s'; set PCBA_ROOT to the source tree", root);
        return 0;
    }
    if (!cli_temp_path("pcba-result", ".json", result_path, sizeof result_path)) {
        snprintf(message, message_cap, "cannot create a temporary result path");
        return 0;
    }
    const char *argv[64];
    size_t n = 0;
    for (size_t i = 0; py.argv[i]; ++i) argv[n++] = py.argv[i];
    argv[n++] = "-X";
    argv[n++] = "utf8";
    argv[n++] = script;
    argv[n++] = "--format";
    argv[n++] = ctx->json ? "quiet" : "human";
    argv[n++] = "--result-json";
    argv[n++] = result_path;
    for (size_t i = 0; bargs[i] && n + 1 < sizeof argv / sizeof argv[0]; ++i) argv[n++] = bargs[i];
    argv[n] = NULL;

    CliProcResult r;
    int launched = cli_run(argv, ctx->json ? CLI_STDOUT_TO_STDERR : CLI_STDOUT_INHERIT, &r);
    int exit_code = r.exit_code;
    cli_proc_result_free(&r);
    if (!launched) {
        snprintf(status, status_cap, "UNVERIFIED");
        snprintf(message, message_cap, "could not launch %s", py.program);
        return 0;
    }
    char *raw = cli_read_file(result_path, NULL);
    cli_remove(result_path);
    if (!raw) {
        snprintf(message, message_cap, "backend exited with code %d without reporting a result", exit_code);
        return 0;
    }
    static const char prefix[] = "{\"status\": \"";
    if (strncmp(raw, prefix, sizeof prefix - 1)) {
        snprintf(message, message_cap, "malformed backend result");
        free(raw);
        return 0;
    }
    const char *s = raw + sizeof prefix - 1;
    size_t len = strcspn(s, "\"");
    snprintf(status, status_cap, "%.*s", (int)len, s);
    extract_message(raw, message, message_cap);
    if (!strcmp(status, "PASS") && exit_code != 0) {
        /* Never turn an inconsistent backend outcome into success. */
        snprintf(status, status_cap, "ERROR");
        snprintf(message, message_cap, "backend reported PASS but exited with code %d", exit_code);
    }
    *raw_result = raw;
    return 1;
}

int pcba_run_backend(PcbaContext *ctx, const char *command, const char *const *bargs) {
    char status[32], message[1024], *raw = NULL;
    pcba_call_backend(ctx, bargs, status, sizeof status, &raw, message, sizeof message);
    int code = pcba_finish(ctx, command, status, message, raw);
    free(raw);
    return code;
}

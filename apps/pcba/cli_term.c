#include "cli.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static const PcbaStatusInfo STATUSES[] = {
    {"PASS", PCBA_EXIT_PASS, "the check ran and passed"},
    {"FAIL", PCBA_EXIT_FAIL, "the check ran and failed"},
    {"INVALID_INPUT", PCBA_EXIT_INVALID_INPUT, "an input file or value violates its contract"},
    {"UNVERIFIED", PCBA_EXIT_UNVERIFIED, "a required tool, model, data or geometry was unavailable"},
    {"NOT_IMPLEMENTED", PCBA_EXIT_NOT_IMPLEMENTED, "the capability does not exist yet"},
    {"USAGE", PCBA_EXIT_USAGE, "invalid command-line usage"},
    {"ERROR", PCBA_EXIT_INTERNAL, "internal or environment error; nothing was established"},
};

const PcbaStatusInfo *pcba_statuses(size_t *count) {
    *count = sizeof STATUSES / sizeof STATUSES[0];
    return STATUSES;
}

int pcba_exit_for_status(const char *status) {
    for (size_t i = 0; i < sizeof STATUSES / sizeof STATUSES[0]; ++i)
        if (status && !strcmp(status, STATUSES[i].status)) return STATUSES[i].exit_code;
    return PCBA_EXIT_INTERNAL;
}

const char *pcba_status_for_exit(int exit_code) {
    for (size_t i = 0; i < sizeof STATUSES / sizeof STATUSES[0]; ++i)
        if (STATUSES[i].exit_code == exit_code) return STATUSES[i].status;
    return "ERROR";
}

static const char *meaning(const char *status) {
    for (size_t i = 0; i < sizeof STATUSES / sizeof STATUSES[0]; ++i)
        if (!strcmp(status, STATUSES[i].status)) return STATUSES[i].meaning;
    return "";
}

void pcba_term_init(PcbaContext *ctx, int no_color_flag) {
    char value[64];
    int vt = cli_console_init();
    ctx->color = 0;
    if (no_color_flag || ctx->json || cli_getenv("NO_COLOR", value, sizeof value)) return;
    if (cli_getenv("PCBA_FORCE_COLOR", value, sizeof value)) { ctx->color = 1; return; }
    if (cli_getenv("TERM", value, sizeof value) && !strcmp(value, "dumb")) return;
    ctx->color = vt && cli_stdout_is_terminal();
}

const char *pcba_style(const PcbaContext *ctx, PcbaStyle style) {
    static const char *codes[] = {"\x1b[0m", "\x1b[1m", "\x1b[2m", "\x1b[1;32m", "\x1b[1;31m",
                                  "\x1b[1;33m", "\x1b[36m", "\x1b[38;5;208m"};
    return ctx->color ? codes[style] : "";
}

const char *pcba_status_style(const PcbaContext *ctx, const char *status) {
    if (!strcmp(status, "PASS")) return pcba_style(ctx, PCBA_STYLE_PASS);
    if (!strcmp(status, "UNVERIFIED") || !strcmp(status, "NOT_IMPLEMENTED")) return pcba_style(ctx, PCBA_STYLE_WARN);
    return pcba_style(ctx, PCBA_STYLE_FAIL);
}

void pcba_banner(const PcbaContext *ctx) {
    static const char *logo[] = {
        " ___  ___ ___   _   ___ ",
        "| _ \\/ __| _ ) /_\\ |_ _|",
        "|  _/ (__| _ \\/ _ \\ | | ",
        "|_|  \\___|___/_/ \\_\\___|",
    };
    static const char *colors[] = {"\x1b[38;5;214m", "\x1b[38;5;208m", "\x1b[38;5;202m", "\x1b[38;5;37m"};
    for (size_t i = 0; i < 4; ++i)
        printf("  %s%s%s\n", ctx->color ? colors[i] : "", logo[i], pcba_style(ctx, PCBA_STYLE_RESET));
    printf("  %sPCBAI %s%s  %s| Copper Studio V2 | %s | C%s | %s%s\n", pcba_style(ctx, PCBA_STYLE_BOLD),
           pcba_version_string(), pcba_style(ctx, PCBA_STYLE_RESET), pcba_style(ctx, PCBA_STYLE_DIM), PCBA_COMPILER,
           PCBA_C_STANDARD, PCBA_BUILD_TYPE, pcba_style(ctx, PCBA_STYLE_RESET));
    printf("  Deterministic electronics compiler: .copper project -> checked KiCad 9 board, with honest "
           "PASS/FAIL/UNVERIFIED results.\n\n");
}

void pcba_json_string(FILE *f, const char *s) {
    fputc('"', f);
    for (; s && *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { fputc('\\', f); fputc(c, f); }
        else if (c == '\n') fputs("\\n", f);
        else if (c == '\r') fputs("\\r", f);
        else if (c == '\t') fputs("\\t", f);
        else if (c < 0x20) fprintf(f, "\\u%04x", c);
        else fputc(c, f);
    }
    fputc('"', f);
}

void pcba_buf_add(PcbaBuf *b, const char *text) {
    size_t n = strlen(text);
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 1024;
        while (cap < b->len + n + 1) cap *= 2;
        char *grown = realloc(b->data, cap);
        if (!grown) return;
        b->data = grown;
        b->cap = cap;
    }
    memcpy(b->data + b->len, text, n + 1);
    b->len += n;
}

void pcba_buf_json(PcbaBuf *b, const char *text) {
    char one[8];
    pcba_buf_add(b, "\"");
    for (; text && *text; ++text) {
        unsigned char c = (unsigned char)*text;
        if (c == '"' || c == '\\') snprintf(one, sizeof one, "\\%c", c);
        else if (c < 0x20) snprintf(one, sizeof one, "\\u%04x", c);
        else snprintf(one, sizeof one, "%c", c);
        pcba_buf_add(b, one);
    }
    pcba_buf_add(b, "\"");
}

void pcba_buf_free(PcbaBuf *b) {
    free(b->data);
    memset(b, 0, sizeof *b);
}

void pcba_print_check(const PcbaContext *ctx, const char *status, const char *name, const char *detail,
                      const char *hint) {
    printf("  %s%-10s%s %-20s %s\n", pcba_status_style(ctx, status), status, pcba_style(ctx, PCBA_STYLE_RESET), name,
           detail);
    if (hint && *hint && strcmp(status, "PASS")) printf("  %-10s %-20s -> %s\n", "", "", hint);
}

int pcba_finish(const PcbaContext *ctx, const char *command, const char *status, const char *message,
                const char *result_json) {
    int code = pcba_exit_for_status(status);
    if (ctx->json) {
        printf("{\"schema\":1,\"tool\":\"pcba\",\"version\":");
        pcba_json_string(stdout, pcba_version_string());
        printf(",\"command\":");
        pcba_json_string(stdout, command);
        printf(",\"status\":");
        pcba_json_string(stdout, status);
        printf(",\"exit_code\":%d,\"message\":", code);
        pcba_json_string(stdout, message ? message : "");
        printf(",\"result\":");
        if (result_json && *result_json) {
            size_t n = strlen(result_json);
            while (n && (result_json[n - 1] == '\n' || result_json[n - 1] == '\r' || result_json[n - 1] == ' ')) --n;
            fwrite(result_json, 1, n, stdout);
        } else {
            fputs("null", stdout);
        }
        fputs("}\n", stdout);
    } else {
        FILE *f = code == PCBA_EXIT_USAGE || code == PCBA_EXIT_INTERNAL ? stderr : stdout;
        int styled = f == stdout;
        fprintf(f, "%s%s%s %s: %s\n", styled ? pcba_status_style(ctx, status) : "", status,
                styled ? pcba_style(ctx, PCBA_STYLE_RESET) : "", command,
                message && *message ? message : meaning(status));
    }
    fflush(stdout);
    return code;
}

int pcba_usage_error(const PcbaContext *ctx, const PcbaCommand *cmd, const char *fmt, ...) {
    char message[512], path[64] = "pcba", usage[512] = "pcba <command> [arguments]";
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(message, sizeof message, fmt, ap);
    va_end(ap);
    if (cmd) {
        pcba_command_path(cmd, path, sizeof path);
        pcba_command_usage(cmd, usage, sizeof usage);
    }
    if (ctx->json) return pcba_finish(ctx, path, "USAGE", message, NULL);
    fprintf(stderr, "pcba: error: %s\nUsage: %s\nRun 'pcba help%s%s' for arguments and examples.\n", message, usage,
            cmd ? " " : "", cmd ? path : "");
    return PCBA_EXIT_USAGE;
}

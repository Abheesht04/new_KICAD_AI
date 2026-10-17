/* pcba: the single user-facing PCBAI command. See cli.h for the module split. */
#include "cli.h"

#include <stdlib.h>
#include <string.h>

static void group_commands(const char *group, char *buf, size_t cap) {
    size_t count, n = 0;
    const PcbaCommand *cmds = pcba_commands(&count);
    buf[0] = '\0';
    for (size_t i = 0; i < count; ++i)
        if (cmds[i].group && !strcmp(cmds[i].group, group) && n + strlen(cmds[i].name) + 3 < cap)
            n += (size_t)snprintf(buf + n, cap - n, "%s%s", n ? ", " : "", cmds[i].name);
}

int main(int argc, char **argv) {
    PcbaContext ctx;
    char buf[512];
    int no_color = 0, help = 0, version = 0, n = 0, options_done = 0;
    memset(&ctx, 0, sizeof ctx);
    argv = cli_utf8_argv(argc, argv);

    if (cli_self_path(ctx.exe_dir, sizeof ctx.exe_dir)) {
        char *slash = strrchr(ctx.exe_dir, '/'), *back = strrchr(ctx.exe_dir, '\\');
        if (back > slash) slash = back;
        if (slash) *slash = '\0';
    }

    /* Global options may appear anywhere before "--". */
    char **rest = calloc((size_t)argc + 1, sizeof *rest);
    if (!rest) return PCBA_EXIT_INTERNAL;
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (!options_done && !strcmp(a, "--json")) ctx.json = 1;
        else if (!options_done && !strcmp(a, "--no-color")) no_color = 1;
        else if (!options_done && (!strcmp(a, "-h") || !strcmp(a, "--help"))) help = 1;
        else if (!options_done && !strcmp(a, "--version")) version = 1;
        else {
            if (!strcmp(a, "--")) options_done = 1;
            rest[n++] = argv[i];
        }
    }
    pcba_term_init(&ctx, no_color);

    /* Children (copper.py) use the copper-core built beside pcba unless COPPER_CORE says otherwise. */
    cli_default_copper_core();

    int code;
    int consumed = 0;
    const PcbaCommand *cmd = n ? pcba_find_command(n, rest, &consumed) : NULL;
    if (version && n == 0) code = pcba_version(&ctx);
    else if (n == 0) code = pcba_help(&ctx, 0, NULL);
    else if (!cmd && consumed == 1 && n == 1) { /* bare group, e.g. "pcba project" */
        if (help) code = pcba_help(&ctx, 1, rest);
        else {
            group_commands(rest[0], buf, sizeof buf);
            code = pcba_usage_error(&ctx, NULL, "'%s' needs a subcommand: %s", rest[0], buf);
        }
    } else if (!cmd) {
        code = consumed == 1 ? (group_commands(rest[0], buf, sizeof buf),
                                pcba_usage_error(&ctx, NULL, "unknown %s subcommand '%s' (choose: %s)", rest[0], rest[1], buf))
                             : pcba_usage_error(&ctx, NULL, "unknown command '%s'", rest[0]);
    } else if (help) {
        code = pcba_command_help(&ctx, cmd);
    } else {
        PcbaArgs args;
        char err[512];
        if (!pcba_parse_args(cmd, n - consumed, rest + consumed, &args, err, sizeof err))
            code = pcba_usage_error(&ctx, cmd, "%s", err);
        else
            code = cmd->run(&ctx, cmd, &args);
    }
    free(rest);
    return code;
}

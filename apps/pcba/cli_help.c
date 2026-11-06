#include "cli.h"

#include <string.h>

const char *pcba_version_string(void) { return PCBA_VERSION; }

static void wrap(const char *text, const char *indent, size_t width) {
    size_t col = 0;
    while (*text) {
        size_t word = strcspn(text, " ");
        if (col && col + 1 + word > width) { printf("\n"); col = 0; }
        if (!col) { printf("%s", indent); col = strlen(indent); }
        else { printf(" "); ++col; }
        printf("%.*s", (int)word, text);
        col += word;
        text += word;
        while (*text == ' ') ++text;
    }
    if (col) printf("\n");
}

static void print_lines(const char *text, const char *indent) {
    while (*text) {
        size_t n = strcspn(text, "\n");
        printf("%s%.*s\n", indent, (int)n, text);
        text += n;
        if (*text) ++text;
    }
}

static void print_exit_codes(void) {
    size_t count;
    const PcbaStatusInfo *s = pcba_statuses(&count);
    printf("Exit codes:\n");
    for (size_t i = 0; i < count; ++i) printf("  %-3d %-16s %s\n", s[i].exit_code, s[i].status, s[i].meaning);
}

static void print_entry(const PcbaContext *ctx, const PcbaCommand *cmd) {
    char usage[512];
    pcba_command_usage(cmd, usage, sizeof usage);
    if (strlen(usage) <= 52) printf("  %s%-52s%s %s\n", pcba_style(ctx, PCBA_STYLE_INFO), usage,
                                    pcba_style(ctx, PCBA_STYLE_RESET), cmd->summary);
    else printf("  %s%s%s\n  %-52s %s\n", pcba_style(ctx, PCBA_STYLE_INFO), usage, pcba_style(ctx, PCBA_STYLE_RESET),
                "", cmd->summary);
}

static const char *section_title(const char *group) {
    static const char *const titles[][2] = {{"general", "General"}, {"project", "Project"}, {"simulate", "Simulation"},
                                            {"parts", "Parts"}, {"library", "Library"}, {"dfm", "DFM"}};
    for (size_t i = 0; i < sizeof titles / sizeof titles[0]; ++i)
        if (!strcmp(group, titles[i][0])) return titles[i][1];
    return group;
}

static int help_json(const PcbaContext *ctx, const char *group) {
    size_t count, n = 0, sc;
    const PcbaCommand *cmds = pcba_commands(&count);
    const PcbaStatusInfo *s = pcba_statuses(&sc);
    char path[64], usage[512];
    printf("{\"schema\":1,\"tool\":\"pcba\",\"version\":");
    pcba_json_string(stdout, pcba_version_string());
    printf(",\"command\":\"help\",\"status\":\"PASS\",\"exit_code\":0,\"message\":\"\",\"result\":{\"commands\":[");
    for (size_t i = 0; i < count; ++i) {
        if (group && (!cmds[i].group || strcmp(cmds[i].group, group))) continue;
        pcba_command_path(&cmds[i], path, sizeof path);
        pcba_command_usage(&cmds[i], usage, sizeof usage);
        printf("%s{\"command\":", n++ ? "," : "");
        pcba_json_string(stdout, path);
        printf(",\"usage\":");
        pcba_json_string(stdout, usage);
        printf(",\"summary\":");
        pcba_json_string(stdout, cmds[i].summary);
        printf(",\"stability\":\"%s\"}", cmds[i].planned ? "NOT_IMPLEMENTED" : "stable");
    }
    printf("],\"exit_codes\":{");
    for (size_t i = 0; i < sc; ++i) printf("%s\"%s\":%d", i ? "," : "", s[i].status, s[i].exit_code);
    printf("}}}\n");
    (void)ctx;
    return PCBA_EXIT_PASS;
}

static int top_help(const PcbaContext *ctx, const char *group) {
    size_t count;
    const PcbaCommand *cmds = pcba_commands(&count);
    const char *section = "";
    if (ctx->json) return help_json(ctx, group);
    if (!group) pcba_banner(ctx);
    printf("%sUsage:%s pcba <command> [arguments] [--json] [--no-color]\n\n", pcba_style(ctx, PCBA_STYLE_BOLD),
           pcba_style(ctx, PCBA_STYLE_RESET));
    for (size_t i = 0; i < count; ++i) {
        const PcbaCommand *c = &cmds[i];
        const char *g = c->group ? c->group : "general";
        if (c->planned || (group && (!c->group || strcmp(c->group, group)))) continue;
        if (strcmp(section, g)) {
            printf("%s%s%s commands:%s\n", *section ? "\n" : "", pcba_style(ctx, PCBA_STYLE_ACCENT),
                   section_title(g), pcba_style(ctx, PCBA_STYLE_RESET));
            section = g;
        }
        print_entry(ctx, c);
    }
    int planned = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!cmds[i].planned || (group && (!cmds[i].group || strcmp(cmds[i].group, group)))) continue;
        if (!planned++) printf("\n%sPlanned, NOT_IMPLEMENTED (exit 4):%s\n", pcba_style(ctx, PCBA_STYLE_WARN),
                               pcba_style(ctx, PCBA_STYLE_RESET));
        print_entry(ctx, &cmds[i]);
    }
    if (!group) {
        printf("\nGlobal options:\n"
               "  --json        Exactly one JSON document on stdout; progress and errors go to stderr\n"
               "  --no-color    Plain text (also NO_COLOR=1; colour is only used on a terminal)\n"
               "  -h, --help    Help for pcba or for one command\n\n"
               "Examples:\n"
               "  pcba doctor\n"
               "  pcba project check examples/v2/divider.copper\n"
               "  pcba project build examples/v2/divider.copper --out out/divider\n"
               "  pcba project verify --out out/divider\n\n");
        print_exit_codes();
        printf("\nRun 'pcba help <command>' for arguments and examples.\n"
               "Developer tools, not the user workflow: copper-core (machine backend), hwc (legacy V1 CLI), "
               "fabcheck_demo (DFM demo).\n");
    }
    return PCBA_EXIT_PASS;
}

int pcba_command_help(const PcbaContext *ctx, const PcbaCommand *cmd) {
    char path[64], usage[512];
    pcba_command_path(cmd, path, sizeof path);
    pcba_command_usage(cmd, usage, sizeof usage);
    if (ctx->json) return help_json(ctx, cmd->group);
    printf("%spcba %s%s - %s\n\n", pcba_style(ctx, PCBA_STYLE_BOLD), path, pcba_style(ctx, PCBA_STYLE_RESET),
           cmd->summary);
    printf("Usage: %s\n\n", usage);
    wrap(cmd->description, "  ", 96);
    if (cmd->planned)
        printf("\n%sStatus: NOT_IMPLEMENTED%s (%s). Running it exits 4 and does nothing.\n",
               pcba_style(ctx, PCBA_STYLE_WARN), pcba_style(ctx, PCBA_STYLE_RESET), cmd->planned);
    if (cmd->option_count) {
        printf("\nOptions:\n");
        for (size_t i = 0; i < cmd->option_count; ++i) {
            char left[64];
            const PcbaOption *o = &cmd->options[i];
            snprintf(left, sizeof left, "%s%s%s", o->name, o->metavar ? " " : "", o->metavar ? o->metavar : "");
            printf("  %-26s %s%s\n", left, o->required ? "(required) " : "", o->help);
        }
    }
    printf("\nGlobal options: --json, --no-color, -h/--help\n\nExamples:\n");
    print_lines(cmd->examples, "  ");
    printf("\n");
    print_exit_codes();
    return PCBA_EXIT_PASS;
}

int pcba_help(PcbaContext *ctx, int argc, char **argv) {
    int consumed;
    if (argc == 0) return top_help(ctx, NULL);
    const PcbaCommand *cmd = pcba_find_command(argc, argv, &consumed);
    if (cmd && consumed == argc) return pcba_command_help(ctx, cmd);
    if (argc == 1 && pcba_is_group(argv[0])) return top_help(ctx, argv[0]);
    return pcba_usage_error(ctx, NULL, "no such command: %s%s%s", argv[0], argc > 1 ? " " : "", argc > 1 ? argv[1] : "");
}

int pcba_version(const PcbaContext *ctx) {
    char root[1024] = "";
    cli_tool_root(root, sizeof root);
    if (ctx->json) {
        printf("{\"schema\":1,\"tool\":\"pcba\",\"version\":");
        pcba_json_string(stdout, pcba_version_string());
        printf(",\"command\":\"version\",\"status\":\"PASS\",\"exit_code\":0,\"message\":\"\",\"result\":{\"version\":");
        pcba_json_string(stdout, pcba_version_string());
        printf(",\"compiler\":");
        pcba_json_string(stdout, PCBA_COMPILER);
        printf(",\"c_standard\":\"C%s\",\"build_type\":", PCBA_C_STANDARD);
        pcba_json_string(stdout, PCBA_BUILD_TYPE);
        printf(",\"source_root\":");
        pcba_json_string(stdout, root);
        printf(",\"kicad_target\":\"9.x\"}}\n");
        return PCBA_EXIT_PASS;
    }
    printf("pcba %s\n  compiler      %s\n  C standard    C%s\n  build type    %s\n  source root   %s\n"
           "  KiCad target  9.x (native validation)\n",
           pcba_version_string(), PCBA_COMPILER, PCBA_C_STANDARD, PCBA_BUILD_TYPE, root);
    return PCBA_EXIT_PASS;
}

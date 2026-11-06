#include "cli.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

int pcba_is_group(const char *word) {
    size_t count;
    const PcbaCommand *cmds = pcba_commands(&count);
    for (size_t i = 0; i < count; ++i)
        if (cmds[i].group && !strcmp(cmds[i].group, word)) return 1;
    return 0;
}

const PcbaCommand *pcba_find_command(int argc, char **argv, int *consumed) {
    size_t count;
    const PcbaCommand *cmds = pcba_commands(&count);
    *consumed = 0;
    if (argc < 1) return NULL;
    int group = pcba_is_group(argv[0]);
    *consumed = group ? 1 : 0;
    if (group && argc < 2) return NULL;
    for (size_t i = 0; i < count; ++i) {
        if (group ? (cmds[i].group && !strcmp(cmds[i].group, argv[0]) && !strcmp(cmds[i].name, argv[1]))
                  : (!cmds[i].group && !strcmp(cmds[i].name, argv[0]))) {
            *consumed = group ? 2 : 1;
            return &cmds[i];
        }
    }
    return NULL;
}

void pcba_command_path(const PcbaCommand *cmd, char *buf, size_t cap) {
    snprintf(buf, cap, "%s%s%s", cmd->group ? cmd->group : "", cmd->group ? " " : "", cmd->name);
}

void pcba_command_usage(const PcbaCommand *cmd, char *buf, size_t cap) {
    char path[64];
    int optional = 0;
    pcba_command_path(cmd, path, sizeof path);
    int n = snprintf(buf, cap, "pcba %s%s%s", path, *cmd->positional ? " " : "", cmd->positional);
    for (size_t i = 0; i < cmd->option_count && n > 0 && (size_t)n < cap; ++i) {
        const PcbaOption *o = &cmd->options[i];
        if (o->required) n += snprintf(buf + n, cap - (size_t)n, " %s %s", o->name, o->metavar);
        else optional = 1;
    }
    if (optional && n > 0 && (size_t)n < cap) snprintf(buf + n, cap - (size_t)n, " [options]");
}

static int option_index(const PcbaCommand *cmd, const char *name, size_t len) {
    for (size_t i = 0; i < cmd->option_count; ++i)
        if (strlen(cmd->options[i].name) == len && !strncmp(cmd->options[i].name, name, len)) return (int)i;
    return -1;
}

int pcba_parse_args(const PcbaCommand *cmd, int argc, char **argv, PcbaArgs *out, char *err, size_t cap) {
    memset(out, 0, sizeof *out);
    int options_done = 0;
    for (int i = 0; i < argc; ++i) {
        const char *a = argv[i];
        if (!options_done && !strcmp(a, "--")) { options_done = 1; continue; }
        if (!options_done && a[0] == '-' && a[1] != '\0') {
            const char *eq = strchr(a, '=');
            size_t len = eq ? (size_t)(eq - a) : strlen(a);
            int k = a[1] == '-' ? option_index(cmd, a, len) : -1;
            if (k < 0) { snprintf(err, cap, "unknown option '%.*s'", (int)len, a); return 0; }
            const PcbaOption *o = &cmd->options[k];
            if (out->values[k]) { snprintf(err, cap, "option %s given more than once", o->name); return 0; }
            if (!o->metavar) {
                if (eq) { snprintf(err, cap, "option %s takes no value", o->name); return 0; }
                out->values[k] = "";
            } else if (eq) {
                out->values[k] = eq + 1;
            } else if (i + 1 < argc) {
                out->values[k] = argv[++i];
            } else {
                snprintf(err, cap, "option %s requires a value (%s)", o->name, o->metavar);
                return 0;
            }
            if (o->metavar && !*out->values[k]) { snprintf(err, cap, "option %s requires a non-empty value", o->name); return 0; }
            continue;
        }
        if (out->positional_count == cmd->max_positional) { snprintf(err, cap, "unexpected argument '%s'", a); return 0; }
        out->positional[out->positional_count++] = a;
    }
    if (out->positional_count < cmd->min_positional) {
        snprintf(err, cap, "missing required argument %s", cmd->positional);
        return 0;
    }
    for (size_t k = 0; k < cmd->option_count; ++k)
        if (cmd->options[k].required && !out->values[k]) {
            snprintf(err, cap, "missing required option %s %s", cmd->options[k].name, cmd->options[k].metavar);
            return 0;
        }
    return 1;
}

const char *pcba_opt(const PcbaCommand *cmd, const PcbaArgs *args, const char *name) {
    int k = option_index(cmd, name, strlen(name));
    return k < 0 ? NULL : args->values[k];
}

int pcba_parse_double(const char *text, double *out) {
    char *end;
    errno = 0;
    double v = strtod(text, &end);
    if (end == text || *end || errno || !isfinite(v)) return 0;
    *out = v;
    return 1;
}

int pcba_parse_count(const char *text, long min, long max, long *out) {
    char *end;
    errno = 0;
    long v = strtol(text, &end, 10);
    if (end == text || *end || errno || v < min || v > max) return 0;
    *out = v;
    return 1;
}

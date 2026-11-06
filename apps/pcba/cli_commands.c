#include "cli.h"

#include <string.h>

/* Backend argument list (NULL-terminated) built from validated pcba arguments. */
typedef struct {
    const char *v[48];
    size_t n;
} Bargs;

static void push(Bargs *b, const char *s) {
    if (b->n + 1 < sizeof b->v / sizeof b->v[0]) b->v[b->n++] = s;
    b->v[b->n] = NULL;
}

/* Copy each present pcba option to the backend under its backend name. */
static void pass(Bargs *b, const PcbaCommand *cmd, const PcbaArgs *a, const char *const map[][2], size_t count) {
    for (size_t i = 0; i < count; ++i) {
        const char *value = pcba_opt(cmd, a, map[i][0]);
        if (!value) continue;
        push(b, map[i][1]);
        if (*value) push(b, value);
    }
}

static int finish_named(PcbaContext *ctx, const PcbaCommand *cmd, const char *status, const char *message) {
    char path[64];
    pcba_command_path(cmd, path, sizeof path);
    return pcba_finish(ctx, path, status, message, NULL);
}

static int missing_file(PcbaContext *ctx, const PcbaCommand *cmd, const char *path, int dir_ok) {
    char message[1200];
    if (cli_is_file(path) || (dir_ok && cli_is_dir(path))) return -1;
    snprintf(message, sizeof message, "%s not found: %s", dir_ok ? "file or directory" : "file", path);
    return finish_named(ctx, cmd, "INVALID_INPUT", message);
}

static int backend(PcbaContext *ctx, const PcbaCommand *cmd, Bargs *b) {
    char path[64];
    pcba_command_path(cmd, path, sizeof path);
    return pcba_run_backend(ctx, path, b->v);
}

enum { ANY_NUMBER, NON_NEGATIVE, POSITIVE };

static int check_numbers(PcbaContext *ctx, const PcbaCommand *cmd, const PcbaArgs *a, const char *const *names,
                         int sign) {
    static const char *const words[] = {"", " non-negative", " positive"};
    for (size_t i = 0; names[i]; ++i) {
        const char *v = pcba_opt(cmd, a, names[i]);
        double d;
        if (v && (!pcba_parse_double(v, &d) || (sign == NON_NEGATIVE && d < 0) || (sign == POSITIVE && d <= 0)))
            return pcba_usage_error(ctx, cmd, "%s needs a finite%s number, got '%s'", names[i], words[sign], v);
    }
    return -1;
}

static int run_help(PcbaContext *ctx, const PcbaCommand *cmd, const PcbaArgs *a) {
    (void)cmd;
    return pcba_help(ctx, (int)a->positional_count, (char **)a->positional);
}

static int run_version(PcbaContext *ctx, const PcbaCommand *cmd, const PcbaArgs *a) {
    (void)cmd;
    (void)a;
    return pcba_version(ctx);
}

static int run_new(PcbaContext *ctx, const PcbaCommand *cmd, const PcbaArgs *a) {
    static const char *const numbers[] = {"--vin", "--r", "--r2", "--c", "--l", "--target", "--target-tolerance", NULL};
    static const char *const map[][2] = {{"--out", "--out"}, {"--vin", "--vin"}, {"--r", "--r"}, {"--r2", "--r2"},
                                         {"--c", "--c"}, {"--l", "--l"}, {"--target", "--target"},
                                         {"--target-tolerance", "--target-tolerance"}, {"--catalogue", "--catalogue"},
                                         {"--rules", "--rules"}, {"--force", "--force"}};
    const char *t = a->positional[0];
    if (strcmp(t, "divider") && strcmp(t, "rc") && strcmp(t, "rl") && strcmp(t, "diode"))
        return pcba_usage_error(ctx, cmd, "unknown template '%s' (choose divider, rc, rl or diode)", t);
    int rc = check_numbers(ctx, cmd, a, numbers, ANY_NUMBER);
    if (rc >= 0) return rc;
    const char *files[] = {pcba_opt(cmd, a, "--catalogue"), pcba_opt(cmd, a, "--rules")};
    for (size_t i = 0; i < 2; ++i)
        if (files[i] && (rc = missing_file(ctx, cmd, files[i], 0)) >= 0) return rc;
    Bargs b = {{0}, 0};
    push(&b, "new");
    push(&b, t);
    pass(&b, cmd, a, map, sizeof map / sizeof map[0]);
    return backend(ctx, cmd, &b);
}

static int run_check(PcbaContext *ctx, const PcbaCommand *cmd, const PcbaArgs *a) {
    int rc = missing_file(ctx, cmd, a->positional[0], 0);
    if (rc >= 0) return rc;
    Bargs b = {{0}, 0};
    push(&b, "check");
    if (!strcmp(cmd->name, "dc")) { push(&b, "--scope"); push(&b, "dc"); }
    push(&b, a->positional[0]);
    return backend(ctx, cmd, &b);
}

static int run_build(PcbaContext *ctx, const PcbaCommand *cmd, const PcbaArgs *a) {
    static const char *const map[][2] = {{"--out", "--out"}, {"--symbols", "--symbol-dir"},
                                         {"--footprints", "--footprint-dir"}, {"--no-route", "--no-route"},
                                         {"--draft", "--draft"}};
    int rc = missing_file(ctx, cmd, a->positional[0], 0);
    if (rc >= 0) return rc;
    Bargs b = {{0}, 0};
    push(&b, "build");
    push(&b, a->positional[0]);
    pass(&b, cmd, a, map, sizeof map / sizeof map[0]);
    return backend(ctx, cmd, &b);
}

static int run_verify(PcbaContext *ctx, const PcbaCommand *cmd, const PcbaArgs *a) {
    static const char *const map[][2] = {{"--out", "--out"}, {"--project", "--project"}, {"--kicad-cli", "--kicad-cli"}};
    const char *out = pcba_opt(cmd, a, "--out");
    char message[1200];
    if (!cli_is_dir(out)) {
        snprintf(message, sizeof message, "build directory not found: %s (run 'pcba project build' first)", out);
        return finish_named(ctx, cmd, "INVALID_INPUT", message);
    }
    Bargs b = {{0}, 0};
    push(&b, cmd->name); /* verify | fabricate */
    pass(&b, cmd, a, map, sizeof map / sizeof map[0]);
    return backend(ctx, cmd, &b);
}

static int run_transient(PcbaContext *ctx, const PcbaCommand *cmd, const PcbaArgs *a) {
    double dt;
    long steps;
    const char *dt_text = pcba_opt(cmd, a, "--dt"), *steps_text = pcba_opt(cmd, a, "--steps");
    if (!pcba_parse_double(dt_text, &dt) || dt <= 0 || dt > 1000)
        return pcba_usage_error(ctx, cmd, "--dt must be a time step in seconds, 0 < dt <= 1000 (got '%s')", dt_text);
    if (!pcba_parse_count(steps_text, 1, 100000, &steps))
        return pcba_usage_error(ctx, cmd, "--steps must be an integer from 1 to 100000 (got '%s')", steps_text);
    const char *method = pcba_opt(cmd, a, "--method");
    if (method && strcmp(method, "be") && strcmp(method, "trap"))
        return pcba_usage_error(ctx, cmd, "--method must be be or trap (got '%s')", method);
    int rc = missing_file(ctx, cmd, a->positional[0], 0);
    if (rc >= 0) return rc;
    Bargs b = {{0}, 0};
    push(&b, "transient");
    push(&b, a->positional[0]);
    push(&b, "--dt");
    push(&b, dt_text);
    push(&b, "--steps");
    push(&b, steps_text);
    push(&b, "--method");
    push(&b, method ? method : "be");
    if (pcba_opt(cmd, a, "--out")) { push(&b, "--out"); push(&b, pcba_opt(cmd, a, "--out")); }
    return backend(ctx, cmd, &b);
}

static int run_ac(PcbaContext *ctx, const PcbaCommand *cmd, const PcbaArgs *a) {
    double start, stop;
    long points;
    const char *start_text = pcba_opt(cmd, a, "--start"), *stop_text = pcba_opt(cmd, a, "--stop");
    const char *points_text = pcba_opt(cmd, a, "--points"), *sweep = pcba_opt(cmd, a, "--sweep");
    if (!sweep) sweep = "log";
    if (strcmp(sweep, "log") && strcmp(sweep, "lin"))
        return pcba_usage_error(ctx, cmd, "--sweep must be log or lin (got '%s')", sweep);
    if (!pcba_parse_double(start_text, &start) || !pcba_parse_double(stop_text, &stop) || start < 0 || stop < start)
        return pcba_usage_error(ctx, cmd, "--start and --stop must be finite frequencies with 0 <= start <= stop");
    if (!strcmp(sweep, "log") && start <= 0)
        return pcba_usage_error(ctx, cmd, "a log sweep needs --start > 0 (use --sweep lin to include 0 Hz)");
    if (!pcba_parse_count(points_text, 1, 100000, &points))
        return pcba_usage_error(ctx, cmd, "--points must be an integer from 1 to 100000 (got '%s')", points_text);
    if ((points == 1) != (start == stop))
        return pcba_usage_error(ctx, cmd, "use --points 1 exactly when --start equals --stop");
    int rc = missing_file(ctx, cmd, a->positional[0], 0);
    if (rc >= 0) return rc;
    Bargs b = {{0}, 0};
    push(&b, "ac");
    push(&b, a->positional[0]);
    push(&b, "--start");
    push(&b, start_text);
    push(&b, "--stop");
    push(&b, stop_text);
    push(&b, "--points");
    push(&b, points_text);
    push(&b, "--sweep");
    push(&b, sweep);
    if (pcba_opt(cmd, a, "--out")) { push(&b, "--out"); push(&b, pcba_opt(cmd, a, "--out")); }
    return backend(ctx, cmd, &b);
}

/* spec parse | check | lower: EngineeringSpec frontend and compiler handoff (docs/engineering/ENGINEERING_SPEC.md). */
static int run_spec(PcbaContext *ctx, const PcbaCommand *cmd, const PcbaArgs *a) {
    static const char *const parse_map[][2] = {{"--file", "--file"}, {"--out", "--out"}, {"--force", "--force"}};
    static const char *const lower_map[][2] = {{"--out", "--out"}, {"--catalogue", "--catalogue"}, {"--force", "--force"}};
    int rc;
    Bargs b = {{0}, 0};
    if (!strcmp(cmd->name, "parse")) {
        const char *file = pcba_opt(cmd, a, "--file");
        if ((a->positional_count == 1) == (file != NULL))
            return pcba_usage_error(ctx, cmd, "give the description as one quoted argument or with --file, not both");
        if (file && (rc = missing_file(ctx, cmd, file, 0)) >= 0) return rc;
        push(&b, "spec-parse");
        if (a->positional_count) push(&b, a->positional[0]);
        pass(&b, cmd, a, parse_map, sizeof parse_map / sizeof parse_map[0]);
        return backend(ctx, cmd, &b);
    }
    if ((rc = missing_file(ctx, cmd, a->positional[0], 0)) >= 0) return rc;
    if (!strcmp(cmd->name, "check")) {
        push(&b, "spec-check");
        push(&b, a->positional[0]);
        return backend(ctx, cmd, &b);
    }
    const char *catalogue = pcba_opt(cmd, a, "--catalogue");
    if (catalogue && (rc = missing_file(ctx, cmd, catalogue, 0)) >= 0) return rc;
    push(&b, "spec-lower");
    push(&b, a->positional[0]);
    pass(&b, cmd, a, lower_map, sizeof lower_map / sizeof lower_map[0]);
    return backend(ctx, cmd, &b);
}

static int run_parts_search(PcbaContext *ctx, const PcbaCommand *cmd, const PcbaArgs *a) {
    const char *catalogue = pcba_opt(cmd, a, "--catalogue");
    int rc;
    if (catalogue && (rc = missing_file(ctx, cmd, catalogue, 0)) >= 0) return rc;
    Bargs b = {{0}, 0};
    push(&b, "catalogue");
    push(&b, "--search");
    push(&b, a->positional[0]);
    if (catalogue) { push(&b, "--file"); push(&b, catalogue); }
    return backend(ctx, cmd, &b);
}

static int run_parts_add(PcbaContext *ctx, const PcbaCommand *cmd, const PcbaArgs *a) {
    static const char *const ratings[] = {"--power", "--voltage", "--height", NULL};
    static const char *const fractions[] = {"--tolerance", NULL};
    static const char *const map[][2] = {{"--catalogue", "--file"}, {"--id", "--id"}, {"--kind", "--kind"},
                                         {"--symbol", "--symbol"}, {"--footprint", "--footprint"},
                                         {"--power", "--power"}, {"--voltage", "--voltage"},
                                         {"--tolerance", "--tolerance"}, {"--height", "--height"},
                                         {"--manufacturer", "--manufacturer"}, {"--mpn", "--mpn"},
                                         {"--datasheet", "--datasheet"}, {"--model", "--model"},
                                         {"--pinmap", "--pinmap"}, {"--provenance", "--provenance"}};
    const char *kind = pcba_opt(cmd, a, "--kind");
    if (strlen(kind) != 1 || !strchr("RCLDVIJUQM", kind[0]))
        return pcba_usage_error(ctx, cmd, "--kind must be one of R C L D V I J U Q M (got '%s')", kind);
    int rc = check_numbers(ctx, cmd, a, ratings, POSITIVE);
    if (rc < 0) rc = check_numbers(ctx, cmd, a, fractions, NON_NEGATIVE);
    if (rc >= 0) return rc;
    Bargs b = {{0}, 0};
    push(&b, "part-add");
    pass(&b, cmd, a, map, sizeof map / sizeof map[0]);
    return backend(ctx, cmd, &b);
}

static int run_parts_remove(PcbaContext *ctx, const PcbaCommand *cmd, const PcbaArgs *a) {
    static const char *const map[][2] = {{"--catalogue", "--file"}, {"--id", "--id"}};
    int rc = missing_file(ctx, cmd, pcba_opt(cmd, a, "--catalogue"), 0);
    if (rc >= 0) return rc;
    Bargs b = {{0}, 0};
    push(&b, "part-remove");
    pass(&b, cmd, a, map, sizeof map / sizeof map[0]);
    return backend(ctx, cmd, &b);
}

static int run_parts_import(PcbaContext *ctx, const PcbaCommand *cmd, const PcbaArgs *a) {
    static const char *const map[][2] = {{"--into", "--into"}, {"--symbols", "--symbol-dir"}, {"--footprints", "--footprint-dir"}};
    int rc = missing_file(ctx, cmd, a->positional[0], 0);
    if (rc >= 0) return rc;
    Bargs b = {{0}, 0};
    push(&b, "part-import");
    push(&b, a->positional[0]);
    pass(&b, cmd, a, map, sizeof map / sizeof map[0]);
    return backend(ctx, cmd, &b);
}

/* design: request text -> verified KiCad project (tools/design.py). */
static int run_design(PcbaContext *ctx, const PcbaCommand *cmd, const PcbaArgs *a) {
    static const char *const map[][2] = {{"--out", "--out"}, {"--profile", "--profile"}, {"--answers", "--answers"},
                                         {"--symbols", "--symbol-dir"}, {"--footprints", "--footprint-dir"},
                                         {"--kicad-cli", "--kicad-cli"}, {"--force", "--force"}};
    const char *profile = pcba_opt(cmd, a, "--profile");
    int rc;
    if (!*a->positional[0]) return pcba_usage_error(ctx, cmd, "the request text is empty");
    if (profile && (rc = missing_file(ctx, cmd, profile, 0)) >= 0) return rc;
    Bargs b = {{0}, 0};
    push(&b, "design");
    push(&b, a->positional[0]);
    pass(&b, cmd, a, map, sizeof map / sizeof map[0]);
    return backend(ctx, cmd, &b);
}

/* place | route: tools/layout.py through copper.py; copper-core judges every candidate. */
static int run_layout(PcbaContext *ctx, const PcbaCommand *cmd, const PcbaArgs *a) {
    static const char *const place_map[][2] = {{"--out", "--out"}, {"--seeds", "--seeds"}, {"--iterations", "--iterations"},
                                               {"--report", "--report"}, {"--footprints", "--footprint-dir"}, {"--force", "--force"}};
    static const char *const route_map[][2] = {{"--out", "--out"}, {"--report", "--report"}, {"--footprints", "--footprint-dir"}};
    int place = !strcmp(cmd->name, "place"), rc = missing_file(ctx, cmd, a->positional[0], 0);
    if (rc >= 0) return rc;
    if (place) {
        const char *seeds = pcba_opt(cmd, a, "--seeds"), *iterations = pcba_opt(cmd, a, "--iterations");
        long n;
        if (iterations && !pcba_parse_count(iterations, 0, 100000, &n))
            return pcba_usage_error(ctx, cmd, "--iterations must be an integer from 0 to 100000 (got '%s')", iterations);
        for (const char *s = seeds; s && *s; ++s)
            if (!strchr("0123456789,", *s) || (*s == ',' && (s == seeds || s[1] == ',' || !s[1])))
                return pcba_usage_error(ctx, cmd, "--seeds must be comma-separated integers (got '%s')", seeds);
        if (seeds && !*seeds) return pcba_usage_error(ctx, cmd, "--seeds must not be empty");
    }
    Bargs b = {{0}, 0};
    push(&b, cmd->name);
    push(&b, a->positional[0]);
    if (place) pass(&b, cmd, a, place_map, sizeof place_map / sizeof place_map[0]);
    else pass(&b, cmd, a, route_map, sizeof route_map / sizeof route_map[0]);
    return backend(ctx, cmd, &b);
}

/* library env | scan | search | pair: tools/kicad_library.py through copper.py. */
static int run_library(PcbaContext *ctx, const PcbaCommand *cmd, const PcbaArgs *a) {
    static const char *const scan_map[][2] = {{"--symbols", "--symbol-dir"}, {"--footprints", "--footprint-dir"},
                                              {"--out", "--out"}, {"--project", "--project"}, {"--jobs", "--jobs"}};
    static const char *const env_map[][2] = {{"--project", "--project"}, {"--out", "--out"}};
    static const char *const search_map[][2] = {{"--index", "--index"}, {"--kind", "--kind"}, {"--limit", "--limit"}};
    static const char *const pair_map[][2] = {{"--index", "--index"}, {"--limit", "--limit"}};
    const char *jobs = pcba_opt(cmd, a, "--jobs"), *limit = pcba_opt(cmd, a, "--limit"), *kind = pcba_opt(cmd, a, "--kind");
    const char *project = pcba_opt(cmd, a, "--project");
    char backend_name[32];
    long n;
    if (jobs && !pcba_parse_count(jobs, 1, 256, &n))
        return pcba_usage_error(ctx, cmd, "--jobs must be an integer from 1 to 256 (got '%s')", jobs);
    if (limit && !pcba_parse_count(limit, 1, 10000, &n))
        return pcba_usage_error(ctx, cmd, "--limit must be an integer from 1 to 10000 (got '%s')", limit);
    if (kind && strcmp(kind, "symbol") && strcmp(kind, "footprint"))
        return pcba_usage_error(ctx, cmd, "--kind must be symbol or footprint (got '%s')", kind);
    if (project && !cli_is_dir(project)) return finish_named(ctx, cmd, "INVALID_INPUT", "--project must be a project folder");
    snprintf(backend_name, sizeof backend_name, "library-%s", cmd->name);
    Bargs b = {{0}, 0};
    push(&b, backend_name);
    if (cmd->max_positional) {
        if (!*a->positional[0]) return pcba_usage_error(ctx, cmd, "the argument is empty");
        push(&b, a->positional[0]);
    }
    if (!strcmp(cmd->name, "scan")) pass(&b, cmd, a, scan_map, sizeof scan_map / sizeof scan_map[0]);
    else if (!strcmp(cmd->name, "env")) pass(&b, cmd, a, env_map, sizeof env_map / sizeof env_map[0]);
    else if (!strcmp(cmd->name, "search")) pass(&b, cmd, a, search_map, sizeof search_map / sizeof search_map[0]);
    else pass(&b, cmd, a, pair_map, sizeof pair_map / sizeof pair_map[0]);
    return backend(ctx, cmd, &b);
}

/* blocks: the circuit blocks a spec can use (tools/blocks.py). */
static int run_blocks(PcbaContext *ctx, const PcbaCommand *cmd, const PcbaArgs *a) {
    (void)a;
    Bargs b = {{0}, 0};
    push(&b, "blocks");
    return backend(ctx, cmd, &b);
}

/* rules import: a KiCad project's design rules -> a (rules ...) profile (tools/kicad_rules.py). */
static int run_rules_import(PcbaContext *ctx, const PcbaCommand *cmd, const PcbaArgs *a) {
    static const char *const map[][2] = {{"--out", "--out"}, {"--base", "--base"}, {"--date", "--date"},
                                         {"--report", "--report"}, {"--force", "--force"}};
    const char *date = pcba_opt(cmd, a, "--date"), *base = pcba_opt(cmd, a, "--base");
    int rc;
    if (date) {
        int ok = strlen(date) == 10 && date[4] == '-' && date[7] == '-';
        for (int i = 0; ok && i < 10; ++i) ok = i == 4 || i == 7 || (date[i] >= '0' && date[i] <= '9');
        if (!ok) return pcba_usage_error(ctx, cmd, "--date must be YYYY-MM-DD (got '%s')", date);
    }
    if ((rc = missing_file(ctx, cmd, a->positional[0], 1)) >= 0) return rc;
    if (base && (rc = missing_file(ctx, cmd, base, 0)) >= 0) return rc;
    Bargs b = {{0}, 0};
    push(&b, "rules-import");
    push(&b, a->positional[0]);
    pass(&b, cmd, a, map, sizeof map / sizeof map[0]);
    return backend(ctx, cmd, &b);
}

static int run_dfm(PcbaContext *ctx, const PcbaCommand *cmd, const PcbaArgs *a) {
    int rc = missing_file(ctx, cmd, a->positional[0], 1);
    if (rc >= 0) return rc;
    Bargs b = {{0}, 0};
    push(&b, "dfm");
    push(&b, a->positional[0]);
    return backend(ctx, cmd, &b);
}

/* ---- option tables ---- */
static const PcbaOption NO_OPTIONS[1] = {{"", NULL, 0, ""}};
static const PcbaOption DOCTOR_OPTIONS[] = {
    {"--out", "DIR", 0, "Output folder to test for write access (default: out)"},
    {"--kicad-cli", "PATH", 0, "kicad-cli executable (default: KICAD_CLI, PATH, installed KiCad 9)"},
    {"--symbols", "DIR", 0, "KiCad symbol library directory (default: KICAD_SYMBOL_DIR or installed KiCad)"},
    {"--footprints", "DIR", 0, "KiCad footprint library directory (default: KICAD_FOOTPRINT_DIR or installed KiCad)"},
};
static const PcbaOption NEW_OPTIONS[] = {
    {"--out", "FILE", 1, "Project file to create"},
    {"--vin", "V", 0, "Source voltage (default 5)"},
    {"--r", "OHM", 0, "First resistor (default 10000)"},
    {"--r2", "OHM", 0, "Second resistor, divider only (default 20000)"},
    {"--c", "F", 0, "Capacitor, rc only (default 1e-6)"},
    {"--l", "H", 0, "Inductor, rl only (default 0.01)"},
    {"--target", "V", 0, "Required OUT voltage; adds a voltage target"},
    {"--target-tolerance", "V", 0, "Allowed deviation from --target (default 0.05)"},
    {"--catalogue", "FILE", 0, "Part catalogue to embed (default profiles/default.parts)"},
    {"--rules", "FILE", 0, "Process rule profile to embed, e.g. profiles/conservative.rules"},
    {"--force", NULL, 0, "Replace an existing project file"},
};
static const PcbaOption BUILD_OPTIONS[] = {
    {"--out", "DIR", 1, "Output directory for the KiCad project and reports"},
    {"--symbols", "DIR", 0, "KiCad symbol library directory (default: KICAD_SYMBOL_DIR or installed KiCad 9)"},
    {"--footprints", "DIR", 0, "KiCad footprint library directory (default: KICAD_FOOTPRINT_DIR or installed KiCad 9)"},
    {"--no-route", NULL, 0, "Export placement only; unrouted connections are reported as failures"},
    {"--draft", NULL, 0, "Allow an electrically failing design to export as a labelled failed draft"},
};
static const PcbaOption VERIFY_OPTIONS[] = {
    {"--out", "DIR", 1, "Build directory produced by 'pcba project build'"},
    {"--project", "FILE", 0, "Source .copper to check for freshness (default: path recorded at build)"},
    {"--kicad-cli", "PATH", 0, "kicad-cli executable; KiCad 9.x only"},
};
static const PcbaOption AC_OPTIONS[] = {
    {"--start", "HZ", 1, "First frequency (> 0 for a log sweep)"},
    {"--stop", "HZ", 1, "Last frequency"},
    {"--points", "N", 1, "Number of frequencies, 1..100000 (1 only when start == stop)"},
    {"--sweep", "log|lin", 0, "Frequency spacing (default log)"},
    {"--out", "CSV", 0, "Write every node voltage and branch current phasor (re, im, magnitude, phase)"},
};
static const PcbaOption TRANSIENT_OPTIONS[] = {
    {"--dt", "S", 1, "Fixed time step in seconds, 0 < dt <= 1000"},
    {"--steps", "N", 1, "Number of steps, 1..100000"},
    {"--out", "CSV", 0, "Write the waveform here (default: CSV on stdout)"},
    {"--method", "be|trap", 0, "Backward Euler (default) or trapezoidal; trapezoidal takes its first step with Backward Euler"},
};
static const PcbaOption SEARCH_OPTIONS[] = {
    {"--catalogue", "FILE", 0, "Catalogue to search (default profiles/default.parts)"},
};
static const PcbaOption ADD_OPTIONS[] = {
    {"--catalogue", "FILE", 1, "Catalogue file to create or extend"},
    {"--id", "ID", 1, "Part identifier (letters, digits, _ . -)"},
    {"--kind", "K", 1, "Model kind: R C L D V I J U, Q (BJT) or M (MOSFET); Q/M need --model"},
    {"--symbol", "LIB:NAME", 0, "KiCad symbol, e.g. Device:R"},
    {"--footprint", "LIB:NAME", 0, "KiCad footprint, e.g. Resistor_SMD:R_0603_1608Metric"},
    {"--power", "W", 0, "Power rating from the datasheet; omit when unknown"},
    {"--voltage", "V", 0, "Voltage rating from the datasheet; omit when unknown"},
    {"--tolerance", "FRACTION", 0, "Value tolerance, e.g. 0.01"},
    {"--height", "MM", 0, "Assembled height from the datasheet; omit when unknown"},
    {"--manufacturer", "TEXT", 0, "Manufacturer (procurement metadata only)"},
    {"--mpn", "TEXT", 0, "Manufacturer part number (procurement metadata only)"},
    {"--datasheet", "URL", 0, "Datasheet reference"},
    {"--model", "ID", 0, "Device model id (copper-core models); default: the kind's model"},
    {"--pinmap", "MAP", 0, "Terminal to symbol pin map, e.g. cathode=1,anode=2"},
    {"--provenance", "TEXT", 0, "Where the ratings come from, e.g. a datasheet revision"},
};
static const PcbaOption REMOVE_OPTIONS[] = {
    {"--catalogue", "FILE", 1, "Catalogue file to edit (the bundled default is read-only)"},
    {"--id", "ID", 1, "Part identifier to remove"},
};
static const PcbaOption DESIGN_OPTIONS[] = {
    {"--out", "DIR", 1, "Output directory: specs, .copper stages, build/ (KiCad project) and design.json"},
    {"--profile", "FILE", 0, "Design profile with documented project defaults (e.g. profiles/esp32_led.design)"},
    {"--answers", "K=V,...", 0, "Answers to clarification questions, e.g. mcu_part=esp32_wroom_32e,led_current=0.005"},
    {"--symbols", "DIR", 0, "KiCad 9.x symbol library directory"},
    {"--footprints", "DIR", 0, "KiCad 9.x footprint library directory"},
    {"--kicad-cli", "PATH", 0, "kicad-cli for ERC/DRC/netlist gates; KiCad 9.x only"},
    {"--force", NULL, 0, "Reuse a non-empty output directory"},
};
static const PcbaOption PLACE_OPTIONS[] = {
    {"--out", "FILE", 1, "The placed .copper project to write (only when a feasible board is found)"},
    {"--seeds", "LIST", 0, "Comma-separated annealing seeds (default 1,2,3)"},
    {"--iterations", "N", 0, "Annealing iterations per seed, 0..100000 (default 1500)"},
    {"--report", "FILE", 0, "Write every candidate's hard and soft terms as JSON"},
    {"--footprints", "DIR", 0, "KiCad footprint library directory (default: KICAD_FOOTPRINT_DIR or installed KiCad)"},
    {"--force", NULL, 0, "Replace an existing --out file"},
};
static const PcbaOption ROUTE_OPTIONS[] = {
    {"--out", "DIR", 1, "Directory for layout.json and, when feasible, routed.copper"},
    {"--report", "FILE", 0, "Write the layout report here instead of DIR/layout.json"},
    {"--footprints", "DIR", 0, "KiCad footprint library directory (default: KICAD_FOOTPRINT_DIR or installed KiCad)"},
};
static const PcbaOption IMPORT_OPTIONS[] = {
    {"--into", "FILE", 0, "User catalogue to merge into (created if missing; the bundled default is read-only)"},
    {"--symbols", "DIR", 0, "KiCad 9.x symbol library directory (default: KICAD_SYMBOL_DIR or installed KiCad)"},
    {"--footprints", "DIR", 0, "KiCad 9.x footprint library directory (default: KICAD_FOOTPRINT_DIR or installed KiCad)"},
};
static const PcbaOption SCAN_OPTIONS[] = {
    {"--symbols", "DIR", 0, "Index this symbol folder instead of the library tables (default: KICAD_SYMBOL_DIR)"},
    {"--footprints", "DIR", 0, "Index this footprint folder instead of the library tables (default: KICAD_FOOTPRINT_DIR)"},
    {"--project", "DIR", 0, "Also read this KiCad project's sym-lib-table and fp-lib-table (they override global names)"},
    {"--out", "DIR", 0, "Where to write index.sqlite and environment.json (default out/library)"},
    {"--jobs", "N", 0, "Parallel parser processes, 1..256 (default: CPU count)"},
};
static const PcbaOption RULES_IMPORT_OPTIONS[] = {
    {"--out", "FILE", 1, "The .rules profile to write"},
    {"--base", "FILE", 0, "Profile for limits KiCad does not set (current, deratings, height), e.g. profiles/conservative.rules"},
    {"--date", "YYYY-MM-DD", 0, "Date recorded with every limit (default: today)"},
    {"--report", "FILE", 0, "Write the import report (every value considered, what was not imported) as JSON"},
    {"--force", NULL, 0, "Replace an existing --out file"},
};
static const PcbaOption ENV_OPTIONS[] = {
    {"--project", "DIR", 0, "KiCad project folder: its library tables and KIPRJMOD"},
    {"--out", "FILE", 0, "Write the JSON snapshot here"},
};
static const PcbaOption LIB_SEARCH_OPTIONS[] = {
    {"--index", "FILE", 0, "Library index (default out/library/index.sqlite)"},
    {"--kind", "symbol|footprint", 0, "What to search (default symbol)"},
    {"--limit", "N", 0, "Most results, 1..10000 (default 20)"},
};
static const PcbaOption PAIR_OPTIONS[] = {
    {"--index", "FILE", 0, "Library index (default out/library/index.sqlite)"},
    {"--limit", "N", 0, "Most candidates listed, 1..10000 (default 50)"},
};
static const PcbaOption SPEC_PARSE_OPTIONS[] = {
    {"--file", "FILE", 0, "Read the description from a UTF-8 text file instead of the argument"},
    {"--out", "FILE", 0, "Write the EngineeringSpec (.espec); open questions are recorded in it"},
    {"--force", NULL, 0, "Replace an existing --out file"},
};
static const PcbaOption SPEC_LOWER_OPTIONS[] = {
    {"--out", "FILE", 1, "The .copper project to write"},
    {"--catalogue", "FILE", 0, "Part catalogue overlay (default: the spec's catalogue profile, then the bundled one)"},
    {"--force", NULL, 0, "Replace an existing --out file"},
};
static const PcbaOption TEST_OPTIONS[] = {
    {"--quick", NULL, 0, "C unit tests only (default)"},
    {"--full", NULL, 0, "Every registered test, including Python, KiCad-library and CLI suites"},
};

#define OPTS(x) x, sizeof x / sizeof x[0]
#define NONE NO_OPTIONS, 0

static const PcbaCommand COMMANDS[] = {
    {NULL, "help", "[command]", 0, 2, "Show help for pcba or one command",
     "Lists every command, or shows the arguments, examples and exit codes of one command.",
     "pcba help\npcba help project build", NONE, NULL, run_help},
    {NULL, "version", "", 0, 0, "Show version and build information",
     "Prints the pcba version, compiler, C standard, build type and source root.", "pcba version\npcba version --json",
     NONE, NULL, run_version},
    {NULL, "doctor", "", 0, 0, "Check Python, copper-core, KiCad 9.x, libraries and the output folder",
     "Diagnoses everything the pipeline needs and says how to fix what is missing. Exit 0 only when native KiCad 9.x "
     "validation is possible; 3 (UNVERIFIED) when KiCad 9.x or its libraries are missing; 1 when a core requirement "
     "is broken. Desktop checks are informational.",
     "pcba doctor\npcba doctor --kicad-cli \"C:\\Program Files\\KiCad\\9.0\\bin\\kicad-cli.exe\"", OPTS(DOCTOR_OPTIONS),
     NULL, pcba_cmd_doctor},
    {NULL, "test", "", 0, 0, "Run the registered test suite from this build tree",
     "Runs CTest for the build tree that contains this pcba. Skipped suites (for example KiCad 9.x not installed) "
     "make the result UNVERIFIED.",
     "pcba test\npcba test --full", OPTS(TEST_OPTIONS), NULL, pcba_cmd_test},
    {"project", "new", "<template>", 1, 1, "Create a .copper project from a template (divider, rc, rl, diode)",
     "Writes an editable project with explicit values, pins, nets and placements. Ratings come from the selected "
     "catalogue; nothing is invented.",
     "pcba project new divider --out my_board.copper --vin 5 --r 10000 --r2 20000\n"
     "pcba project new rc --out filter.copper --c 1e-6",
     OPTS(NEW_OPTIONS), NULL, run_new},
    {"project", "check", "<file.copper>", 1, 1, "Validate a project and run DC analysis plus available checks",
     "Parses and validates the project, solves the DC operating point, and runs every check the project's data "
     "supports. Reports electrical and physical/DFM status separately. A source project without board geometry is "
     "UNVERIFIED for physical DFM; unknown ratings are UNVERIFIED, never PASS.",
     "pcba project check examples/v2/divider.copper\npcba project check examples/v2/divider.copper --json",
     NONE, NULL, run_check},
    {"project", "build", "<file.copper>", 1, 1, "Build a KiCad 9 project, BOM and reports from a .copper project",
     "Resolves real KiCad symbols and footprints, places and routes a deterministic candidate, rechecks it with the "
     "exact copper kernel and writes .kicad_sch/.kicad_pcb/.kicad_pro, BOM, reports and a hashed manifest. Status "
     "covers internal checks only; native KiCad verification is a separate step.",
     "pcba project build examples/v2/divider.copper --out out/divider", OPTS(BUILD_OPTIONS), NULL, run_build},
    {"project", "verify", "", 0, 0, "Re-check a build: hashes, source freshness, re-analysis, KiCad 9 ERC/DRC",
     "Re-runs the internal analysis and requires it to reproduce the stored report, checks the manifest hashes and "
     "the source file, then runs KiCad 9.x ERC, DRC, schematic parity and pin/net parity. PASS only when every check "
     "ran and passed; a missing or non-9.x KiCad is UNVERIFIED.",
     "pcba project verify --out out/divider", OPTS(VERIFY_OPTIONS), NULL, run_verify},
    {"project", "fabricate", "", 0, 0, "Export Gerber and drill files after a PASSing verification",
     "Runs verify first and exports CAM files only when it PASSes with no internal findings. Fabrication export is "
     "not fabricator acceptance.",
     "pcba project fabricate --out out/divider", OPTS(VERIFY_OPTIONS), NULL, run_verify},
    {"simulate", "dc", "<file.copper>", 1, 1, "Solve the DC operating point (R, V, I, D, C open, L short)",
     "Status is the DC solve itself: PASS when solved, FAIL for singular/non-converged systems, UNVERIFIED when a "
     "device has no supported model. Rating and DFM findings are listed for information.",
     "pcba simulate dc examples/v2/divider.copper", NONE, NULL, run_check},
    {"simulate", "ac", "<file.copper>", 1, 1, "Small-signal AC sweep (complex MNA: magnitude and phase)",
     "Solves A(jw) x = b(jw) on the project's circuit at each frequency. Independent sources need (ac MAGNITUDE "
     "PHASE_DEG); a DC value is not AC excitation. Nonlinear devices are linearised at the DC operating point (the "
     "diode model has no capacitance). A device without an AC model is UNVERIFIED, never omitted; a singular "
     "frequency is FAIL.",
     "pcba simulate ac examples/v2/rc_ac.copper --start 1 --stop 1e4 --points 41\n"
     "pcba simulate ac examples/v2/rc_ac.copper --start 0 --stop 100 --points 11 --sweep lin --out out/rc_ac.csv",
     OPTS(AC_OPTIONS), NULL, run_ac},
    {"simulate", "transient", "<file.copper>", 1, 1, "Fixed-step transient (Backward Euler or trapezoidal)",
     "Zero initial stored energy with constant sources switched on at t=0+. Every step is transactional: device "
     "state is committed only after the step solves and validates. A failed step reports its time, step, solver "
     "status, iterations, residual and device, and no successful waveform is claimed. Trapezoidal is second order "
     "and energy-preserving for LC; Backward Euler is first order and damps. No adaptive step.",
     "pcba simulate transient examples/v2/rc.copper --dt 1e-4 --steps 100 --out out/rc.csv\n"
     "pcba simulate transient examples/v2/rc.copper --dt 1e-4 --steps 100 --method trap", OPTS(TRANSIENT_OPTIONS),
     NULL, run_transient},
    {NULL, "design", "\"request\"", 1, 1, "Turn a request into a verified KiCad project, explaining each decision",
     "Runs the whole pipeline from text: EngineeringSpec, decisions, complete spec, .copper, DC analysis, placement and "
     "routing, KiCad project, verification. Each decision is reported with its source: the request, --answers, the "
     "design profile, the catalogue or a named design rule. An unsettled decision stops the run with a question "
     "(UNVERIFIED, exit 3); an unsupported design (NOT_IMPLEMENTED) or a contradiction (INVALID_INPUT) stops it too. "
     "Reports 11 quality gates in design.json. The board is design.kicad_pcb, KiCad's native format; no legacy .pcb.",
     "pcba design \"Build me an ESP32 LED PCB. Generate the KiCad PCB.\" --out out/esp32_led --profile profiles/esp32_led.design\n"
     "pcba design \"a board with an ESP32 and a red LED\" --out out/led --answers mcu_part=esp32_wroom_32e,led_function=gpio_indicator",
     OPTS(DESIGN_OPTIONS), NULL, run_design},
    {"spec", "parse", "[\"text\"]", 0, 1, "Turn a constrained-English description into an EngineeringSpec",
     "Deterministic frontend (no model, no network): reads components, values with SI prefixes and units, "
     "connections, GPIO intent, requirements, board size and analyses. Unknown text is INVALID_INPUT; a missing "
     "decision (a value, a terminal, an exact MCU variant, the supply) is a clarification question and the result is "
     "UNVERIFIED. Every value not read verbatim is marked inferred or defaulted.",
     "pcba spec parse \"a 5 V supply on VIN, a 10k resistor from VIN to OUT, a 20k resistor from OUT to GND\" "
     "--out divider.espec\npcba spec parse --file request.txt --json",
     OPTS(SPEC_PARSE_OPTIONS), NULL, run_spec},
    {"spec", "check", "<file.espec>", 1, 1, "Validate an EngineeringSpec and print its fingerprint and questions",
     "Checks a hand-written or generated .espec against schema version 1: roles, units, values, terminals, nets, "
     "requirements and analyses. Reports the order-independent semantic fingerprint, open questions and every "
     "inferred or defaulted value.",
     "pcba spec check divider.espec", NONE, NULL, run_spec},
    {"spec", "lower", "<file.espec>", 1, 1, "Hand a complete EngineeringSpec to the compiler as a .copper project",
     "Only a spec with no errors and no open questions is lowered. Each component binds a catalogue part (its "
     "(part ID), or the bundled generic part for its role); a role with no part is UNVERIFIED. The written project "
     "is loaded by copper-core before it is published. No placement is invented.",
     "pcba spec lower divider.espec --out divider.copper", OPTS(SPEC_LOWER_OPTIONS), NULL, run_spec},
    {"parts", "search", "<query>", 1, 1, "Search a part catalogue",
     "Case-insensitive substring search over catalogue entries (IDs, kinds, symbols, footprints, ratings).",
     "pcba parts search resistor\npcba parts search 0603 --catalogue custom.parts", OPTS(SEARCH_OPTIONS), NULL,
     run_parts_search},
    {"parts", "add", "", 0, 0, "Add one part definition to a catalogue file",
     "Adds a part with real KiCad symbol/footprint identifiers, an optional device model and terminal pin map, "
     "and datasheet ratings with their provenance. Omit a rating you do not know: it stays UNKNOWN and checks "
     "report UNVERIFIED. The whole catalogue is validated by copper-core before the file is written.",
     "pcba parts add --catalogue custom.parts --id my_r --kind R --symbol Device:R "
     "--footprint Resistor_SMD:R_0603_1608Metric --power <W> --voltage <V> --provenance \"<datasheet>\"\n"
     "pcba parts add --catalogue custom.parts --id my_d --kind D --model shockley.diode --pinmap cathode=1,anode=2 "
     "--symbol Device:D --footprint Diode_SMD:D_SOD-123",
     OPTS(ADD_OPTIONS), NULL, run_parts_add},
    {"parts", "remove", "", 0, 0, "Remove one part definition from a catalogue file",
     "Removes a part by id. Projects keep their own embedded snapshot until re-created.",
     "pcba parts remove --catalogue custom.parts --id my_r", OPTS(REMOVE_OPTIONS), NULL, run_parts_remove},
    {"parts", "import", "<catalogue-file>", 1, 1, "Validate a catalogue against the KiCad libraries and import it",
     "Checks every part with copper-core (schema, model, pin map, ratings, MCU boundary), requires a provenance for "
     "any rating or model data, and checks it against the native KiCad 9.x libraries: the symbol and footprint "
     "exist, symbol pin numbers equal footprint pad numbers, and pin maps and MCU pins name real, consistently "
     "named pins. Reports accepted, rejected, duplicate and unverified parts. With --into, the parts are merged "
     "only when every one is accepted. Without usable libraries the result is UNVERIFIED and nothing is written.",
     "pcba parts import profiles/mcu_modules.parts\npcba parts import vendor.parts --into my.parts", OPTS(IMPORT_OPTIONS),
     NULL, run_parts_import},
    {"library", "env", "", 0, 0, "Detect KiCad installs, select one by policy, read the library tables",
     "Finds every KiCad install (kicad-cli version), selects the highest 9.x (never another major version), and reads "
     "the global and project sym-lib-table and fp-lib-table with ${VAR} expansion: the process environment, then "
     "kicad_common.json, then the install's defaults. Each library is listed as ok, disabled, missing, unresolved, "
     "unsupported_type, duplicate or overridden. UNVERIFIED when no KiCad 9.x is installed.",
     "pcba library env\npcba library env --project my_board --out env.json", OPTS(ENV_OPTIONS), NULL, run_library},
    {"library", "scan", "", 0, 0, "Index KiCad symbols and footprints into a searchable SQLite cache",
     "Writes index.sqlite (symbols: description, keywords, pins, footprint filters; footprints: pads, courtyard, "
     "attributes; full-text search) and environment.json. The index is a regenerable cache: only libraries whose "
     "files changed are parsed again. A library that cannot be read is listed and left out (UNVERIFIED).",
     "pcba library scan\npcba library scan --project my_board\n"
     "pcba library scan --symbols /usr/share/kicad/symbols --footprints /usr/share/kicad/footprints",
     OPTS(SCAN_OPTIONS), NULL, run_library},
    {"library", "search", "<query>", 1, 1, "Full-text search of the library index",
     "Every word must match (as a prefix) the name, description or keywords/tags. Ranked by relevance, then id.",
     "pcba library search \"esp32 wroom\"\npcba library search \"0603 resistor\" --kind footprint", OPTS(LIB_SEARCH_OPTIONS),
     NULL, run_library},
    {"library", "pair", "<LIB:SYMBOL>", 1, 1, "Footprints whose pad numbers equal a symbol's pin numbers",
     "Pairs by the exact set of pin and pad numbers (unnumbered mechanical holes are not pads). The symbol's default "
     "footprint and the footprints its filters accept are ranked first; filters never add a footprint whose pads "
     "differ. UNVERIFIED when no indexed footprint has the same pad numbers.",
     "pcba library pair Device:R\npcba library pair Connector_Generic:Conn_01x03 --limit 5", OPTS(PAIR_OPTIONS), NULL,
     run_library},
    {NULL, "place", "<file.copper>", 1, 1, "Search placements; write the best feasible one",
     "Deterministic staged search: the project's own placement (when complete), a constructive placement that "
     "respects the outline, edge clearance, courtyard gap and keepouts, and one seeded simulated-annealing "
     "result per seed. Every candidate is routed (rip-up/reroute, traces widened for explicit current budgets) "
     "and re-checked by copper-core. The best is chosen by hard violations, then unrouted connections, then the "
     "soft score; a soft gain never outweighs a hard violation. Parts marked (fixed) do not move. Exit 1 (FAIL) "
     "when no feasible board was found; nothing is written then. Not a claim of global optimality.",
     "pcba place board.copper --out placed.copper\npcba place board.copper --out placed.copper --seeds 1,2,3,4 "
     "--iterations 3000 --report place.json",
     OPTS(PLACE_OPTIONS), NULL, run_layout},
    {NULL, "route", "<file.copper>", 1, 1, "Route the project's placement and re-check it exactly",
     "Routes every net with the deterministic two-layer A* router and rip-up/reroute, widens traces for "
     "explicit current budgets, and re-checks the board with copper-core. Writes DIR/layout.json (per-net "
     "effort, length, bends, vias, unrouted connections, congestion, DFM findings) and, only when the board is "
     "feasible, DIR/routed.copper. An unroutable board is FAIL with its unrouted connections, never a partial "
     "pass.",
     "pcba route placed.copper --out out/route", OPTS(ROUTE_OPTIONS), NULL, run_layout},
    {NULL, "blocks", "", 0, 0, "List the circuit blocks a spec can use and their parameters",
     "Reusable circuits (I2C pull-ups, USB-C sink, LDO, I2C sensor, reset/boot, a part's reference circuit). A spec "
     "declares one with 'use block NAME: key=value ...'; every value it adds comes from a cited standard, the bound "
     "part's datasheet data or a stated parameter, and a missing one is a question.",
     "pcba blocks\npcba blocks --json", NONE, NULL, run_blocks},
    {"rules", "import", "<project.kicad_pro|folder>", 1, 1, "Turn a KiCad project's design rules into a Copper profile",
     "Reads the board constraints and net classes (.kicad_pro), custom rules (.kicad_dru) and mask settings "
     "(.kicad_pcb setup). Each limit is the strictest KiCad applies to every object, recorded with its source and the "
     "date; copper-core must accept the profile before it is written. KiCad limits one global profile cannot express "
     "(conditional or layer rules, maximums, hole-to-copper clearance, text size) are listed and make the result "
     "UNVERIFIED: KiCad DRC still checks them.",
     "pcba rules import my_board --out my_board.rules\n"
     "pcba rules import my_board/my_board.kicad_pro --out p.rules --base profiles/conservative.rules --report import.json",
     OPTS(RULES_IMPORT_OPTIONS), NULL, run_rules_import},
    {"dfm", "check", "<file-or-build-dir>", 1, 1, "Run physical DFM checks on a board",
     "Runs the exact-geometry DFM rules on a .copper file with board geometry or on a build directory "
     "(resolved.copper). Without geometry the result is UNVERIFIED, never PASS.",
     "pcba dfm check out/divider\npcba dfm check out/divider/resolved.copper", NONE, NULL, run_dfm},
};

const PcbaCommand *pcba_commands(size_t *count) {
    *count = sizeof COMMANDS / sizeof COMMANDS[0];
    return COMMANDS;
}

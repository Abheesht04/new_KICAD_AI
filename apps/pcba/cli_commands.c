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

static int run_planned(PcbaContext *ctx, const PcbaCommand *cmd, const PcbaArgs *a) {
    char message[256], path[64];
    (void)a;
    pcba_command_path(cmd, path, sizeof path);
    snprintf(message, sizeof message, "'pcba %s' is planned (%s); nothing was run", path, cmd->planned);
    return pcba_finish(ctx, path, "NOT_IMPLEMENTED", message, NULL);
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
    int rc = missing_file(ctx, cmd, a->positional[0], 0);
    if (rc >= 0) return rc;
    Bargs b = {{0}, 0};
    push(&b, "transient");
    push(&b, a->positional[0]);
    push(&b, "--dt");
    push(&b, dt_text);
    push(&b, "--steps");
    push(&b, steps_text);
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

static int run_library_scan(PcbaContext *ctx, const PcbaCommand *cmd, const PcbaArgs *a) {
    static const char *const map[][2] = {{"--symbols", "--symbol-dir"}, {"--footprints", "--footprint-dir"},
                                         {"--out", "--out"}};
    Bargs b = {{0}, 0};
    push(&b, "library-scan");
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
static const PcbaOption SCAN_OPTIONS[] = {
    {"--symbols", "DIR", 0, "KiCad symbol library directory"},
    {"--footprints", "DIR", 0, "KiCad footprint library directory"},
    {"--out", "DIR", 0, "Where to write the inventories (default out/library)"},
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
    {"simulate", "transient", "<file.copper>", 1, 1, "Backward Euler transient from zero stored energy",
     "Fixed-step Backward Euler with constant sources switched on at t=0 and zero initial capacitor voltage and "
     "inductor current. Fails without emitting a waveform if any step does not solve.",
     "pcba simulate transient examples/v2/rc.copper --dt 1e-4 --steps 100 --out out/rc.csv", OPTS(TRANSIENT_OPTIONS),
     NULL, run_transient},
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
    {"parts", "import", "<catalogue-file>", 1, 1, "Import an external catalogue with provenance",
     "Schema-validated catalogue import (accepted/rejected/duplicate reporting) does not exist yet.",
     "pcba parts import jlcpcb_export.csv", NONE, "Prompt 7: user-extensible part registry", run_planned},
    {"library", "scan", "", 0, 0, "Index installed KiCad symbol and footprint libraries",
     "Writes JSON and CSV inventories of every symbol and footprint in the selected KiCad libraries.",
     "pcba library scan\npcba library scan --symbols /usr/share/kicad/symbols --footprints /usr/share/kicad/footprints",
     OPTS(SCAN_OPTIONS), NULL, run_library_scan},
    {NULL, "place", "<file.copper>", 1, 1, "Automatic component placement",
     "There is no placement optimizer yet; placements come from (at x y rotation) in the project.",
     "pcba place board.copper", NONE, "Prompt 8: placement and routing optimization", run_planned},
    {NULL, "route", "<file.copper>", 1, 1, "Standalone routing",
     "Routing currently runs inside 'pcba project build' (deterministic two-layer A* candidate). A standalone, "
     "optimizing router does not exist yet.",
     "pcba route board.copper", NONE, "Prompt 8: placement and routing optimization", run_planned},
    {"dfm", "check", "<file-or-build-dir>", 1, 1, "Run physical DFM checks on a board",
     "Runs the exact-geometry DFM rules on a .copper file with board geometry or on a build directory "
     "(resolved.copper). Without geometry the result is UNVERIFIED, never PASS.",
     "pcba dfm check out/divider\npcba dfm check out/divider/resolved.copper", NONE, NULL, run_dfm},
};

const PcbaCommand *pcba_commands(size_t *count) {
    *count = sizeof COMMANDS / sizeof COMMANDS[0];
    return COMMANDS;
}

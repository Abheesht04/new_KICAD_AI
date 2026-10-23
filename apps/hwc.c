#include "nlp_rules.h"
#include "kicad_schematic.h"

#include "component.h"
#include "component_model.h"
#include "design.h"
#include "design_validate.h"
#include "diagnostic.h"
#include "dfm.h"
#include "dfm_pcb.h"
#include "defect.h"
#include "kicad_generic_provider.h"
#include "mna.h"
#include "mna_from_design.h"
#include "node_map.h"
#include "part_provider.h"
#include "part_registry.h"
#include "range.h"
#include "vec.h"
#include "cli_os.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#include <process.h>
#include <direct.h>
#define HWC_ACCESS _access
#define HWC_MKDIR(p) _mkdir(p)
#define HWC_WAIT _P_WAIT
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#define HWC_ACCESS access
#define HWC_MKDIR(p) mkdir((p), 0775)
#endif

#define HWC_STATE_PATH "out/current_design.hwc"
#define HWC_SCHEMATIC_PATH "out/current_design.kicad_sch"
#define HWC_LINE_CAP 4096
#define HWC_ARG_CAP 64

static void print_help(void) {
    puts("Hardware Compiler legacy V1 CLI (deprecated). The primary command is now: pcba --help");
    puts("\nV2: hwc project new|check|build|transient|classify|verify|catalogue --help");
    puts("\nLegacy commands:");
    puts("  make resistor [value]                         create one resistor (default 10k)");
    puts("  make resistor divider <Vin> to <Vout> using <Rtop> and <Rbottom>");
    puts("  catalogue -help | list | search <terms> | rebuild | footprints");
    puts("  DFM -help | -apply | -coverage                 run design checks / show coverage");
    puts("  physics-apply                                  run the supported DC MNA solve");
    puts("  validate                                       audit pin/net/ID/model invariants");
    puts("  kicad-check [path]                             run installed kicad-cli ERC + SVG export");
    puts("  status                                         show the last saved design");
    puts("  help | quit");
    puts("\nExamples:");
    puts("  make resistor");
    puts("  make resistor 10k");
    puts("  make resistor divider 5 V to 3.3 V using 10k ohm and 20k ohm");
    puts("  catalogue search resistor");
    puts("  DFM -apply");
    puts("  physics-apply");
    puts("  kicad-check");
    puts("\nThe prompt frontend is a deterministic rules parser, not a trained model.");
    puts("KiCad catalogue helpers read your installed libraries. Schematic V1 export supports Device:R.");
}

static int file_exists(const char *path) { return path && HWC_ACCESS(path, 0) == 0; }

static int ensure_out_dir(void) {
    if (HWC_MKDIR("out") == 0 || errno == EEXIST) return 1;
    fprintf(stderr, "ERROR: cannot create output directory 'out' (errno=%d)\n", errno);
    return 0;
}

static int equals_ci(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        ++a; ++b;
    }
    return *a == '\0' && *b == '\0';
}

static int starts_ci(const char *text, const char *prefix) {
    if (!text || !prefix) return 0;
    while (*prefix) {
        if (!*text || tolower((unsigned char)*text) != tolower((unsigned char)*prefix)) return 0;
        ++text; ++prefix;
    }
    return 1;
}

static int tokenise(char *line, char **argv, int cap) {
    if (!line || !argv || cap <= 0) return -1;
    int argc = 0;
    char *read = line;
    char *write = line;
    while (*read) {
        while (*read && isspace((unsigned char)*read)) ++read;
        if (!*read) break;
        if (argc >= cap) return -1;
        argv[argc++] = write;
        char quote = 0;
        if (*read == '"' || *read == '\'') quote = *read++;
        if (quote) {
            while (*read && *read != quote) *write++ = *read++;
            if (*read == quote) ++read;
        } else {
            while (*read && !isspace((unsigned char)*read)) *write++ = *read++;
        }
        while (*read && isspace((unsigned char)*read)) ++read;
        *write++ = '\0';
    }
    return argc;
}

/* Python helpers run through cli_run: an argv array, never a shell or a joined
 * command string, so paths with spaces and metacharacters survive on Windows. */
static int run_python(const char *launcher, const char *launcher_flag, const char *script, int argc,
                      const char *const args[]) {
    const char *av[HWC_ARG_CAP + 6];
    int n = 0;
    av[n++] = launcher;
    if (launcher_flag) av[n++] = launcher_flag;
    av[n++] = script;
    for (int i = 0; i < argc && n < HWC_ARG_CAP + 4; ++i) av[n++] = args[i];
    av[n] = NULL;
    CliProcResult result;
    if (!cli_run(av, CLI_STDOUT_INHERIT, &result)) {
        fprintf(stderr, "ERROR: could not launch Python (%s). Install Python 3 or set HWC_PYTHON.\n", launcher);
        return 127;
    }
    int exit_code = result.exit_code;
    cli_proc_result_free(&result);
    return exit_code;
}

static int tool_script(const char *name, char *path, size_t cap) {
    char root[1024];
    return cli_tool_root(root, sizeof root) && snprintf(path, cap, "%s/tools/%s", root, name) > 0 && file_exists(path);
}

static int spawn_python_bridge(int argc, const char *const args[]) {
    char script[1200];
    if (!tool_script("hwc_kicad.py", script, sizeof script)) {
        fprintf(stderr, "ERROR: catalogue helper tools/hwc_kicad.py not found; set PCBA_ROOT to the source tree\n");
        return 127;
    }
    char launcher[1024];
#ifdef _WIN32
    if (cli_getenv("HWC_PY_LAUNCHER", launcher, sizeof launcher)) return run_python(launcher, "-3", script, argc, args);
    return run_python("py", "-3", script, argc, args);
#else
    if (cli_getenv("HWC_PYTHON", launcher, sizeof launcher)) return run_python(launcher, NULL, script, argc, args);
    return run_python("python3", NULL, script, argc, args);
#endif
}

static int ensure_symbol_catalogue(void) {
    if (file_exists("build/kicad_catalog.json")) return 1;
    const char *args[] = {"catalog", "--out", "build/kicad_catalog.json", "--csv", "build/kicad_catalog.csv"};
    int rc = spawn_python_bridge((int)(sizeof(args)/sizeof(args[0])), args);
    return rc == 0;
}

static int ensure_footprint_catalogue(void) {
    if (file_exists("build/kicad_footprints.json")) return 1;
    const char *args[] = {"footprints", "--out", "build/kicad_footprints.json", "--csv", "build/kicad_footprints.csv"};
    return spawn_python_bridge((int)(sizeof(args)/sizeof(args[0])), args) == 0;
}

static int print_full_catalogue(void) {
    if (!ensure_symbol_catalogue()) return 2;
    FILE *f = fopen("build/kicad_catalog.csv", "rb");
    if (!f) {
        const char *args[] = {"catalog", "--out", "build/kicad_catalog.json", "--csv", "build/kicad_catalog.csv"};
        if (spawn_python_bridge((int)(sizeof(args)/sizeof(args[0])), args) != 0) return 2;
        f = fopen("build/kicad_catalog.csv", "rb");
    }
    if (!f) { fprintf(stderr, "ERROR: cannot read build/kicad_catalog.csv\n"); return 2; }
    char line[8192];
    while (fgets(line, sizeof(line), f)) fputs(line, stdout);
    fclose(f);
    return 0;
}

static int command_catalogue(int argc, char **argv) {
    if (argc < 2 || equals_ci(argv[1], "-help") || equals_ci(argv[1], "--help") || equals_ci(argv[1], "help")) {
        puts("catalogue -help       show this help");
        puts("catalogue rebuild     index installed KiCad .kicad_sym libraries");
        puts("catalogue list        print the entire indexed symbol catalogue as CSV");
        puts("catalogue search TERM [TERM ...]  search library ID, description, keywords and footprint filters");
        puts("catalogue footprints  index installed .pretty libraries");
        puts("catalogue footprint-search TERM [TERM ...]  search indexed footprints");
        puts("Outputs: build/kicad_catalog.json, build/kicad_catalog.csv, build/kicad_footprints.json, build/kicad_footprints.csv");
        puts("Source of truth: locally installed KiCad libraries; MPN, price and stock are not inferred.");
        return 0;
    }
    if (equals_ci(argv[1], "list") || equals_ci(argv[1], "-list")) return print_full_catalogue();
    if (equals_ci(argv[1], "rebuild") || equals_ci(argv[1], "-rebuild")) {
        const char *args[] = {"catalog", "--out", "build/kicad_catalog.json", "--csv", "build/kicad_catalog.csv"};
        return spawn_python_bridge((int)(sizeof(args)/sizeof(args[0])), args);
    }
    if (equals_ci(argv[1], "footprints")) {
        const char *args[] = {"footprints", "--out", "build/kicad_footprints.json", "--csv", "build/kicad_footprints.csv"};
        return spawn_python_bridge((int)(sizeof(args)/sizeof(args[0])), args);
    }
    if (equals_ci(argv[1], "search")) {
        if (argc < 3) { fprintf(stderr, "usage: catalogue search <term> [term ...]\n"); return 64; }
        if (!ensure_symbol_catalogue()) return 2;
        const char *args[HWC_ARG_CAP]; int n = 0;
        args[n++] = "search";
        for (int i = 2; i < argc && n < HWC_ARG_CAP; ++i) args[n++] = argv[i];
        return spawn_python_bridge(n, args);
    }
    if (equals_ci(argv[1], "footprint-search")) {
        if (argc < 3) { fprintf(stderr, "usage: catalogue footprint-search <term> [term ...]\n"); return 64; }
        if (!ensure_footprint_catalogue()) return 2;
        const char *args[HWC_ARG_CAP]; int n = 0;
        args[n++] = "search-footprint";
        for (int i = 2; i < argc && n < HWC_ARG_CAP; ++i) args[n++] = argv[i];
        return spawn_python_bridge(n, args);
    }
    fprintf(stderr, "Unknown catalogue operation '%s'; use 'catalogue -help'.\n", argv[1]);
    return 64;
}

static int save_intent(const HwcIntent *intent) {
    if (!intent || !ensure_out_dir()) return 0;
    FILE *f = fopen(HWC_STATE_PATH, "wb");
    if (!f) { fprintf(stderr, "ERROR: cannot write %s\n", HWC_STATE_PATH); return 0; }
    fprintf(f, "HWC_STATE 1\nkind=%s\npackage=%s\nresistor_ohm=%.17g\ninput_voltage_v=%.17g\ntarget_voltage_v=%.17g\nr_top_ohm=%.17g\nr_bottom_ohm=%.17g\n",
            hwc_intent_kind_name(intent->kind), intent->package,
            intent->resistor_ohm, intent->input_voltage_v, intent->target_voltage_v,
            intent->r_top_ohm, intent->r_bottom_ohm);
    int ok = fclose(f) == 0;
    return ok;
}

static int load_intent(HwcIntent *intent) {
    if (!intent) return 0;
    FILE *f = fopen(HWC_STATE_PATH, "rb");
    if (!f) return 0;
    memset(intent, 0, sizeof(*intent));
    snprintf(intent->package, sizeof(intent->package), "0603");
    char line[256];
    int valid_header = 0;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strcmp(line, "HWC_STATE 1") == 0) { valid_header = 1; continue; }
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq++ = '\0';
        if (strcmp(line, "kind") == 0) {
            if (strcmp(eq, "make_resistor") == 0) intent->kind = HWC_INTENT_MAKE_RESISTOR;
            else if (strcmp(eq, "resistor_divider") == 0) intent->kind = HWC_INTENT_RESISTOR_DIVIDER;
        } else if (strcmp(line, "package") == 0) snprintf(intent->package, sizeof(intent->package), "%s", eq);
        else {
            char *end = NULL; errno = 0; double v = strtod(eq, &end);
            if (end == eq || *end || errno == ERANGE || !isfinite(v)) { fclose(f); return 0; }
            if (strcmp(line, "resistor_ohm") == 0) intent->resistor_ohm = v;
            else if (strcmp(line, "input_voltage_v") == 0) intent->input_voltage_v = v;
            else if (strcmp(line, "target_voltage_v") == 0) intent->target_voltage_v = v;
            else if (strcmp(line, "r_top_ohm") == 0) intent->r_top_ohm = v;
            else if (strcmp(line, "r_bottom_ohm") == 0) intent->r_bottom_ohm = v;
        }
    }
    fclose(f);
    if (!valid_header) return 0;
    if (intent->kind == HWC_INTENT_MAKE_RESISTOR) return intent->resistor_ohm > 0.0;
    if (intent->kind == HWC_INTENT_RESISTOR_DIVIDER) return intent->input_voltage_v > intent->target_voltage_v && intent->target_voltage_v > 0.0 && intent->r_top_ohm > 0.0 && intent->r_bottom_ohm > 0.0;
    return 0;
}

static int add_resistor(Design *design, const char *reference, double ohms,
                        const char *package, uint32_t *component_id) {
    PartRegistry registry;
    if (part_registry_init(&registry) != 0) return 0;
    PartProvider provider = kicad_generic_provider();
    int rc = part_registry_register(&registry, provider);
    if (rc != 0) { part_registry_free(&registry); return 0; }

    PartRequest req;
    memset(&req, 0, sizeof(req));
    req.role = "resistor";
    req.value = range_exact(ohms);
    req.required_package = package;
    PartResult result;
    memset(&result, 0, sizeof(result));
    int resolved = part_registry_find(&registry, &req, &result);
    part_registry_free(&registry);
    if (!resolved) {
        fprintf(stderr, "ERROR: no generic resistor provider match for package %s\n", package);
        return 0;
    }

    Component c;
    component_init(&c);
    snprintf(c.reference, sizeof(c.reference), "%s", reference);
    c.manufacturer = result.manufacturer;
    if (result.manufacturer_part_number) snprintf(c.manufacturer_part_number, sizeof(c.manufacturer_part_number), "%s", result.manufacturer_part_number);
    c.package = package;
    c.footprint = result.footprint;
    c.symbol = result.symbol;
    c.model.kind = COMPONENT_RESISTOR;
    c.model.data.resistor.resistance_ohm = range_exact(ohms);
    component_set_value(&c, ohms, 0.0);
    c.dimensions.width_mm = 1.6;
    c.dimensions.length_mm = 0.8;
    c.dimensions.height_mm = 0.8;
    int ok = component_add_pin(&c, 1, "1") == 0 && component_add_pin(&c, 2, "2") == 0 &&
             design_add_component(design, &c, component_id) == 0;
    component_free(&c);
    return ok;
}

static int build_design_from_intent(const HwcIntent *intent, Design *design,
                                    uint32_t *ground_net_id) {
    if (!intent || !design || design_init(design) != 0) return 0;
    if (intent->kind == HWC_INTENT_MAKE_RESISTOR) {
        if (!add_resistor(design, "R1", intent->resistor_ohm, intent->package, NULL)) { design_free(design); return 0; }
        if (ground_net_id) *ground_net_id = UINT32_MAX;
        return 1;
    }

    /* The ideal input voltage is a simulation boundary/excitation component.
     * It remains in Design for MNA, but is not emitted as a physical BOM part. */
    Component source;
    component_init(&source);
    snprintf(source.reference, sizeof(source.reference), "VSIM");
    source.model.kind = COMPONENT_SOURCE;
    source.model.data.source.voltage_dc_v = intent->input_voltage_v;
    if (component_add_pin(&source, 1, "positive") != 0 || component_add_pin(&source, 2, "negative") != 0) {
        component_free(&source); design_free(design); return 0;
    }
    uint32_t source_id = UINT32_MAX, r1_id = UINT32_MAX, r2_id = UINT32_MAX;
    int ok = design_add_component(design, &source, &source_id) == 0;
    component_free(&source);
    if (!ok || !add_resistor(design, "R1", intent->r_top_ohm, intent->package, &r1_id) ||
        !add_resistor(design, "R2", intent->r_bottom_ohm, intent->package, &r2_id)) {
        design_free(design); return 0;
    }
    uint32_t vin = UINT32_MAX, vout = UINT32_MAX, gnd = UINT32_MAX;
    if (design_add_net(design, "VIN", &vin) != 0 || design_add_net(design, "VOUT", &vout) != 0 || design_add_net(design, "GND", &gnd) != 0 ||
        design_connect_pin(design, source_id, 1, vin) != 0 || design_connect_pin(design, source_id, 2, gnd) != 0 ||
        design_connect_pin(design, r1_id, 1, vin) != 0 || design_connect_pin(design, r1_id, 2, vout) != 0 ||
        design_connect_pin(design, r2_id, 1, vout) != 0 || design_connect_pin(design, r2_id, 2, gnd) != 0) {
        design_free(design); return 0;
    }
    if (ground_net_id) *ground_net_id = gnd;
    return 1;
}

static void print_diagnostics(const DiagnosticList *list) {
    for (size_t i = 0; i < diagnostic_count(list); ++i) {
        const Diagnostic *d = diagnostic_get(list, i);
        if (!d) continue;
        printf("  [%s] %s: %s\n", diagnostic_severity_name(d->severity),
               d->rule_name ? d->rule_name : "diagnostic", d->message);
    }
}

static int run_design_checks(const Design *design, DiagnosticList *diagnostics) {
    int errors = design_validate_structure(design, diagnostics);
    return errors;
}

static void print_design_summary(const HwcIntent *intent, const Design *design) {
    printf("Intent: %s\n", hwc_intent_kind_name(intent->kind));
    printf("Components: %zu | Nets: %zu\n", vec_len(design->components), vec_len(design->nets));
    for (size_t i = 0; i < vec_len(design->components); ++i) {
        const Component *c = &design->components[i];
        if (c->model.kind == COMPONENT_RESISTOR) {
            char rval[48];
            double ohms = c->model.data.resistor.resistance_ohm.nominal;
            if (ohms >= 1e6) snprintf(rval, sizeof(rval), "%.6g MOhm", ohms / 1e6);
            else if (ohms >= 1e3) snprintf(rval, sizeof(rval), "%.6g kOhm", ohms / 1e3);
            else snprintf(rval, sizeof(rval), "%.6g Ohm", ohms);
            printf("  %s: %s, symbol=%s, footprint=%s\n", c->reference, rval,
                   c->symbol ? c->symbol : "(none)", c->footprint ? c->footprint : "(none)");
        } else if (c->model.kind == COMPONENT_SOURCE) {
            printf("  %s: ideal DC excitation %.6g V (simulation boundary, not a BOM part)\n",
                   c->reference, c->model.data.source.voltage_dc_v);
        }
    }
}

static int command_make(const char *prompt) {
    HwcIntent intent;
    char error[512] = {0};
    if (!hwc_intent_parse(prompt, &intent, error, sizeof(error))) {
        fprintf(stderr, "PROMPT ERROR: %s\n", error);
        return 2;
    }
    if (intent.used_default_value) puts("Assumption: no value was specified; using 10 kOhm explicitly for this V1 template.");
    for (size_t i = 0; i < strlen(prompt); ++i) {
        if (isdigit((unsigned char)prompt[i]) && i + 3 < strlen(prompt) &&
            ((prompt[i] == '0' && prompt[i+1] == '4' && prompt[i+2] == '0' && prompt[i+3] == '2') ||
             (prompt[i] == '0' && prompt[i+1] == '8' && prompt[i+2] == '0' && prompt[i+3] == '5'))) {
            snprintf(intent.package, sizeof(intent.package), "%s", prompt[i+1] == '4' ? "0402" : "0805");
        }
    }

    Design design;
    uint32_t ground_id = UINT32_MAX;
    if (!build_design_from_intent(&intent, &design, &ground_id)) {
        fprintf(stderr, "ERROR: failed to construct Design using the registered part provider.\n");
        return 2;
    }

    if (!save_intent(&intent)) {
        fprintf(stderr, "ERROR: could not save active Design state.\n");
        design_free(&design); return 2;
    }
    char export_error[512] = {0};
    if (!hwc_kicad_schematic_write(HWC_SCHEMATIC_PATH, &design, &intent, export_error, sizeof(export_error))) {
        fprintf(stderr, "SCHEMATIC EXPORT FAILED: %s\n", export_error);
        design_free(&design); return 2;
    }

    puts("\nDESIGN CREATED");
    print_design_summary(&intent, &design);
    printf("KiCad schematic: %s\n", HWC_SCHEMATIC_PATH);
    puts("State saved: out/current_design.hwc");
    if (intent.kind == HWC_INTENT_MAKE_RESISTOR) {
        puts("Note: this is an unconnected symbol template. Run 'DFM -apply' to see the expected unconnected-pin findings.");
    } else {
        puts("Note: divider topology is represented in Design and in the schematic; the ideal VIN excitation is an analysis boundary.");
    }
    design_free(&design);
    return 0;
}

static int load_active_design(HwcIntent *intent, Design *design, uint32_t *ground_id) {
    if (!load_intent(intent)) {
        fprintf(stderr, "No valid active design state. Run 'make resistor' or a supported divider prompt first.\n");
        return 0;
    }
    if (!build_design_from_intent(intent, design, ground_id)) {
        fprintf(stderr, "ERROR: could not reconstruct active Design from saved state.\n");
        return 0;
    }
    return 1;
}

static int command_validate(void) {
    HwcIntent intent; Design design; uint32_t ground_id;
    if (!load_active_design(&intent, &design, &ground_id)) return 2;
    DiagnosticList diagnostics; diagnostic_list_init(&diagnostics);
    int errors = run_design_checks(&design, &diagnostics);
    printf("STRUCTURAL VALIDATION: %s (%d errors)\n", errors == 0 ? "PASS" : "FAIL", errors);
    print_diagnostics(&diagnostics);
    diagnostic_list_free(&diagnostics); design_free(&design);
    return errors == 0 ? 0 : 2;
}

static int command_dfm_coverage(void) {
    size_t count = 0;
    const PcbDefectCoverage *items = pcb_defect_coverage(&count);
    if (!items) { fprintf(stderr, "ERROR: DFM coverage registry unavailable\n"); return 2; }
    puts("PCB DFM COVERAGE (real board geometry is required to execute physical rules)");
    for (size_t i = 0; i < count; ++i) {
        printf("  %-13s %-28s %s\n", pcb_defect_coverage_status_name(items[i].status),
               items[i].name, items[i].detail);
    }
    return 0;
}

static void print_dfm_help(void) {
    puts("DFM -help      show this rule list");
    puts("DFM -apply     rebuild current Design and run structural + logical DFM checks");
    puts("DFM -coverage  list physical PCB rules and their current coverage");
    puts("\nLogical Design checks wired into this CLI:");
    puts("  floating/unconnected pins; duplicate references and pin numbers;");
    puts("  invalid net IDs; pin<->net reciprocity; orphan net connections;");
    puts("  one-terminal nets/open-node warnings; invalid resistor/source values;");
    puts("  missing footprints; component-height limit.");
    puts("\nExisting PCB geometry DFM rule families:");
    puts("  board size, layer count, trace width/clearance, via drill/diameter/annular ring,");
    puts("  pad annular ring, hole clearance, copper-to-edge, component height,");
    puts("  copper shorts/clearances, open connections, invalid geometry, report consistency.");
    puts("Current limitation: DFM -apply only runs Design-level checks until a PCB geometry file is imported/built.");
    puts("Run 'DFM -coverage' to see PARTIAL/UNSUPPORTED checks. A PASS is not fabrication signoff.");
}

static int command_dfm_apply(void) {
    HwcIntent intent; Design design; uint32_t ground_id;
    if (!load_active_design(&intent, &design, &ground_id)) return 2;
    (void)ground_id;
    DiagnosticList diagnostics; diagnostic_list_init(&diagnostics);
    int structure_errors = design_validate_structure(&design, &diagnostics);

    DfmRegistry registry;
    dfm_registry_init(&registry);
    int setup_ok = dfm_register_builtin_rules(&registry) == 0;
    DfmProfile profile = {
        .layer_count = 2, .copper_weight_oz = 1.0,
        .min_trace_width_mm = 0.127, .min_clearance_mm = 0.127,
        .min_via_diameter_mm = 0.5, .min_drill_mm = 0.3,
        .min_annular_ring_mm = 0.13, .board_edge_clearance_mm = 0.25,
        .max_component_height_mm = 1.6
    };
    int dfm_errors = setup_ok ? dfm_run_all(&registry, &design, &profile, &diagnostics) : -1;
    puts("DFM APPLY");
    printf("  profile: conservative logical defaults (2-layer, 1 oz reference only)\n");
    printf("  structural errors: %d\n", structure_errors);
    printf("  built-in DFM rules: %zu\n", vec_len(registry.rules));
    printf("  logical DFM errors: %d\n", dfm_errors);
    print_diagnostics(&diagnostics);
    puts("  physical PCB DFM: NOT RUN (no physical PcbBoard was supplied by the current command path)");
    puts("  coverage: PARTIAL; use 'DFM -coverage' for implemented and unsupported rule families");
    dfm_registry_free(&registry); diagnostic_list_free(&diagnostics); design_free(&design);
    return structure_errors == 0 && dfm_errors == 0 ? 0 : 2;
}

static int command_physics_apply(void) {
    HwcIntent intent; Design design; uint32_t ground_id;
    if (!load_active_design(&intent, &design, &ground_id)) return 2;
    if (intent.kind != HWC_INTENT_RESISTOR_DIVIDER) {
        fprintf(stderr, "PHYSICS ERROR: the saved design is a standalone resistor with unconnected pins; DC MNA requires connected nodes and a reference ground.\n");
        fprintf(stderr, "Create a divider first, e.g. 'make resistor divider 5 V to 3.3 V using 10k ohm and 20k ohm'.\n");
        design_free(&design); return 2;
    }

    DiagnosticList diagnostics; diagnostic_list_init(&diagnostics);
    int errors = design_validate_structure(&design, &diagnostics);
    if (errors != 0) {
        fprintf(stderr, "PHYSICS BLOCKED: Design topology is inconsistent (%d errors).\n", errors);
        print_diagnostics(&diagnostics); diagnostic_list_free(&diagnostics); design_free(&design); return 2;
    }

    MnaCircuit circuit; memset(&circuit, 0, sizeof(circuit));
    if (!design_to_mna(&design, ground_id, &circuit, &diagnostics)) {
        fprintf(stderr, "PHYSICS ERROR: Design-to-MNA translation failed.\n");
        print_diagnostics(&diagnostics); diagnostic_list_free(&diagnostics); design_free(&design); return 2;
    }
    MnaOptions options; mna_options_default(&options);
    MnaResult result; memset(&result, 0, sizeof(result));
    mna_solve(&circuit, &options, &result);
    if (result.status != MNA_STATUS_SUCCESS) {
        fprintf(stderr, "MNA SOLVE FAILED: status=%d\n", (int)result.status);
        mna_result_free(&result); mna_circuit_free(&circuit);
        diagnostic_list_free(&diagnostics); design_free(&design); return 2;
    }
    NodeMap map;
    if (!node_map_build(&design, ground_id, &map)) {
        fprintf(stderr, "PHYSICS ERROR: could not map Design nets to MNA node indices.\n");
        mna_result_free(&result); mna_circuit_free(&circuit);
        diagnostic_list_free(&diagnostics); design_free(&design); return 2;
    }
    const Net *vin_net = NULL, *vout_net = NULL;
    for (size_t i = 0; i < vec_len(design.nets); ++i) {
        if (design.nets[i].name && strcmp(design.nets[i].name, "VIN") == 0) vin_net = &design.nets[i];
        if (design.nets[i].name && strcmp(design.nets[i].name, "VOUT") == 0) vout_net = &design.nets[i];
    }
    if (!vin_net || !vout_net) {
        fprintf(stderr, "PHYSICS ERROR: required VIN or VOUT net is missing.\n");
        node_map_free(&map); mna_result_free(&result); mna_circuit_free(&circuit);
        diagnostic_list_free(&diagnostics); design_free(&design); return 2;
    }
    size_t vin_row = node_map_row(&map, vin_net->id);
    size_t vout_row = node_map_row(&map, vout_net->id);
    if (vin_row == SIZE_MAX || vout_row == SIZE_MAX) {
        fprintf(stderr, "PHYSICS ERROR: an expected signal net was incorrectly mapped to ground.\n");
        node_map_free(&map); mna_result_free(&result); mna_circuit_free(&circuit);
        diagnostic_list_free(&diagnostics); design_free(&design); return 2;
    }
    double vin = mna_node_voltage(&result, vin_row + 1);
    double vout = mna_node_voltage(&result, vout_row + 1);
    double total_r = intent.r_top_ohm + intent.r_bottom_ohm;
    double current = intent.input_voltage_v / total_r;
    double ptop = current * current * intent.r_top_ohm;
    double pbottom = current * current * intent.r_bottom_ohm;
    MnaValidation validation; memset(&validation, 0, sizeof(validation));
    (void)mna_validate(&circuit, &result, &options, &validation);

    puts("PHYSICS APPLY — supported DC electrical solve");
    printf("  VIN  = %.9g V\n", vin);
    printf("  VOUT = %.9g V\n", vout);
    printf("  I    = %.9g A\n", current);
    printf("  R1 power = %.9g W\n", ptop);
    printf("  R2 power = %.9g W\n", pbottom);
    printf("  MNA residual = %.3e\n", result.maximum_residual);
    printf("  KCL residual = %.3e A\n", result.maximum_kcl_residual_a);
    printf("  independent MNA validation = %s\n", validation.valid ? "PASS" : "FAIL");
    double target_error = fabs(vout - intent.target_voltage_v);
    printf("  requested VOUT = %.9g V; absolute target error = %.9g V\n", intent.target_voltage_v, target_error);
    if (target_error > fmax(0.01, 0.01 * intent.target_voltage_v)) {
        puts("  TARGET: WARNING — selected resistor values do not meet the requested target within 1% or 10 mV.");
    } else {
        puts("  TARGET: within 1% or 10 mV tolerance.");
    }
    puts("  scope: DC resistive operating point only; this does not claim AC, transient, thermal, SI, or reliability signoff.");

    node_map_free(&map); mna_result_free(&result); mna_circuit_free(&circuit);
    diagnostic_list_free(&diagnostics); design_free(&design);
    return validation.valid ? 0 : 2;
}

static int command_kicad_check(const char *path) {
    if (path && !path[0]) {
        fprintf(stderr, "usage: kicad-check [path-to-kicad_sch]\n");
        return 64;
    }
    const char *bridge_args[] = {"validate", path ? path : HWC_SCHEMATIC_PATH};
    return spawn_python_bridge(2, bridge_args);
}

static int command_status(void) {
    HwcIntent intent; Design design; uint32_t ground;
    if (!load_active_design(&intent, &design, &ground)) return 2;
    print_design_summary(&intent, &design);
    printf("Ground net ID: %s", ground == UINT32_MAX ? "(not defined for standalone resistor)\n" : "");
    if (ground != UINT32_MAX) printf("%u\n", ground);
    printf("Schematic: %s\n", HWC_SCHEMATIC_PATH);
    design_free(&design);
    return 0;
}

static int execute_command(char *line, int interactive) {
    while (*line && isspace((unsigned char)*line)) ++line;
    size_t length = strlen(line);
    while (length && isspace((unsigned char)line[length - 1])) line[--length] = '\0';
    if (!*line) return 0;
    if (equals_ci(line, "quit") || equals_ci(line, "exit")) return interactive ? -1 : 0;
    if (equals_ci(line, "help") || equals_ci(line, "--help") || equals_ci(line, "-help")) { print_help(); return 0; }
    if (starts_ci(line, "make ") || equals_ci(line, "make")) return command_make(line);
    if (starts_ci(line, "catalogue") || starts_ci(line, "catalog ")) {
        char copy[HWC_LINE_CAP]; snprintf(copy, sizeof(copy), "%s", line);
        char *args[HWC_ARG_CAP]; int argc = tokenise(copy, args, HWC_ARG_CAP);
        if (argc < 0) { fprintf(stderr, "too many command arguments\n"); return 64; }
        return command_catalogue(argc, args);
    }
    if (starts_ci(line, "dfm")) {
        char copy[HWC_LINE_CAP]; snprintf(copy, sizeof(copy), "%s", line);
        char *args[8]; int argc = tokenise(copy, args, 8);
        if (argc < 0) return 64;
        if (argc < 2 || equals_ci(args[1], "-help") || equals_ci(args[1], "--help") || equals_ci(args[1], "help")) { print_dfm_help(); return 0; }
        if (equals_ci(args[1], "-apply") || equals_ci(args[1], "apply")) return command_dfm_apply();
        if (equals_ci(args[1], "-coverage") || equals_ci(args[1], "coverage")) return command_dfm_coverage();
        fprintf(stderr, "Unknown DFM operation '%s'; use 'DFM -help'.\n", args[1]); return 64;
    }
    if (starts_ci(line, "physics-apply")) {
        if (strlen(line) > strlen("physics-apply") &&
            !isspace((unsigned char)line[strlen("physics-apply")])) {
            fprintf(stderr, "Unknown physics-apply option; use 'physics-apply -help'.\n"); return 64;
        }
        if (strstr(line, "-help") || strstr(line, "--help")) {
            puts("physics-apply: run Design -> NodeMap -> MNA -> independent residual/KCL validation.");
            puts("Supported in V1: DC resistor divider with ideal VIN excitation. A standalone resistor has no determined operating point.");
            puts("This does not yet solve AC/transient, thermal, signal-integrity, or manufacturing physics.");
            return 0;
        }
        return command_physics_apply();
    }
    if (equals_ci(line, "validate")) return command_validate();
    if (starts_ci(line, "kicad-check") &&
        (line[11] == '\0' || isspace((unsigned char)line[11]))) {
        char copy[HWC_LINE_CAP];
        snprintf(copy, sizeof(copy), "%s", line);
        char *args[8];
        int argc = tokenise(copy, args, 8);
        if (argc < 0 || argc > 2) {
            fprintf(stderr, "usage: kicad-check [path-to-kicad_sch]\n");
            return 64;
        }
        if (argc == 1) return command_kicad_check(NULL);
        return command_kicad_check(args[1]);
    }
    if (equals_ci(line, "status")) return command_status();
    fprintf(stderr, "Unknown command. Type 'help'.\n");
    return 64;
}

int main(int argc, char **argv) {
    argv = cli_utf8_argv(argc, argv);
    int wants_help = argc > 1 && (equals_ci(argv[1], "help") || equals_ci(argv[1], "--help") || equals_ci(argv[1], "-help"));
    if (!wants_help && !ensure_out_dir()) return 2;
    if (argc > 1) {
        if (equals_ci(argv[1], "project")) { /* legacy alias: forwards to tools/copper.py; prefer pcba */
            char script[1200], launcher[1024];
            if (!tool_script("copper.py", script, sizeof script)) {
                fprintf(stderr, "ERROR: tools/copper.py not found; set PCBA_ROOT to the source tree\n");
                return 127;
            }
            cli_default_copper_core();
#ifdef _WIN32
            if (!cli_getenv("HWC_PYTHON", launcher, sizeof launcher)) snprintf(launcher, sizeof launcher, "python");
#else
            if (!cli_getenv("HWC_PYTHON", launcher, sizeof launcher)) snprintf(launcher, sizeof launcher, "python3");
#endif
            return run_python(launcher, NULL, script, argc - 2, (const char *const *)argv + 2);
        }
        if (equals_ci(argv[1], "kicad-check")) {
            if (argc > 3) {
                fprintf(stderr, "usage: hwc kicad-check [path-to-kicad_sch]\n");
                return 64;
            }
            return command_kicad_check(argc == 3 ? argv[2] : NULL);
        }
        char line[HWC_LINE_CAP] = {0};
        size_t used = 0;
        for (int i = 1; i < argc; ++i) {
            size_t n = strlen(argv[i]);
            if (used + n + 2 >= sizeof(line)) { fprintf(stderr, "command line too long\n"); return 64; }
            if (used) line[used++] = ' ';
            memcpy(line + used, argv[i], n); used += n; line[used] = '\0';
        }
        return execute_command(line, 0);
    }

    puts("Hardware Compiler CLI — type 'help' for commands, 'quit' to exit.");
    char line[HWC_LINE_CAP];
    for (;;) {
        fputs("hwc> ", stdout); fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;
        line[strcspn(line, "\r\n")] = '\0';
        int result = execute_command(line, 1);
        if (result == -1) break;
        if (result != 0) printf("command exit: %d\n", result);
    }
    return 0;
}

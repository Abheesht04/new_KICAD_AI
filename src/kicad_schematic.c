#include "kicad_schematic.h"

#include "IR.h"
#include "arena.h"
#include "component.h"
#include "design_validate.h"
#include "diagnostic.h"
#include "vec.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#define HWC_PATH_SEP '\\'
#else
#include <sys/stat.h>
#define HWC_PATH_SEP '/'
#endif

static void errset(char *dst, size_t cap, const char *message) {
    if (dst && cap) (void)snprintf(dst, cap, "%s", message ? message : "schematic export failed");
}

static int append(IRNode *parent, IRNode *child) {
    return parent && child && ir_list_append(parent, child) == IR_STATUS_OK;
}

static IRNode *new_list(Arena *arena, const char *head) {
    return ir_node_list_named(arena, head);
}

static IRNode *add_list(Arena *arena, IRNode *parent, const char *head) {
    IRNode *node = new_list(arena, head);
    if (!append(parent, node)) return NULL;
    return node;
}

static int add_atom(Arena *arena, IRNode *parent, const char *text) {
    return append(parent, ir_node_atom(arena, text));
}

static int add_string(Arena *arena, IRNode *parent, const char *text) {
    return append(parent, ir_node_string(arena, text ? text : ""));
}

static int add_num(Arena *arena, IRNode *parent, double value) {
    return append(parent, ir_node_atom_double(arena, value));
}

static IRNode *add_at(Arena *arena, IRNode *parent, double x, double y, double angle) {
    IRNode *n = add_list(arena, parent, "at");
    if (!n || !add_num(arena, n, x) || !add_num(arena, n, y) || !add_num(arena, n, angle)) return NULL;
    return n;
}

static void make_uuid(char out[37]) {
    static int seeded = 0;
    if (!seeded) {
        srand((unsigned)(time(NULL) ^ (uintptr_t)&seeded));
        seeded = 1;
    }
    unsigned char b[16];
    for (size_t i = 0; i < sizeof(b); ++i) b[i] = (unsigned char)(rand() & 0xff);
    b[6] = (unsigned char)((b[6] & 0x0f) | 0x40);
    b[8] = (unsigned char)((b[8] & 0x3f) | 0x80);
    (void)snprintf(out, 37,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],b[8],b[9],b[10],b[11],b[12],b[13],b[14],b[15]);
}

static int add_uuid(Arena *arena, IRNode *parent, const char *head, const char *uuid) {
    IRNode *n = add_list(arena, parent, head);
    return n && add_string(arena, n, uuid);
}

static int add_font_effects(Arena *arena, IRNode *property, int hidden, const char *justify) {
    IRNode *effects = add_list(arena, property, "effects");
    IRNode *font = effects ? add_list(arena, effects, "font") : NULL;
    IRNode *size = font ? add_list(arena, font, "size") : NULL;
    if (!size || !add_num(arena, size, 1.27) || !add_num(arena, size, 1.27)) return 0;
    if (justify) {
        IRNode *j = add_list(arena, effects, "justify");
        if (!j || !add_atom(arena, j, justify)) return 0;
    }
    if (hidden) {
        IRNode *h = add_list(arena, effects, "hide");
        if (!h || !add_atom(arena, h, "yes")) return 0;
    }
    return 1;
}

static int add_property(Arena *arena, IRNode *parent, const char *name,
                        const char *value, double x, double y, double angle,
                        int hidden, const char *justify) {
    IRNode *p = add_list(arena, parent, "property");
    if (!p || !add_string(arena, p, name) || !add_string(arena, p, value) ||
        !add_at(arena, p, x, y, angle) || !add_font_effects(arena, p, hidden, justify)) return 0;
    return 1;
}

static int add_label(Arena *arena, IRNode *root, const char *name,
                     double x, double y) {
    char uuid[37]; make_uuid(uuid);
    IRNode *n = add_list(arena, root, "label");
    if (!n || !add_string(arena, n, name) || !add_at(arena, n, x, y, 0)) return 0;
    IRNode *effects = add_list(arena, n, "effects");
    IRNode *font = effects ? add_list(arena, effects, "font") : NULL;
    IRNode *size = font ? add_list(arena, font, "size") : NULL;
    if (!size || !add_num(arena, size, 1.27) || !add_num(arena, size, 1.27)) return 0;
    IRNode *justify = add_list(arena, effects, "justify");
    if (!justify || !add_atom(arena, justify, "left") || !add_atom(arena, justify, "bottom")) return 0;
    return add_uuid(arena, n, "uuid", uuid);
}

static int add_wire(Arena *arena, IRNode *root, double x1, double y1, double x2, double y2) {
    char uuid[37]; make_uuid(uuid);
    IRNode *wire = add_list(arena, root, "wire");
    IRNode *pts = wire ? add_list(arena, wire, "pts") : NULL;
    IRNode *a = pts ? add_list(arena, pts, "xy") : NULL;
    IRNode *b = pts ? add_list(arena, pts, "xy") : NULL;
    if (!a || !b || !add_num(arena, a, x1) || !add_num(arena, a, y1) ||
        !add_num(arena, b, x2) || !add_num(arena, b, y2)) return 0;
    IRNode *stroke = add_list(arena, wire, "stroke");
    IRNode *width = stroke ? add_list(arena, stroke, "width") : NULL;
    IRNode *type = stroke ? add_list(arena, stroke, "type") : NULL;
    if (!width || !add_atom(arena, width, "0") || !type || !add_atom(arena, type, "default")) return 0;
    return add_uuid(arena, wire, "uuid", uuid);
}

static int join_path(char *out, size_t cap, const char *a, const char *b) {
    if (!out || cap == 0 || !a || !b) return 0;
    int n = snprintf(out, cap, "%s%c%s", a, HWC_PATH_SEP, b);
    return n > 0 && (size_t)n < cap;
}

static int find_symbol_dir(char *out, size_t cap) {
    const char *env = getenv("KICAD_SYMBOL_DIR");
    if (env && env[0]) { snprintf(out, cap, "%s", env); return 1; }
#ifdef _WIN32
    const char *pf = getenv("ProgramFiles");
    if (!pf || !*pf) pf = "C:\\Program Files";
    const char *versions[] = {"10.0", "9.0", "8.0"};
    for (size_t i = 0; i < sizeof(versions)/sizeof(versions[0]); ++i) {
        int n = snprintf(out, cap, "%s\\KiCad\\%s\\share\\kicad\\symbols", pf, versions[i]);
        if (n > 0 && (size_t)n < cap) {
            char candidate[1024];
            if (join_path(candidate, sizeof(candidate), out, "Device.kicad_sym")) {
                FILE *f = fopen(candidate, "rb");
                if (f) { fclose(f); return 1; }
            }
        }
    }
#else
    const char *candidates[] = {"/usr/share/kicad/symbols", "/usr/local/share/kicad/symbols",
        "/Applications/KiCad/KiCad.app/Contents/SharedSupport/symbols"};
    for (size_t i = 0; i < sizeof(candidates)/sizeof(candidates[0]); ++i) {
        char candidate[1024];
        if (join_path(candidate, sizeof(candidate), candidates[i], "Device.kicad_sym")) {
            FILE *f = fopen(candidate, "rb");
            if (f) { fclose(f); snprintf(out, cap, "%s", candidates[i]); return 1; }
        }
    }
#endif
    out[0] = '\0';
    return 0;
}

static IRNode *clone_symbol_definition(Arena *arena, const char *lib_id,
                                       char *error, size_t error_cap) {
    char library[128], local_name[128];
    const char *colon = strchr(lib_id, ':');
    if (!colon || (size_t)(colon - lib_id) >= sizeof(library) || strlen(colon + 1) >= sizeof(local_name)) {
        errset(error, error_cap, "symbol library ID must be Library:Symbol");
        return NULL;
    }
    memcpy(library, lib_id, (size_t)(colon - lib_id));
    library[colon - lib_id] = '\0';
    snprintf(local_name, sizeof(local_name), "%s", colon + 1);

    char symbol_dir[1024], file_name[256], path[1400];
    int have_dir = find_symbol_dir(symbol_dir, sizeof(symbol_dir));
    IRNode *definition = NULL;
    Arena source_arena;
    arena_init(&source_arena, 65536);
    IRError parse_error;

    if (have_dir) {
        snprintf(file_name, sizeof(file_name), "%s.kicad_sym", library);
        if (!join_path(path, sizeof(path), symbol_dir, file_name)) {
            arena_destroy(&source_arena);
            errset(error, error_cap, "KiCad symbol path is too long");
            return NULL;
        }
        IRNode *lib_root = ir_parse_file(&source_arena, path, &parse_error);
        if (lib_root) {
            const IRNode *found = ir_list_find_named(lib_root, "symbol", local_name);
            if (found) definition = ir_node_clone(arena, found, &parse_error);
        }
    }

    /* Portable regression fallback: project ships a complete Device:R symbol
     * expression sourced from KiCad 9.0.6 for parser/exporter tests. */
    if (!definition && strcmp(lib_id, "Device:R") == 0) {
        const char *fallbacks[] = {
            "tests/fixtures/kicad9/symbols/R.symbol.txt",
            "tests\\fixtures\\kicad9\\symbols\\R.symbol.txt"
        };
#ifdef HWC_SOURCE_DIR
        char absolute_fallback[1400];
        (void)snprintf(absolute_fallback, sizeof(absolute_fallback),
                       "%s/tests/fixtures/kicad9/symbols/R.symbol.txt", HWC_SOURCE_DIR);
        IRNode *absolute_fixture = ir_parse_file(&source_arena, absolute_fallback, &parse_error);
        if (absolute_fixture && ir_list_head(absolute_fixture) &&
            ir_list_head(absolute_fixture)->text &&
            strcmp(ir_list_head(absolute_fixture)->text, "symbol") == 0) {
            const IRNode *name = ir_child_at(absolute_fixture, 1);
            if (name && name->text && strcmp(name->text, local_name) == 0)
                definition = ir_node_clone(arena, absolute_fixture, &parse_error);
        }
#endif
        for (size_t i = 0; !definition && i < sizeof(fallbacks)/sizeof(fallbacks[0]); ++i) {
            IRNode *fixture = ir_parse_file(&source_arena, fallbacks[i], &parse_error);
            if (fixture && ir_list_head(fixture) && ir_list_head(fixture)->text &&
                strcmp(ir_list_head(fixture)->text, "symbol") == 0) {
                const IRNode *name = ir_child_at(fixture, 1);
                if (name && name->text && strcmp(name->text, local_name) == 0) {
                    definition = ir_node_clone(arena, fixture, &parse_error);
                    break;
                }
            }
        }
    }
    arena_destroy(&source_arena);
    if (!definition) {
        char message[256];
        snprintf(message, sizeof(message), "cannot resolve KiCad symbol %s; set KICAD_SYMBOL_DIR to your symbols directory", lib_id);
        errset(error, error_cap, message);
        return NULL;
    }

    /* Embedded definitions use a fully-qualified name like Device:R. */
    IRNode *name_node = (IRNode *)ir_child_at(definition, 1);
    if (!name_node || ir_node_detach(name_node) != IR_STATUS_OK) {
        errset(error, error_cap, "symbol definition has an unexpected structure");
        return NULL;
    }
    IRNode *insert_before = (IRNode *)ir_child_at(definition, 1);
    IRNode *qualified_name = ir_node_string(arena, lib_id);
    if (!qualified_name || ir_list_insert_before(definition, insert_before, qualified_name) != IR_STATUS_OK) {
        errset(error, error_cap, "failed to qualify embedded symbol name");
        return NULL;
    }
    return definition;
}

static const Net *find_net_name(const Design *design, const char *name) {
    if (!design || !name) return NULL;
    for (size_t i = 0; i < vec_len(design->nets); ++i) {
        const Net *net = &design->nets[i];
        if (net->name && strcmp(net->name, name) == 0) return net;
    }
    return NULL;
}

static int pin_is_on_net(const Component *component, uint16_t pin_number,
                         const Net *net) {
    const Pin *pin = component_find_pin_const(component, pin_number);
    return component && pin && net && pin->net_id >= 0 &&
           (uint32_t)pin->net_id == net->id;
}

static int divider_topology_matches(const Design *design,
                                    const Component *r1,
                                    const Component *r2,
                                    char *error, size_t error_capacity) {
    const Net *vin = find_net_name(design, "VIN");
    const Net *vout = find_net_name(design, "VOUT");
    const Net *gnd = find_net_name(design, "GND");
    const Component *source = NULL;
    for (size_t i = 0; i < vec_len(design->components); ++i) {
        const Component *c = &design->components[i];
        if (c->model.kind == COMPONENT_SOURCE && strcmp(c->reference, "VSIM") == 0) {
            source = c;
            break;
        }
    }
    if (!vin || !vout || !gnd || !source) {
        errset(error, error_capacity,
               "divider export requires VIN, VOUT, GND nets and VSIM excitation in the Design");
        return 0;
    }
    if (!pin_is_on_net(source, 1, vin) || !pin_is_on_net(source, 2, gnd) ||
        !pin_is_on_net(r1, 1, vin) || !pin_is_on_net(r1, 2, vout) ||
        !pin_is_on_net(r2, 1, vout) || !pin_is_on_net(r2, 2, gnd)) {
        errset(error, error_capacity,
               "divider topology mismatch: expected VSIM(VIN,GND), R1(VIN,VOUT), R2(VOUT,GND)");
        return 0;
    }
    return 1;
}

static char *resistance_text(double ohms, char out[48]) {
    if (ohms >= 1e6 && fmod(ohms, 1e6) == 0.0) snprintf(out, 48, "%.0fM", ohms / 1e6);
    else if (ohms >= 1e6) snprintf(out, 48, "%.6gM", ohms / 1e6);
    else if (ohms >= 1e3 && fmod(ohms, 1e3) == 0.0) snprintf(out, 48, "%.0fk", ohms / 1e3);
    else if (ohms >= 1e3) snprintf(out, 48, "%.6gk", ohms / 1e3);
    else snprintf(out, 48, "%.6g", ohms);
    return out;
}

static int add_resistor_instance(Arena *arena, IRNode *root, const Component *component,
                                 double x, double y, const char *root_uuid,
                                 const char *project_name) {
    char uuid[37], pin1_uuid[37], pin2_uuid[37], rval[48];
    make_uuid(uuid); make_uuid(pin1_uuid); make_uuid(pin2_uuid);
    IRNode *symbol = add_list(arena, root, "symbol");
    IRNode *lib_id = symbol ? add_list(arena, symbol, "lib_id") : NULL;
    if (!lib_id || !add_string(arena, lib_id, "Device:R") || !add_at(arena, symbol, x, y, 0)) return 0;
    IRNode *unit = add_list(arena, symbol, "unit");
    if (!unit || !add_atom(arena, unit, "1")) return 0;
    const char *flags[] = {"exclude_from_sim", "in_bom", "on_board", "dnp", "fields_autoplaced"};
    const char *flag_values[] = {"no", "yes", "yes", "no", "yes"};
    for (size_t i = 0; i < sizeof(flags)/sizeof(flags[0]); ++i) {
        IRNode *f = add_list(arena, symbol, flags[i]);
        if (!f || !add_atom(arena, f, flag_values[i])) return 0;
    }
    if (!add_uuid(arena, symbol, "uuid", uuid)) return 0;
    resistance_text(component->value, rval);
    if (!add_property(arena, symbol, "Reference", component->reference, x + 4.5, y - 1.27, 0, 0, NULL) ||
        !add_property(arena, symbol, "Value", rval, x + 4.5, y + 1.27, 0, 0, NULL) ||
        !add_property(arena, symbol, "Footprint", component->footprint ? component->footprint : "", x, y, 0, 1, NULL) ||
        !add_property(arena, symbol, "Datasheet", "~", x, y, 0, 1, NULL) ||
        !add_property(arena, symbol, "Description", "Resistor", x, y, 0, 1, NULL)) return 0;
    IRNode *pin1 = add_list(arena, symbol, "pin");
    IRNode *pin2 = add_list(arena, symbol, "pin");
    if (!pin1 || !add_string(arena, pin1, "1") || !add_uuid(arena, pin1, "uuid", pin1_uuid) ||
        !pin2 || !add_string(arena, pin2, "2") || !add_uuid(arena, pin2, "uuid", pin2_uuid)) return 0;
    IRNode *instances = add_list(arena, symbol, "instances");
    IRNode *project = instances ? add_list(arena, instances, "project") : NULL;
    IRNode *path = project ? add_list(arena, project, "path") : NULL;
    if (!project || !add_string(arena, project, project_name) || !path || !add_string(arena, path, root_uuid)) return 0;
    IRNode *reference = add_list(arena, path, "reference");
    IRNode *iu = add_list(arena, path, "unit");
    return reference && add_string(arena, reference, component->reference) && iu && add_atom(arena, iu, "1");
}

static int add_text(Arena *arena, IRNode *root, const char *text, double x, double y) {
    char uuid[37]; make_uuid(uuid);
    IRNode *n = add_list(arena, root, "text");
    if (!n || !add_string(arena, n, text) || !add_at(arena, n, x, y, 0)) return 0;
    IRNode *effects = add_list(arena, n, "effects");
    IRNode *font = effects ? add_list(arena, effects, "font") : NULL;
    IRNode *size = font ? add_list(arena, font, "size") : NULL;
    return size && add_num(arena, size, 1.27) && add_num(arena, size, 1.27) && add_uuid(arena, n, "uuid", uuid);
}

static int make_parent_dir(const char *path) {
    char tmp[1024];
    const char *last1 = strrchr(path, '/');
    const char *last2 = strrchr(path, '\\');
    const char *last = last1;
    if (last2 && (!last || last2 > last)) last = last2;
    if (!last) return 1;
    size_t n = (size_t)(last - path);
    if (n == 0 || n >= sizeof(tmp)) return 0;
    memcpy(tmp, path, n); tmp[n] = '\0';
#ifdef _WIN32
    return _mkdir(tmp) == 0 || errno == EEXIST;
#else
    return mkdir(tmp, 0775) == 0 || errno == EEXIST;
#endif
}

int hwc_kicad_schematic_write(const char *path, const Design *design,
                              const HwcIntent *intent,
                              char *error, size_t error_capacity) {
    if (!path || !design || !intent) { errset(error, error_capacity, "invalid schematic export arguments"); return 0; }

    const Component *resistors[2] = {NULL, NULL};
    size_t resistor_count = 0;
    for (size_t i = 0; i < vec_len(design->components); ++i) {
        const Component *c = &design->components[i];
        if (c->model.kind == COMPONENT_RESISTOR) {
            if (resistor_count >= 2) { errset(error, error_capacity, "V1 schematic backend supports at most two resistors"); return 0; }
            if (!c->symbol || strcmp(c->symbol, "Device:R") != 0) { errset(error, error_capacity, "V1 schematic backend currently supports Device:R only"); return 0; }
            resistors[resistor_count++] = c;
        }
    }
    const size_t expected = intent->kind == HWC_INTENT_RESISTOR_DIVIDER ? 2u : 1u;
    if (resistor_count != expected) { errset(error, error_capacity, "Design resistor count does not match parsed intent"); return 0; }

    if (intent->kind == HWC_INTENT_RESISTOR_DIVIDER) {
        DiagnosticList diagnostics;
        diagnostic_list_init(&diagnostics);
        int structural_errors = design_validate_structure(design, &diagnostics);
        diagnostic_list_free(&diagnostics);
        if (structural_errors != 0) {
            errset(error, error_capacity,
                   "divider schematic export blocked: Design has structural pin/net errors; run 'validate'");
            return 0;
        }
        if (!divider_topology_matches(design, resistors[0], resistors[1],
                                      error, error_capacity)) return 0;
    }

    Arena arena;
    arena_init(&arena, 65536);
    IRNode *root = new_list(&arena, "kicad_sch");
    if (!root) goto oom;
    IRNode *version = add_list(&arena, root, "version");
    if (!version || !add_atom(&arena, version, "20250114")) goto oom;
    IRNode *generator = add_list(&arena, root, "generator");
    if (!generator || !add_string(&arena, generator, "hardware_compiler")) goto oom;
    IRNode *gv = add_list(&arena, root, "generator_version");
    if (!gv || !add_string(&arena, gv, "0.3.0")) goto oom;
    char root_uuid[37]; make_uuid(root_uuid);
    if (!add_uuid(&arena, root, "uuid", root_uuid)) goto oom;
    IRNode *paper = add_list(&arena, root, "paper");
    if (!paper || !add_string(&arena, paper, "A4")) goto oom;
    IRNode *lib_symbols = add_list(&arena, root, "lib_symbols");
    if (!lib_symbols) goto oom;
    IRNode *r_symbol = clone_symbol_definition(&arena, "Device:R", error, error_capacity);
    if (!r_symbol || !append(lib_symbols, r_symbol)) { if (!error || !error[0]) errset(error, error_capacity, "failed to embed Device:R"); goto fail; }

    const char *project_name = strrchr(path, '/');
    const char *last_backslash = strrchr(path, '\\');
    if (!project_name || (last_backslash && last_backslash > project_name)) project_name = last_backslash;
    project_name = project_name ? project_name + 1 : path;
    char project_buf[256]; snprintf(project_buf, sizeof(project_buf), "%s", project_name);
    char *ext = strrchr(project_buf, '.'); if (ext) *ext = '\0';

    if (intent->kind == HWC_INTENT_RESISTOR_DIVIDER) {
        char title[192];
        snprintf(title, sizeof(title), "Hardware Compiler | resistor divider | VIN %.6g V | target %.6g V",
                 intent->input_voltage_v, intent->target_voltage_v);
        if (!add_text(&arena, root, title, 90.0, 42.0)) goto oom;
        const double x = 100.0, y1 = 60.0, y2 = 85.0;
        if (!add_wire(&arena, root, x, y1 + 3.81, x, y2 - 3.81) ||
            !add_label(&arena, root, "VIN", x, y1 - 3.81) ||
            !add_label(&arena, root, "VOUT", x, (y1 + 3.81 + y2 - 3.81) * 0.5) ||
            !add_label(&arena, root, "GND", x, y2 + 3.81)) goto oom;
        if (!add_resistor_instance(&arena, root, resistors[0], x, y1, root_uuid, project_buf) ||
            !add_resistor_instance(&arena, root, resistors[1], x, y2, root_uuid, project_buf)) goto oom;
    } else {
        if (!add_text(&arena, root, "Hardware Compiler | unconnected resistor template", 88.0, 50.0) ||
            !add_resistor_instance(&arena, root, resistors[0], 100.0, 75.0, root_uuid, project_buf)) goto oom;
    }

    IRNode *sheet_instances = add_list(&arena, root, "sheet_instances");
    IRNode *sheet_path = sheet_instances ? add_list(&arena, sheet_instances, "path") : NULL;
    if (!sheet_path || !add_string(&arena, sheet_path, "/")) goto oom;
    IRNode *page = add_list(&arena, sheet_path, "page");
    if (!page || !add_string(&arena, page, "1")) goto oom;
    IRNode *ef = add_list(&arena, root, "embedded_fonts");
    if (!ef || !add_atom(&arena, ef, "no")) goto oom;

    if (ir_validate_tree(root, NULL) != IR_STATUS_OK) {
        errset(error, error_capacity, "constructed schematic IR failed its tree-invariant check");
        goto fail;
    }
    if (!make_parent_dir(path)) { errset(error, error_capacity, "could not create output directory"); goto fail; }
    FILE *f = fopen(path, "wb");
    if (!f) { errset(error, error_capacity, "could not open schematic output path"); goto fail; }
    IRWriteOptions opts = ir_write_options_default();
    opts.pretty = true; opts.indent_width = 2; opts.final_newline = true;
    IRStatus status = ir_write(f, root, &opts, NULL);
    if (fclose(f) != 0 || status != IR_STATUS_OK) { errset(error, error_capacity, "failed while writing schematic file"); remove(path); goto fail; }
    arena_destroy(&arena);
    return 1;

oom:
    errset(error, error_capacity, "out of memory while constructing schematic IR");
fail:
    arena_destroy(&arena);
    return 0;
}

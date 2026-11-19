#include "schematic.h"
#include "filesystem.h"

#include <stdio.h>
#include <string.h>

static const IRNode *sch_head(const IRNode *n) { return ir_list_head(n); }
static bool sch_is_list(const IRNode *n, const char *name) {
    const IRNode *h = sch_head(n);
    return n && n->kind == IR_NODE_LIST && h && h->kind == IR_NODE_ATOM &&
           strcmp(h->text, name) == 0;
}
static const char *sch_text_at(const IRNode *n, size_t index) {
    const IRNode *v = ir_child_at(n, index);
    return v && (v->kind == IR_NODE_ATOM || v->kind == IR_NODE_STRING) ? v->text : NULL;
}
static const IRNode *sch_field(const IRNode *n, const char *name) {
    return ir_list_find(n, name);
}
static const char *sch_field_text(const IRNode *n, const char *name) {
    return sch_text_at(sch_field(n, name), 1);
}
static const char *sch_property(const IRNode *n, const char *name) {
    return sch_text_at(ir_list_find_named(n, "property", name), 2);
}
static bool sch_u32(const IRNode *n, size_t index, uint32_t *out) {
    uint64_t value = 0;
    if (!n || !out || ir_atom_to_u64(ir_child_at(n, index), &value) != IR_STATUS_OK ||
        value > UINT32_MAX) return false;
    *out = (uint32_t)value;
    return true;
}
static bool sch_bool_field(const IRNode *n, const char *name, bool fallback) {
    const IRNode *field = sch_field(n, name);
    bool value = fallback;
    if (field && ir_atom_to_bool(ir_child_at(field, 1), &value) == IR_STATUS_OK)
        return value;
    return fallback;
}
static bool sch_at(const IRNode *n, G2Point *position, int32_t *rotation) {
    const IRNode *at = sch_field(n, "at");
    int64_t x = 0, y = 0, angle = 0;
    if (!at || ir_atom_to_nm(ir_child_at(at, 1), &x) != IR_STATUS_OK ||
        ir_atom_to_nm(ir_child_at(at, 2), &y) != IR_STATUS_OK) return false;
    if (rotation && ir_atom_to_i64(ir_child_at(at, 3), &angle) != IR_STATUS_OK)
        angle = 0;
    if (angle < INT32_MIN / 1000LL || angle > INT32_MAX / 1000LL) return false;
    position->x = x;
    position->y = y;
    if (rotation) *rotation = (int32_t)(angle * 1000LL);
    return true;
}
static void sch_diag(SchematicDiag *diag, SchematicStatus status,
                     const char *message) {
    if (!diag) return;
    diag->status = status;
    snprintf(diag->message, sizeof(diag->message), "%s", message ? message : "");
}
static size_t count_direct(const IRNode *n, const char *name) {
    size_t count = 0;
    if (!n || n->kind != IR_NODE_LIST) return 0;
    for (const IRNode *c = n->first_child ? n->first_child->next : NULL;
         c; c = c->next) if (sch_is_list(c, name)) ++count;
    return count;
}
static size_t count_library_pins_recursive(const IRNode *n) {
    size_t count = 0;
    if (!n || n->kind != IR_NODE_LIST) return 0;
    for (const IRNode *c = n->first_child; c; c = c->next) {
        if (sch_is_list(c, "pin")) ++count;
        else if (c->kind == IR_NODE_LIST) count += count_library_pins_recursive(c);
    }
    return count;
}
static uint32_t sub_symbol_unit(const char *name, uint32_t fallback) {
    if (!name) return fallback;
    const char *last = strrchr(name, '_');
    if (!last || last == name) return fallback;
    const char *previous = last;
    while (previous > name && previous[-1] != '_') --previous;
    if (previous == name || previous[-1] != '_') return fallback;
    uint32_t value = 0;
    for (const char *p = previous; p < last; ++p) {
        if (*p < '0' || *p > '9') return fallback;
        uint32_t digit = (uint32_t)(*p - '0');
        if (value > (UINT32_MAX - digit) / 10u) return fallback;
        value = value * 10u + digit;
    }
    return value;
}
static void fill_library_pins(const IRNode *n, SchematicIR *out,
                              size_t *cursor, unsigned depth, uint32_t unit) {
    if (!n || n->kind != IR_NODE_LIST || depth > 128u) return;
    for (const IRNode *c = n->first_child; c; c = c->next) {
        if (sch_is_list(c, "pin")) {
            SchematicLibPin *pin = &out->library_pins[(*cursor)++];
            pin->electrical_type = sch_text_at(c, 1);
            pin->number = sch_text_at(sch_field(c, "number"), 1);
            pin->name = sch_text_at(sch_field(c, "name"), 1);
            pin->unit = unit;
        } else if (sch_is_list(c, "symbol")) {
            fill_library_pins(c, out, cursor, depth + 1u,
                              sub_symbol_unit(sch_text_at(c, 1), unit));
        } else if (c->kind == IR_NODE_LIST) {
            fill_library_pins(c, out, cursor, depth + 1u, unit);
        }
    }
}
static size_t count_symbol_instance_pins(const IRNode *symbol) {
    return count_direct(symbol, "pin");
}
static size_t count_wire_points(const IRNode *wire) {
    const IRNode *pts = sch_field(wire, "pts");
    size_t count = 0;
    if (!pts) return 0;
    for (const IRNode *p = pts->first_child ? pts->first_child->next : NULL;
         p; p = p->next) if (sch_is_list(p, "xy")) ++count;
    return count;
}
static bool fill_wire_points(const IRNode *wire, SchematicIR *out,
                             size_t *cursor) {
    const IRNode *pts = sch_field(wire, "pts");
    if (!pts) return false;
    for (const IRNode *p = pts->first_child ? pts->first_child->next : NULL;
         p; p = p->next) {
        if (sch_is_list(p, "xy")) {
            int64_t x = 0, y = 0;
            if (ir_atom_to_nm(ir_child_at(p, 1), &x) != IR_STATUS_OK ||
                ir_atom_to_nm(ir_child_at(p, 2), &y) != IR_STATUS_OK) return false;
            out->wire_points[*cursor].position = (G2Point){x, y};
            ++*cursor;
        }
    }
    return true;
}
static SchematicLabelKind label_kind(const IRNode *n) {
    if (sch_is_list(n, "global_label")) return SCHEMATIC_LABEL_GLOBAL;
    if (sch_is_list(n, "hierarchical_label")) return SCHEMATIC_LABEL_HIERARCHICAL;
    return SCHEMATIC_LABEL_LOCAL;
}
static bool fill_symbol(const IRNode *node, SchematicIR *out,
                        size_t *pin_cursor, SchematicSymbol *symbol) {
    memset(symbol, 0, sizeof(*symbol));
    symbol->raw_node = node;
    symbol->uuid = sch_field_text(node, "uuid");
    symbol->lib_id = sch_field_text(node, "lib_id");
    symbol->reference = sch_property(node, "Reference");
    symbol->value = sch_property(node, "Value");
    symbol->footprint = sch_property(node, "Footprint");
    symbol->datasheet = sch_property(node, "Datasheet");
    symbol->description = sch_property(node, "Description");
    symbol->sheet_path = NULL;
    const IRNode *instances = sch_field(node, "instances");
    if (instances) {
        const IRNode *project = ir_child_at(instances, 1);
        const IRNode *path = sch_field(project, "path");
        symbol->sheet_path = sch_text_at(path, 1);
    }
    if (!sch_at(node, &symbol->position, &symbol->rotation_mdeg))
        return false;
    (void)sch_u32(sch_field(node, "unit"), 1, &symbol->unit);
    symbol->mirrored_x = false;
    symbol->mirrored_y = false;
    const char *mirror = sch_text_at(sch_field(node, "mirror"), 1);
    if (mirror && strcmp(mirror, "x") == 0) symbol->mirrored_x = true;
    if (mirror && strcmp(mirror, "y") == 0) symbol->mirrored_y = true;
    symbol->in_bom = sch_bool_field(node, "in_bom", true);
    symbol->on_board = sch_bool_field(node, "on_board", true);
    symbol->dnp = sch_bool_field(node, "dnp", false);
    symbol->first_pin = (uint32_t)*pin_cursor;
    symbol->pin_count = (uint32_t)count_symbol_instance_pins(node);
    for (const IRNode *pin_node = node->first_child ? node->first_child->next : NULL;
         pin_node; pin_node = pin_node->next) {
        if (sch_is_list(pin_node, "pin")) {
            SchematicSymbolPin *pin = &out->symbol_pins[(*pin_cursor)++];
            pin->number = sch_text_at(pin_node, 1);
            pin->uuid = sch_field_text(pin_node, "uuid");
            if (!pin->number) return false;
        }
    }
    return symbol->uuid && symbol->lib_id && symbol->reference && symbol->value;
}

SchematicStatus schematic_init(SchematicIR *out, Arena *arena) {
    if (!out || !arena) return SCHEMATIC_ERR_ARGUMENT;
    memset(out, 0, sizeof(*out));
    out->arena = arena;
    return SCHEMATIC_OK;
}

SchematicStatus schematic_parse(Arena *arena, const char *source,
                                size_t source_length, SchematicIR *out,
                                SchematicDiag *diag) {
    IRError error;
    const IRNode *lib_symbols;
    size_t library_count = 0, library_pin_count = 0, symbol_count = 0;
    size_t symbol_pin_count = 0, wire_count = 0, wire_point_count = 0;
    size_t label_count = 0, junction_count = 0, no_connect_count = 0;
    size_t sheet_instance_count = 0;
    if (!arena || !source || !out) {
        sch_diag(diag, SCHEMATIC_ERR_ARGUMENT, "arena, source and output are required");
        return SCHEMATIC_ERR_ARGUMENT;
    }
    if (diag) memset(diag, 0, sizeof(*diag));
    schematic_init(out, arena);
    ir_error_clear(&error);
    IRNode *root = ir_parse(arena, source, source_length, &error);
    if (!root) {
        sch_diag(diag, SCHEMATIC_ERR_PARSE,
                 error.message ? error.message : "schematic S-expression parse failed");
        return SCHEMATIC_ERR_PARSE;
    }
    if (!sch_is_list(root, "kicad_sch")) {
        sch_diag(diag, SCHEMATIC_ERR_ROOT, "root expression is not kicad_sch");
        return SCHEMATIC_ERR_ROOT;
    }
    out->source_root = root;
    (void)sch_u32(sch_field(root, "version"), 1, &out->file_version);
    out->generator = sch_field_text(root, "generator");
    out->generator_version = sch_field_text(root, "generator_version");
    out->root_uuid = sch_field_text(root, "uuid");
    out->paper = sch_field_text(root, "paper");
    lib_symbols = sch_field(root, "lib_symbols");
    library_count = count_direct(lib_symbols, "symbol");
    if (lib_symbols) {
        for (const IRNode *n = lib_symbols->first_child ? lib_symbols->first_child->next : NULL;
             n; n = n->next) if (sch_is_list(n, "symbol"))
            library_pin_count += count_library_pins_recursive(n);
    }
    symbol_count = count_direct(root, "symbol");
    wire_count = count_direct(root, "wire");
    label_count = count_direct(root, "label") + count_direct(root, "global_label") +
                  count_direct(root, "hierarchical_label");
    junction_count = count_direct(root, "junction");
    no_connect_count = count_direct(root, "no_connect");
    const IRNode *sheet_instances = sch_field(root, "sheet_instances");
    sheet_instance_count = count_direct(sheet_instances, "path");
    for (const IRNode *n = root->first_child ? root->first_child->next : NULL;
         n; n = n->next) if (sch_is_list(n, "wire")) wire_point_count += count_wire_points(n);

#define SCH_ALLOC(member, count, type) do { \
    out->member = (count) ? (type *)arena_alloc_array(arena, sizeof(type), (count)) : NULL; \
    if ((count) && !out->member) { sch_diag(diag, SCHEMATIC_ERR_MEMORY, "schematic array allocation failed"); return SCHEMATIC_ERR_MEMORY; } \
} while (0)
    SCH_ALLOC(library_symbols, library_count, SchematicLibrarySymbol);
    SCH_ALLOC(library_pins, library_pin_count, SchematicLibPin);
    SCH_ALLOC(symbols, symbol_count, SchematicSymbol);
    /* Determine the exact number of placed-symbol pin instances before allocation. */
    symbol_pin_count = 0;
    for (const IRNode *n = root->first_child ? root->first_child->next : NULL; n; n = n->next)
        if (sch_is_list(n, "symbol")) symbol_pin_count += count_symbol_instance_pins(n);
    out->symbol_pin_count = symbol_pin_count;
    out->symbol_pins = symbol_pin_count ? (SchematicSymbolPin *)arena_alloc_array(arena, sizeof(SchematicSymbolPin), symbol_pin_count) : NULL;
    if (symbol_pin_count && !out->symbol_pins) return SCHEMATIC_ERR_MEMORY;
    SCH_ALLOC(wires, wire_count, SchematicWire);
    SCH_ALLOC(wire_points, wire_point_count, SchematicPoint);
    SCH_ALLOC(labels, label_count, SchematicLabel);
    SCH_ALLOC(junctions, junction_count, SchematicJunction);
    SCH_ALLOC(no_connects, no_connect_count, SchematicNoConnect);
    SCH_ALLOC(sheet_instances, sheet_instance_count, SchematicSheetInstance);
#undef SCH_ALLOC
    out->library_symbol_count = library_count;
    out->library_pin_count = library_pin_count;
    out->symbol_count = symbol_count;
    out->wire_count = wire_count;
    out->wire_point_count = wire_point_count;
    out->label_count = label_count;
    out->junction_count = junction_count;
    out->no_connect_count = no_connect_count;
    out->sheet_instance_count = sheet_instance_count;

    size_t li = 0, lp = 0;
    if (lib_symbols) for (const IRNode *n = lib_symbols->first_child ? lib_symbols->first_child->next : NULL;
                         n; n = n->next) if (sch_is_list(n, "symbol")) {
        SchematicLibrarySymbol *lib = &out->library_symbols[li++];
        lib->lib_id = sch_text_at(n, 1);
        lib->reference_prefix = sch_property(n, "Reference");
        lib->default_value = sch_property(n, "Value");
        lib->description = sch_property(n, "Description");
        lib->first_pin = (uint32_t)lp;
        lib->pin_count = (uint32_t)count_library_pins_recursive(n);
        lib->raw_node = n;
        fill_library_pins(n, out, &lp, 0, 0);
    }
    size_t si = 0, sp = 0, wi = 0, wp = 0, lbi = 0, ji = 0, nci = 0;
    for (const IRNode *n = root->first_child ? root->first_child->next : NULL; n; n = n->next) {
        if (sch_is_list(n, "symbol")) {
            if (!fill_symbol(n, out, &sp, &out->symbols[si++])) {
                sch_diag(diag, SCHEMATIC_ERR_PARSE, "invalid placed symbol record");
                return SCHEMATIC_ERR_PARSE;
            }
        } else if (sch_is_list(n, "wire")) {
            SchematicWire *wire = &out->wires[wi++];
            wire->uuid = sch_field_text(n, "uuid");
            wire->first_point = (uint32_t)wp;
            wire->point_count = (uint32_t)count_wire_points(n);
            wire->raw_node = n;
            if (!fill_wire_points(n, out, &wp)) {
                sch_diag(diag, SCHEMATIC_ERR_PARSE, "invalid wire point");
                return SCHEMATIC_ERR_PARSE;
            }
        } else if (sch_is_list(n, "label") || sch_is_list(n, "global_label") ||
                   sch_is_list(n, "hierarchical_label")) {
            SchematicLabel *label = &out->labels[lbi++];
            label->kind = label_kind(n);
            label->text = sch_text_at(n, 1);
            label->uuid = sch_field_text(n, "uuid");
            if (!sch_at(n, &label->position, &label->rotation_mdeg)) {
                sch_diag(diag, SCHEMATIC_ERR_PARSE, "invalid schematic label position");
                return SCHEMATIC_ERR_PARSE;
            }
            label->raw_node = n;
        } else if (sch_is_list(n, "junction")) {
            SchematicJunction *j = &out->junctions[ji++];
            j->uuid = sch_field_text(n, "uuid");
            if (!sch_at(n, &j->position, NULL)) {
                sch_diag(diag, SCHEMATIC_ERR_PARSE, "invalid junction position");
                return SCHEMATIC_ERR_PARSE;
            }
        } else if (sch_is_list(n, "no_connect")) {
            SchematicNoConnect *nc = &out->no_connects[nci++];
            nc->uuid = sch_field_text(n, "uuid");
            if (!sch_at(n, &nc->position, NULL)) {
                sch_diag(diag, SCHEMATIC_ERR_PARSE, "invalid no-connect position");
                return SCHEMATIC_ERR_PARSE;
            }
        }
    }
    size_t shi = 0;
    if (sheet_instances) for (const IRNode *p = sheet_instances->first_child ? sheet_instances->first_child->next : NULL;
                             p; p = p->next) if (sch_is_list(p, "path")) {
        SchematicSheetInstance *si_rec = &out->sheet_instances[shi++];
        si_rec->path = sch_text_at(p, 1);
        si_rec->page = sch_text_at(sch_field(p, "page"), 1);
    }

    /* Preserve unknown root-level record counts for honest import coverage. */
    for (const IRNode *n = root->first_child ? root->first_child->next : NULL; n; n = n->next) {
        if (n->kind != IR_NODE_LIST) continue;
        const char *h = sch_text_at(n, 0);
        bool known = h && (!strcmp(h, "lib_symbols") || !strcmp(h, "symbol") ||
            !strcmp(h, "wire") || !strcmp(h, "junction") || !strcmp(h, "no_connect") ||
            !strcmp(h, "label") || !strcmp(h, "global_label") ||
            !strcmp(h, "hierarchical_label") || !strcmp(h, "sheet_instances") ||
            !strcmp(h, "version") || !strcmp(h, "generator") ||
            !strcmp(h, "generator_version") || !strcmp(h, "uuid") ||
            !strcmp(h, "paper") || !strcmp(h, "embedded_fonts"));
        if (!known) {
            ++out->unmodelled_top_level_records;
            if (diag) {
                ++diag->unsupported_top_level_records;
                if (!diag->first_unsupported[0] && h)
                    snprintf(diag->first_unsupported, sizeof(diag->first_unsupported), "%s", h);
                if (!out->first_unmodelled_top_level && h)
                    out->first_unmodelled_top_level = h;
            }
        }
    }
    if (diag) sch_diag(diag, SCHEMATIC_OK, "schematic parsed");
    return schematic_validate(out, diag);
}

SchematicStatus schematic_load_kicad(const char *path, Arena *arena,
                                     SchematicIR *out, SchematicDiag *diag) {
    const unsigned char *data = NULL;
    size_t size = 0;
    FsStatus fs;
    if (!path || !arena || !out) return SCHEMATIC_ERR_ARGUMENT;
    fs = fs_read_file_arena(arena, path, &data, &size);
    if (fs != FS_OK) {
        sch_diag(diag, SCHEMATIC_ERR_IO, fs_status_name(fs));
        return SCHEMATIC_ERR_IO;
    }
    return schematic_parse(arena, (const char *)data, size, out, diag);
}

SchematicStatus schematic_validate(const SchematicIR *s, SchematicDiag *diag) {
    if (!s || !s->source_root || !s->root_uuid) {
        sch_diag(diag, SCHEMATIC_ERR_ROOT, "schematic is missing source root or root UUID");
        return SCHEMATIC_ERR_ROOT;
    }
    for (size_t i = 0; i < s->symbol_count; ++i) {
        const SchematicSymbol *sym = &s->symbols[i];
        if (!sym->uuid || !sym->lib_id || !sym->reference || !sym->value) {
            sch_diag(diag, SCHEMATIC_ERR_PARSE, "placed symbol has missing identity fields");
            return SCHEMATIC_ERR_PARSE;
        }
        for (size_t j = i + 1; j < s->symbol_count; ++j) {
            if (strcmp(sym->uuid, s->symbols[j].uuid) == 0) {
                sch_diag(diag, SCHEMATIC_ERR_PARSE, "duplicate placed symbol UUID");
                return SCHEMATIC_ERR_PARSE;
            }
        }
    }
    if (diag) sch_diag(diag, SCHEMATIC_OK, "schematic model is structurally valid");
    return SCHEMATIC_OK;
}

const SchematicLibrarySymbol *schematic_find_library_symbol(
    const SchematicIR *s, const char *lib_id) {
    if (!s || !lib_id) return NULL;
    for (size_t i = 0; i < s->library_symbol_count; ++i)
        if (s->library_symbols[i].lib_id && strcmp(s->library_symbols[i].lib_id, lib_id) == 0)
            return &s->library_symbols[i];
    return NULL;
}
const SchematicSymbol *schematic_find_symbol_uuid(const SchematicIR *s,
                                                    const char *uuid) {
    if (!s || !uuid) return NULL;
    for (size_t i = 0; i < s->symbol_count; ++i)
        if (s->symbols[i].uuid && strcmp(s->symbols[i].uuid, uuid) == 0)
            return &s->symbols[i];
    return NULL;
}
const char *schematic_status_name(SchematicStatus status) {
    static const char *const names[] = {"OK", "ARGUMENT", "IO", "PARSE", "ROOT", "MEMORY", "RANGE"};
    return (unsigned)status < sizeof(names) / sizeof(names[0]) ? names[status] : "UNKNOWN";
}

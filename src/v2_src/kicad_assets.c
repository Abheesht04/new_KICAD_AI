#include "kicad_assets.h"
#include "filesystem.h"

#include <stdlib.h>
#include <string.h>

static void set_error(KicadAssetError *error, KicadAssetStatus code,
                      const char *message, size_t item_index) {
    if (!error) return;
    error->code = code;
    error->message = message;
    error->item_index = item_index;
    ir_error_clear(&error->ir_error);
}

static const IRNode *head_node(const IRNode *node) {
    return ir_list_head(node);
}

static bool is_named_list(const IRNode *node, const char *name) {
    const IRNode *head = head_node(node);
    return node && node->kind == IR_NODE_LIST && head &&
           head->kind == IR_NODE_ATOM && strcmp(head->text, name) == 0;
}

static const char *node_text(const IRNode *node) {
    if (!node || (node->kind != IR_NODE_ATOM && node->kind != IR_NODE_STRING))
        return NULL;
    return node->text;
}

static const char *list_second_text(const IRNode *node) {
    return node_text(ir_child_at(node, 1));
}

static const IRNode *field(const IRNode *node, const char *name) {
    return ir_list_find(node, name);
}

static const char *field_second_text(const IRNode *node, const char *name) {
    return list_second_text(field(node, name));
}

/* KiCad sub-symbol names normally end in _<unit>_<body-style>.
 * For example R_0_1 is common unit 0/body 1; U_2_1 is unit 2/body 1. */
static void symbol_units_from_name(const char *name, uint32_t *out_unit,
                                   uint32_t *out_body_style) {
    if (out_unit) *out_unit = 0;
    if (out_body_style) *out_body_style = 0;
    if (!name) return;
    const char *body_begin = strrchr(name, '_');
    if (!body_begin || body_begin == name) return;
    ++body_begin;

    const char *unit_end = body_begin - 1;
    const char *unit_begin = unit_end;
    while (unit_begin > name && unit_begin[-1] != '_') --unit_begin;
    if (unit_begin == name || unit_begin[-1] != '_' || unit_begin == unit_end)
        return;

    uint32_t unit = 0, body = 0;
    for (const char *p = unit_begin; p < unit_end; ++p) {
        if (*p < '0' || *p > '9') return;
        unsigned digit = (unsigned)(*p - '0');
        if (unit > (UINT32_MAX - digit) / 10u) return;
        unit = unit * 10u + digit;
    }
    for (const char *p = body_begin; *p; ++p) {
        if (*p < '0' || *p > '9') return;
        unsigned digit = (unsigned)(*p - '0');
        if (body > (UINT32_MAX - digit) / 10u) return;
        body = body * 10u + digit;
    }
    if (out_unit) *out_unit = unit;
    if (out_body_style) *out_body_style = body;
}

typedef struct PinWalk {
    Arena *arena;
    KicadSymbolPin *pins;
    size_t capacity;
    size_t count;
    bool failed;
} PinWalk;

static void walk_symbol_pins(PinWalk *walk, const IRNode *node,
                             uint32_t unit, uint32_t body_style,
                             bool root_symbol) {
    if (!node || walk->failed) return;

    if (is_named_list(node, "pin")) {
        const char *electrical = node_text(ir_child_at(node, 1));
        const char *name = field_second_text(node, "name");
        const char *number = field_second_text(node, "number");
        if (!name || !number || !electrical || walk->count >= walk->capacity) {
            walk->failed = true;
            return;
        }
        KicadSymbolPin *pin = &walk->pins[walk->count++];
        pin->number = number;
        pin->name = name;
        pin->electrical_type = electrical;
        pin->unit = unit;
        pin->body_style = body_style;
        return;
    }

    if (!root_symbol && is_named_list(node, "symbol")) {
        const char *sub_name = list_second_text(node);
        symbol_units_from_name(sub_name, &unit, &body_style);
    }

    if (node->kind != IR_NODE_LIST) return;
    for (const IRNode *child = node->first_child; child; child = child->next)
        walk_symbol_pins(walk, child, unit, body_style, false);
}

static size_t count_named_recursive(const IRNode *node, const char *name) {
    if (!node || node->kind != IR_NODE_LIST) return 0;
    size_t count = 0;
    for (const IRNode *child = node->first_child; child; child = child->next) {
        if (is_named_list(child, name)) {
            if (count == SIZE_MAX) return SIZE_MAX;
            ++count;
        }
        size_t nested = count_named_recursive(child, name);
        if (nested > SIZE_MAX - count) return SIZE_MAX;
        count += nested;
    }
    return count;
}

KicadAssetStatus kicad_symbol_library_parse(
    Arena *arena, const char *text, size_t length,
    KicadSymbolLibrary *out, KicadAssetError *error) {
    if (out) memset(out, 0, sizeof(*out));
    if (!arena || !text || length == 0 || !out) {
        set_error(error, KICAD_ASSET_ERR_ARGUMENT, "arena, input and output are required", SIZE_MAX);
        return KICAD_ASSET_ERR_ARGUMENT;
    }

    IRError ir_error = {0};
    IRNode *root = ir_parse(arena, text, length, &ir_error);
    if (!root) {
        set_error(error, KICAD_ASSET_ERR_PARSE, "could not parse KiCad symbol library", SIZE_MAX);
        if (error) error->ir_error = ir_error;
        return KICAD_ASSET_ERR_PARSE;
    }
    if (!is_named_list(root, "kicad_symbol_lib")) {
        set_error(error, KICAD_ASSET_ERR_SCHEMA, "root must be kicad_symbol_lib", SIZE_MAX);
        return KICAD_ASSET_ERR_SCHEMA;
    }

    uint64_t version = 0;
    const IRNode *version_field = field(root, "version");
    if (!version_field || ir_list_length(version_field) != 2 ||
        ir_atom_to_u64(ir_child_at(version_field, 1), &version) != IR_STATUS_OK ||
        version > UINT32_MAX) {
        set_error(error, KICAD_ASSET_ERR_SCHEMA, "missing or invalid symbol library version", SIZE_MAX);
        return KICAD_ASSET_ERR_SCHEMA;
    }

    size_t symbol_count = 0;
    for (const IRNode *child = root->first_child; child; child = child->next) {
        if (is_named_list(child, "symbol")) {
            if (symbol_count == SIZE_MAX) {
                set_error(error, KICAD_ASSET_ERR_RANGE, "symbol count overflow", SIZE_MAX);
                return KICAD_ASSET_ERR_RANGE;
            }
            ++symbol_count;
        }
    }
    KicadSymbolAsset *symbols = symbol_count
        ? (KicadSymbolAsset *)arena_alloc_array(arena, sizeof(*symbols), symbol_count)
        : NULL;
    if (symbol_count && !symbols) {
        set_error(error, KICAD_ASSET_ERR_MEMORY, "could not allocate symbol index", SIZE_MAX);
        return KICAD_ASSET_ERR_MEMORY;
    }
    if (symbols) memset(symbols, 0, sizeof(*symbols) * symbol_count);

    size_t index = 0;
    for (const IRNode *child = root->first_child; child; child = child->next) {
        if (!is_named_list(child, "symbol")) continue;
        KicadSymbolAsset *symbol = &symbols[index];
        symbol->name = list_second_text(child);
        if (!symbol->name || !symbol->name[0]) {
            set_error(error, KICAD_ASSET_ERR_SCHEMA, "symbol has no name", index);
            return KICAD_ASSET_ERR_SCHEMA;
        }
        symbol->extends = field_second_text(child, "extends");
        symbol->pins_inherited = symbol->extends != NULL;
        symbol->footprint_default = NULL;
        symbol->footprint_filter = NULL;
        /* Preserve the exact default footprint separately from wildcard filters. */
        for (const IRNode *property = child->first_child; property; property = property->next) {
            if (!is_named_list(property, "property")) continue;
            const char *property_name = list_second_text(property);
            const char *property_value = node_text(ir_child_at(property, 2));
            if (property_name && property_value) {
                if (strcmp(property_name, "Footprint") == 0) symbol->footprint_default = property_value;
                else if (strcmp(property_name, "ki_fp_filters") == 0) symbol->footprint_filter = property_value;
                else if (strcmp(property_name, "Datasheet") == 0) symbol->datasheet = property_value;
            }
        }

        size_t pin_count = count_named_recursive(child, "pin");
        if (pin_count == SIZE_MAX) {
            set_error(error, KICAD_ASSET_ERR_RANGE, "symbol pin count overflow", index);
            return KICAD_ASSET_ERR_RANGE;
        }
        symbol->pins = pin_count
            ? (KicadSymbolPin *)arena_alloc_array(arena, sizeof(*symbol->pins), pin_count)
            : NULL;
        if (pin_count && !symbol->pins) {
            set_error(error, KICAD_ASSET_ERR_MEMORY, "could not allocate symbol pin index", index);
            return KICAD_ASSET_ERR_MEMORY;
        }
        if (symbol->pins) memset(symbol->pins, 0, sizeof(*symbol->pins) * pin_count);
        PinWalk walk = {arena, symbol->pins, pin_count, 0, false};
        walk_symbol_pins(&walk, child, 0, 0, true);
        if (walk.failed || walk.count != pin_count) {
            set_error(error, KICAD_ASSET_ERR_SCHEMA, "malformed symbol pin definition", index);
            return KICAD_ASSET_ERR_SCHEMA;
        }
        symbol->pin_count = pin_count;

        /* Duplicate number in the same unit is invalid. Repeated numbers across
         * distinct units are allowed by KiCad multi-unit symbols. */
        for (size_t a = 0; a < pin_count; ++a) {
            for (size_t b = a + 1; b < pin_count; ++b) {
                if (symbols[index].pins[a].unit == symbols[index].pins[b].unit &&
                    symbols[index].pins[a].body_style == symbols[index].pins[b].body_style &&
                    strcmp(symbols[index].pins[a].number, symbols[index].pins[b].number) == 0) {
                    set_error(error, KICAD_ASSET_ERR_DUPLICATE, "duplicate symbol pin number within one unit", index);
                    return KICAD_ASSET_ERR_DUPLICATE;
                }
            }
        }
        ++index;
    }

    out->format_version = (uint32_t)version;
    out->symbols = symbols;
    out->symbol_count = symbol_count;
    set_error(error, KICAD_ASSET_OK, NULL, SIZE_MAX);
    return KICAD_ASSET_OK;
}

const KicadSymbolAsset *kicad_symbol_find(
    const KicadSymbolLibrary *library, const char *name) {
    if (!library || !name) return NULL;
    for (size_t i = 0; i < library->symbol_count; ++i)
        if (library->symbols[i].name && strcmp(library->symbols[i].name, name) == 0)
            return &library->symbols[i];
    return NULL;
}

static bool atom_to_nm(const IRNode *node, int64_t *out) {
    return node && ir_atom_to_nm(node, out) == IR_STATUS_OK;
}

static bool atom_to_mdeg(const IRNode *node, int64_t *out) {
    if (!node || !out || node->kind != IR_NODE_ATOM || !node->text || !node->text[0])
        return false;
    const char *p = node->text;
    bool negative = false;
    if (*p == '+' || *p == '-') {
        negative = *p == '-';
        ++p;
    }
    if (!*p) return false;
    uint64_t whole = 0;
    bool any = false;
    while (*p >= '0' && *p <= '9') {
        any = true;
        unsigned digit = (unsigned)(*p++ - '0');
        if (whole > (UINT64_MAX - digit) / 10u) return false;
        whole = whole * 10u + digit;
    }
    if (!any) return false;
    uint64_t fraction = 0;
    unsigned digits = 0;
    if (*p == '.') {
        ++p;
        while (*p >= '0' && *p <= '9') {
            if (digits < 3) {
                fraction = fraction * 10u + (unsigned)(*p - '0');
                ++digits;
            } else if (*p != '0') {
                return false;
            }
            ++p;
        }
    }
    if (*p != '\0') return false;
    while (digits < 3) { fraction *= 10u; ++digits; }
    if (whole > (uint64_t)INT64_MAX / 1000u) return false;
    uint64_t magnitude = whole * 1000u + fraction;
    if (magnitude > (uint64_t)INT64_MAX + (negative ? 1u : 0u)) return false;
    if (negative) {
        *out = magnitude == (uint64_t)INT64_MAX + 1u
            ? INT64_MIN : -(int64_t)magnitude;
    } else {
        *out = (int64_t)magnitude;
    }
    return true;
}

static bool parse_pad(const IRNode *node, Arena *arena, KicadFootprintPad *pad) {
    if (!is_named_list(node, "pad")) return false;
    pad->number = list_second_text(node);
    pad->type = node_text(ir_child_at(node, 2));
    pad->shape = node_text(ir_child_at(node, 3));
    if (!pad->number || !pad->type || !pad->shape) return false;

    const IRNode *at = field(node, "at");
    const IRNode *size = field(node, "size");
    if (!at || ir_list_length(at) < 3 || !size || ir_list_length(size) != 3)
        return false;
    if (!atom_to_nm(ir_child_at(at, 1), &pad->x_nm) ||
        !atom_to_nm(ir_child_at(at, 2), &pad->y_nm) ||
        !atom_to_nm(ir_child_at(size, 1), &pad->size_x_nm) ||
        !atom_to_nm(ir_child_at(size, 2), &pad->size_y_nm))
        return false;
    const IRNode *angle = ir_child_at(at, 3);
    if (angle && !atom_to_mdeg(angle, &pad->rotation_mdeg)) return false;

    const IRNode *drill = field(node, "drill");
    if (drill) {
        const IRNode *value = ir_child_at(drill, 1);
        const IRNode *x = NULL;
        const IRNode *y = NULL;
        if (value && value->kind == IR_NODE_ATOM && strcmp(value->text, "oval") == 0) {
            x = ir_child_at(drill, 2);
            y = ir_child_at(drill, 3);
        } else {
            x = value;
            y = value; /* round drill */
        }
        if (!x || !atom_to_nm(x, &pad->drill_x_nm) ||
            !y || !atom_to_nm(y, &pad->drill_y_nm))
            return false;
        const IRNode *offset = field(drill, "offset");
        if (offset) {
            if (ir_list_length(offset) != 3 ||
                !atom_to_nm(ir_child_at(offset, 1), &pad->drill_offset_x_nm) ||
                !atom_to_nm(ir_child_at(offset, 2), &pad->drill_offset_y_nm))
                return false;
        }
    }
    (void)arena;
    return true;
}

KicadAssetStatus kicad_footprint_parse(
    Arena *arena, const char *text, size_t length,
    KicadFootprintAsset *out, KicadAssetError *error) {
    if (out) memset(out, 0, sizeof(*out));
    if (!arena || !text || length == 0 || !out) {
        set_error(error, KICAD_ASSET_ERR_ARGUMENT, "arena, input and output are required", SIZE_MAX);
        return KICAD_ASSET_ERR_ARGUMENT;
    }

    IRError ir_error = {0};
    IRNode *root = ir_parse(arena, text, length, &ir_error);
    if (!root) {
        set_error(error, KICAD_ASSET_ERR_PARSE, "could not parse KiCad footprint", SIZE_MAX);
        if (error) error->ir_error = ir_error;
        return KICAD_ASSET_ERR_PARSE;
    }
    if (!is_named_list(root, "footprint")) {
        set_error(error, KICAD_ASSET_ERR_SCHEMA, "root must be footprint", SIZE_MAX);
        return KICAD_ASSET_ERR_SCHEMA;
    }

    out->name = list_second_text(root);
    const IRNode *version = field(root, "version");
    uint64_t version_value = 0;
    if (!version || ir_list_length(version) != 2 ||
        ir_atom_to_u64(ir_child_at(version, 1), &version_value) != IR_STATUS_OK ||
        version_value > UINT32_MAX) {
        set_error(error, KICAD_ASSET_ERR_SCHEMA, "footprint has missing or invalid format version", SIZE_MAX);
        return KICAD_ASSET_ERR_SCHEMA;
    }
    out->format_version = (uint32_t)version_value;
    out->description = field_second_text(root, "descr");
    out->tags = field_second_text(root, "tags");
    if (!out->name || !out->name[0]) {
        set_error(error, KICAD_ASSET_ERR_SCHEMA, "footprint has no name", SIZE_MAX);
        return KICAD_ASSET_ERR_SCHEMA;
    }

    size_t count = 0;
    for (const IRNode *child = root->first_child; child; child = child->next) {
        if (is_named_list(child, "pad")) {
            if (count == SIZE_MAX) {
                set_error(error, KICAD_ASSET_ERR_RANGE, "footprint pad count overflow", SIZE_MAX);
                return KICAD_ASSET_ERR_RANGE;
            }
            ++count;
        }
    }
    KicadFootprintPad *pads = count
        ? (KicadFootprintPad *)arena_alloc_array(arena, sizeof(*pads), count)
        : NULL;
    if (count && !pads) {
        set_error(error, KICAD_ASSET_ERR_MEMORY, "could not allocate footprint pad index", SIZE_MAX);
        return KICAD_ASSET_ERR_MEMORY;
    }
    if (pads) memset(pads, 0, sizeof(*pads) * count);
    size_t index = 0;
    for (const IRNode *child = root->first_child; child; child = child->next) {
        if (!is_named_list(child, "pad")) continue;
        if (!parse_pad(child, arena, &pads[index])) {
            set_error(error, KICAD_ASSET_ERR_SCHEMA, "malformed footprint pad", index);
            return KICAD_ASSET_ERR_SCHEMA;
        }
        ++index;
    }

    out->pads = pads;
    out->pad_count = count;
    set_error(error, KICAD_ASSET_OK, NULL, SIZE_MAX);
    return KICAD_ASSET_OK;
}

bool kicad_footprint_has_pad(const KicadFootprintAsset *footprint,
                             const char *pad_number) {
    if (!footprint || !pad_number) return false;
    for (size_t i = 0; i < footprint->pad_count; ++i)
        if (footprint->pads[i].number &&
            strcmp(footprint->pads[i].number, pad_number) == 0) return true;
    return false;
}

static const char *entry_after_colon(const char *library_id) {
    if (!library_id) return NULL;
    const char *colon = strchr(library_id, ':');
    return colon && colon[1] ? colon + 1 : NULL;
}

static bool symbol_has_pin_recursive(const KicadSymbolLibrary *library,
                                     const KicadSymbolAsset *symbol,
                                     const char *number, size_t depth) {
    if (!library || !symbol || !number || depth > 32) return false;
    for (size_t i = 0; i < symbol->pin_count; ++i)
        if (strcmp(symbol->pins[i].number, number) == 0) return true;
    if (symbol->extends) {
        const KicadSymbolAsset *parent = kicad_symbol_find(library, symbol->extends);
        if (parent && parent != symbol)
            return symbol_has_pin_recursive(library, parent, number, depth + 1);
    }
    return false;
}

KicadAssetStatus kicad_part_assets_validate(
    const PartRecord *part,
    const KicadSymbolLibrary *symbol_library,
    const KicadFootprintAsset *footprint,
    KicadAssetError *error) {
    if (!part || !part->symbol_id || !part->footprint_id || !symbol_library || !footprint) {
        set_error(error, KICAD_ASSET_ERR_ARGUMENT, "part, symbol library and footprint are required", SIZE_MAX);
        return KICAD_ASSET_ERR_ARGUMENT;
    }
    const char *symbol_name = entry_after_colon(part->symbol_id);
    const char *footprint_name = entry_after_colon(part->footprint_id);
    if (!symbol_name || !footprint_name) {
        set_error(error, KICAD_ASSET_ERR_SCHEMA, "KiCad asset IDs must be Nickname:EntryName", SIZE_MAX);
        return KICAD_ASSET_ERR_SCHEMA;
    }
    const KicadSymbolAsset *symbol = kicad_symbol_find(symbol_library, symbol_name);
    if (!symbol) {
        set_error(error, KICAD_ASSET_ERR_NOT_FOUND, "symbol entry does not exist in imported library", SIZE_MAX);
        return KICAD_ASSET_ERR_NOT_FOUND;
    }
    if (strcmp(footprint_name, footprint->name) != 0) {
        set_error(error, KICAD_ASSET_ERR_NOT_FOUND, "footprint entry name does not match imported footprint", SIZE_MAX);
        return KICAD_ASSET_ERR_NOT_FOUND;
    }
    if (part->pin_count == 0) {
        set_error(error, KICAD_ASSET_ERR_SCHEMA, "part has no logical terminal-to-pin mappings", SIZE_MAX);
        return KICAD_ASSET_ERR_SCHEMA;
    }
    for (size_t i = 0; i < part->pin_count; ++i) {
        const PartPinMap *map = &part->pins[i];
        if (!symbol_has_pin_recursive(symbol_library, symbol, map->symbol_pin, 0)) {
            set_error(error, KICAD_ASSET_ERR_NOT_FOUND, "mapped symbol pin does not exist", i);
            return KICAD_ASSET_ERR_NOT_FOUND;
        }
        if (!kicad_footprint_has_pad(footprint, map->footprint_pad)) {
            set_error(error, KICAD_ASSET_ERR_NOT_FOUND, "mapped footprint pad does not exist", i);
            return KICAD_ASSET_ERR_NOT_FOUND;
        }
        if (strcmp(map->symbol_pin, map->footprint_pad) != 0) {
            set_error(error, KICAD_ASSET_ERR_SCHEMA,
                      "symbol pin and footprint pad numbers differ; KiCad pin-pad identity must be explicit and equal",
                      i);
            return KICAD_ASSET_ERR_SCHEMA;
        }
    }
    set_error(error, KICAD_ASSET_OK, NULL, SIZE_MAX);
    return KICAD_ASSET_OK;
}


typedef struct SymbolLibraryCacheEntry {
    const char *nickname;
    KicadSymbolLibrary library;
    bool loaded;
    KicadAssetStatus status;
} SymbolLibraryCacheEntry;

static const char *library_id_name(const char *id) {
    if (!id) return NULL;
    const char *colon = strchr(id, ':');
    if (!colon || colon == id || !colon[1]) return NULL;
    return colon + 1;
}

static bool copy_nickname(const char *id, char *out, size_t capacity) {
    if (!id || !out || capacity == 0) return false;
    const char *colon = strchr(id, ':');
    if (!colon || colon == id) return false;
    size_t length = (size_t)(colon - id);
    if (length >= capacity) return false;
    memcpy(out, id, length);
    out[length] = '\0';
    return true;
}

static KicadAssetStatus load_symbol_library_path(
    Arena *arena, const char *symbol_root, const char *nickname,
    KicadSymbolLibrary *out, KicadAssetError *error) {
    char filename[FS_PATH_MAX], path[FS_PATH_MAX];
    if (snprintf(filename, sizeof(filename), "%s.kicad_sym", nickname) < 0 ||
        fs_path_join(path, sizeof(path), symbol_root, filename) != FS_OK) {
        set_error(error, KICAD_ASSET_ERR_RANGE, "symbol library path is too long", SIZE_MAX);
        return KICAD_ASSET_ERR_RANGE;
    }
    void *data = NULL;
    size_t length = 0;
    FsStatus fs = fs_read_file(path, &data, &length);
    if (fs != FS_OK) {
        set_error(error, fs == FS_ERR_NOT_FOUND ? KICAD_ASSET_ERR_NOT_FOUND : KICAD_ASSET_ERR_IO,
                  "could not read KiCad symbol library", SIZE_MAX);
        return fs == FS_ERR_NOT_FOUND ? KICAD_ASSET_ERR_NOT_FOUND : KICAD_ASSET_ERR_IO;
    }
    if (memchr(data, '\0', length) != NULL) {
        free(data);
        set_error(error, KICAD_ASSET_ERR_PARSE, "NUL byte in KiCad symbol library", SIZE_MAX);
        return KICAD_ASSET_ERR_PARSE;
    }
    KicadAssetStatus status = kicad_symbol_library_parse(arena, (const char *)data,
                                                          length, out, error);
    free(data);
    return status;
}

static KicadAssetStatus load_footprint_path(
    Arena *arena, const char *footprint_root, const char *footprint_id,
    KicadFootprintAsset *out, KicadAssetError *error) {
    char nickname[256], directory_name[300], entry_name[512];
    char library_dir[FS_PATH_MAX], path[FS_PATH_MAX];
    if (!copy_nickname(footprint_id, nickname, sizeof(nickname))) {
        set_error(error, KICAD_ASSET_ERR_SCHEMA, "invalid footprint library ID", SIZE_MAX);
        return KICAD_ASSET_ERR_SCHEMA;
    }
    const char *entry = library_id_name(footprint_id);
    if (!entry || snprintf(directory_name, sizeof(directory_name), "%s.pretty", nickname) < 0 ||
        snprintf(entry_name, sizeof(entry_name), "%s.kicad_mod", entry) < 0 ||
        fs_path_join(library_dir, sizeof(library_dir), footprint_root, directory_name) != FS_OK ||
        fs_path_join(path, sizeof(path), library_dir, entry_name) != FS_OK) {
        set_error(error, KICAD_ASSET_ERR_RANGE, "footprint path is too long", SIZE_MAX);
        return KICAD_ASSET_ERR_RANGE;
    }
    void *data = NULL;
    size_t length = 0;
    FsStatus fs = fs_read_file(path, &data, &length);
    if (fs != FS_OK) {
        set_error(error, fs == FS_ERR_NOT_FOUND ? KICAD_ASSET_ERR_NOT_FOUND : KICAD_ASSET_ERR_IO,
                  "could not read KiCad footprint asset", SIZE_MAX);
        return fs == FS_ERR_NOT_FOUND ? KICAD_ASSET_ERR_NOT_FOUND : KICAD_ASSET_ERR_IO;
    }
    if (memchr(data, '\0', length) != NULL) {
        free(data);
        set_error(error, KICAD_ASSET_ERR_PARSE, "NUL byte in KiCad footprint", SIZE_MAX);
        return KICAD_ASSET_ERR_PARSE;
    }
    KicadAssetStatus status = kicad_footprint_parse(arena, (const char *)data,
                                                     length, out, error);
    free(data);
    return status;
}

KicadAssetStatus kicad_part_catalogue_validate_paths(
    Arena *arena, const PartCatalogue *catalogue,
    const char *symbol_root, const char *footprint_root,
    KicadAssetValidationReport *report, KicadAssetError *error) {
    if (report) memset(report, 0, sizeof(*report));
    if (!arena || !catalogue || !symbol_root || !footprint_root || !report) {
        set_error(error, KICAD_ASSET_ERR_ARGUMENT,
                  "arena, catalogue, symbol root, footprint root and report are required",
                  SIZE_MAX);
        return KICAD_ASSET_ERR_ARGUMENT;
    }
    size_t capacity = catalogue->part_count;
    SymbolLibraryCacheEntry *cache = capacity
        ? (SymbolLibraryCacheEntry *)arena_alloc_array(arena, sizeof(*cache), capacity)
        : NULL;
    if (capacity && !cache) {
        set_error(error, KICAD_ASSET_ERR_MEMORY, "could not allocate symbol library cache", SIZE_MAX);
        return KICAD_ASSET_ERR_MEMORY;
    }
    if (cache) memset(cache, 0, sizeof(*cache) * capacity);
    size_t cache_count = 0;
    KicadAssetStatus first_status = KICAD_ASSET_OK;

    for (size_t i = 0; i < catalogue->part_count; ++i) {
        const PartRecord *part = &catalogue->parts[i];
        char nickname[256];
        if (!part->symbol_id || !copy_nickname(part->symbol_id, nickname, sizeof(nickname))) {
            ++report->checked_parts;
            ++report->failed_parts;
            if (first_status == KICAD_ASSET_OK) {
                first_status = KICAD_ASSET_ERR_SCHEMA;
                set_error(error, first_status, "part has an invalid symbol library ID", i);
            }
            continue;
        }

        SymbolLibraryCacheEntry *entry = NULL;
        for (size_t c = 0; c < cache_count; ++c)
            if (strcmp(cache[c].nickname, nickname) == 0) { entry = &cache[c]; break; }
        if (!entry) {
            entry = &cache[cache_count++];
            entry->nickname = arena_alloc(arena, strlen(nickname) + 1);
            if (!entry->nickname) {
                ++report->checked_parts; ++report->failed_parts;
                if (first_status == KICAD_ASSET_OK) {
                    first_status = KICAD_ASSET_ERR_MEMORY;
                    set_error(error, first_status, "could not store symbol library nickname", i);
                }
                continue;
            }
            memcpy((char *)entry->nickname, nickname, strlen(nickname) + 1);
            KicadAssetError local = {0};
            entry->status = load_symbol_library_path(arena, symbol_root, nickname,
                                                      &entry->library, &local);
            entry->loaded = entry->status == KICAD_ASSET_OK;
            if (!entry->loaded && first_status == KICAD_ASSET_OK) {
                first_status = entry->status;
                set_error(error, entry->status, local.message, i);
            }
        }

        ++report->checked_parts;
        if (!entry->loaded) {
            ++report->failed_parts;
            continue;
        }

        KicadFootprintAsset footprint;
        KicadAssetError local = {0};
        KicadAssetStatus status = load_footprint_path(
            arena, footprint_root, part->footprint_id, &footprint, &local);
        if (status == KICAD_ASSET_OK)
            status = kicad_part_assets_validate(part, &entry->library, &footprint, &local);

        if (status == KICAD_ASSET_OK) {
            ++report->passed_parts;
        } else {
            ++report->failed_parts;
            if (first_status == KICAD_ASSET_OK) {
                first_status = status;
                set_error(error, status, local.message, i);
            }
        }
    }

    if (first_status == KICAD_ASSET_OK)
        set_error(error, KICAD_ASSET_OK, NULL, SIZE_MAX);
    return first_status;
}

const char *kicad_asset_status_name(KicadAssetStatus status) {
    static const char *const names[] = {
        "OK", "ARGUMENT_ERROR", "PARSE_ERROR", "SCHEMA_ERROR", "MEMORY_ERROR",
        "NOT_FOUND", "DUPLICATE", "IO_ERROR", "RANGE_ERROR"
    };
    if ((unsigned)status >= sizeof(names) / sizeof(names[0])) return "UNKNOWN";
    return names[status];
}

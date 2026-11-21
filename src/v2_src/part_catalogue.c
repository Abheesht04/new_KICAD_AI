#include "part_catalogue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static void set_error(PartCatalogueError *error,
                      PartCatalogueStatus code,
                      size_t item_index,
                      const char *field,
                      const char *message)
{
    if (!error) return;
    memset(error, 0, sizeof(*error));
    error->code = code;
    error->item_index = item_index;
    error->field = field;
    error->message = message;
    error->ir_error.code = IR_STATUS_OK;
}

static bool is_named_list(const IRNode *node, const char *name)
{
    const IRNode *head = ir_list_head(node);
    return node && node->kind == IR_NODE_LIST &&
           head && head->kind == IR_NODE_ATOM &&
           strcmp(head->text, name) == 0;
}

static size_t count_named(const IRNode *list, const char *name)
{
    size_t count = 0;
    for (const IRNode *child = list ? list->first_child : NULL;
         child; child = child->next) {
        if (is_named_list(child, name)) ++count;
    }
    return count;
}

static bool field_is_one_of(const IRNode *child,
                            const char *const *names,
                            size_t name_count)
{
    const IRNode *head = ir_list_head(child);
    if (!head || head->kind != IR_NODE_ATOM) return false;
    for (size_t i = 0; i < name_count; ++i) {
        if (strcmp(head->text, names[i]) == 0) return true;
    }
    return false;
}

static bool fields_are_known(const IRNode *list,
                             const char *const *names,
                             size_t name_count)
{
    const IRNode *head = ir_list_head(list);
    for (const IRNode *child = list ? list->first_child : NULL;
         child; child = child->next) {
        if (child == head) continue;
        if (!field_is_one_of(child, names, name_count)) return false;
    }
    return true;
}

static const IRNode *unique_field(const IRNode *list,
                                  const char *name,
                                  bool required,
                                  bool *valid)
{
    size_t count = count_named(list, name);
    if (count > 1 || (required && count != 1)) {
        *valid = false;
        return NULL;
    }
    return count == 1 ? ir_list_find(list, name) : NULL;
}

static const char *field_text(const IRNode *list,
                              const char *name,
                              bool required,
                              bool *valid)
{
    const IRNode *field = unique_field(list, name, required, valid);
    if (!field) return NULL;
    if (ir_list_length(field) != 2) {
        *valid = false;
        return NULL;
    }
    const IRNode *value = ir_child_at(field, 1);
    if (!value || (value->kind != IR_NODE_ATOM &&
                   value->kind != IR_NODE_STRING)) {
        *valid = false;
        return NULL;
    }
    if (required && value->text_len == 0) {
        *valid = false;
        return NULL;
    }
    return value->text;
}

static const char *node_text(const IRNode *node)
{
    if (!node || (node->kind != IR_NODE_ATOM &&
                  node->kind != IR_NODE_STRING)) return NULL;
    return node->text;
}

static bool valid_identifier(const char *text)
{
    if (!text || !text[0] || !((text[0] >= 'a' && text[0] <= 'z'))) {
        return false;
    }
    for (size_t i = 1; text[i]; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (!((c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}

static bool valid_model_id(const char *text)
{
    if (!text || !text[0] || text[0] == '.' || text[strlen(text) - 1] == '.') {
        return false;
    }
    bool previous_dot = false;
    for (size_t i = 0; text[i]; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (c == '.') {
            if (previous_dot) return false;
            previous_dot = true;
            continue;
        }
        previous_dot = false;
        if (!((c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}

static bool valid_kicad_id(const char *text)
{
    if (!text || !text[0]) return false;
    const char *colon = strchr(text, ':');
    if (!colon || colon == text || colon[1] == '\0' ||
        strchr(colon + 1, ':')) return false;
    if (strstr(text, "..") || strchr(text, '\\') || strchr(text, '/')) {
        return false;
    }
    return true;
}

static PartKind parse_kind(const char *text)
{
    if (!text) return PART_KIND_INVALID;
    if (!strcmp(text, "R")) return PART_KIND_RESISTOR;
    if (!strcmp(text, "C")) return PART_KIND_CAPACITOR;
    if (!strcmp(text, "L")) return PART_KIND_INDUCTOR;
    if (!strcmp(text, "D")) return PART_KIND_DIODE;
    if (!strcmp(text, "Q")) return PART_KIND_TRANSISTOR;
    if (!strcmp(text, "M")) return PART_KIND_MOSFET;
    if (!strcmp(text, "U")) return PART_KIND_IC;
    if (!strcmp(text, "J")) return PART_KIND_CONNECTOR;
    if (!strcmp(text, "S")) return PART_KIND_SWITCH;
    return PART_KIND_INVALID;
}

static PartRatingName parse_rating_name(const char *text)
{
    if (!text) return PART_RATING_INVALID;
    if (!strcmp(text, "resistance")) return PART_RATING_RESISTANCE;
    if (!strcmp(text, "capacitance")) return PART_RATING_CAPACITANCE;
    if (!strcmp(text, "inductance")) return PART_RATING_INDUCTANCE;
    if (!strcmp(text, "voltage")) return PART_RATING_VOLTAGE;
    if (!strcmp(text, "current")) return PART_RATING_CURRENT;
    if (!strcmp(text, "power")) return PART_RATING_POWER;
    if (!strcmp(text, "tolerance")) return PART_RATING_TOLERANCE;
    if (!strcmp(text, "height")) return PART_RATING_HEIGHT;
    if (!strcmp(text, "dropout")) return PART_RATING_DROPOUT;
    if (!strcmp(text, "quiescent_current")) return PART_RATING_QUIESCENT_CURRENT;
    if (!strcmp(text, "temperature")) return PART_RATING_TEMPERATURE;
    return PART_RATING_INVALID;
}

static PartRatingBound parse_bound(const char *text)
{
    if (!text) return PART_BOUND_INVALID;
    if (!strcmp(text, "nominal")) return PART_BOUND_NOMINAL;
    if (!strcmp(text, "minimum")) return PART_BOUND_MINIMUM;
    if (!strcmp(text, "typical")) return PART_BOUND_TYPICAL;
    if (!strcmp(text, "maximum")) return PART_BOUND_MAXIMUM;
    return PART_BOUND_INVALID;
}

static PartRatingUnit parse_unit(const char *text)
{
    if (!text) return PART_UNIT_INVALID;
    if (!strcmp(text, "uohm")) return PART_UNIT_UOHM;
    if (!strcmp(text, "pF")) return PART_UNIT_PF;
    if (!strcmp(text, "nH")) return PART_UNIT_NH;
    if (!strcmp(text, "uV")) return PART_UNIT_UV;
    if (!strcmp(text, "uA")) return PART_UNIT_UA;
    if (!strcmp(text, "uW")) return PART_UNIT_UW;
    if (!strcmp(text, "ppm")) return PART_UNIT_PPM;
    if (!strcmp(text, "um")) return PART_UNIT_UM;
    if (!strcmp(text, "mC")) return PART_UNIT_MC;
    return PART_UNIT_INVALID;
}

static PartRatingUnit expected_unit(PartRatingName name)
{
    switch (name) {
        case PART_RATING_RESISTANCE: return PART_UNIT_UOHM;
        case PART_RATING_CAPACITANCE: return PART_UNIT_PF;
        case PART_RATING_INDUCTANCE: return PART_UNIT_NH;
        case PART_RATING_VOLTAGE:
        case PART_RATING_DROPOUT: return PART_UNIT_UV;
        case PART_RATING_CURRENT:
        case PART_RATING_QUIESCENT_CURRENT: return PART_UNIT_UA;
        case PART_RATING_POWER: return PART_UNIT_UW;
        case PART_RATING_TOLERANCE: return PART_UNIT_PPM;
        case PART_RATING_HEIGHT: return PART_UNIT_UM;
        case PART_RATING_TEMPERATURE: return PART_UNIT_MC;
        default: return PART_UNIT_INVALID;
    }
}

static bool valid_iso_date(const char *text)
{
    if (!text || strlen(text) != 10 ||
        text[4] != '-' || text[7] != '-') return false;
    for (size_t i = 0; i < 10; ++i) {
        if (i == 4 || i == 7) continue;
        if (text[i] < '0' || text[i] > '9') return false;
    }
    int year = (text[0]-'0')*1000 + (text[1]-'0')*100 +
               (text[2]-'0')*10 + (text[3]-'0');
    int month = (text[5]-'0')*10 + (text[6]-'0');
    int day = (text[8]-'0')*10 + (text[9]-'0');
    if (year < 2000 || month < 1 || month > 12) return false;
    static const int month_days[12] =
        {31,28,31,30,31,30,31,31,30,31,30,31};
    int max_day = month_days[month - 1];
    bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    if (month == 2 && leap) max_day = 29;
    return day >= 1 && day <= max_day;
}

static PartCatalogueStatus parse_source(const IRNode *node,
                                        PartSource *out,
                                        size_t item_index,
                                        const char *field,
                                        PartCatalogueError *error)
{
    bool valid = true;
    PartSource result;
    memset(&result, 0, sizeof(result));

    if (!is_named_list(node, "source") &&
        !is_named_list(node, "identity_source")) {
        set_error(error, PART_CATALOGUE_ERR_SCHEMA, item_index, field,
                  "expected a (source ...) or (identity_source ...) provenance record");
        return PART_CATALOGUE_ERR_SCHEMA;
    }
    static const char *const source_fields[] = {
        "source", "identity_source", "url", "document", "revision", "page",
        "status", "verified_by", "verified_on"
    };
    if (!fields_are_known(node, source_fields,
                          sizeof(source_fields) / sizeof(source_fields[0]))) {
        set_error(error, PART_CATALOGUE_ERR_SCHEMA, item_index, field,
                  "source record contains an unknown field");
        return PART_CATALOGUE_ERR_SCHEMA;
    }

    result.url = field_text(node, "url", true, &valid);
    result.document = field_text(node, "document", true, &valid);
    result.revision = field_text(node, "revision", false, &valid);

    const char *status = field_text(node, "status", true, &valid);
    const char *verified_by = field_text(node, "verified_by", false, &valid);
    const char *verified_on = field_text(node, "verified_on", false, &valid);

    const IRNode *page_node = unique_field(node, "page", false, &valid);
    if (page_node) {
        uint64_t page = 0;
        if (ir_list_length(page_node) != 2 ||
            ir_atom_to_u64(ir_child_at(page_node, 1), &page) != IR_STATUS_OK ||
            page > UINT32_MAX) {
            valid = false;
        } else {
            result.page = (uint32_t)page;
        }
    }

    if (!valid || !result.url || !result.document || !status) {
        set_error(error, PART_CATALOGUE_ERR_SCHEMA, item_index, field,
                  "source requires unique url, document and status fields");
        return PART_CATALOGUE_ERR_SCHEMA;
    }

    if (!strcmp(status, "verified")) {
        if (!verified_by || !valid_iso_date(verified_on)) {
            set_error(error, PART_CATALOGUE_ERR_SCHEMA, item_index, field,
                      "verified source requires verified_by and valid verified_on date");
            return PART_CATALOGUE_ERR_SCHEMA;
        }
        result.status = PART_SOURCE_VERIFIED;
        result.verified_by = verified_by;
        result.verified_on = verified_on;
    } else if (!strcmp(status, "unverified")) {
        if (verified_by || verified_on) {
            set_error(error, PART_CATALOGUE_ERR_SCHEMA, item_index, field,
                      "unverified source must not claim verifier identity or date");
            return PART_CATALOGUE_ERR_SCHEMA;
        }
        result.status = PART_SOURCE_UNVERIFIED;
    } else {
        set_error(error, PART_CATALOGUE_ERR_SCHEMA, item_index, field,
                  "source status must be verified or unverified");
        return PART_CATALOGUE_ERR_SCHEMA;
    }

    *out = result;
    return PART_CATALOGUE_OK;
}

static PartCatalogueStatus parse_pin(const IRNode *node,
                                     PartPinMap *out,
                                     size_t item_index,
                                     PartCatalogueError *error)
{
    if (!is_named_list(node, "pin") || ir_list_length(node) != 4) {
        set_error(error, PART_CATALOGUE_ERR_SCHEMA, item_index, "pins",
                  "pin syntax is (pin logical_terminal symbol_pin footprint_pad)");
        return PART_CATALOGUE_ERR_SCHEMA;
    }
    const char *terminal = node_text(ir_child_at(node, 1));
    const char *symbol_pin = node_text(ir_child_at(node, 2));
    const char *pad = node_text(ir_child_at(node, 3));
    if (!terminal || !terminal[0] || !symbol_pin || !symbol_pin[0] ||
        !pad || !pad[0]) {
        set_error(error, PART_CATALOGUE_ERR_SCHEMA, item_index, "pins",
                  "pin terminal, symbol pin and footprint pad must be non-empty");
        return PART_CATALOGUE_ERR_SCHEMA;
    }
    out->terminal = terminal;
    out->symbol_pin = symbol_pin;
    out->footprint_pad = pad;
    return PART_CATALOGUE_OK;
}

static PartCatalogueStatus parse_rating(const IRNode *node,
                                        PartRating *out,
                                        size_t item_index,
                                        PartCatalogueError *error)
{
    if (!is_named_list(node, "rating") || ir_list_length(node) != 6) {
        set_error(error, PART_CATALOGUE_ERR_SCHEMA, item_index, "ratings",
                  "rating syntax is (rating NAME BOUND INTEGER UNIT (source ...))");
        return PART_CATALOGUE_ERR_SCHEMA;
    }

    const char *name_text = node_text(ir_child_at(node, 1));
    const char *bound_text = node_text(ir_child_at(node, 2));
    const IRNode *value_node = ir_child_at(node, 3);
    const char *unit_text = node_text(ir_child_at(node, 4));
    const IRNode *source_node = ir_child_at(node, 5);

    PartRating result;
    memset(&result, 0, sizeof(result));
    result.name = parse_rating_name(name_text);
    result.bound = parse_bound(bound_text);
    result.unit = parse_unit(unit_text);

    if (result.name == PART_RATING_INVALID ||
        result.bound == PART_BOUND_INVALID ||
        result.unit == PART_UNIT_INVALID ||
        result.unit != expected_unit(result.name) ||
        ir_atom_to_i64(value_node, &result.value) != IR_STATUS_OK) {
        set_error(error, PART_CATALOGUE_ERR_SCHEMA, item_index, "ratings",
                  "unknown rating, bound, invalid integer or incompatible unit");
        return PART_CATALOGUE_ERR_SCHEMA;
    }

    /* G-068 (2026-11-21): a temperature may be zero or negative (e.g. a -55 C minimum); every other rating must be
     * strictly positive. Before, `value < 0` was tested for every rating, so no negative temperature parsed. */
    if (result.name != PART_RATING_TEMPERATURE && result.value <= 0) {
        set_error(error, PART_CATALOGUE_ERR_RANGE, item_index, "ratings",
                  "ratings must be positive (temperature may be signed)");
        return PART_CATALOGUE_ERR_RANGE;
    }

    PartCatalogueStatus status =
        parse_source(source_node, &result.source, item_index, "rating.source", error);
    if (status != PART_CATALOGUE_OK) return status;

    *out = result;
    return PART_CATALOGUE_OK;
}

static const char *generated_part_id(Arena *arena,
                                     const char *manufacturer,
                                     const char *mpn,
                                     const char *symbol,
                                     const char *footprint,
                                     const char *description)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    const char *fields[] = {manufacturer, mpn, symbol, footprint, description};
    bool any = false;
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        const unsigned char *p = (const unsigned char *)(fields[i] ? fields[i] : "");
        if (fields[i] && fields[i][0]) any = true;
        while (*p) {
            hash ^= (uint64_t)tolower(*p++);
            hash *= UINT64_C(1099511628211);
        }
        hash ^= UINT64_C(0xff);
        hash *= UINT64_C(1099511628211);
    }
    if (!any) return NULL;
    char *id = (char *)arena_alloc(arena, 32u);
    if (!id) return NULL;
    (void)snprintf(id, 32u, "generated_%016llx", (unsigned long long)hash);
    return id;
}

static PartCatalogueStatus parse_part(Arena *arena,
                                      const IRNode *node,
                                      PartRecord *out,
                                      size_t item_index,
                                      PartCatalogueError *error)
{
    bool valid = true;
    PartRecord result;
    memset(&result, 0, sizeof(result));

    if (!is_named_list(node, "part")) {
        set_error(error, PART_CATALOGUE_ERR_SCHEMA, item_index, "part",
                  "expected a (part ...) record");
        return PART_CATALOGUE_ERR_SCHEMA;
    }

    result.source_record = node;
    result.id = field_text(node, "id", false, &valid);
    const char *kind_text = field_text(node, "kind", false, &valid);
    result.manufacturer = field_text(node, "manufacturer", false, &valid);
    result.mpn = field_text(node, "mpn", false, &valid);
    result.symbol_id = field_text(node, "symbol", false, &valid);
    result.footprint_id = field_text(node, "footprint", false, &valid);
    result.model_id = field_text(node, "model", false, &valid);
    result.package = field_text(node, "package", false, &valid);
    result.description = field_text(node, "description", false, &valid);
    result.datasheet = field_text(node, "datasheet", false, &valid);
    result.supplier = field_text(node, "supplier", false, &valid);

    result.kind = parse_kind(kind_text);
    if (!result.id) {
        result.id = generated_part_id(arena, result.manufacturer, result.mpn,
                                      result.symbol_id, result.footprint_id,
                                      result.description);
        result.id_generated = result.id != NULL;
    }

    const IRNode *identity_source =
        unique_field(node, "identity_source", false, &valid);
    const IRNode *aliases_node = unique_field(node, "aliases", false, &valid);
    const IRNode *ratings_node = unique_field(node, "ratings", false, &valid);
    const IRNode *pins_node = unique_field(node, "pins", false, &valid);

    static const char *const part_fields[] = {
        "part", "id", "kind", "manufacturer", "mpn", "symbol",
        "footprint", "model", "package", "description", "datasheet", "supplier",
        "identity_source", "ratings", "pins", "aliases"
    };
    /* Unknown vendor-specific fields remain available through source_record.
       The typed resolver deliberately ignores fields it does not understand. */
    (void)part_fields;

    if (!valid || !valid_identifier(result.id) ||
        (result.symbol_id && !valid_kicad_id(result.symbol_id)) ||
        (result.footprint_id && !valid_kicad_id(result.footprint_id)) ||
        (result.model_id && !valid_model_id(result.model_id))) {
        set_error(error, PART_CATALOGUE_ERR_SCHEMA, item_index, "part",
                  "part requires an explicit id or enough metadata to derive one; optional catalogue fields may be absent");
        return PART_CATALOGUE_ERR_SCHEMA;
    }

    PartCatalogueStatus status = PART_CATALOGUE_OK;
    if (identity_source) {
        status = parse_source(identity_source, &result.identity_source,
                              item_index, "identity_source", error);
        if (status != PART_CATALOGUE_OK) return status;
    }

    static const char *const rating_list_fields[] = {"ratings", "rating"};
    static const char *const pin_list_fields[] = {"pins", "pin"};
    if ((ratings_node &&
         !fields_are_known(ratings_node, rating_list_fields,
                           sizeof(rating_list_fields) / sizeof(rating_list_fields[0]))) ||
        (pins_node &&
         !fields_are_known(pins_node, pin_list_fields,
                           sizeof(pin_list_fields) / sizeof(pin_list_fields[0])))) {
        set_error(error, PART_CATALOGUE_ERR_SCHEMA, item_index, "part",
                  "ratings/pins list contains an unknown entry");
        return PART_CATALOGUE_ERR_SCHEMA;
    }

    size_t rating_count = ratings_node ? count_named(ratings_node, "rating") : 0;
    size_t pin_count = pins_node ? count_named(pins_node, "pin") : 0;
    size_t alias_count = aliases_node ? count_named(aliases_node, "alias") : 0;
    if (alias_count) {
        result.aliases = arena_alloc_array(arena, sizeof(*result.aliases), alias_count);
        if (!result.aliases) {
            set_error(error, PART_CATALOGUE_ERR_MEMORY, item_index, "aliases",
                      "could not allocate aliases");
            return PART_CATALOGUE_ERR_MEMORY;
        }
        for (const IRNode *a = aliases_node->first_child; a; a = a->next) {
            if (!is_named_list(a, "alias")) continue;
            const char *value = node_text(ir_child_at(a, 1));
            if (!value || !value[0] || ir_list_length(a) != 2) {
                set_error(error, PART_CATALOGUE_ERR_SCHEMA, item_index, "aliases",
                          "alias entries must contain one non-empty string");
                return PART_CATALOGUE_ERR_SCHEMA;
            }
            result.aliases[result.alias_count++] = value;
        }
    }
    if (rating_count > PART_CATALOGUE_MAX_RATINGS ||
        pin_count > PART_CATALOGUE_MAX_PINS) {
        set_error(error, PART_CATALOGUE_ERR_RANGE, item_index, "part",
                  "part exceeds rating or pin-map capacity");
        return PART_CATALOGUE_ERR_RANGE;
    }

    if (rating_count) {
        result.ratings = arena_alloc_array(arena, sizeof(*result.ratings), rating_count);
        if (!result.ratings) {
            set_error(error, PART_CATALOGUE_ERR_MEMORY, item_index, "ratings",
                      "could not allocate rating array");
            return PART_CATALOGUE_ERR_MEMORY;
        }
        memset(result.ratings, 0, rating_count * sizeof(*result.ratings));
    }

    if (pin_count) {
        result.pins = arena_alloc_array(arena, sizeof(*result.pins), pin_count);
        if (!result.pins) {
            set_error(error, PART_CATALOGUE_ERR_MEMORY, item_index, "pins",
                      "could not allocate pin-map array");
            return PART_CATALOGUE_ERR_MEMORY;
        }
        memset(result.pins, 0, pin_count * sizeof(*result.pins));
    }

    size_t ri = 0;
    for (const IRNode *child = ratings_node ? ratings_node->first_child : NULL;
         child; child = child->next) {
        if (!is_named_list(child, "rating")) continue;
        status = parse_rating(child, &result.ratings[ri], item_index, error);
        if (status != PART_CATALOGUE_OK) return status;

        for (size_t j = 0; j < ri; ++j) {
            if (result.ratings[j].name == result.ratings[ri].name &&
                result.ratings[j].bound == result.ratings[ri].bound) {
                set_error(error, PART_CATALOGUE_ERR_DUPLICATE, item_index, "ratings",
                          "duplicate rating with the same name and bound");
                return PART_CATALOGUE_ERR_DUPLICATE;
            }
        }
        ++ri;
    }
    result.rating_count = ri;

    size_t pi = 0;
    for (const IRNode *child = pins_node ? pins_node->first_child : NULL;
         child; child = child->next) {
        if (!is_named_list(child, "pin")) continue;
        status = parse_pin(child, &result.pins[pi], item_index, error);
        if (status != PART_CATALOGUE_OK) return status;

        for (size_t j = 0; j < pi; ++j) {
            /* One logical terminal may fan out to several physical pins/pads
             * (for example, multiple USB-C VBUS contacts). Physical pin and
             * pad identifiers themselves must remain one-to-one. */
            if (!strcmp(result.pins[j].symbol_pin, result.pins[pi].symbol_pin) ||
                !strcmp(result.pins[j].footprint_pad, result.pins[pi].footprint_pad)) {
                set_error(error, PART_CATALOGUE_ERR_DUPLICATE, item_index, "pins",
                          "symbol pins and footprint pads must be unique");
                return PART_CATALOGUE_ERR_DUPLICATE;
            }
        }
        ++pi;
    }
    result.pin_count = pi;

    /* Missing pin maps are permitted for discovery. They remain unresolved
       until the selected symbol and footprint are inspected and mapped. */

    *out = result;
    return PART_CATALOGUE_OK;
}

const char *part_catalogue_status_name(PartCatalogueStatus status)
{
    static const char *const names[] = {
        "OK", "ARGUMENT_ERROR", "PARSE_ERROR", "SCHEMA_ERROR",
        "DUPLICATE", "RANGE_ERROR", "MEMORY_ERROR"
    };
    if ((unsigned)status >= sizeof(names) / sizeof(names[0])) return "UNKNOWN_STATUS";
    return names[status];
}

const char *part_kind_name(PartKind kind)
{
    static const char *const names[] = {
        "INVALID", "R", "C", "L", "D", "Q", "M", "U", "J", "S"
    };
    if ((unsigned)kind >= sizeof(names) / sizeof(names[0])) return "INVALID";
    return names[kind];
}

const char *part_rating_name(PartRatingName name)
{
    static const char *const names[] = {
        "invalid", "resistance", "capacitance", "inductance", "voltage",
        "current", "power", "tolerance", "height", "dropout",
        "quiescent_current", "temperature"
    };
    if ((unsigned)name >= sizeof(names) / sizeof(names[0])) return "invalid";
    return names[name];
}

const char *part_rating_bound_name(PartRatingBound bound)
{
    static const char *const names[] = {
        "invalid", "nominal", "minimum", "typical", "maximum"
    };
    if ((unsigned)bound >= sizeof(names) / sizeof(names[0])) return "invalid";
    return names[bound];
}

const char *part_rating_unit_name(PartRatingUnit unit)
{
    static const char *const names[] = {
        "invalid", "uohm", "pF", "nH", "uV", "uA", "uW", "ppm", "um", "mC"
    };
    if ((unsigned)unit >= sizeof(names) / sizeof(names[0])) return "invalid";
    return names[unit];
}

static int compare_part_id_ptr(const void *lhs, const void *rhs)
{
    const PartRecord *a = *(PartRecord *const *)lhs;
    const PartRecord *b = *(PartRecord *const *)rhs;
    return strcmp(a->id, b->id);
}

static PartCatalogueStatus part_catalogue_from_ir(
    Arena *arena, const IRNode *root, PartCatalogue *out,
    PartCatalogueError *error)
{
    if (!arena || !root || !out) {
        set_error(error, PART_CATALOGUE_ERR_ARGUMENT, SIZE_MAX, NULL,
                  "arena, parsed root and output are required");
        return PART_CATALOGUE_ERR_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));

    if (!is_named_list(root, "copper_parts")) {
        set_error(error, PART_CATALOGUE_ERR_SCHEMA, SIZE_MAX, "root",
                  "root expression must be (copper_parts ...)");
        return PART_CATALOGUE_ERR_SCHEMA;
    }

    static const char *const root_fields[] = {"copper_parts", "schema", "part"};
    if (!fields_are_known(root, root_fields,
                          sizeof(root_fields) / sizeof(root_fields[0]))) {
        set_error(error, PART_CATALOGUE_ERR_SCHEMA, SIZE_MAX, "root",
                  "catalogue contains an unknown top-level field");
        return PART_CATALOGUE_ERR_SCHEMA;
    }

    bool valid = true;
    const IRNode *schema_node = unique_field(root, "schema", true, &valid);
    uint64_t schema = 0;
    if (!valid || !schema_node || ir_list_length(schema_node) != 2 ||
        ir_atom_to_u64(ir_child_at(schema_node, 1), &schema) != IR_STATUS_OK ||
        schema != PART_CATALOGUE_SCHEMA_VERSION) {
        set_error(error, PART_CATALOGUE_ERR_SCHEMA, SIZE_MAX, "schema",
                  "unsupported or missing catalogue schema version");
        return PART_CATALOGUE_ERR_SCHEMA;
    }

    size_t count = count_named(root, "part");
    if (count == 0) {
        set_error(error, PART_CATALOGUE_ERR_SCHEMA, SIZE_MAX, "part",
                  "catalogue must contain at least one part");
        return PART_CATALOGUE_ERR_SCHEMA;
    }

    PartRecord *records = arena_alloc_array(arena, sizeof(*records), count);
    if (!records) {
        set_error(error, PART_CATALOGUE_ERR_MEMORY, SIZE_MAX, "parts",
                  "could not allocate part records");
        return PART_CATALOGUE_ERR_MEMORY;
    }
    memset(records, 0, count * sizeof(*records));

    size_t index = 0;
    for (const IRNode *child = root->first_child; child; child = child->next) {
        if (!is_named_list(child, "part")) continue;
        PartCatalogueStatus status =
            parse_part(arena, child, &records[index], index, error);
        if (status != PART_CATALOGUE_OK) return status;

        ++index;
    }

    PartRecord **by_id = (PartRecord **)arena_alloc_array(
        arena, sizeof(*by_id), count);
    if (!by_id) {
        set_error(error, PART_CATALOGUE_ERR_MEMORY, SIZE_MAX, "index",
                  "could not allocate catalogue lookup index");
        return PART_CATALOGUE_ERR_MEMORY;
    }
    for (size_t i = 0; i < count; ++i) by_id[i] = &records[i];
    qsort(by_id, count, sizeof(*by_id), compare_part_id_ptr);
    for (size_t i = 1; i < count; ++i) {
        if (strcmp(by_id[i - 1]->id, by_id[i]->id) == 0) {
            set_error(error, PART_CATALOGUE_ERR_DUPLICATE, i, "id",
                      "duplicate part identifier");
            return PART_CATALOGUE_ERR_DUPLICATE;
        }
    }
    out->schema_version = (uint32_t)schema;
    out->parts = records;
    out->part_count = count;
    out->by_id = by_id;
    set_error(error, PART_CATALOGUE_OK, SIZE_MAX, NULL, NULL);
    return PART_CATALOGUE_OK;
}

PartCatalogueStatus part_catalogue_parse(
    Arena *arena, const char *text, size_t length,
    PartCatalogue *out, PartCatalogueError *error)
{
    if (out) memset(out, 0, sizeof(*out));
    set_error(error, PART_CATALOGUE_OK, SIZE_MAX, NULL, NULL);
    if (!arena || !text || length == 0 || !out) {
        set_error(error, PART_CATALOGUE_ERR_ARGUMENT, SIZE_MAX, NULL,
                  "arena, non-empty input and output are required");
        return PART_CATALOGUE_ERR_ARGUMENT;
    }

    IRError ir_error = {0};
    IRNode *root = ir_parse(arena, text, length, &ir_error);
    if (!root) {
        PartCatalogueStatus status =
            ir_error.code == IR_STATUS_OUT_OF_MEMORY
                ? PART_CATALOGUE_ERR_MEMORY : PART_CATALOGUE_ERR_PARSE;
        set_error(error, status, SIZE_MAX, NULL,
                  "could not parse catalogue S-expression");
        if (error) error->ir_error = ir_error;
        return status;
    }
    return part_catalogue_from_ir(arena, root, out, error);
}

PartCatalogueStatus part_catalogue_parse_file(
    Arena *arena, const char *path,
    PartCatalogue *out, PartCatalogueError *error)
{
    if (out) memset(out, 0, sizeof(*out));
    set_error(error, PART_CATALOGUE_OK, SIZE_MAX, NULL, NULL);
    if (!arena || !path || !path[0] || !out) {
        set_error(error, PART_CATALOGUE_ERR_ARGUMENT, SIZE_MAX, NULL,
                  "arena, path and output are required");
        return PART_CATALOGUE_ERR_ARGUMENT;
    }

    IRError ir_error = {0};
    IRNode *root = ir_parse_file(arena, path, &ir_error);
    if (!root) {
        PartCatalogueStatus status =
            ir_error.code == IR_STATUS_OUT_OF_MEMORY
                ? PART_CATALOGUE_ERR_MEMORY : PART_CATALOGUE_ERR_PARSE;
        set_error(error, status, SIZE_MAX, NULL,
                  "could not read or parse catalogue file");
        if (error) error->ir_error = ir_error;
        return status;
    }
    return part_catalogue_from_ir(arena, root, out, error);
}

const PartRecord *part_catalogue_find(
    const PartCatalogue *catalogue, const char *part_id)
{
    if (!catalogue || !part_id) return NULL;
    if (catalogue->by_id) {
        size_t lo = 0, hi = catalogue->part_count;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2u;
            int cmp = strcmp(catalogue->by_id[mid]->id, part_id);
            if (cmp < 0) lo = mid + 1u;
            else hi = mid;
        }
        if (lo < catalogue->part_count &&
            strcmp(catalogue->by_id[lo]->id, part_id) == 0)
            return catalogue->by_id[lo];
        return NULL;
    }
    for (size_t i = 0; i < catalogue->part_count; ++i)
        if (catalogue->parts[i].id &&
            strcmp(catalogue->parts[i].id, part_id) == 0)
            return &catalogue->parts[i];
    return NULL;
}

const PartRating *part_record_find_rating(
    const PartRecord *part, PartRatingName name, PartRatingBound bound)
{
    if (!part) return NULL;
    for (size_t i = 0; i < part->rating_count; ++i) {
        if (part->ratings[i].name == name && part->ratings[i].bound == bound) {
            return &part->ratings[i];
        }
    }
    return NULL;
}

bool part_record_sources_verified(const PartRecord *part)
{
    if (!part || part->identity_source.status != PART_SOURCE_VERIFIED) return false;
    for (size_t i = 0; i < part->rating_count; ++i) {
        if (part->ratings[i].source.status != PART_SOURCE_VERIFIED) return false;
    }
    return true;
}

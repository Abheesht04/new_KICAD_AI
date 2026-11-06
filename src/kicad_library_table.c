#include "kicad_library_table.h"

#include <string.h>

static const char *const names[] = {
    "OK",           "ARGUMENT_ERROR",      "PARSE_ERROR", "SCHEMA_ERROR",
    "MEMORY_ERROR", "UNRESOLVED_VARIABLE", "RANGE_ERROR"};

static const char *find_path_variable(const KicadLibPathContext *context,
                                      const char *name, size_t name_length) {
  if (name == NULL || name_length == 0) {
    return NULL;
  }

  // KIPRJMOD is special and cannot be overridden.
  if (name_length == sizeof("KIPRJMOD") - 1 &&
      memcmp(name, "KIPRJMOD", name_length) == 0) {

    return context != NULL ? context->project_dir : NULL;
  }

  if (context == NULL ||
      (context->variable_count > 0 && context->variables == NULL)) {
    return NULL;
  }

  for (size_t i = 0; i < context->variable_count; ++i) {
    const KicadPathVariable *variable = &context->variables[i];

    if (variable->name == NULL || variable->value == NULL) {
      continue;
    }

    size_t variable_length = strlen(variable->name);

    if (variable_length == name_length &&
        memcmp(variable->name, name, name_length) == 0) {
      return variable->value;
    }
  }

  return NULL;
}

static bool is_named_list(const IRNode *node, const char *name) {
  if (node == NULL || name == NULL || node->kind != IR_NODE_LIST) {
    return false;
  }

  const IRNode *head = ir_list_head(node);

  return head != NULL && head->kind == IR_NODE_ATOM &&
         strcmp(head->text, name) == 0;
}

static const char *get_field_text(const IRNode *list, const char *field_name,
                                  bool required, bool *valid) {
  const IRNode *field = ir_list_find(list, field_name);

  if (field == NULL) {
    if (required) {
      *valid = false;
    }
    return NULL;
  }

  if (ir_list_length(field) != 2) {
    *valid = false;
    return NULL;
  }

  const IRNode *value = ir_child_at(field, 1);

  if (value == NULL ||
      (value->kind != IR_NODE_ATOM && value->kind != IR_NODE_STRING)) {
    *valid = false;
    return NULL;
  }

  return value->text;
}

KicadLibStatus kicad_lib_table_parse(Arena *arena, const char *text,
                                     size_t length, KicadLibTableKind kind,
                                     KicadLibTable *out, IRError *error) {
  if (out == NULL) {
    return KICAD_LIB_ERR_ARGUMENT;
  }

  memset(out, 0, sizeof(*out));

  if (arena == NULL || text == NULL || length == 0 ||
      (kind != KICAD_LIB_TABLE_SYMBOLS && kind != KICAD_LIB_TABLE_FOOTPRINTS)) {
    return KICAD_LIB_ERR_ARGUMENT;
  }

  IRError local_error = {0};
  IRError *parse_error = error != NULL ? error : &local_error;

  IRNode *root = ir_parse(arena, text, length, parse_error);

  if (root == NULL) {
    if (parse_error->code == IR_STATUS_OUT_OF_MEMORY) {
      return KICAD_LIB_ERR_MEMORY;
    }
    return KICAD_LIB_ERR_PARSE;
  }

  const char *expected_root =
      kind == KICAD_LIB_TABLE_SYMBOLS ? "sym_lib_table" : "fp_lib_table";

  if (!is_named_list(root, expected_root)) {
    return KICAD_LIB_ERR_SCHEMA;
  }

  // Read the table version.
  const IRNode *version_node = ir_list_find(root, "version");
  const IRNode *version_value = ir_child_at(version_node, 1);

  uint64_t version = 0;

  if (version_node == NULL || ir_list_length(version_node) != 2 ||
      version_value == NULL ||
      ir_atom_to_u64(version_value, &version) != IR_STATUS_OK ||
      version > UINT32_MAX) {
    return KICAD_LIB_ERR_SCHEMA;
  }

  // First pass: count library entries.
  size_t entry_count = 0;

  for (const IRNode *child = root->first_child; child != NULL;
       child = child->next) {

    if (is_named_list(child, "lib")) {
      if (entry_count == SIZE_MAX) {
        return KICAD_LIB_ERR_MEMORY;
      }
      ++entry_count;
    }
  }

  // Allocate all entries in one arena allocation.
  KicadLibEntry *entries = NULL;

  if (entry_count > 0) {
    entries = arena_alloc_array(arena, sizeof(*entries), entry_count);

    if (entries == NULL) {
      return KICAD_LIB_ERR_MEMORY;
    }

    memset(entries, 0, entry_count * sizeof(*entries));
  }

  // Second pass: extract each library record.
  size_t index = 0;

  for (const IRNode *child = root->first_child; child != NULL;
       child = child->next) {

    if (!is_named_list(child, "lib")) {
      continue;
    }

    bool valid = true;

    KicadLibEntry *entry = &entries[index];

    entry->nickname = get_field_text(child, "name", true, &valid);

    entry->type = get_field_text(child, "type", true, &valid);

    entry->uri = get_field_text(child, "uri", true, &valid);

    entry->options = get_field_text(child, "options", false, &valid);

    entry->description = get_field_text(child, "descr", false, &valid);

    if (!valid || entry->nickname == NULL || entry->nickname[0] == '\0' ||
        entry->type == NULL || entry->type[0] == '\0' || entry->uri == NULL ||
        entry->uri[0] == '\0') {
      return KICAD_LIB_ERR_SCHEMA;
    }

    entry->source_order = index;
    ++index;
  }

  // Commit the completed result.
  out->kind = kind;
  out->version = (uint32_t)version;
  out->entries = entries;
  out->entry_count = entry_count;

  if (error != NULL) {
    ir_error_clear(error);
  }

  return KICAD_LIB_OK;
}
const char *kicad_lib_status_name(KicadLibStatus status) {
  static const char *const names[] = {"OK", "ARGUMENT_ERROR", "PARSE_ERROR",
                                      "SCHEMA_ERROR", "MEMORY_ERROR"};

  if ((unsigned)status >= sizeof(names) / sizeof(names[0])) {
    return "UNKNOWN_STATUS";
  }

  return names[status];
}

static const KicadLibEntry *find_in_table(const KicadLibTable *table,
                                          const char *nickname) {
  if (table == NULL || nickname == NULL) {
    return NULL;
  }

  for (size_t i = 0; i < table->entry_count; ++i) {
    const KicadLibEntry *entry = &table->entries[i];

    if (entry->nickname != NULL && strcmp(entry->nickname, nickname) == 0) {
      return entry;
    }
  }

  return NULL;
}

const KicadLibEntry *kicad_lib_find_entry(const KicadLibTable *project_table,
                                          const KicadLibTable *global_table,
                                          const char *nickname) {
  if (nickname == NULL || nickname[0] == '\0') {
    return NULL;
  }

  if (project_table != NULL && global_table != NULL &&
      project_table->kind != global_table->kind) {
    return NULL;
  }

  const KicadLibEntry *entry = find_in_table(project_table, nickname);

  if (entry != NULL) {
    return entry;
  }

  return find_in_table(global_table, nickname);
}

static KicadLibStatus measure_expanded_uri(const char *uri,
                                           const KicadLibPathContext *context,
                                           size_t *out_length) {
  if (uri == NULL || out_length == NULL) {
    return KICAD_LIB_ERR_ARGUMENT;
  }

  size_t total = 0;
  size_t i = 0;
  size_t length = strlen(uri);

  while (i < length) {

    if (uri[i] == '$' && i + 1 < length && uri[i + 1] == '{') {

      size_t start = i + 2;
      size_t end = start;

      while (end < length && uri[end] != '}') {
        ++end;
      }

      if (end == length || end == start) {
        return KICAD_LIB_ERR_SCHEMA;
      }

      const char *value = find_path_variable(context, uri + start, end - start);

      if (value == NULL) {
        return KICAD_LIB_ERR_UNRESOLVED_VARIABLE;
      }

      size_t value_length = strlen(value);

      if (value_length > SIZE_MAX - total) {
        return KICAD_LIB_ERR_RANGE;
      }

      total += value_length;
      i = end + 1;

    } else {
      if (total == SIZE_MAX) {
        return KICAD_LIB_ERR_RANGE;
      }

      ++total;
      ++i;
    }
  }

  *out_length = total;

  return KICAD_LIB_OK;
}

KicadLibStatus kicad_lib_expand_uri(Arena *arena, const char *uri,
                                    const KicadLibPathContext *context,
                                    const char **out_path) {
  if (out_path == NULL) {
    return KICAD_LIB_ERR_ARGUMENT;
  }

  *out_path = NULL;

  if (arena == NULL || uri == NULL) {
    return KICAD_LIB_ERR_ARGUMENT;
  }

  size_t expanded_length = 0;

  KicadLibStatus status = measure_expanded_uri(uri, context, &expanded_length);

  if (status != KICAD_LIB_OK) {
    return status;
  }

  if (expanded_length == SIZE_MAX) {
    return KICAD_LIB_ERR_RANGE;
  }

  char *expanded = arena_alloc(arena, expanded_length + 1);

  if (expanded == NULL) {
    return KICAD_LIB_ERR_MEMORY;
  }

  size_t input_pos = 0;
  size_t output_pos = 0;

  size_t input_length = strlen(uri);

  while (input_pos < input_length) {

    if (uri[input_pos] == '$' && input_pos + 1 < input_length &&
        uri[input_pos + 1] == '{') {

      size_t start = input_pos + 2;
      size_t end = start;

      while (end < input_length && uri[end] != '}') {
        ++end;
      }

      const char *value = find_path_variable(context, uri + start, end - start);

      size_t value_length = strlen(value);

      memcpy(expanded + output_pos, value, value_length);

      output_pos += value_length;
      input_pos = end + 1;

    } else {
      expanded[output_pos++] = uri[input_pos++];
    }
  }

  expanded[output_pos] = '\0';

  *out_path = expanded;

  return KICAD_LIB_OK;
}

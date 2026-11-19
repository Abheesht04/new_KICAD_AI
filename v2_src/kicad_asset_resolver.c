#include "kicad_asset_resolver.h"
#include "filesystem.h"
#include "kicad_library_table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *arena_strdup_local(Arena *arena, const char *value) {
    if (!arena || !value) return NULL;
    size_t n = strlen(value);
    char *copy = (char *)arena_alloc(arena, n + 1u);
    if (!copy) return NULL;
    memcpy(copy, value, n + 1u);
    return copy;
}

static bool env_copy(const char *name, char *out, size_t capacity) {
    if (!name || !out || capacity == 0) return false;
    out[0] = '\0';
#ifdef _WIN32
    char *value = NULL;
    size_t value_size = 0;
    if (_dupenv_s(&value, &value_size, name) != 0 || !value) {
        free(value);
        return false;
    }
    size_t n = strlen(value);
    bool ok = n < capacity;
    if (ok) memcpy(out, value, n + 1u);
    free(value);
    return ok;
#else
    const char *value = getenv(name);
    if (!value) return false;
    size_t n = strlen(value);
    if (n >= capacity) return false;
    memcpy(out, value, n + 1u);
    return true;
#endif
}

static bool set_root(Arena *arena, const char *candidate,
                     const char **out_root, const char **out_source,
                     const char *source) {
    if (!candidate || !candidate[0] || !fs_directory_exists(candidate)) return false;
    char absolute[FS_PATH_MAX];
    const char *value = candidate;
    if (fs_path_absolute(absolute, sizeof(absolute), candidate) == FS_OK)
        value = absolute;
    *out_root = arena_strdup_local(arena, value);
    *out_source = arena_strdup_local(arena, source);
    return *out_root != NULL && *out_source != NULL;
}

static bool try_env_roots(Arena *arena, const char *const *variables,
                          size_t variable_count, const char **out_root,
                          const char **out_source) {
    char candidate[FS_PATH_MAX];
    for (size_t i = 0; i < variable_count; ++i) {
        if (env_copy(variables[i], candidate, sizeof(candidate)) &&
            set_root(arena, candidate, out_root, out_source, variables[i])) return true;
    }
    return false;
}

#ifdef _WIN32
static bool try_program_files(Arena *arena, const char *subpath,
                              const char **out_root, const char **out_source) {
    const char *vars[] = {"ProgramFiles", "ProgramW6432", "ProgramFiles(x86)"};
    char base[FS_PATH_MAX], candidate[FS_PATH_MAX];
    for (size_t i = 0; i < sizeof(vars) / sizeof(vars[0]); ++i) {
        if (!env_copy(vars[i], base, sizeof(base))) continue;
        char share_root[FS_PATH_MAX];
        if (fs_path_join(share_root, sizeof(share_root), base, "KiCad/9.0/share/kicad") != FS_OK)
            continue;
        if (fs_path_join(candidate, sizeof(candidate), share_root, subpath) != FS_OK)
            continue;
        if (set_root(arena, candidate, out_root, out_source, "KiCad 9.0 installation")) return true;
    }
    return false;
}
#endif

static bool try_unix_root(Arena *arena, const char *const *candidates,
                          size_t count, const char **out_root,
                          const char **out_source) {
    for (size_t i = 0; i < count; ++i)
        if (set_root(arena, candidates[i], out_root, out_source, "system KiCad installation"))
            return true;
    return false;
}

KicadAssetDiscoveryStatus kicad_asset_roots_discover(
    Arena *arena, const KicadAssetRootOptions *options, KicadAssetRoots *out) {
    if (!arena || !out) return KICAD_DISCOVERY_ERR_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (options && options->project_directory && fs_directory_exists(options->project_directory)) {
        out->project_directory = arena_strdup_local(arena, options->project_directory);
        if (!out->project_directory) return KICAD_DISCOVERY_ERR_MEMORY;
        char table_path[FS_PATH_MAX];
        if (options->symbol_table_path) {
            if (fs_file_exists(options->symbol_table_path))
                out->symbol_table_path = arena_strdup_local(arena, options->symbol_table_path);
        } else if (fs_path_join(table_path, sizeof(table_path), options->project_directory,
                                "sym-lib-table") == FS_OK && fs_file_exists(table_path)) {
            out->symbol_table_path = arena_strdup_local(arena, table_path);
        }
        if (options->footprint_table_path) {
            if (fs_file_exists(options->footprint_table_path))
                out->footprint_table_path = arena_strdup_local(arena, options->footprint_table_path);
        } else if (fs_path_join(table_path, sizeof(table_path), options->project_directory,
                                "fp-lib-table") == FS_OK && fs_file_exists(table_path)) {
            out->footprint_table_path = arena_strdup_local(arena, table_path);
        }
    } else if (options) {
        if (options->symbol_table_path && fs_file_exists(options->symbol_table_path))
            out->symbol_table_path = arena_strdup_local(arena, options->symbol_table_path);
        if (options->footprint_table_path && fs_file_exists(options->footprint_table_path))
            out->footprint_table_path = arena_strdup_local(arena, options->footprint_table_path);
    }

    if (options && options->symbol_root) {
        if (set_root(arena, options->symbol_root, &out->symbol_root,
                     &out->symbol_source, "explicit --symbol-root"))
            out->symbols_available = true;
    }
    if (options && options->footprint_root) {
        if (set_root(arena, options->footprint_root, &out->footprint_root,
                     &out->footprint_source, "explicit --footprint-root"))
            out->footprints_available = true;
    }

    if (!out->symbols_available) {
        static const char *const envs[] = {"KICAD9_SYMBOL_DIR", "KICAD_SYMBOL_DIR"};
        out->symbols_available = try_env_roots(arena, envs, 2u,
            &out->symbol_root, &out->symbol_source);
    }
    if (!out->footprints_available) {
        static const char *const envs[] = {"KICAD9_FOOTPRINT_DIR", "KICAD_FOOTPRINT_DIR"};
        out->footprints_available = try_env_roots(arena, envs, 2u,
            &out->footprint_root, &out->footprint_source);
    }

    if (options && options->copper_asset_root) {
        char path[FS_PATH_MAX];
        if (!out->symbols_available &&
            fs_path_join(path, sizeof(path), options->copper_asset_root, "symbols") == FS_OK &&
            set_root(arena, path, &out->symbol_root, &out->symbol_source,
                     "Copper imported KiCad assets")) out->symbols_available = true;
        if (!out->footprints_available &&
            fs_path_join(path, sizeof(path), options->copper_asset_root, "footprints") == FS_OK &&
            set_root(arena, path, &out->footprint_root, &out->footprint_source,
                     "Copper imported KiCad assets")) out->footprints_available = true;
    }

#ifdef _WIN32
    if (!out->symbols_available)
        out->symbols_available = try_program_files(arena, "symbols",
            &out->symbol_root, &out->symbol_source);
    if (!out->footprints_available)
        out->footprints_available = try_program_files(arena, "footprints",
            &out->footprint_root, &out->footprint_source);
#else
    if (!out->symbols_available) {
        static const char *const paths[] = {
            "/usr/share/kicad/symbols", "/usr/local/share/kicad/symbols",
            "/opt/kicad/share/kicad/symbols"};
        out->symbols_available = try_unix_root(arena, paths, 3u,
            &out->symbol_root, &out->symbol_source);
    }
    if (!out->footprints_available) {
        static const char *const paths[] = {
            "/usr/share/kicad/footprints", "/usr/local/share/kicad/footprints",
            "/opt/kicad/share/kicad/footprints"};
        out->footprints_available = try_unix_root(arena, paths, 3u,
            &out->footprint_root, &out->footprint_source);
    }
#endif
    if (out->symbol_table_path) out->symbols_available = true;
    if (out->footprint_table_path) out->footprints_available = true;
    if (out->symbols_available && out->footprints_available) return KICAD_DISCOVERY_OK;
    if (out->symbols_available || out->footprints_available) return KICAD_DISCOVERY_PARTIAL;
    return KICAD_DISCOVERY_NOT_FOUND;
}

const char *kicad_asset_discovery_status_name(KicadAssetDiscoveryStatus s) {
    switch (s) {
    case KICAD_DISCOVERY_OK: return "OK";
    case KICAD_DISCOVERY_PARTIAL: return "PARTIAL";
    case KICAD_DISCOVERY_NOT_FOUND: return "NOT_FOUND";
    case KICAD_DISCOVERY_ERR_ARGUMENT: return "ARGUMENT_ERROR";
    case KICAD_DISCOVERY_ERR_MEMORY: return "MEMORY_ERROR";
    default: return "UNKNOWN";
    }
}

const char *kicad_pin_map_resolution_name(KicadPinMapResolution s) {
    switch (s) {
    case KICAD_PIN_MAP_EXPLICIT_VALID: return "EXPLICIT_VALID";
    case KICAD_PIN_MAP_EXPLICIT_INVALID: return "EXPLICIT_INVALID";
    case KICAD_PIN_MAP_INFERRED_UNVERIFIED: return "INFERRED_UNVERIFIED";
    case KICAD_PIN_MAP_INCOMPLETE: return "INCOMPLETE";
    case KICAD_PIN_MAP_NOT_APPLICABLE: return "NOT_APPLICABLE";
    default: return "NOT_CHECKED";
    }
}

static bool split_library_id(const char *id, char *nickname, size_t nickname_cap,
                             const char **entry) {
    if (!id || !nickname || !entry) return false;
    const char *colon = strchr(id, ':');
    if (!colon || colon == id || !colon[1] || strchr(colon + 1, ':')) return false;
    size_t n = (size_t)(colon - id);
    if (n >= nickname_cap) return false;
    memcpy(nickname, id, n); nickname[n] = '\0';
    *entry = colon + 1;
    return true;
}

static size_t build_kicad_path_variables(KicadPathVariable *variables,
                                         char storage[][FS_PATH_MAX],
                                         size_t capacity,
                                         const KicadAssetRoots *roots) {
    static const char *const names[] = {
        "KICAD9_SYMBOL_DIR", "KICAD9_FOOTPRINT_DIR",
        "KICAD_SYMBOL_DIR", "KICAD_FOOTPRINT_DIR"
    };
    const char *fallbacks[] = {
        roots ? roots->symbol_root : NULL,
        roots ? roots->footprint_root : NULL,
        roots ? roots->symbol_root : NULL,
        roots ? roots->footprint_root : NULL
    };
    size_t count = 0;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]) && count < capacity; ++i) {
        if (env_copy(names[i], storage[count], FS_PATH_MAX)) {
            variables[count].name = names[i];
            variables[count].value = storage[count];
            ++count;
        } else if (fallbacks[i]) {
            size_t n = strlen(fallbacks[i]);
            if (n < FS_PATH_MAX) {
                memcpy(storage[count], fallbacks[i], n + 1u);
                variables[count].name = names[i];
                variables[count].value = storage[count];
                ++count;
            }
        }
    }
    return count;
}

static bool expand_table_entry_uri(Arena *arena, const KicadAssetRoots *roots,
                                   const char *table_path, KicadLibTableKind kind,
                                   const char *nickname, const char **out_uri) {
    void *data = NULL;
    size_t length = 0;
    if (!arena || !table_path || !nickname || !out_uri) return false;
    *out_uri = NULL;
    if (fs_read_file(table_path, &data, &length) != FS_OK) return false;
    if (memchr(data, '\0', length) != NULL) { free(data); return false; }
    KicadLibTable table;
    IRError parse_error = {0};
    KicadLibStatus parse_status = kicad_lib_table_parse(arena,
        (const char *)data, length, kind, &table, &parse_error);
    free(data);
    if (parse_status != KICAD_LIB_OK) return false;
    const KicadLibEntry *entry = kicad_lib_find_entry(&table, NULL, nickname);
    if (!entry || !entry->uri) return false;

    KicadPathVariable variables[8];
    char storage[8][FS_PATH_MAX];
    size_t variable_count = build_kicad_path_variables(variables, storage, 8u, roots);
    KicadLibPathContext context;
    memset(&context, 0, sizeof(context));
    context.project_dir = roots ? roots->project_directory : NULL;
    context.variables = variables;
    context.variable_count = variable_count;
    return kicad_lib_expand_uri(arena, entry->uri, &context, out_uri) == KICAD_LIB_OK;
}

static KicadAssetStatus load_symbol_from_table(Arena *arena,
                                                const KicadAssetRoots *roots,
                                                const char *id,
                                                KicadSymbolLibrary *lib,
                                                KicadAssetError *error,
                                                const char **out_path) {
    char nickname[256];
    const char *entry = NULL;
    if (!roots || !roots->symbol_table_path ||
        !split_library_id(id, nickname, sizeof(nickname), &entry))
        return KICAD_ASSET_ERR_NOT_FOUND;
    (void)entry;
    const char *path = NULL;
    if (!expand_table_entry_uri(arena, roots, roots->symbol_table_path,
                                KICAD_LIB_TABLE_SYMBOLS, nickname, &path))
        return KICAD_ASSET_ERR_NOT_FOUND;
    void *data = NULL; size_t length = 0;
    FsStatus fs = fs_read_file(path, &data, &length);
    if (fs != FS_OK) return fs == FS_ERR_NOT_FOUND ? KICAD_ASSET_ERR_NOT_FOUND : KICAD_ASSET_ERR_IO;
    if (memchr(data, '\0', length) != NULL) { free(data); return KICAD_ASSET_ERR_PARSE; }
    KicadAssetStatus status = kicad_symbol_library_parse(arena,
        (const char *)data, length, lib, error);
    free(data);
    if (status == KICAD_ASSET_OK && out_path) *out_path = arena_strdup_local(arena, path);
    return status;
}

static KicadAssetStatus load_footprint_from_table(Arena *arena,
                                                   const KicadAssetRoots *roots,
                                                   const char *id,
                                                   KicadFootprintAsset *fp,
                                                   KicadAssetError *error,
                                                   const char **out_path) {
    char nickname[256], filename[512], path[FS_PATH_MAX];
    const char *entry = NULL, *directory = NULL;
    if (!roots || !roots->footprint_table_path ||
        !split_library_id(id, nickname, sizeof(nickname), &entry))
        return KICAD_ASSET_ERR_NOT_FOUND;
    if (!expand_table_entry_uri(arena, roots, roots->footprint_table_path,
                                KICAD_LIB_TABLE_FOOTPRINTS, nickname, &directory))
        return KICAD_ASSET_ERR_NOT_FOUND;
    int n = snprintf(filename, sizeof(filename), "%s.kicad_mod", entry);
    if (n < 0 || (size_t)n >= sizeof(filename) ||
        fs_path_join(path, sizeof(path), directory, filename) != FS_OK)
        return KICAD_ASSET_ERR_RANGE;
    void *data = NULL; size_t length = 0;
    FsStatus fs = fs_read_file(path, &data, &length);
    if (fs != FS_OK) return fs == FS_ERR_NOT_FOUND ? KICAD_ASSET_ERR_NOT_FOUND : KICAD_ASSET_ERR_IO;
    if (memchr(data, '\0', length) != NULL) { free(data); return KICAD_ASSET_ERR_PARSE; }
    KicadAssetStatus status = kicad_footprint_parse(arena,
        (const char *)data, length, fp, error);
    free(data);
    if (status == KICAD_ASSET_OK && out_path) *out_path = arena_strdup_local(arena, path);
    return status;
}

static KicadAssetStatus load_symbol(Arena *arena, const char *root,
                                   const char *id, KicadSymbolLibrary *lib,
                                   KicadAssetError *error, const char **out_path) {
    char nickname[256], filename[300], path[FS_PATH_MAX];
    const char *entry = NULL;
    if (!split_library_id(id, nickname, sizeof(nickname), &entry))
        return KICAD_ASSET_ERR_SCHEMA;
    (void)entry;
    int n = snprintf(filename, sizeof(filename), "%s.kicad_sym", nickname);
    if (n < 0 || (size_t)n >= sizeof(filename) ||
        fs_path_join(path, sizeof(path), root, filename) != FS_OK)
        return KICAD_ASSET_ERR_RANGE;
    void *data = NULL; size_t length = 0;
    FsStatus fs = fs_read_file(path, &data, &length);
    if (fs != FS_OK) return fs == FS_ERR_NOT_FOUND ? KICAD_ASSET_ERR_NOT_FOUND : KICAD_ASSET_ERR_IO;
    if (memchr(data, '\0', length) != NULL) { free(data); return KICAD_ASSET_ERR_PARSE; }
    KicadAssetStatus status = kicad_symbol_library_parse(arena, (const char *)data,
                                                          length, lib, error);
    free(data);
    if (status == KICAD_ASSET_OK && out_path) *out_path = arena_strdup_local(arena, path);
    return status;
}

static KicadAssetStatus load_footprint(Arena *arena, const char *root,
                                      const char *id, KicadFootprintAsset *fp,
                                      KicadAssetError *error, const char **out_path) {
    char nickname[256], dirname[300], filename[512], libdir[FS_PATH_MAX], path[FS_PATH_MAX];
    const char *entry = NULL;
    if (!split_library_id(id, nickname, sizeof(nickname), &entry)) return KICAD_ASSET_ERR_SCHEMA;
    int n1 = snprintf(dirname, sizeof(dirname), "%s.pretty", nickname);
    int n2 = snprintf(filename, sizeof(filename), "%s.kicad_mod", entry);
    if (n1 < 0 || (size_t)n1 >= sizeof(dirname) || n2 < 0 ||
        (size_t)n2 >= sizeof(filename) ||
        fs_path_join(libdir, sizeof(libdir), root, dirname) != FS_OK ||
        fs_path_join(path, sizeof(path), libdir, filename) != FS_OK)
        return KICAD_ASSET_ERR_RANGE;
    void *data = NULL; size_t length = 0;
    FsStatus fs = fs_read_file(path, &data, &length);
    if (fs != FS_OK) return fs == FS_ERR_NOT_FOUND ? KICAD_ASSET_ERR_NOT_FOUND : KICAD_ASSET_ERR_IO;
    if (memchr(data, '\0', length) != NULL) { free(data); return KICAD_ASSET_ERR_PARSE; }
    KicadAssetStatus status = kicad_footprint_parse(arena, (const char *)data,
                                                     length, fp, error);
    free(data);
    if (status == KICAD_ASSET_OK && out_path) *out_path = arena_strdup_local(arena, path);
    return status;
}

static bool has_seen_number(const KicadSymbolAsset *symbol, size_t before,
                            const char *number) {
    if (!symbol || !number) return false;
    size_t seen = 0;
    for (size_t i = 0; i < before && i < symbol->pin_count; ++i) {
        if (symbol->pins[i].number && strcmp(symbol->pins[i].number, number) == 0) ++seen;
    }
    return seen != 0;
}

static void count_symbol_pad_coverage(KicadPartAssetResolution *out) {
    if (!out || !out->symbol || !out->footprint) return;
    out->unique_symbol_pin_count = 0;
    out->matching_pad_count = 0;
    out->missing_pad_count = 0;
    for (size_t i = 0; i < out->symbol->pin_count; ++i) {
        const char *number = out->symbol->pins[i].number;
        if (!number || !number[0] || has_seen_number(out->symbol, i, number)) continue;
        ++out->unique_symbol_pin_count;
        if (kicad_footprint_has_pad(out->footprint, number)) ++out->matching_pad_count;
        else ++out->missing_pad_count;
    }
}

static const char *suggest_symbol(const PartRecord *part, Arena *arena) {
    const char *name = NULL;
    switch (part->kind) {
    case PART_KIND_RESISTOR: name = "Device:R"; break;
    case PART_KIND_CAPACITOR: name = "Device:C"; break;
    case PART_KIND_INDUCTOR: name = "Device:L"; break;
    case PART_KIND_DIODE:
        if ((part->description && strstr(part->description, "LED")) ||
            (part->description && strstr(part->description, "led"))) name = "Device:LED";
        else name = "Device:D";
        break;
    default: break;
    }
    return name ? arena_strdup_local(arena, name) : NULL;
}

KicadAssetStatus kicad_part_asset_resolve(
    Arena *arena, const PartRecord *part, const KicadAssetRoots *roots,
    KicadPartAssetResolution *out, KicadAssetError *error) {
    if (out) memset(out, 0, sizeof(*out));
    if (!arena || !part || !roots || !out) return KICAD_ASSET_ERR_ARGUMENT;
    out->symbol_status = KICAD_ASSET_ERR_NOT_FOUND;
    out->footprint_status = KICAD_ASSET_ERR_NOT_FOUND;
    out->mapping_status = KICAD_ASSET_ERR_NOT_FOUND;
    out->pin_map_resolution = KICAD_PIN_MAP_NOT_CHECKED;
    out->suggested_symbol_id = part->symbol_id ? NULL : suggest_symbol(part, arena);
    out->effective_symbol_id = part->symbol_id ? part->symbol_id : out->suggested_symbol_id;
    out->symbol_is_suggestion = !part->symbol_id && out->effective_symbol_id != NULL;

    if (!out->effective_symbol_id || !out->effective_symbol_id[0]) {
        out->symbol_status = KICAD_ASSET_ERR_NOT_FOUND;
        out->message = "Catalogue record has no symbol ID and no safe generic symbol suggestion.";
    } else if (!roots->symbols_available && !roots->symbol_table_path) {
        out->symbol_status = KICAD_ASSET_ERR_IO;
        out->message = "KiCad symbol library root was not discovered; supply --symbol-root.";
    } else {
        KicadSymbolLibrary *lib = (KicadSymbolLibrary *)arena_alloc(arena, sizeof(*lib));
        if (!lib) return KICAD_ASSET_ERR_MEMORY;
        memset(lib, 0, sizeof(*lib));
        KicadAssetError local = {0};
        bool explicit_symbol_root = roots->symbol_source &&
            strcmp(roots->symbol_source, "explicit --symbol-root") == 0;
        if (explicit_symbol_root && roots->symbol_root) {
            out->symbol_status = load_symbol(arena, roots->symbol_root,
                out->effective_symbol_id, lib, &local, &out->symbol_path);
        } else {
            out->symbol_status = load_symbol_from_table(arena, roots,
                out->effective_symbol_id, lib, &local, &out->symbol_path);
            if (out->symbol_status != KICAD_ASSET_OK && roots->symbols_available && roots->symbol_root) {
                memset(&local, 0, sizeof(local));
                out->symbol_status = load_symbol(arena, roots->symbol_root,
                    out->effective_symbol_id, lib, &local, &out->symbol_path);
            }
        }
        if (out->symbol_status == KICAD_ASSET_OK) {
            char nickname[256]; const char *entry = NULL;
            if (!split_library_id(out->effective_symbol_id, nickname, sizeof(nickname), &entry))
                out->symbol_status = KICAD_ASSET_ERR_SCHEMA;
            else {
                out->symbol = kicad_symbol_find(lib, entry);
                if (!out->symbol) out->symbol_status = KICAD_ASSET_ERR_NOT_FOUND;
                else {
                    out->symbol_found = true;
                    out->symbol_library = lib;
                    out->suggested_footprint_filter = out->symbol->footprint_filter;
                    if (out->symbol->footprint_default && out->symbol->footprint_default[0] &&
                        !part->footprint_id) {
                        out->suggested_footprint_id = out->symbol->footprint_default;
                        out->effective_footprint_id = out->suggested_footprint_id;
                        out->footprint_is_suggestion = true;
                    }
                }
            }
        }
        if (out->symbol_status != KICAD_ASSET_OK && local.message) out->message = local.message;
    }

    if (!out->effective_footprint_id) out->effective_footprint_id = part->footprint_id;
    if (!out->effective_footprint_id || !out->effective_footprint_id[0]) {
        out->footprint_status = KICAD_ASSET_ERR_NOT_FOUND;
        if (!out->message) out->message = "Catalogue record has no footprint ID; no package was guessed.";
    } else if (!roots->footprints_available && !roots->footprint_table_path) {
        out->footprint_status = KICAD_ASSET_ERR_IO;
        if (!out->message) out->message = "KiCad footprint library root was not discovered; supply --footprint-root.";
    } else {
        KicadFootprintAsset *fp = (KicadFootprintAsset *)arena_alloc(arena, sizeof(*fp));
        if (!fp) return KICAD_ASSET_ERR_MEMORY;
        memset(fp, 0, sizeof(*fp));
        KicadAssetError local = {0};
        bool explicit_footprint_root = roots->footprint_source &&
            strcmp(roots->footprint_source, "explicit --footprint-root") == 0;
        if (explicit_footprint_root && roots->footprint_root) {
            out->footprint_status = load_footprint(arena, roots->footprint_root,
                out->effective_footprint_id, fp, &local, &out->footprint_path);
        } else {
            out->footprint_status = load_footprint_from_table(arena, roots,
                out->effective_footprint_id, fp, &local, &out->footprint_path);
            if (out->footprint_status != KICAD_ASSET_OK && roots->footprints_available && roots->footprint_root) {
                memset(&local, 0, sizeof(local));
                out->footprint_status = load_footprint(arena, roots->footprint_root,
                    out->effective_footprint_id, fp, &local, &out->footprint_path);
            }
        }
        if (out->footprint_status == KICAD_ASSET_OK) {
            const char *colon = strchr(out->effective_footprint_id, ':');
            const char *name = colon ? colon + 1 : NULL;
            if (name && fp->name && strcmp(fp->name, name) == 0) {
                out->footprint = fp;
                out->footprint_found = true;
            } else out->footprint_status = KICAD_ASSET_ERR_NOT_FOUND;
        }
        if (out->footprint_status != KICAD_ASSET_OK && local.message && !out->message)
            out->message = local.message;
    }

    if (out->symbol_found && out->footprint_found) {
        count_symbol_pad_coverage(out);
        if (part->pin_count > 0) {
            KicadAssetError local = {0};
            out->mapping_status = kicad_part_assets_validate(part, out->symbol_library,
                                                               out->footprint, &local);
            if (out->mapping_status == KICAD_ASSET_OK) {
                out->pin_map_resolution = KICAD_PIN_MAP_EXPLICIT_VALID;
                out->explicit_mapping_valid = true;
            } else {
                out->pin_map_resolution = KICAD_PIN_MAP_EXPLICIT_INVALID;
                if (!out->message) out->message = local.message;
            }
        } else {
            out->inferred_mapping_possible = out->unique_symbol_pin_count > 0 &&
                out->missing_pad_count == 0;
            out->mapping_status = out->inferred_mapping_possible ? KICAD_ASSET_OK : KICAD_ASSET_ERR_SCHEMA;
            out->pin_map_resolution = out->inferred_mapping_possible ?
                KICAD_PIN_MAP_INFERRED_UNVERIFIED : KICAD_PIN_MAP_INCOMPLETE;
            if (!out->message) out->message = out->inferred_mapping_possible ?
                "Pin numbers match footprint pad numbers, but no explicit terminal mapping exists; this is an unverified inference." :
                "No explicit pin mapping exists and one or more symbol pin numbers do not match footprint pads.";
        }
    } else if (!out->symbol_found && !out->footprint_found) {
        out->pin_map_resolution = KICAD_PIN_MAP_NOT_APPLICABLE;
    }

    if (error) {
        error->code = (!out->symbol_found || !out->footprint_found) ?
            KICAD_ASSET_ERR_NOT_FOUND : out->mapping_status;
        error->message = out->message;
        error->item_index = SIZE_MAX;
        ir_error_clear(&error->ir_error);
    }
    if (out->explicit_mapping_valid) return KICAD_ASSET_OK;
    if (out->inferred_mapping_possible) return KICAD_ASSET_ERR_SCHEMA;
    if (!out->symbol_found || !out->footprint_found) return KICAD_ASSET_ERR_NOT_FOUND;
    return out->mapping_status;
}

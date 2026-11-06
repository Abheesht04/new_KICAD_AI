/* Native KiCad integration (v2-native-kicad-integration): the Excellon reader on a real KiCad 9.0.6 drill file,
 * KiCad library tables, filesystem helpers, and native KiCad 9 project creation. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "copper_project.h"
#include "excellon.h"
#include "filesystem.h"
#include "kicad_library_table.h"

#define MM(v) ((G2Nm)((v) * 1000000.0))

static void test_excellon_reads_kicad_906(void)
{
    /* manufacturing_audit/drill_test/design.drl: KiCad 9.0.6's drill export of the divider board (J1 header). */
    Arena arena;
    arena_init(&arena, 1 << 16);
    ExcFile f;
    assert(exc_parse_path(&arena, KICAD9_CAM_DIR "/drill_test/design.drl", &f) == EXC_OK);
    assert(f.units == EXC_UNITS_METRIC && f.zeros == EXC_ZEROS_DECIMAL && f.absolute && f.fmat == 2);
    assert(f.file_plating == EXC_PLATING_MIXED && f.layer_from == 1 && f.layer_to == 2 && f.saw_end);
    assert(f.tool_count == 1 && f.error_count == 0);
    const ExcTool *t1 = exc_find_tool(&f, 1);
    assert(t1 && t1->diameter == MM(1.0) && t1->plating == EXC_PLATING_PLATED && t1->function == EXC_DRILL_COMPONENT);
    assert(f.hit_count == 3 && t1->hit_count == 3);
    const G2Nm y[] = {MM(-12.0), MM(-14.54), MM(-17.08)};
    for (size_t i = 0; i < 3; ++i)
        assert(f.hits[i].pos.x == MM(8.0) && f.hits[i].pos.y == y[i]);
    arena_destroy(&arena);
}

static ExcStatus parse(const char *text, ExcFile *f, Arena *arena)
{
    arena_reset(arena);
    return exc_parse(arena, text, strlen(text), "inline", f);
}

static int has_issue(const ExcFile *f, ExcIssueCode code)
{
    for (size_t i = 0; i < f->issue_count; ++i)
        if (f->issues[i].code == code)
            return 1;
    return 0;
}

static void test_excellon_refuses_what_it_cannot_read(void)
{
    G2Nm nm;
    assert(exc_decimal_to_nm("-14.54", 6, EXC_UNITS_METRIC, &nm) == EXC_ISSUE_NONE && nm == -14540000);
    assert(exc_decimal_to_nm("0.001", 5, EXC_UNITS_METRIC, &nm) == EXC_ISSUE_NONE && nm == 1000);
    assert(exc_decimal_to_nm("1.0", 3, EXC_UNITS_INCH, &nm) == EXC_ISSUE_NONE && nm == 25400000); /* exact */
    assert(exc_decimal_to_nm("1.2.3", 5, EXC_UNITS_METRIC, &nm) == EXC_ISSUE_BAD_NUMBER);

    Arena arena;
    arena_init(&arena, 1 << 16);
    ExcFile f;
    /* A hit with no tool selected, and a tool used but never defined: content errors, never silently dropped. */
    assert(parse("M48\nMETRIC\nT1C0.800\n%\nG90\nX1.0Y1.0\nT2\nX2.0Y2.0\nM30\n", &f, &arena) == EXC_ERR_CONTENT);
    assert(has_issue(&f, EXC_ISSUE_HIT_BEFORE_TOOL) && has_issue(&f, EXC_ISSUE_UNDEFINED_TOOL));
    /* No M48 header and no units: refused. */
    assert(parse("T1C0.8\nT1\nX1.0Y1.0\nM30\n", &f, &arena) == EXC_ERR_CONTENT && has_issue(&f, EXC_ISSUE_MISSING_HEADER));
    /* Implicit-decimal coordinates (zero suppression) are not guessed. */
    assert(parse("M48\nMETRIC,LZ\nT1C0.800\n%\nG90\nT1\nX010000Y010000\nM30\n", &f, &arena) == EXC_ERR_CONTENT);
    assert(has_issue(&f, EXC_ISSUE_UNSUPPORTED_FORMAT));
    arena_destroy(&arena);
}

static void test_library_tables(void)
{
    static const char table[] =
        "(sym_lib_table (version 7)\n"
        "  (lib (name \"Device\") (type \"KiCad\") (uri \"${KICAD9_SYMBOL_DIR}/Device.kicad_sym\") (options \"\") (descr \"Devices\"))\n"
        "  (lib (name \"Mine\") (type \"KiCad\") (uri \"${KIPRJMOD}/libs/mine.kicad_sym\") (options \"\") (descr \"\")))\n";
    Arena arena;
    arena_init(&arena, 1 << 16);
    KicadLibTable t, wrong;
    IRError e;
    assert(kicad_lib_table_parse(&arena, table, sizeof table - 1, KICAD_LIB_TABLE_SYMBOLS, &t, &e) == KICAD_LIB_OK);
    assert(t.version == 7 && t.entry_count == 2 && !strcmp(t.entries[1].nickname, "Mine"));
    assert(kicad_lib_table_parse(&arena, table, sizeof table - 1, KICAD_LIB_TABLE_FOOTPRINTS, &wrong, &e) == KICAD_LIB_ERR_SCHEMA);

    KicadPathVariable vars[] = {{"KICAD9_SYMBOL_DIR", "C:/KiCad/9.0/share/kicad/symbols"}};
    KicadLibPathContext ctx = {"D:/boards/demo", vars, 1};
    const char *path;
    assert(kicad_lib_expand_uri(&arena, t.entries[0].uri, &ctx, &path) == KICAD_LIB_OK &&
           !strcmp(path, "C:/KiCad/9.0/share/kicad/symbols/Device.kicad_sym"));
    assert(kicad_lib_expand_uri(&arena, t.entries[1].uri, &ctx, &path) == KICAD_LIB_OK && !strcmp(path, "D:/boards/demo/libs/mine.kicad_sym"));
    ctx.variable_count = 0; /* undefined ${VAR}: an error, never left in the path */
    assert(kicad_lib_expand_uri(&arena, t.entries[0].uri, &ctx, &path) == KICAD_LIB_ERR_UNRESOLVED_VARIABLE);
    assert(!strcmp(kicad_lib_status_name(KICAD_LIB_ERR_UNRESOLVED_VARIABLE), "UNRESOLVED_VARIABLE")); /* was UNKNOWN_STATUS */

    /* The project table's nickname wins over the global one. */
    KicadLibEntry local = {"Device", "KiCad", "${KIPRJMOD}/Device.kicad_sym", "", "", 0};
    KicadLibTable project = {KICAD_LIB_TABLE_SYMBOLS, 7, &local, 1};
    assert(kicad_lib_find_entry(&project, &t, "Device") == &local && kicad_lib_find_entry(&project, &t, "Mine") == &t.entries[1]);
    assert(kicad_lib_find_entry(&project, &t, "Absent") == NULL);
    arena_destroy(&arena);
}

static void temp_root(char *out, size_t cap)
{
    const char *base = getenv("TEMP");
    if (!base)
        base = getenv("TMPDIR");
    if (!base)
        base = "/tmp";
    char name[64];
    snprintf(name, sizeof name, "copper-native-%lu-%lu", (unsigned long)time(NULL), (unsigned long)clock());
    assert(fs_path_join(out, cap, base, name) == FS_OK);
}

static void test_filesystem_and_project(void)
{
    char root[FS_PATH_MAX], path[FS_PATH_MAX], name[FS_PATH_MAX];
    temp_root(root, sizeof root);
    assert(fs_create_directories(root) == FS_OK && fs_directory_exists(root));

    assert(fs_path_join(path, sizeof path, root, "x.txt") == FS_OK && fs_path_basename(name, sizeof name, path) == FS_OK && !strcmp(name, "x.txt"));
    assert(fs_write_file_exclusive(path, "a", 1) == FS_OK && fs_file_exists(path));
    assert(fs_write_file_exclusive(path, "b", 1) == FS_ERR_ALREADY_EXISTS); /* never overwrites */

    Arena arena;
    arena_init(&arena, 1 << 16);
    assert(copper_project_name_is_valid("sensor_board") && !copper_project_name_is_valid("../escape") && !copper_project_name_is_valid(""));
    CopperProjectCreateOptions o = {root, "sensor_board", true, NULL};
    CopperProject p;
    CopperProjectError err;
    assert(copper_project_create(&arena, &o, &p, &err) == COPPER_PROJECT_OK);
    assert(fs_file_exists(p.project_file) && fs_file_exists(p.schematic_file) && fs_file_exists(p.board_file));
    assert(fs_directory_exists(p.cache_dir) && fs_directory_exists(p.logs_dir));
    assert(copper_project_create(&arena, &o, &p, &err) == COPPER_PROJECT_ERR_ALREADY_EXISTS); /* never overwrites */

    CopperProject opened;
    char dir[FS_PATH_MAX];
    assert(fs_path_join(dir, sizeof dir, root, "sensor_board") == FS_OK);
    assert(copper_project_open(&arena, dir, &opened, &err) == COPPER_PROJECT_OK);
    assert(opened.has_schematic && opened.has_board && !strcmp(opened.name, "sensor_board"));
    assert(copper_project_open(&arena, root, &opened, &err) != COPPER_PROJECT_OK); /* no .kicad_pro: not a project */

    arena_destroy(&arena);
    assert(fs_remove_tree(root) == FS_OK && !fs_directory_exists(root));
}

int main(void)
{
    test_excellon_reads_kicad_906();
    test_excellon_refuses_what_it_cannot_read();
    test_library_tables();
    test_filesystem_and_project();
    printf("kicad_native: PASSED\n");
    return 0;
}

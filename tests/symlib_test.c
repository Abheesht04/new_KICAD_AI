/*
 * symlib_test.c  - inline-text tests always run; the real-library test runs
 * only when KICAD_SYM_DIR points at a KiCad "symbols" directory (it is the
 * user's own install; nothing from it is stored in this repo).
 */
#include "symlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static const char *LIB_OK =
"(kicad_symbol_lib (version 20241209) (generator \"t\")\n"
" (symbol \"D0\" (pin_numbers (hide yes)) (pin_names (offset 1.016) (hide yes))\n"
"  (in_bom yes) (on_board yes)\n"
"  (property \"Reference\" \"D\" (at 0 2.54 0))\n"
"  (property \"Value\" \"D0\" (at 0 -2.54 0))\n"
"  (property \"Footprint\" \"\" (at 0 0 0))\n"
"  (property \"ki_fp_filters\" \"D*DO?35* SOD-123\" (at 0 0 0))\n"
"  (symbol \"D0_0_1\" (polyline (pts (xy -1.27 1.27) (xy -1.27 -1.27)) (stroke (width 0)))\n"
"   (rectangle (start -1.27 1.27) (end 1.27 -1.27)))\n"
"  (symbol \"D0_1_1\"\n"
"   (pin passive line (at -3.81 0 0) (length 2.54) (name \"K\" (effects)) (number \"1\" (effects)))\n"
"   (pin passive line (at 3.81 0 180) (length 2.54) (name \"A\" (effects)) (number \"2\" (effects)))))\n"
" (symbol \"D1\" (extends \"D0\")\n"
"  (property \"Value\" \"D1\" (at 0 -2.54 0))\n"
"  (property \"Footprint\" \"Diode_THT:D_DO-35\" (at 0 0 0)))\n"
" (symbol \"D2\" (extends \"D1\") (property \"Value\" \"D2\" (at 0 0 0)))\n"
" (symbol \"Dual\"\n"
"  (symbol \"Dual_1_1\" (pin input line (at 0 5.08 270) (length 2.54) (name \"~\" (effects)) (number \"A\" (effects))))\n"
"  (symbol \"Dual_2_1\" (pin output line (at 0 -5.08 90) (length 2.54) (name \"Y\" (effects)) (number \"B\" (effects)) (hide yes)))\n"
"  (symbol \"Dual_0_1\" (pin power_in line (at 5.08 0 180) (length 2.54) (name \"V\" (effects)) (number \"V\" (effects)))))\n"
")\n";

static int load(const char *txt, SymLib *lib, SymError *e, Arena *a)
{
    return symlib_load_text(a, txt, strlen(txt), "inline", lib, e);
}

static void test_basic(void)
{
    Arena a; SymLib lib; SymError e; const SymbolDef *d, *d1, *d2, *dual; const SymPin *p;
    arena_init(&a, 1 << 16);
    CHECK(load(LIB_OK, &lib, &e, &a) == SYM_OK);
    CHECK(lib.symbol_count == 4);
    CHECK(strcmp(lib.symbols[0].name, "D0") == 0);        /* sorted */
    d = symlib_find(&lib, "D0");
    CHECK(d && d->pin_count == 2 && d->unit_count == 1);
    CHECK(d && d->pin_numbers_hidden && d->pin_names_hidden && d->pin_name_offset == 1016000);
    CHECK(d && d->has_body && d->body_min.x == -1270000 && d->body_max.y == 1270000);
    p = symdef_pin_by_number(d, "1", 1);
    CHECK(p && strcmp(p->name, "K") == 0 && p->type == SYM_PIN_PASSIVE);
    CHECK(p && p->pos.x == -3810000 && p->pos.y == 0 && p->angle_deg == 0 && p->length == 2540000);
    CHECK(p && sympin_body_end(p).x == -1270000);          /* body edge */
    p = symdef_pin_by_number(d, "2", 1);
    CHECK(p && sympin_body_end(p).x == 1270000);

    d1 = symlib_find(&lib, "D1");
    CHECK(d1 && d1->pin_count == 2 && d1->pins == d->pins);   /* inherited */
    CHECK(d1 && d1->pin_numbers_hidden && d1->has_body);
    CHECK(d1 && strcmp(symdef_prop(d1, "Value"), "D1") == 0);        /* override */
    CHECK(d1 && strcmp(symdef_prop(d1, "Footprint"), "Diode_THT:D_DO-35") == 0);
    CHECK(d1 && strcmp(symdef_prop(d1, "Reference"), "D") == 0);     /* inherited */
    d2 = symlib_find(&lib, "D2");                                   /* 2-deep chain */
    CHECK(d2 && d2->pin_count == 2 && strcmp(symdef_prop(d2, "Value"), "D2") == 0);
    CHECK(d2 && strcmp(symdef_prop(d2, "Footprint"), "Diode_THT:D_DO-35") == 0);
    CHECK(symdef_prop(d2, "Nope") == NULL);

    dual = symlib_find(&lib, "Dual");
    CHECK(dual && dual->pin_count == 3 && dual->unit_count == 2);
    p = symdef_pin_by_number(dual, "B", 2);
    CHECK(p && p->hidden && p->unit == 2 && p->type == SYM_PIN_OUTPUT && sympin_body_end(p).y == -2540000);
    CHECK(symdef_pin_by_number(dual, "B", 1) == NULL);        /* unit 2 only */
    CHECK(symdef_pin_by_number(dual, "V", 1) != NULL);        /* common pin */
    CHECK(symlib_find(&lib, "Missing") == NULL);

    CHECK(symdef_fp_filter_match(d, "D_DO-35_SOD27_P7.62mm_Horizontal"));
    CHECK(symdef_fp_filter_match(d, "sod-123"));
    CHECK(!symdef_fp_filter_match(d, "R_0603_1608Metric"));
    CHECK(!symdef_fp_filter_match(dual, "anything"));          /* no filter != all */
    arena_destroy(&a);
}

static void expect_err(const char *txt, SymStatus want, const char *sym)
{
    Arena a; SymLib lib; SymError e;
    arena_init(&a, 1 << 16);
    CHECK(load(txt, &lib, &e, &a) == want);
    CHECK(e.code == want && lib.symbol_count == 0 && lib.symbols == NULL);
    if (sym) CHECK(e.symbol && strcmp(e.symbol, sym) == 0);
    arena_destroy(&a);
}

static void test_errors(void)
{
    expect_err("(kicad_symbol_lib (symbol \"A\" (extends \"B\")) (symbol \"B\" (extends \"A\")))",
               SYM_ERR_EXTENDS, "A");
    expect_err("(kicad_symbol_lib (symbol \"A\" (extends \"Nope\")))", SYM_ERR_EXTENDS, "A");
    expect_err("(kicad_symbol_lib (symbol \"A\") (symbol \"A\"))", SYM_ERR_DUPLICATE, "A");
    expect_err("(kicad_symbol_lib (symbol \"A\" (symbol \"A_1_1\" (pin passive line (at 0 0 45)"
               " (length 1) (name \"x\") (number \"1\")))))", SYM_ERR_NUMBER, "A");
    expect_err("(kicad_symbol_lib (symbol \"A\" (symbol \"A_1_1\" (pin weird line (at 0 0 0)"
               " (length 1) (name \"x\") (number \"1\")))))", SYM_ERR_FORMAT, "A");
    expect_err("(kicad_symbol_lib (symbol \"A\" (symbol \"B_1_1\")))", SYM_ERR_FORMAT, "A");
    expect_err("(kicad_symbol_lib (symbol \"P\" (symbol \"P_1_1\" (pin passive line (at 0 0 0)"
               " (length 1) (name \"x\") (number \"1\")))) (symbol \"C\" (extends \"P\")"
               " (symbol \"C_1_1\" (pin passive line (at 0 0 0) (length 1) (name \"x\")"
               " (number \"1\")))))", SYM_ERR_EXTENDS, "C");   /* child with own pins */
    expect_err("(not_a_lib)", SYM_ERR_FORMAT, NULL);
    expect_err("(kicad_symbol_lib (symbol \"A\"", SYM_ERR_PARSE, NULL);
    {   /* missing file */
        Arena a; SymLib lib; SymError e;
        arena_init(&a, 4096);
        CHECK(symlib_load_file(&a, "no/such/file.kicad_sym", &lib, &e) == SYM_ERR_IO);
        arena_destroy(&a);
    }
}

static void test_glob(void)
{
    CHECK(symlib_glob_match("R_*", "R_0603_1608Metric"));
    CHECK(!symlib_glob_match("R_*", "C_0603"));
    CHECK(symlib_glob_match("D*DO?35*", "D_DO-35_SOD27"));
    CHECK(symlib_glob_match("*", ""));
    CHECK(!symlib_glob_match("?", ""));
    CHECK(symlib_glob_match("a*b*c", "aXXbYYc"));
    CHECK(!symlib_glob_match("a*b*c", "aXXbYY"));
}

static void test_real_diode(const char *dir)
{
    char path[1024]; Arena a; SymLib lib; SymError e; const SymbolDef *s; const SymPin *k, *an;
    size_t i, ext = 0, with_pins = 0;
    snprintf(path, sizeof path, "%s/Diode.kicad_sym", dir);
    arena_init(&a, 1 << 20);
    CHECK(symlib_load_file(&a, path, &lib, &e) == SYM_OK);
    if (e.code != SYM_OK) printf("  load error: %s (%s)\n", e.message, e.symbol ? e.symbol : "-");
    CHECK(lib.symbol_count == 762);
    for (i = 0; i < lib.symbol_count; ++i) {
        if (lib.symbols[i].extends) ++ext;
        if (lib.symbols[i].pin_count) ++with_pins;
    }
    CHECK(ext == 701);
    CHECK(with_pins == 762);              /* every stub now carries pins */
    s = symlib_find(&lib, "1N4148");
    CHECK(s && s->extends && strcmp(s->extends, "1N4001") == 0 && s->pin_count == 2);
    CHECK(s && strcmp(symdef_prop(s, "Footprint"), "Diode_THT:D_DO-35_SOD27_P7.62mm_Horizontal") == 0);
    CHECK(s && strcmp(symdef_prop(s, "Description"), "100V 0.15A standard switching diode, DO-35") == 0);
    k = symdef_pin_by_number(s, "1", 1);
    an = symdef_pin_by_number(s, "2", 1);
    CHECK(k && strcmp(k->name, "K") == 0 && k->pos.x == -3810000 && k->pos.y == 0 && k->angle_deg == 0);
    CHECK(an && strcmp(an->name, "A") == 0 && an->pos.x == 3810000 && an->angle_deg == 180);
    CHECK(s && symdef_fp_filter_match(s, "D_DO-35_SOD27_P7.62mm_Horizontal"));
    printf("  real Diode.kicad_sym: %zu symbols, %zu extends, ok\n", lib.symbol_count, ext);
    arena_destroy(&a);
}

int main(void)
{
    const char *dir = getenv("KICAD_SYM_DIR");
    test_basic();
    test_errors();
    test_glob();
    if (dir && *dir) test_real_diode(dir);
    else printf("  (KICAD_SYM_DIR not set: real-library test UNVERIFIED/skipped)\n");
    if (failures) { printf("symlib_test: %d FAILED\n", failures); return 1; }
    printf("symlib_test: all passed\n");
    return 0;
}

/* sch_rules (src/v2_src/sch_rules.c, Step 3d): KiCad PWR_FLAGs on the nets the blocks say are external supplies, and
 * the project's library tables.
 *
 * Inputs:
 *   - SCH below: Copper-written, in the form copper_design_compile.py writes (header, lib_symbols, a symbol instance,
 *     labels, sheet_instances); not a KiCad export.
 *   - fixtures/sample_resistor_lol: a real KiCad 9.0 project (it already has one PWR_FLAG; KiCad 9's own form of a
 *     PWR_FLAG instance is copied from it).
 *   - fixtures/compose/kicad/symbols/power.kicad_sym: the PWR_FLAG symbol copied verbatim from KiCad 10.0.5's
 *     power.kicad_sym (an extract, like the other fixture libraries).
 * kicad-cli ERC on the composed golden boards (manus/golden_boards.ps1) checks the result end to end. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "sch_rules.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FIX COPPER_TEST_SOURCE_DIR "/tests/fixtures/"

static Arena arena;

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    assert(f);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc((size_t)n + 1);
    assert(s && fread(s, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    s[n] = 0;
    if (len) *len = (size_t)n;
    return s;
}

static size_t occurrences(const char *s, const char *what)
{
    size_t n = 0;
    for (const char *p = strstr(s, what); p; p = strstr(p + 1, what)) n++;
    return n;
}

static const char SCH[] =
    "(kicad_sch\n"
    "  (version 20250114)\n"
    "  (generator \"eeschema\")\n"
    "  (generator_version \"9.0\")\n"
    "  (uuid \"5855e28d-8b80-5d6c-bb8d-f1d75f6bfcf1\")\n"
    "  (paper \"A4\")\n"
    "  (lib_symbols\n"
    "    (symbol \"Device:R\"\n"
    "      (pin_numbers\n"
    "        (hide yes))))\n"
    "  (symbol\n"
    "    (lib_id \"Device:R\")\n"
    "    (at 45.72 50.8 0)\n"
    "    (unit 1)\n"
    "    (uuid \"e29b6da1-9fff-5d51-a743-f39a9ebe55a5\")\n"
    "    (property \"Reference\" \"R1\"\n"
    "      (at 50.8 46.99 0))\n"
    "    (instances\n"
    "      (project \"Demo\"\n"
    "        (path \"/5855e28d-8b80-5d6c-bb8d-f1d75f6bfcf1\"\n"
    "          (reference \"R1\")\n"
    "          (unit 1)))))\n"
    "  (label \"+5V\"\n"
    "    (at 40.64 50.8 0)\n"
    "    (uuid \"d41ae8b1-cc7e-55d5-9536-f82403fc9f51\"))\n"
    "  (label \"GND\"\n"
    "    (at 40.64 181.61 0)\n"
    "    (uuid \"b537921c-1926-5d62-b73d-b3c61b3ecf75\"))\n"
    "  (sheet_instances\n"
    "    (path \"/\"\n"
    "      (page \"1\")))\n"
    "  (embedded_fonts no))\n";

static const char HINTS[] =
    "{\"format\": \"copper.layout-hints/1\", \"keepouts\": [], \"decoupling\": [], \"power_flags\": ["
    "{\"net\": \"+5V\", \"port\": \"power.vout\", \"why\": \"w\"}, {\"net\": \"GND\", \"port\": \"power.gnd\", \"why\": \"w\"}]}";

/* every uuid in the text is different */
static void assert_unique_uuids(const char *s)
{
    size_t n = occurrences(s, "(uuid \"");
    const char **seen = malloc(n * sizeof *seen);
    size_t k = 0;
    for (const char *p = strstr(s, "(uuid \""); p; p = strstr(p + 1, "(uuid \"")) {
        for (size_t i = 0; i < k; i++) assert(strncmp(seen[i], p + 7, 36));
        seen[k++] = p + 7;
    }
    free(seen);
}

static void test_power_flags(const char *power, size_t power_len)
{
    const char *out = NULL;
    size_t out_len = 0;
    SchRulesReport r;
    SchRulesStatus st = sch_power_flags(&arena, SCH, strlen(SCH), HINTS, strlen(HINTS), power, power_len, &out, &out_len, &r);
    if (st != SCH_RULES_OK) fprintf(stderr, "power flags: %s\n", r.message);
    assert(st == SCH_RULES_OK && r.flags_added == 2 && r.flags_present == 0);
    /* the library symbol, verbatim from KiCad's power library, renamed to its library id, inside lib_symbols */
    assert(occurrences(out, "(symbol \"power:PWR_FLAG\"") == 1);
    assert(strstr(out, "(property \"Description\" \"Special symbol for telling ERC where power comes from\""));
    assert(strstr(out, "(symbol \"PWR_FLAG_0_0\""));
    assert(strstr(out, "(symbol \"power:PWR_FLAG\"") < strstr(out, "(lib_id \"Device:R\")"));
    /* two instances in KiCad 9's form, below everything else (max y 181.61 + 12.7, on the 1.27 mm grid), each with
       a label of its net at its pin */
    assert(occurrences(out, "(lib_id \"power:PWR_FLAG\")") == 2);
    assert(strstr(out, "(at 25.4 194.31 0)") && strstr(out, "(at 40.64 194.31 0)"));
    assert(strstr(out, "(reference \"#FLG01\")") && strstr(out, "(reference \"#FLG02\")"));
    assert(strstr(out, "(property \"Reference\" \"#FLG01\"") && strstr(out, "(property \"Value\" \"PWR_FLAG\""));
    assert(occurrences(out, "(project \"Demo\"") == 3 && occurrences(out, "(path \"/5855e28d-8b80-5d6c-bb8d-f1d75f6bfcf1\"") == 3);
    assert(occurrences(out, "(label \"+5V\"") == 2 && occurrences(out, "(label \"GND\"") == 2);
    {
        const char *f1 = strstr(out, "(lib_id \"power:PWR_FLAG\")");
        const char *lab = strstr(f1, "(label \"+5V\"");
        assert(lab && !strncmp(strstr(lab, "(at "), "(at 25.4 194.31 0)", 18));   /* the +5V label at #FLG01's pin */
    }
    assert(strstr(out, "(lib_id \"power:PWR_FLAG\")") < strstr(out, "(sheet_instances"));
    assert(strstr(out, "(embedded_fonts no))\n") && out[out_len - 1] == '\n');
    assert_unique_uuids(out);
    /* deterministic, and idempotent: a schematic that has PWR_FLAGs is left as it is */
    const char *again = NULL;
    size_t again_len = 0;
    assert(sch_power_flags(&arena, SCH, strlen(SCH), HINTS, strlen(HINTS), power, power_len, &again, &again_len, &r) == SCH_RULES_OK);
    assert(again_len == out_len && !memcmp(again, out, out_len));
    assert(sch_power_flags(&arena, out, out_len, HINTS, strlen(HINTS), power, power_len, &again, &again_len, &r) == SCH_RULES_OK);
    assert(r.flags_added == 0 && r.flags_present == 2 && again_len == out_len && !memcmp(again, out, out_len));
    /* the real KiCad 9 project already has one */
    size_t kl = 0;
    char *k9 = slurp(FIX "sample_resistor_lol/sample_resistor_lol.kicad_sch", &kl);
    static const char h9[] = "{\"power_flags\": [{\"net\": \"test net\", \"port\": \"x.y\", \"why\": \"w\"}]}";
    assert(sch_power_flags(&arena, k9, kl, h9, strlen(h9), power, power_len, &again, &again_len, &r) == SCH_RULES_OK);
    assert(r.flags_added == 0 && r.flags_present == 1 && again_len == kl && !memcmp(again, k9, kl));
    free(k9);
    /* no power_flags: nothing to do */
    static const char none[] = "{\"format\": \"copper.layout-hints/1\", \"keepouts\": []}";
    assert(sch_power_flags(&arena, SCH, strlen(SCH), none, strlen(none), power, power_len, &again, &again_len, &r) == SCH_RULES_OK);
    assert(r.flags_added == 0 && again_len == strlen(SCH) && !memcmp(again, SCH, again_len));
    /* refused: a net the schematic does not have; a power library without PWR_FLAG; hints that do not parse */
    static const char ghost[] = "{\"power_flags\": [{\"net\": \"+12V\", \"port\": \"x.y\", \"why\": \"w\"}]}";
    assert(sch_power_flags(&arena, SCH, strlen(SCH), ghost, strlen(ghost), power, power_len, &again, &again_len, &r) == SCH_RULES_INVALID_INPUT);
    assert(strstr(r.message, "+12V"));
    static const char nolib[] = "(kicad_symbol_lib (version 20251024) (symbol \"GND\" (power global)))";
    assert(sch_power_flags(&arena, SCH, strlen(SCH), HINTS, strlen(HINTS), nolib, strlen(nolib), &again, &again_len, &r) == SCH_RULES_INVALID_INPUT);
    assert(sch_power_flags(&arena, SCH, strlen(SCH), "{", 1, power, power_len, &again, &again_len, &r) == SCH_RULES_INVALID_INPUT);
}

static void test_lib_tables(void)
{
    static const char pcb[] = "(kicad_pcb (version 20241229)\n  (footprint \"Resistor_SMD:R_0603_1608Metric\" (layer \"F.Cu\"))\n"
                              "  (footprint \"Connector_PinHeader_2.54mm:PinHeader_1x02_P2.54mm_Vertical\" (layer \"F.Cu\"))\n"
                              "  (footprint \"Resistor_SMD:R_0603_1608Metric\" (layer \"F.Cu\")))\n";
    const char *sym = NULL, *fp = NULL;
    size_t sl = 0, fl = 0;
    SchRulesReport r;
    assert(sch_lib_tables(&arena, SCH, strlen(SCH), pcb, strlen(pcb), &sym, &sl, &fp, &fl, &r) == SCH_RULES_OK);
    assert(r.sym_libs == 1 && r.fp_libs == 2);
    static const char want_sym[] =
        "(sym_lib_table\n  (version 7)\n"
        "  (lib (name \"Device\")(type \"KiCad\")(uri \"${KICAD9_SYMBOL_DIR}/Device.kicad_sym\")(options \"\")(descr \"" SCH_LIB_TABLE_DESCR "\"))\n)\n";
    static const char want_fp[] =
        "(fp_lib_table\n  (version 7)\n"
        "  (lib (name \"Connector_PinHeader_2.54mm\")(type \"KiCad\")(uri \"${KICAD9_FOOTPRINT_DIR}/Connector_PinHeader_2.54mm.pretty\")(options \"\")(descr \"" SCH_LIB_TABLE_DESCR "\"))\n"
        "  (lib (name \"Resistor_SMD\")(type \"KiCad\")(uri \"${KICAD9_FOOTPRINT_DIR}/Resistor_SMD.pretty\")(options \"\")(descr \"" SCH_LIB_TABLE_DESCR "\"))\n)\n";
    if (strcmp(sym, want_sym)) fprintf(stderr, "sym table:\n%s", sym);
    if (strcmp(fp, want_fp)) fprintf(stderr, "fp table:\n%s", fp);
    assert(sl == strlen(want_sym) && !strcmp(sym, want_sym));
    assert(fl == strlen(want_fp) && !strcmp(fp, want_fp));
    assert(sch_lib_table_is_copper(sym, sl) && sch_lib_table_is_copper(fp, fl));
    static const char theirs[] = "(sym_lib_table (version 7) (lib (name \"Mine\")(type \"KiCad\")(uri \"/x/Mine.kicad_sym\")(options \"\")(descr \"\")))";
    assert(!sch_lib_table_is_copper(theirs, strlen(theirs)));
    static const char mixed[] = "(sym_lib_table (version 7) (lib (name \"A\")(type \"KiCad\")(uri \"u\")(options \"\")(descr \"" SCH_LIB_TABLE_DESCR "\"))"
                                " (lib (name \"Mine\")(type \"KiCad\")(uri \"/x\")(options \"\")(descr \"\")))";
    assert(!sch_lib_table_is_copper(mixed, strlen(mixed)));
    /* the real KiCad 9 project: Device, Transistor_BJT and power, sorted */
    size_t kl = 0, pl = 0;
    char *k9 = slurp(FIX "sample_resistor_lol/sample_resistor_lol.kicad_sch", &kl);
    char *k9p = slurp(FIX "sample_resistor_lol/sample_resistor_lol.kicad_pcb", &pl);
    assert(sch_lib_tables(&arena, k9, kl, k9p, pl, &sym, &sl, &fp, &fl, &r) == SCH_RULES_OK);
    assert(r.sym_libs == 3);
    assert(strstr(sym, "(name \"Device\")") < strstr(sym, "(name \"Transistor_BJT\")") && strstr(sym, "(name \"Transistor_BJT\")") < strstr(sym, "(name \"power\")"));
    free(k9); free(k9p);
    /* a library name Copper will not quote: refused */
    static const char bad[] = "(kicad_sch (lib_symbols) (symbol (lib_id \"My Lib:R\")))";
    assert(sch_lib_tables(&arena, bad, strlen(bad), pcb, strlen(pcb), &sym, &sl, &fp, &fl, &r) == SCH_RULES_INVALID_INPUT);
    assert(strstr(r.message, "My Lib"));
}

int main(void)
{
    arena_init(&arena, 1u << 22);
    size_t pl = 0;
    char *power = slurp(FIX "compose/kicad/symbols/power.kicad_sym", &pl);
    test_power_flags(power, pl);
    test_lib_tables();
    free(power);
    arena_destroy(&arena);
    puts("sch_rules: all tests passed");
    return 0;
}

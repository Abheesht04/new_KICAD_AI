/* blocks (src/v2_src/blocks.c): loading and checking "copper.block/1" definitions.
 *
 * The block fixture (tests/fixtures/blocks/led_indicator.block.json) is Copper-written test data. The part catalogues
 * are the repository's real parts_library files (Step 1b; unverified datasheet transcriptions). Each negative case
 * changes one thing in the valid fixture and expects exactly the finding for that change. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "blocks.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FIXTURE COPPER_TEST_SOURCE_DIR "/tests/fixtures/blocks/led_indicator.block.json"
#define LIB COPPER_TEST_SOURCE_DIR "/../../parts_library/"

static Arena arena;
static char *fixture;
static const PartCatalogue *cats[2];

static char *slurp(const char *path)
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
    return s;
}

static void dump(const BlockReport *r)
{
    for (size_t i = 0; i < r->finding_count; i++)
        fprintf(stderr, "  %s %s: %s\n", r->findings[i].code, r->findings[i].path, r->findings[i].message);
}

static BlockReport check(const char *text, bool with_catalogues)
{
    BlockReport r;
    BlockStatus st = block_check_text(&arena, text, strlen(text), with_catalogues ? cats : NULL, with_catalogues ? 2 : 0, &r);
    assert(st == r.verdict);
    return r;
}

/* The fixture with the first occurrence of `from` replaced by `to` (which must occur exactly once). */
static char *mutate(const char *from, const char *to)
{
    const char *at = strstr(fixture, from);
    if (!at || strstr(at + 1, from)) { fprintf(stderr, "mutation anchor not unique: %s\n", from); assert(!"anchor"); }
    size_t a = (size_t)(at - fixture), lf = strlen(from), lt = strlen(to), n = strlen(fixture);
    char *s = malloc(n - lf + lt + 1);
    assert(s);
    memcpy(s, fixture, a);
    memcpy(s + a, to, lt);
    memcpy(s + a + lt, at + lf, n - a - lf + 1);
    return s;
}

static size_t count(const BlockReport *r, const char *code)
{
    size_t n = 0;
    for (size_t i = 0; i < r->finding_count; i++) if (!strcmp(r->findings[i].code, code)) n++;
    return n;
}

/* One change, one expected finding code (and no VALID verdict). */
static void expect(const char *from, const char *to, const char *code)
{
    char *text = mutate(from, to);
    BlockReport r = check(text, true);
    if (r.verdict != BLOCK_INVALID || count(&r, code) == 0) {
        fprintf(stderr, "change '%s' -> '%s': expected %s, got %s\n", from, to, code, block_status_name(r.verdict));
        dump(&r);
        assert(!"unexpected result");
    }
    free(text);
}

static void test_valid(void)
{
    BlockReport r = check(fixture, true);
    if (r.verdict != BLOCK_VALID) dump(&r);
    assert(r.verdict == BLOCK_VALID && r.finding_count == 0 && r.catalogue_checked);
    assert(!strcmp(r.name, "led_indicator") && r.parameters == 3 && r.ports == 2 && r.nets == 1 && r.parts == 2 && r.formulas == 1);
    BlockReport u = check(fixture, false);
    assert(u.verdict == BLOCK_UNVERIFIED && !u.catalogue_checked && u.finding_count == 0);
}

static void test_structure(void)
{
    expect("\"copper.block/1\"", "\"copper.block/2\"", "FORMAT");
    expect("\"summary\":", "\"colour\": \"red\", \"summary\":", "UNKNOWN_KEY");
    expect("\"name\": \"led_indicator\"", "\"name\": \"LED indicator\"", "BAD_NAME");
    expect("\"nets\": [\"led_a\"],", "", "UNKNOWN_NET");              /* led_a no longer declared */
    expect("{\"name\": \"gnd\", \"kind\": \"ground\"}", "{\"name\": \"drive\", \"kind\": \"ground\"}", "DUPLICATE");
    expect("\"page\": 2}", "\"page\": 2, \"page\": 3}", "JSON");      /* the parser rejects a duplicate key */
}

static void test_sources_and_parameters(void)
{
    expect(", \"standard\": \"circuit theory\"", "", "MISSING_KEY");                       /* no url and no standard */
    expect("\"default\": 2000000, \"source\": \"kingbright_apt1608ec\"", "\"default\": 2000000", "NO_SOURCE");
    expect("\"source\": \"copper_default\"", "\"source\": \"nobody\"", "UNKNOWN_SOURCE");
    expect("\"required\": false, \"default\": 2000000, \"source\": \"kingbright_apt1608ec\"", "\"required\": false",
           "MISSING_KEY");                                                                  /* optional without default */
    expect("\"unit\": \"uA\"", "\"unit\": \"mA\"", "BAD_UNIT");
    expect("\"default\": 2000, ", "\"default\": 50000, ", "BAD_RANGE");                    /* above its max 30000 */
    expect("\"min\": 1000000, \"max\": 24000000", "\"min\": 24000000, \"max\": 1000000", "BAD_RANGE");
    expect("\"default\": 2000, ", "\"default\": 2.5, ", "BAD_NUMBER");                     /* integers only */
}

static void test_ports_nets_pins(void)
{
    expect("\"kind\": \"gpio\"", "\"kind\": \"power\"", "BAD_KIND");
    expect("\"kind\": \"gpio\"", "\"kind\": \"power_in\"", "MISSING_KEY");                /* power_in needs its range */
    expect("{\"name\": \"gnd\", \"kind\": \"ground\"}", "{\"name\": \"gnd\", \"kind\": \"ground\"}, {\"name\": \"spare\", \"kind\": \"gpio\"}",
           "UNUSED_PORT");
    expect("\"pins\": {\"anode\": \"led_a\"", "\"pins\": {\"anode\": \"led_b\"", "UNKNOWN_NET");
    expect("\"nets\": [\"led_a\"]", "\"nets\": [\"led_a\", \"spare_net\"]", "SINGLE_ENDED_NET");
    expect("\"prefix\": \"D\"", "\"prefix\": \"d1\"", "BAD_NAME");
}

static void test_formulas_and_values(void)
{
    expect("\"function\": \"led_series_resistor\"", "\"function\": \"magic\"", "UNKNOWN_FUNCTION");
    expect("\"v_drive\": \"drive_voltage\"", "\"v_supply\": \"drive_voltage\"", "BAD_INPUT");
    expect("\"current\": \"led_current\"", "\"current\": \"drive_voltage\"", "UNIT_MISMATCH");
    expect("\"select\": {\"kind\": \"R\", \"package\": \"0603\"", "\"select\": {\"kind\": \"C\", \"package\": \"0603\"",
           "UNIT_MISMATCH");                                                    /* a capacitor valued in uohm */
    expect("\"value\": {\"formula\": \"r_led\"}", "\"value\": {\"formula\": \"r_other\"}", "UNKNOWN_FORMULA");
}

/* Step 2c additions: parameters derived from a port, port currents, signal levels, I2C pull-up flags. */
static void test_composition_keys(void)
{
    expect("\"from\": {\"port\": \"drive\"", "\"from\": {\"port\": \"nowhere\"", "UNKNOWN_PORT");
    expect("\"quantity\": \"voltage_max\"", "\"quantity\": \"brightness\"", "BAD_KIND");
    expect("\"required\": true, \"min\": 1000000", "\"required\": false, \"min\": 1000000", "CONFLICT");   /* derived => required */
    expect("\"quantity\": \"voltage_max\"", "\"quantity\": \"current\"", "UNIT_MISMATCH");  /* uV parameter, current quantity */
    expect("{\"name\": \"drive\", \"kind\": \"gpio\"}", "{\"name\": \"drive\", \"kind\": \"gpio\", \"current\": 5}", "CONFLICT");
    expect("{\"name\": \"drive\", \"kind\": \"gpio\"}", "{\"name\": \"drive\", \"kind\": \"gpio\", \"level_port\": \"gnd\"}", "BAD_KIND");
    expect("{\"name\": \"drive\", \"kind\": \"gpio\"}", "{\"name\": \"drive\", \"kind\": \"gpio\", \"level_port\": \"vcc\"}", "UNKNOWN_PORT");
    expect("{\"name\": \"drive\", \"kind\": \"gpio\"}", "{\"name\": \"drive\", \"kind\": \"gpio\", \"pullup\": true}", "CONFLICT");
    expect("{\"name\": \"drive\", \"kind\": \"gpio\"}", "{\"name\": \"drive\", \"kind\": \"gpio\", \"current_follows\": \"gnd\"}", "CONFLICT");
    expect("{\"name\": \"gnd\", \"kind\": \"ground\"}",
           "{\"name\": \"gnd\", \"kind\": \"ground\"}, {\"name\": \"vcc\", \"kind\": \"power_in\", \"voltage_min\": 1, \"voltage_max\": 2, \"source\": \"ohms_law\", \"current_follows\": \"gnd\"}",
           "BAD_KIND");
    expect("{\"name\": \"gnd\", \"kind\": \"ground\"}",
           "{\"name\": \"gnd\", \"kind\": \"ground\"}, {\"name\": \"vcc\", \"kind\": \"power_in\", \"voltage_min\": 1, \"voltage_max\": 2, \"source\": \"ohms_law\", \"current\": 10}",
           "NO_SOURCE");   /* a current without its own source; also UNUSED_PORT */
}

/* Step 2e: a part may declare keep-outs in its own footprint frame (um), each with a source. */
static void test_part_keepouts(void)
{
    static const char *const ok = "\"role\": \"led\", \"prefix\": \"D\", \"keepouts\": [{\"x1_um\": -1000, \"y1_um\": -2000, \"x2_um\": 1000, \"y2_um\": -500, \"source\": \"kingbright_apt1608ec\"}],";
    char *text = mutate("\"role\": \"led\", \"prefix\": \"D\",", ok);
    BlockReport r = check(text, true);
    if (r.verdict != BLOCK_VALID) dump(&r);
    assert(r.verdict == BLOCK_VALID);
    free(text);
    expect("\"role\": \"led\", \"prefix\": \"D\",", "\"role\": \"led\", \"prefix\": \"D\", \"keepouts\": [{\"x1_um\": 1000, \"y1_um\": -2000, \"x2_um\": -1000, \"y2_um\": -500, \"source\": \"kingbright_apt1608ec\"}],", "BAD_RANGE");
    expect("\"role\": \"led\", \"prefix\": \"D\",", "\"role\": \"led\", \"prefix\": \"D\", \"keepouts\": [{\"x1_um\": -1000, \"y1_um\": -2000, \"x2_um\": 1000, \"y2_um\": -500}],", "MISSING_KEY");
    expect("\"role\": \"led\", \"prefix\": \"D\",", "\"role\": \"led\", \"prefix\": \"D\", \"keepouts\": [{\"x1_um\": -1, \"y1_um\": -2, \"x2_um\": 1, \"y2_um\": 2, \"source\": \"nobody\"}],", "UNKNOWN_SOURCE");
}

/* Step 3d: a power_out or ground port may say it is an external supply (KiCad needs a PWR_FLAG on its net). */
static void test_external_supply(void)
{
    char *text = mutate("{\"name\": \"gnd\", \"kind\": \"ground\"}", "{\"name\": \"gnd\", \"kind\": \"ground\", \"external_supply\": true}");
    BlockReport r = check(text, true);
    if (r.verdict != BLOCK_VALID) dump(&r);
    assert(r.verdict == BLOCK_VALID);
    free(text);
    expect("{\"name\": \"gnd\", \"kind\": \"ground\"}", "{\"name\": \"gnd\", \"kind\": \"ground\", \"external_supply\": 1}", "TYPE");
    expect("{\"name\": \"drive\", \"kind\": \"gpio\"}", "{\"name\": \"drive\", \"kind\": \"gpio\", \"external_supply\": true}", "CONFLICT");
}

static void test_catalogue(void)
{
    expect("\"part_id\": \"led_0603_red_kingbright_apt1608ec\"", "\"part_id\": \"led_that_does_not_exist\"", "UNKNOWN_PART");
    expect("\"pins\": {\"anode\": \"led_a\", \"cathode\": \"gnd\"}", "\"pins\": {\"anode\": \"led_a\", \"cathod\": \"gnd\"}",
           "TERMINAL_MISMATCH");
    expect("\"role\": \"led\", \"prefix\": \"D\"", "\"role\": \"led\", \"prefix\": \"Q\"", "KIND_MISMATCH");
    expect("\"package\": \"0603\"", "\"package\": \"9999\"", "NO_CATALOGUE_MATCH");
    /* Without catalogues the same unknown part is not caught: the verdict says so (UNVERIFIED, not VALID). */
    char *text = mutate("\"part_id\": \"led_0603_red_kingbright_apt1608ec\"", "\"part_id\": \"led_that_does_not_exist\"");
    BlockReport r = check(text, false);
    assert(r.verdict == BLOCK_UNVERIFIED);
    free(text);
}

static void test_json_report_and_functions(void)
{
    BlockReport r = check(fixture, true);
    size_t n1 = 0, n2 = 0;
    const char *j1 = block_report_json(&arena, &r, &n1), *j2 = block_report_json(&arena, &r, &n2);
    assert(j1 && j2 && n1 == n2 && !memcmp(j1, j2, n1));
    assert(strstr(j1, "\"format\": \"copper.block-check/1\"") && strstr(j1, "\"verdict\": \"VALID\""));
    size_t nf = 0;
    const BlockFunction *f = block_functions(&nf);
    bool led = false;
    for (size_t i = 0; i < nf; i++) if (!strcmp(f[i].name, "led_series_resistor") && !strcmp(f[i].output_unit, "uohm")) led = true;
    assert(led);
    BlockReport bad;
    assert(block_check_file(&arena, COPPER_TEST_SOURCE_DIR "/tests/fixtures/blocks/missing.block.json", NULL, 0, &bad) == BLOCK_INVALID &&
           count(&bad, "IO") == 1);
}

int main(void)
{
    arena_init(&arena, 1u << 22);
    fixture = slurp(FIXTURE);
    static PartCatalogue semis, resistors;
    PartCatalogueError e;
    assert(part_catalogue_parse_file(&arena, LIB "semiconductors.parts", &semis, &e) == PART_CATALOGUE_OK);
    assert(part_catalogue_parse_file(&arena, LIB "resistors.parts", &resistors, &e) == PART_CATALOGUE_OK);
    cats[0] = &semis;
    cats[1] = &resistors;
    test_valid();
    test_structure();
    test_sources_and_parameters();
    test_ports_nets_pins();
    test_formulas_and_values();
    test_composition_keys();
    test_part_keepouts();
    test_external_supply();
    test_catalogue();
    test_json_report_and_functions();
    free(fixture);
    arena_destroy(&arena);
    puts("blocks: all tests passed");
    return 0;
}

/* compose (src/v2_src/compose.c): joining checked blocks into a design intent.
 *
 * Inputs are the repository's real blocks_library and parts_library (Copper data, unverified datasheet
 * transcriptions) and KiCad symbols/footprints copied unchanged from the KiCad 10.0.5 libraries
 * (tests/fixtures/compose/kicad, extracted 2026-11-21). The compose file is Copper-written. The composed intent is
 * checked again by the grounding gate (intent_check.c), which is independent code (PIPELINE_HARD_RULES.md H6). */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "compose.h"
#include "intent_check.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROOT COPPER_TEST_SOURCE_DIR "/../../"
#define FIX COPPER_TEST_SOURCE_DIR "/tests/fixtures/compose/"
#define SYMS FIX "kicad/symbols"
#define FPS FIX "kicad/footprints"

static Arena arena;
static char *fixture;
static PartCatalogue cats[5];
static const PartCatalogue *refs[5];
static ComposeInputs inputs;

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

static char *mutate(const char *base, const char *from, const char *to)
{
    const char *at = strstr(base, from);
    if (!at || strstr(at + 1, from)) { fprintf(stderr, "mutation anchor not unique: %s\n", from); assert(!"anchor"); }
    size_t a = (size_t)(at - base), lf = strlen(from), lt = strlen(to), n = strlen(base);
    char *s = malloc(n - lf + lt + 1);
    assert(s);
    memcpy(s, base, a);
    memcpy(s + a, to, lt);
    memcpy(s + a + lt, at + lf, n - a - lf + 1);
    return s;
}

static void dump(const ComposeReport *r)
{
    for (size_t i = 0; i < r->finding_count; i++)
        fprintf(stderr, "  %s %s %s: %s\n", compose_level_name(r->findings[i].level), r->findings[i].code, r->findings[i].where, r->findings[i].message);
}

static ComposeReport run(const char *text)
{
    ComposeReport r;
    ComposeStatus st = compose_design(&arena, text, strlen(text), &inputs, &r);
    assert(st == r.status);
    return r;
}

static size_t count(const ComposeReport *r, const char *code)
{
    size_t n = 0;
    for (size_t i = 0; i < r->finding_count; i++) if (!strcmp(r->findings[i].code, code)) n++;
    return n;
}

static const char *val(const ComposeReport *r, const char *where)
{
    for (size_t i = 0; i < r->value_count; i++) if (!strcmp(r->values[i].where, where)) return r->values[i].value;
    return "";
}

/* One change to the fixture; expect this status and this finding code. */
static void expect(const char *from, const char *to, ComposeStatus status, const char *code)
{
    char *text = mutate(fixture, from, to);
    ComposeReport r = run(text);
    if (r.status != status || (code && !count(&r, code))) {
        fprintf(stderr, "change '%s' -> '%s': expected %s/%s, got %s\n", from, to, compose_status_name(status), code ? code : "-",
                compose_status_name(r.status));
        dump(&r);
        assert(!"unexpected result");
    }
    assert(!r.intent_json);   /* nothing is written unless the design composed */
    free(text);
}

static size_t occurrences(const char *s, const char *what)
{
    size_t n = 0;
    for (const char *p = strstr(s, what); p; p = strstr(p + 1, what)) n++;
    return n;
}

static void test_composes_and_is_grounded(void)
{
    ComposeReport r = run(fixture);
    if (r.status != COMPOSE_COMPOSED) dump(&r);
    assert(r.status == COMPOSE_COMPOSED && r.intent_json && r.blocks == 6 && r.nets == 8);
    /* parts: power 2, ldo 2, mcu 7, buttons 4, led 2, temp 4 */
    assert(r.components == 21);
    /* computed and chosen values */
    assert(strstr(val(&r, "led1.drive_voltage"), "3300000 uV"));           /* derived: the GPIO's level, the 3.3 V rail */
    assert(strstr(val(&r, "led1.r_led"), "at least 650000000 uohm"));       /* (3.3 - 2.0) V / 2 mA */
    assert(strstr(val(&r, "led1.series_resistor"), "CRCW0603680RFKEA"));   /* smallest library value >= 650 ohm */
    assert(strstr(val(&r, "temp.r_pullup"), "966666667..3540658562 uohm"));   /* 2.9 V / 3 mA (up) .. 300 ns / (0.8473 x 100 pF) (down) */
    assert(strstr(val(&r, "temp.scl_pullup"), "CRCW06031K00FKEA"));        /* largest library value in that range */
    assert(strstr(val(&r, "ldo.load_current"), "500370 uA"));              /* ESP32 0.5 A + BOOT pull-up 360 uA + TMP102 10 uA */
    assert(!strcmp(val(&r, "ldo.dissipation"), "850629 uW"));              /* (5.0 - 3.3) V x 500370 uA */
    assert(count(&r, "NO_THERMAL_CHECK") == 1 && count(&r, "SOURCE_CAPACITY_UNKNOWN") == 1 && count(&r, "BASE_SYMBOL") == 1);
    /* the independent gate accepts the intent against the KiCad libraries */
    IntentCheckReport g;
    IntentCheckStatus gs = intent_check(&arena, r.intent_json, r.intent_len, SYMS, FPS, &g);
    if (gs != INTENT_CHECK_GROUNDED)
        for (size_t i = 0; i < g.count; i++) fprintf(stderr, "  gate %s %s %s: %s\n", g.findings[i].code, g.findings[i].reference ? g.findings[i].reference : "-", g.findings[i].item ? g.findings[i].item : "-", g.findings[i].message);
    assert(gs == INTENT_CHECK_GROUNDED && g.components == 21);
    /* layout hints: the module's antenna keep-out, and the decoupling capacitors next to their ICs */
    assert(r.hints_json && strstr(r.hints_json, "\"format\": \"copper.layout-hints/1\""));
    assert(strstr(r.hints_json, "\"reference\": \"U2\"") && strstr(r.hints_json, "-24000,") && strstr(r.hints_json, "-6750"));
    assert(strstr(r.hints_json, "\"capacitor\": \"C3\"") && strstr(r.hints_json, "\"ic\": \"U2\""));
    /* Step 3d: the header's vout and gnd are external supplies: one power flag on each of their nets, nowhere else */
    {
        const char *pf = strstr(r.hints_json, "\"power_flags\"");
        assert(pf && strstr(pf, "\"net\": \"+5V\"") && strstr(pf, "\"net\": \"GND\"") && strstr(pf, "\"port\": \"power.vout\""));
        assert(occurrences(pf, "\"net\":") == 2 && !strstr(pf, "\"net\": \"+3V3\""));   /* +3V3 is driven by the LDO */
    }
    /* Step 3d (G-077): mcu.usb_dn and mcu.usb_dp are unused, so the 22 ohm series resistors each have a floating pin */
    assert(count(&r, "UNCONNECTED_PORT_PARTS") == 2);
    /* deterministic */
    ComposeReport r2 = run(fixture);
    assert(r2.intent_len == r.intent_len && !memcmp(r2.intent_json, r.intent_json, r.intent_len));
    size_t n1, n2;
    const char *j1 = compose_report_json(&arena, &r, &n1), *j2 = compose_report_json(&arena, &r2, &n2);
    assert(j1 && j2 && n1 == n2 && !memcmp(j1, j2, n1) && strstr(j1, "\"status\": \"COMPOSED\""));
}

static void test_usb_power_is_refused(void)
{
    /* G-072: USB-C (4.0..5.5 V, 500 mA) cannot feed the AMS1117 block (needs >= 4.6 V) and the 0.51 A load */
    char *text = mutate(fixture, "{\"id\": \"power\", \"block\": \"power_input_header\", \"parameters\": {\"supply_voltage\": 5000000}}",
                        "{\"id\": \"power\", \"block\": \"usb_c_sink\"}");
    text = mutate(text, "\"ports\": [\"power.vout\", \"ldo.vin\"]", "\"ports\": [\"power.vbus\", \"ldo.vin\"]");
    ComposeReport r = run(text);
    if (r.status != COMPOSE_REFUSED) dump(&r);
    assert(r.status == COMPOSE_REFUSED && count(&r, "VOLTAGE_RANGE") == 1 && count(&r, "CURRENT_BUDGET") == 1 && !r.intent_json);
    free(text);
}

static void test_questions_and_refusals(void)
{
    expect(", \"parameters\": {\"supply_voltage\": 5000000}", "", COMPOSE_NEEDS_INPUT, "NEEDS_INPUT");
    expect(", \"parameters\": {\"bus_capacitance\": 100}", "", COMPOSE_NEEDS_INPUT, "NEEDS_INPUT");      /* never assumed */
    expect("\"bus_capacitance\": 100", "\"bus_capacitance\": 400", COMPOSE_REFUSED, "NO_VALID_PULLUP"); /* Rmin > Rmax */
    expect("{\"name\": \"LED1\", \"ports\": [\"mcu.io4\", \"led1.drive\"]},", "", COMPOSE_REFUSED, "UNDERIVABLE");
    expect("\"ports\": [\"mcu.io4\", \"led1.drive\"]", "\"ports\": [\"mcu.io4\", \"mcu.io5\", \"led1.drive\"]", COMPOSE_REFUSED, "SAME_BLOCK_SHORT");
    expect("\"ports\": [\"ldo.vout\", \"mcu.v3v3\"", "\"ports\": [\"ldo.vout\", \"power.vout\", \"mcu.v3v3\"", COMPOSE_INVALID_INPUT,
           "DUPLICATE_CONNECTION");
    expect("\"ports\": [\"mcu.en\", \"buttons.en\"]", "\"ports\": [\"mcu.en\", \"buttons.en\", \"led1.gnd\"]", COMPOSE_INVALID_INPUT,
           "DUPLICATE_CONNECTION");
    expect("\"ports\": [\"power.gnd\", \"ldo.gnd\"", "\"ports\": [\"ldo.gnd\"", COMPOSE_REFUSED, "UNCONNECTED_POWER");
    expect("\"ports\": [\"mcu.io8\", \"temp.scl\"]", "\"ports\": [\"mcu.io8\", \"temp.scl\", \"temp.sda\"]", COMPOSE_INVALID_INPUT, NULL);
    expect("{\"name\": \"I2C_SDA\", \"ports\": [\"mcu.io9\", \"temp.sda\"]}", "{\"name\": \"I2C_SDA\", \"ports\": [\"mcu.io9\", \"temp.sda\"]}, {\"ports\": [\"temp.scl\"]}",
           COMPOSE_REFUSED, "SINGLE_PORT_NET");
    expect("\"block\": \"ldo_3v3\"", "\"block\": \"ldo_9v9\"", COMPOSE_INVALID_INPUT, "UNKNOWN_BLOCK");
    expect("\"supply_voltage\": 5000000", "\"supply_voltage\": 5000000, \"colour\": 1", COMPOSE_INVALID_INPUT, "UNKNOWN_PARAMETER");
    expect("\"supply_voltage\": 5000000", "\"supply_voltage\": 30000000", COMPOSE_REFUSED, "PARAMETER_RANGE");   /* > 25 V capacitor */
    expect("\"supply_voltage\": 5000000", "\"supply_voltage\": 4000000", COMPOSE_REFUSED, "VOLTAGE_RANGE");      /* LDO needs 4.6 V */
    expect("{\"id\": \"temp\"", "{\"id\": \"temp2\", \"block\": \"i2c_sensor_tmp102\", \"parameters\": {\"bus_capacitance\": 100}},\n    {\"id\": \"temp\"",
           COMPOSE_REFUSED, "UNCONNECTED_POWER");
    expect("\"name\": \"Esp32S3LedSensor\"", "\"name\": \"Esp32 S3\"", COMPOSE_INVALID_INPUT, "BAD_NAME");
    expect("\"copper.compose/1\"", "\"copper.compose/2\"", COMPOSE_INVALID_INPUT, "FORMAT");
}

/* Step 2d: with the prompt, every stated value needs a verbatim quote that contains it. */
static void test_evidence(void)
{
    static const char *prompt = "Make an ESP32-S3 board, 70 mm by 60 mm, powered from 5 V, with an LED and a TMP102 "
                                "temperature sensor; the I2C bus capacitance is about 100 pF.";
    static const char *ev = "\"evidence\": [{\"field\": \"power.supply_voltage\", \"quote\": \"powered from 5 V\"},"
                            " {\"field\": \"temp.bus_capacitance\", \"quote\": \"bus capacitance is about 100 pF\"},"
                            " {\"field\": \"project.board.width_mm\", \"quote\": \"70 mm by 60 mm\"},"
                            " {\"field\": \"project.board.height_mm\", \"quote\": \"70 mm by 60 mm\"}],\n  \"nets\":";
    char *with_ev = mutate(fixture, "\"nets\":", ev);
    inputs.prompt = prompt;
    ComposeReport r = run(with_ev);
    if (r.status != COMPOSE_COMPOSED) dump(&r);
    assert(r.status == COMPOSE_COMPOSED && count(&r, "IGNORED_VALUE") == 0 && r.intent_json);

    /* a value the quote does not contain (12 V, quote says 5 V) is ignored, then asked for */
    char *wrong = mutate(with_ev, "\"supply_voltage\": 5000000", "\"supply_voltage\": 12000000");
    r = run(wrong);
    assert(r.status == COMPOSE_NEEDS_INPUT && count(&r, "IGNORED_VALUE") == 1 && !r.intent_json);
    /* a quote that is not in the prompt */
    char *invented = mutate(with_ev, "\"quote\": \"bus capacitance is about 100 pF\"", "\"quote\": \"bus capacitance is 100 pF\"");
    r = run(invented);
    assert(r.status == COMPOSE_NEEDS_INPUT && count(&r, "IGNORED_VALUE") == 1);
    /* no quote at all, for the board */
    char *noboard = mutate(with_ev, " {\"field\": \"project.board.width_mm\", \"quote\": \"70 mm by 60 mm\"},", "");
    r = run(noboard);
    assert(r.status == COMPOSE_NEEDS_INPUT && count(&r, "IGNORED_VALUE") == 1 && count(&r, "NEEDS_INPUT") == 1);
    /* no board at all: a question, never a made-up size */
    char *missing = mutate(fixture, ", \"board\": {\"width_mm\": 70, \"height_mm\": 60}", "");
    inputs.prompt = NULL;
    r = run(missing);
    assert(r.status == COMPOSE_NEEDS_INPUT && count(&r, "NEEDS_INPUT") == 1);
    /* without a prompt, evidence is not checked (the library caller decides) */
    r = run(wrong);
    /* 12 V is inside the LDO block's 4.6..15 V, so it composes, with (12 - 3.3) V x 500370 uA = 4.35 W to dissipate:
     * reported as UNVERIFIED (Copper has no thermal rating), never as fine */
    assert(r.status == COMPOSE_COMPOSED && !strcmp(val(&r, "ldo.dissipation"), "4353219 uW") && count(&r, "NO_THERMAL_CHECK") == 1);
    free(with_ev); free(wrong); free(invented); free(noboard); free(missing);
}

static void test_two_sensors_on_one_bus(void)
{
    /* a second TMP102 on the same bus brings a second pull-up pair */
    char *text = mutate(fixture, "{\"id\": \"temp\"", "{\"id\": \"temp2\", \"block\": \"i2c_sensor_tmp102\", \"parameters\": {\"bus_capacitance\": 100}},\n    {\"id\": \"temp\"");
    text = mutate(text, "\"ldo.vout\", \"mcu.v3v3\", \"buttons.v3v3\", \"temp.v3v3\"", "\"ldo.vout\", \"mcu.v3v3\", \"buttons.v3v3\", \"temp.v3v3\", \"temp2.v3v3\"");
    text = mutate(text, "\"led1.gnd\", \"temp.gnd\"", "\"led1.gnd\", \"temp.gnd\", \"temp2.gnd\"");
    text = mutate(text, "\"ports\": [\"mcu.io8\", \"temp.scl\"]", "\"ports\": [\"mcu.io8\", \"temp.scl\", \"temp2.scl\"]");
    text = mutate(text, "\"ports\": [\"mcu.io9\", \"temp.sda\"]", "\"ports\": [\"mcu.io9\", \"temp.sda\", \"temp2.sda\"]");
    ComposeReport r = run(text);
    assert(r.status == COMPOSE_REFUSED && count(&r, "DUPLICATE_PULLUP") == 2);
    free(text);
}

int main(void)
{
    arena_init(&arena, 1u << 24);
    fixture = slurp(FIX "esp32s3_led_sensor.compose.json");
    static const char *const files[5] = {"resistors", "capacitors", "semiconductors", "ics", "electromechanical"};
    for (size_t i = 0; i < 5; i++) {
        char path[512];
        snprintf(path, sizeof path, ROOT "parts_library/%s.parts", files[i]);
        PartCatalogueError e;
        assert(part_catalogue_parse_file(&arena, path, &cats[i], &e) == PART_CATALOGUE_OK);
        refs[i] = &cats[i];
    }
    inputs.blocks_dir = ROOT "blocks_library";
    inputs.catalogues = refs;
    inputs.catalogue_count = 5;
    inputs.symbols_root = SYMS;
    test_composes_and_is_grounded();
    test_usb_power_is_refused();
    test_questions_and_refusals();
    test_evidence();
    test_two_sensors_on_one_bus();
    free(fixture);
    arena_destroy(&arena);
    puts("compose: all tests passed");
    return 0;
}

/* template_gen (src/v2_src/template_gen.c): deterministic design generation from analysed requirements.
 *
 * The language model's only job is the requirements analysis; these tests feed hand-written analyses (including
 * invented and mismatched ones) and check what the C generator makes of them. Every generated intent is then run
 * through the grounding gate (intent_check) against the same libraries.
 * Fixture: tests/fixtures/template_gen (Copper-generated KiCad-format parts, not a KiCad export) and the real
 * template file design_templates/copper_templates.v1.json. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "template_gen.h"

#include "filesystem.h"
#include "intent_check.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define SYMBOLS COPPER_TEST_SOURCE_DIR "/tests/fixtures/template_gen/symbols"
#define FOOTPRINTS COPPER_TEST_SOURCE_DIR "/tests/fixtures/template_gen/footprints"
#define TEMPLATES COPPER_TEST_SOURCE_DIR "/../../design_templates/copper_templates.v1.json"

static Arena arena;
static const char *templates;
static size_t templates_len;

static const char *const PROMPT =
    "Design a compact 2-layer low-side MOSFET switch board for driving a 12 V DC load up to 2 A from a 3.3 V "
    "microcontroller GPIO. Include a screw terminal block for 12 V power and load, a 3-pin header for logic input, an "
    "AO3400A SOT-23 MOSFET, a 100 Ohm gate resistor, a 10 kOhm pull-down resistor, and a flyback diode across the "
    "load terminals. State your trace width and clearance assumptions.";

/* An analysis as Manus should return it, with each value backed by a quote from PROMPT. */
static const char *const SWITCH =
    "{\"schema\": \"copper.requirements\", \"schema_version\": 1, \"circuit\": \"low_side_switch\","
    " \"summary\": \"low-side switch\", \"supply_voltage_v\": 12, \"load_current_a\": 2, \"logic_voltage_v\": 3.3,"
    " \"switch_part\": \"AO3400A\", \"gate_resistor_ohm\": 100, \"pulldown_ohm\": 10000, \"flyback_diode\": true,"
    " \"led_color\": null, \"led_current_ma\": null, \"led_forward_voltage_v\": null, \"board_width_mm\": null, \"board_height_mm\": null,"
    " \"evidence\": ["
    "  {\"field\": \"supply_voltage_v\", \"quote\": \"a 12 V DC load\"},"
    "  {\"field\": \"load_current_a\", \"quote\": \"up to 2 A\"},"
    "  {\"field\": \"logic_voltage_v\", \"quote\": \"from a 3.3 V microcontroller GPIO\"},"
    "  {\"field\": \"switch_part\", \"quote\": \"an AO3400A SOT-23 MOSFET\"},"
    "  {\"field\": \"gate_resistor_ohm\", \"quote\": \"a 100 Ohm gate resistor\"},"
    "  {\"field\": \"pulldown_ohm\", \"quote\": \"a 10 kOhm pull-down resistor\"},"
    "  {\"field\": \"flyback_diode\", \"quote\": \"a flyback diode across the load terminals\"}],"
    " \"open_questions\": [], \"unsupported_requests\": [\"state trace width and clearance assumptions\"]}";

static const char *mutate(const char *base, const char *old, const char *new_text)
{
    const char *at = strstr(base, old);
    assert(at && "mutation anchor not found");
    size_t a = (size_t)(at - base), o = strlen(old), n = strlen(new_text), rest = strlen(at + o);
    char *s = arena_alloc(&arena, a + n + rest + 1);
    assert(s);
    memcpy(s, base, a);
    memcpy(s + a, new_text, n);
    memcpy(s + a + n, at + o, rest + 1);
    return s;
}

static TemplateGenStatus gen(const char *req, const char *prompt, TemplateGenOutput *out)
{
    return template_generate(&arena, req, strlen(req), prompt, templates, templates_len, SYMBOLS, FOOTPRINTS, out);
}

static void grounded(const TemplateGenOutput *out)
{
    IntentCheckReport r;
    IntentCheckStatus st = intent_check(&arena, out->intent_json, out->intent_len, SYMBOLS, FOOTPRINTS, &r);
    if (st != INTENT_CHECK_GROUNDED)
        for (size_t i = 0; i < r.count; i++) fprintf(stderr, "  gate: [%s] %s\n", r.findings[i].code, r.findings[i].message);
    assert(st == INTENT_CHECK_GROUNDED);
}

static void test_switch_from_the_owner_prompt(void)
{
    TemplateGenOutput out;
    TemplateGenStatus st = gen(SWITCH, PROMPT, &out);
    if (st != TGEN_GENERATED) fprintf(stderr, "%s\n", out.report_json);
    assert(st == TGEN_GENERATED && out.intent_json);
    /* The part, pins and nets come from the template and the library, not from the analysis. */
    assert(strstr(out.intent_json, "\"value\": \"AO3400A\""));
    assert(strstr(out.intent_json, "\"symbol_id\": \"Transistor_FET:Q_NMOS_GSD\""));
    assert(strstr(out.intent_json, "\"pin_name\": \"G\",\n          \"pad\": \"1\",\n          \"net\": \"GATE\""));
    assert(strstr(out.intent_json, "\"pin_name\": \"D\",\n          \"pad\": \"3\",\n          \"net\": \"LOAD_NEG\""));
    assert(strstr(out.intent_json, "\"reference\": \"D1\""));
    assert(strstr(out.intent_json, "\"value\": \"10k\""));
    /* Values with provenance; gate drive 3.3 V x 10k / 10.1k = 3.267 V. */
    assert(strstr(out.report_json, "\"name\": \"pulldown_ohm\",\n      \"value\": \"10k\",\n      \"provenance\": \"stated\""));
    assert(strstr(out.report_json, "\"name\": \"gate_voltage_on\",\n      \"value\": \"3.267 V\""));
    assert(strstr(out.report_json, "\"id\": \"MOSFET_RATINGS\",\n      \"status\": \"UNVERIFIED\""));
    assert(strstr(out.report_json, "\"id\": \"TRACE_WIDTH\",\n      \"status\": \"NOT_IMPLEMENTED\""));
    grounded(&out);
}

static void test_invented_requirements_are_ignored(void)
{
    TemplateGenOutput out;
    /* A supply value whose quote is not in the prompt: ignored, so the supply becomes a question. */
    const char *req = mutate(SWITCH, "\"quote\": \"a 12 V DC load\"", "\"quote\": \"a 24 V supply\"");
    req = mutate(req, "\"supply_voltage_v\": 12", "\"supply_voltage_v\": 24");
    assert(gen(req, PROMPT, &out) == TGEN_NEEDS_INPUT && !out.intent_json);
    assert(strstr(out.report_json, "\"field\": \"supply_voltage_v\""));
    assert(strstr(out.report_json, "What is the load supply voltage (V)?"));
    /* A real quote that does not contain the claimed number. */
    req = mutate(SWITCH, "\"supply_voltage_v\": 12", "\"supply_voltage_v\": 24");
    assert(gen(req, PROMPT, &out) == TGEN_NEEDS_INPUT);
    assert(strstr(out.report_json, "does not contain this value"));
    /* A value with no quote at all: the template default is used instead, with its reason. */
    req = mutate(SWITCH, "  {\"field\": \"gate_resistor_ohm\", \"quote\": \"a 100 Ohm gate resistor\"},", "");
    req = mutate(req, "\"gate_resistor_ohm\": 100", "\"gate_resistor_ohm\": 470");
    assert(gen(req, PROMPT, &out) == TGEN_GENERATED);
    assert(strstr(out.report_json, "\"name\": \"gate_resistor_ohm\",\n      \"value\": \"100\",\n      \"provenance\": \"default\""));
    assert(strstr(out.report_json, "no quote from the prompt supports it"));
}

static void test_stated_no_flyback_and_scope(void)
{
    TemplateGenOutput out;
    const char *prompt = mutate(PROMPT, "and a flyback diode across the load terminals", "and no flyback diode because the load is resistive");
    const char *req = mutate(SWITCH, "\"flyback_diode\": true", "\"flyback_diode\": false");
    req = mutate(req, "\"quote\": \"a flyback diode across the load terminals\"", "\"quote\": \"no flyback diode\"");
    assert(gen(req, prompt, &out) == TGEN_GENERATED);
    assert(!strstr(out.intent_json, "\"reference\": \"D1\""));
    grounded(&out);
    /* Outside the template's scope (above 60 V). */
    prompt = mutate(PROMPT, "a 12 V DC load", "a 100 V DC load");
    req = mutate(SWITCH, "\"supply_voltage_v\": 12", "\"supply_voltage_v\": 100");
    req = mutate(req, "\"quote\": \"a 12 V DC load\"", "\"quote\": \"a 100 V DC load\"");
    assert(gen(req, prompt, &out) == TGEN_NOT_IMPLEMENTED && !out.intent_json);
    /* A circuit Copper has no template for. */
    req = mutate(SWITCH, "\"circuit\": \"low_side_switch\"", "\"circuit\": \"other\"");
    assert(gen(req, PROMPT, &out) == TGEN_NOT_IMPLEMENTED);
    assert(strstr(out.report_json, "it can generate: low_side_switch, led_indicator"));
    /* Malformed analysis. */
    assert(gen("{\"schema\": \"something-else\"}", PROMPT, &out) == TGEN_INVALID_INPUT);
}

static void test_led_indicator(void)
{
    static const char *const prompt = "A 5 V board with a red LED driven at 5 mA.";
    static const char *const req =
        "{\"schema\": \"copper.requirements\", \"schema_version\": 1, \"circuit\": \"led_indicator\", \"summary\": \"LED\","
        " \"supply_voltage_v\": 5, \"load_current_a\": null, \"logic_voltage_v\": null, \"switch_part\": null,"
        " \"gate_resistor_ohm\": null, \"pulldown_ohm\": null, \"flyback_diode\": null, \"led_color\": \"red\","
        " \"led_current_ma\": 5, \"led_forward_voltage_v\": null, \"board_width_mm\": null, \"board_height_mm\": null,"
        " \"evidence\": [{\"field\": \"supply_voltage_v\", \"quote\": \"A 5 V board\"}, {\"field\": \"led_current_ma\", \"quote\": \"at 5 mA\"},"
        " {\"field\": \"led_color\", \"quote\": \"a red LED\"}], \"open_questions\": [], \"unsupported_requests\": []}";
    TemplateGenOutput out;
    assert(gen(req, prompt, &out) == TGEN_GENERATED);
    /* (5 V - 2 V assumed) / 5 mA = 600 ohm -> E24 rounded up: 620 ohm -> 4.839 mA. */
    assert(strstr(out.report_json, "\"name\": \"led_resistor_ohm\",\n      \"value\": \"620\",\n      \"provenance\": \"computed\""));
    assert(strstr(out.report_json, "\"name\": \"led_current_actual\",\n      \"value\": \"4.839 mA\""));
    assert(strstr(out.report_json, "\"provenance\": \"assumed\""));
    assert(strstr(out.report_json, "\"id\": \"LED_RATINGS\",\n      \"status\": \"UNVERIFIED\""));
    assert(strstr(out.intent_json, "\"value\": \"red\"") && strstr(out.intent_json, "\"value\": \"620\""));
    grounded(&out);
    /* A supply below the LED's forward voltage cannot work. */
    const char *low = mutate(req, "\"supply_voltage_v\": 5", "\"supply_voltage_v\": 1.8");
    low = mutate(low, "\"quote\": \"A 5 V board\"", "\"quote\": \"A 1.8 V board\"");
    assert(gen(low, "A 1.8 V board with a red LED driven at 5 mA.", &out) == TGEN_INVALID_INPUT && !out.intent_json);
    assert(strstr(out.report_json, "SUPPLY_TOO_LOW"));
}

static void test_without_prompt_and_template_mismatch(void)
{
    TemplateGenOutput out;
    assert(gen(SWITCH, NULL, &out) == TGEN_GENERATED);
    assert(strstr(out.report_json, "\"evidence_checked\": false") && strstr(out.report_json, "\"id\": \"EVIDENCE\""));
    /* Libraries without the template's parts: a template error, never a substitution. */
    TemplateGenStatus st = template_generate(&arena, SWITCH, strlen(SWITCH), PROMPT, templates, templates_len,
                                             COPPER_TEST_SOURCE_DIR "/tests/fixtures/intent_check/symbols",
                                             COPPER_TEST_SOURCE_DIR "/tests/fixtures/intent_check/footprints", &out);
    assert(st == TGEN_TEMPLATE_ERROR && !out.intent_json);
    assert(strstr(out.report_json, "TEMPLATE_LIBRARY_MISMATCH"));
}

int main(void)
{
    arena_init(&arena, 1u << 20);
    const unsigned char *data = NULL;
    assert(fs_read_file_arena(&arena, TEMPLATES, &data, &templates_len) == FS_OK);
    templates = (const char *)data;
    test_switch_from_the_owner_prompt();
    test_invented_requirements_are_ignored();
    test_stated_no_flyback_and_scope();
    test_led_indicator();
    test_without_prompt_and_template_mismatch();
    arena_destroy(&arena);
    puts("template_gen: all tests passed");
    return 0;
}

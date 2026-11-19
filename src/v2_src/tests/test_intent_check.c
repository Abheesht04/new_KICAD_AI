/* intent_check (src/v2_src/intent_check.c): the grounding gate for model-written design intents.
 *
 * A valid, fully grounded design is checked first; then each test injects one typical language-model hallucination
 * (an invented part, pin, pad, field or polarity, a dangling net, a forgotten power pin...) and the gate must name it.
 * Fixture: tests/fixtures/intent_check (Copper-generated KiCad-format symbols and footprints, not a KiCad export). */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "intent_check.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define SYMBOLS COPPER_TEST_SOURCE_DIR "/tests/fixtures/intent_check/symbols"
#define FOOTPRINTS COPPER_TEST_SOURCE_DIR "/tests/fixtures/intent_check/footprints"

static const char *const GOOD =
    "{\"schema\": \"copper.design-intent\", \"schema_version\": 1,"
    " \"project\": {\"name\": \"GateFixture\", \"board\": {\"width_mm\": 40, \"height_mm\": 30, \"layers\": 2}},"
    " \"components\": ["
    "  {\"reference\": \"U1\", \"value\": \"VREG\", \"symbol_id\": \"Device:VREG\", \"footprint_id\": \"Package_TO_SOT_SMD:SOT-23\","
    "   \"pins\": [{\"symbol_pin\": \"1\", \"pin_name\": \"IN\", \"pad\": \"1\", \"net\": \"VIN\"},"
    "             {\"symbol_pin\": \"2\", \"pin_name\": \"GND\", \"pad\": \"2\", \"net\": \"GND\"},"
    "             {\"symbol_pin\": \"3\", \"pin_name\": \"OUT\", \"pad\": \"3\", \"net\": \"VOUT\"}],"
    "   \"position\": {\"x_mm\": 8, \"y_mm\": 15, \"rotation_deg\": 0}},"
    "  {\"reference\": \"R2\", \"value\": \"10k\", \"symbol_id\": \"Device:R\", \"footprint_id\": \"Resistor_SMD:R_0603_1608Metric\","
    "   \"pins\": [{\"symbol_pin\": \"1\", \"pin_name\": \"~\", \"pad\": \"1\", \"net\": \"VIN\"},"
    "             {\"symbol_pin\": \"2\", \"pin_name\": \"\", \"pad\": \"2\", \"net\": \"GND\"}]},"
    "  {\"reference\": \"R1\", \"value\": \"1k\", \"symbol_id\": \"Device:R\", \"footprint_id\": \"Resistor_SMD:R_0603_1608Metric\","
    "   \"pins\": [{\"symbol_pin\": \"1\", \"pin_name\": \"~\", \"pad\": \"1\", \"net\": \"VOUT\"},"
    "             {\"symbol_pin\": \"2\", \"pin_name\": \"~\", \"pad\": \"2\", \"net\": \"LED_A\"}]},"
    "  {\"reference\": \"D1\", \"value\": \"RED\", \"symbol_id\": \"Device:LED\", \"footprint_id\": \"LED_SMD:LED_0603_1608Metric\","
    "   \"pins\": [{\"symbol_pin\": \"1\", \"pin_name\": \"K\", \"pad\": \"1\", \"net\": \"GND\"},"
    "             {\"symbol_pin\": \"2\", \"pin_name\": \"A\", \"pad\": \"2\", \"net\": \"LED_A\"}],"
    "   \"position\": {\"x_mm\": 37.5, \"y_mm\": 2.5, \"rotation_deg\": 0}}"
    " ]}";

static Arena arena;

/* GOOD with the first occurrence of `old` replaced by `new_text`. */
static const char *mutate(const char *old, const char *new_text)
{
    const char *at = strstr(GOOD, old);
    assert(at && "mutation anchor not found");
    size_t a = (size_t)(at - GOOD), o = strlen(old), n = strlen(new_text), rest = strlen(at + o);
    char *s = arena_alloc(&arena, a + n + rest + 1);
    assert(s);
    memcpy(s, GOOD, a);
    memcpy(s + a, new_text, n);
    memcpy(s + a + n, at + o, rest + 1);
    return s;
}

static IntentCheckStatus check(const char *json, IntentCheckReport *r)
{
    return intent_check(&arena, json, strlen(json), SYMBOLS, FOOTPRINTS, r);
}

static bool has(const IntentCheckReport *r, const char *code, const char *ref, const char *item)
{
    for (size_t i = 0; i < r->count; i++) {
        const IntentFinding *f = &r->findings[i];
        if (strcmp(f->code, code) != 0) continue;
        if (ref && (!f->reference || strcmp(f->reference, ref) != 0)) continue;
        if (item && (!f->item || strcmp(f->item, item) != 0)) continue;
        return true;
    }
    return false;
}

static void dump(const IntentCheckReport *r)
{
    for (size_t i = 0; i < r->count; i++)
        fprintf(stderr, "  [%s] %s %s: %s\n", r->findings[i].code, r->findings[i].reference ? r->findings[i].reference : "-",
                r->findings[i].item ? r->findings[i].item : "-", r->findings[i].message);
}

/* Exactly one kind of finding expected (`code` on ref/item), nothing else. */
static void expect_only(const char *json, const char *code, const char *ref, const char *item)
{
    IntentCheckReport r;
    IntentCheckStatus st = check(json, &r);
    if (st != INTENT_CHECK_REJECTED || !has(&r, code, ref, item)) { fprintf(stderr, "expected %s %s %s\n", code, ref ? ref : "", item ? item : ""); dump(&r); }
    assert(st == INTENT_CHECK_REJECTED);
    assert(has(&r, code, ref, item));
    for (size_t i = 0; i < r.count; i++) {
        if (strcmp(r.findings[i].code, code) != 0) { dump(&r); }
        assert(strcmp(r.findings[i].code, code) == 0);
    }
}

static void test_grounded_design_passes(void)
{
    IntentCheckReport r;
    IntentCheckStatus st = check(GOOD, &r);
    if (st != INTENT_CHECK_GROUNDED) dump(&r);
    assert(st == INTENT_CHECK_GROUNDED);
    assert(r.count == 0 && r.components == 4 && r.pins == 9 && r.nets == 4);
    size_t len = 0;
    const char *json = intent_check_report_json(&arena, &r, &len);
    assert(json && strstr(json, "\"verdict\": \"GROUNDED\""));
}

static void test_swapped_led_polarity_is_caught(void)
{
    /* The model believes pin 1 is the anode: the numbers are valid, the idea is wrong. */
    const char *json = mutate("{\"symbol_pin\": \"1\", \"pin_name\": \"K\"", "{\"symbol_pin\": \"1\", \"pin_name\": \"A\"");
    expect_only(json, "PIN_NAME_MISMATCH", "D1", "1");
}

static void test_invented_parts_are_rejected(void)
{
    expect_only(mutate("\"Device:LED\"", "\"Device:LED_UltraBright\""), "SYMBOL_NOT_FOUND", "D1", "Device:LED_UltraBright");
    IntentCheckReport r;
    assert(check(mutate("\"symbol_id\": \"Device:R\"", "\"symbol_id\": \"Imaginary:R\""), &r) == INTENT_CHECK_REJECTED);
    assert(has(&r, "SYMBOL_LIBRARY_NOT_FOUND", "R2", "Imaginary:R"));
    expect_only(mutate("\"LED_SMD:LED_0603_1608Metric\"", "\"LED_SMD:LED_0402_Phantom\""), "FOOTPRINT_NOT_FOUND", "D1", NULL);
    expect_only(mutate("\"Device:VREG\"", "\"../../etc:VREG\""), "BAD_LIBRARY_ID", "U1", "symbol_id");
}

static void test_invented_pins_and_pads_are_rejected(void)
{
    IntentCheckReport r;
    /* R1 gets a third pin that does not exist (on the symbol or the footprint). */
    const char *json = mutate("{\"symbol_pin\": \"2\", \"pin_name\": \"~\", \"pad\": \"2\", \"net\": \"LED_A\"}]",
                              "{\"symbol_pin\": \"2\", \"pin_name\": \"~\", \"pad\": \"2\", \"net\": \"LED_A\"},"
                              " {\"symbol_pin\": \"3\", \"pin_name\": \"~\", \"pad\": \"3\", \"net\": \"LED_A\"}]");
    assert(check(json, &r) == INTENT_CHECK_REJECTED);
    assert(has(&r, "UNKNOWN_SYMBOL_PIN", "R1", "3") && has(&r, "UNKNOWN_PAD", "R1", "3"));
    /* U1 maps pin 1 twice instead of its output: pin 3 and pad 3 are unmapped, VOUT is left with R1 alone. */
    json = mutate("{\"symbol_pin\": \"3\", \"pin_name\": \"OUT\", \"pad\": \"3\", \"net\": \"VOUT\"}", "{\"symbol_pin\": \"1\", \"pin_name\": \"IN\", \"pad\": \"1\", \"net\": \"VIN\"}");
    assert(check(json, &r) == INTENT_CHECK_REJECTED);
    assert(has(&r, "DUPLICATE_SYMBOL_PIN", "U1", "1") && has(&r, "DUPLICATE_PAD", "U1", "1"));
    assert(has(&r, "MISSING_SYMBOL_PIN", "U1", "3") && has(&r, "MISSING_PAD", "U1", "3"));
    assert(has(&r, "SINGLE_ENDED_NET", NULL, "VOUT"));
    /* A pin_name left out entirely. */
    expect_only(mutate("\"pin_name\": \"OUT\", ", ""), "PIN_NAME_MISSING", "U1", "3");
}

static void test_invented_fields_and_claims_are_rejected(void)
{
    expect_only(mutate("\"value\": \"RED\"", "\"value\": \"RED\", \"mpn\": \"LTST-C191KRKT\""), "UNKNOWN_FIELD", "D1", "mpn");
    expect_only(mutate("\"schema_version\": 1,", "\"schema_version\": 1, \"routed\": true, \"drc\": \"PASS\","), "UNKNOWN_FIELD", NULL, NULL);
    expect_only(mutate("\"net\": \"VOUT\"}],", "\"net\": \"VOUT\", \"current_ma\": 20}],"), "UNKNOWN_FIELD", "U1", "current_ma");
}

static void test_nets_and_power_pins(void)
{
    /* LED_A renamed on one side only: two single-ended nets. */
    IntentCheckReport r;
    assert(check(mutate("\"pin_name\": \"A\", \"pad\": \"2\", \"net\": \"LED_A\"", "\"pin_name\": \"A\", \"pad\": \"2\", \"net\": \"LED_ANODE\""), &r) == INTENT_CHECK_REJECTED);
    assert(has(&r, "SINGLE_ENDED_NET", NULL, "LED_A") && has(&r, "SINGLE_ENDED_NET", NULL, "LED_ANODE"));
    /* A power input pin quietly "not connected". */
    assert(check(mutate("\"pin_name\": \"IN\", \"pad\": \"1\", \"net\": \"VIN\"", "\"pin_name\": \"IN\", \"pad\": \"1\", \"net\": null"), &r) == INTENT_CHECK_REJECTED);
    assert(has(&r, "UNCONNECTED_POWER_PIN", "U1", "1"));
    /* A malformed net name; VOUT is then left with R1 alone, which is reported too. */
    assert(check(mutate("\"net\": \"VOUT\"}],", "\"net\": \"VOUT; DROP\"}],"), &r) == INTENT_CHECK_REJECTED);
    assert(has(&r, "BAD_NET_NAME", "U1", "3") && has(&r, "SINGLE_ENDED_NET", NULL, "VOUT"));
}

static void test_unsupported_symbols(void)
{
    IntentCheckReport r;
    assert(check(mutate("\"Device:LED\"", "\"Device:LED_Red\""), &r) == INTENT_CHECK_REJECTED);
    assert(has(&r, "DERIVED_SYMBOL", "D1", "Device:LED_Red"));
    assert(check(mutate("\"symbol_id\": \"Device:VREG\"", "\"symbol_id\": \"Device:DUAL\""), &r) == INTENT_CHECK_REJECTED);
    assert(has(&r, "MULTI_UNIT_SYMBOL", "U1", "Device:DUAL"));
}

static void test_positions_and_identity(void)
{
    /* D1 sits exactly 2.5 mm from two edges (accepted above); 1 nm further out is rejected. */
    expect_only(mutate("\"x_mm\": 37.5", "\"x_mm\": 37.500001"), "POSITION_OUTSIDE_BOARD", "D1", "position");
    expect_only(mutate("\"reference\": \"R1\"", "\"reference\": \"R2\""), "DUPLICATE_REFERENCE", "R2", NULL);
    expect_only(mutate("\"reference\": \"R1\"", "\"reference\": \"resistor one\""), "BAD_REFERENCE", NULL, NULL);
    IntentCheckReport r;
    assert(check("{\"schema\": \"copper.design-intent\", ", &r) == INTENT_CHECK_REJECTED && has(&r, "INVALID_JSON", NULL, NULL));
    assert(check(mutate("\"layers\": 2", "\"layers\": 4"), &r) == INTENT_CHECK_REJECTED && has(&r, "SCHEMA", NULL, "project.board.layers"));
}

int main(void)
{
    arena_init(&arena, 1u << 20);
    test_grounded_design_passes();
    test_swapped_led_polarity_is_caught();
    test_invented_parts_are_rejected();
    test_invented_pins_and_pads_are_rejected();
    test_invented_fields_and_claims_are_rejected();
    test_nets_and_power_pins();
    test_unsupported_symbols();
    test_positions_and_identity();
    arena_destroy(&arena);
    puts("intent_check: all tests passed");
    return 0;
}

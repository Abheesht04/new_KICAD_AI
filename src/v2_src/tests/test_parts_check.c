/* parts_check (src/v2_src/parts_check.c): the per-part report on a part catalogue.
 *
 * The catalogues here are written by Copper for the test (parts by "Fixture Mfr", URLs on example.invalid); the
 * sources marked verified say "TEST FIXTURE, not a real verification". They are not part data. The KiCad libraries are
 * the template_gen fixtures: symbols and footprints extracted unchanged from the KiCad 10.0.5 libraries. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "parts_check.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define LIBS COPPER_TEST_SOURCE_DIR "/tests/fixtures/template_gen/"
#define SYMS LIBS "symbols"
#define FPS LIBS "footprints"

#define WHERE "(url \"https://example.invalid/fx.pdf\") (document \"FX datasheet\") (revision \"A, 2026-01\")"
#define BY "(status \"verified\") (verified_by \"TEST FIXTURE, not a real verification\") (verified_on \"2026-11-21\")"
#define SRC_V "(source " WHERE " (page 2) " BY ")"
#define SRC_U "(source " WHERE " (page 2) (status \"unverified\"))"
#define ID_V "(identity_source " WHERE " " BY ")"
#define ID_U "(identity_source " WHERE " (status \"unverified\"))"
#define DS "(datasheet \"https://example.invalid/fx.pdf\")"

#define R_PINS "(pins (pin \"terminal_1\" \"1\" \"1\") (pin \"terminal_2\" \"2\" \"2\"))"
#define RES(id, mpn, fp, idsrc, src) \
    "(part (id \"" id "\") (kind R) (manufacturer \"Fixture Mfr\") (mpn \"" mpn "\") (package \"0603\") " DS \
    " (symbol \"Device:R\") (footprint \"" fp "\") " idsrc \
    " (ratings (rating resistance nominal 10000000000 uohm " src ") (rating power maximum 100000 uW " src "))" \
    " " R_PINS ")"
#define CAP(id, src) \
    "(part (id \"" id "\") (kind C) (manufacturer \"Fixture Mfr\") (mpn \"FX-C100N\") (package \"0603\") " DS \
    " (symbol \"Device:C\") (footprint \"Capacitor_SMD:C_0603_1608Metric\") " ID_V \
    " (ratings (rating capacitance nominal 100000 pF " src ") (rating voltage maximum 50000000 uV " src "))" \
    " " R_PINS ")"
#define CATALOGUE(parts) "(copper_parts (schema 1) " parts ")"

static Arena arena;

static PartsCheckReport run(const char *text, bool kicad)
{
    PartsCheckReport r;
    PartsCheckStatus st = parts_check_text(&arena, text, strlen(text), kicad ? SYMS : NULL, kicad ? FPS : NULL, &r);
    assert(st == r.verdict);
    return r;
}

static size_t count(const PartsCheckReport *r, const char *code, const char *part)
{
    size_t n = 0;
    for (size_t i = 0; i < r->finding_count; i++)
        if (!strcmp(r->findings[i].code, code) && (!part || (r->findings[i].part_id && !strcmp(r->findings[i].part_id, part)))) n++;
    return n;
}

static const char *message(const PartsCheckReport *r, const char *code)
{
    for (size_t i = 0; i < r->finding_count; i++)
        if (!strcmp(r->findings[i].code, code)) return r->findings[i].message;
    return "";
}

static void dump(const PartsCheckReport *r)
{
    for (size_t i = 0; i < r->finding_count; i++)
        fprintf(stderr, "  %s %s %s: %s\n", parts_finding_level_name(r->findings[i].level), r->findings[i].code,
                r->findings[i].part_id ? r->findings[i].part_id : "-", r->findings[i].message);
}

static void test_verified_library_passes(void)
{
    PartsCheckReport r = run(CATALOGUE(RES("r10k", "FX-10K", "Resistor_SMD:R_0603_1608Metric", ID_V, SRC_V) CAP("c100n", SRC_V)), true);
    if (r.verdict != PARTS_CHECK_PASS) dump(&r);
    assert(r.verdict == PARTS_CHECK_PASS && r.finding_count == 0 && r.kicad_checked);
    assert(r.part_count == 2);
    for (size_t i = 0; i < 2; i++) {
        assert(!strcmp(r.parts[i].kicad, "PASS") && r.parts[i].identity_verified);
        assert(r.parts[i].ratings == 2 && r.parts[i].ratings_verified == 2);
    }
}

static void test_without_kicad_roots_is_never_pass(void)
{
    PartsCheckReport r = run(CATALOGUE(RES("r10k", "FX-10K", "Resistor_SMD:R_0603_1608Metric", ID_V, SRC_V)), false);
    assert(r.verdict == PARTS_CHECK_UNVERIFIED && !r.kicad_checked);
    assert(count(&r, "KICAD_NOT_RUN", NULL) == 1 && !strcmp(r.parts[0].kicad, "NOT_RUN"));
}

static void test_unverified_sources_are_listed(void)
{
    PartsCheckReport r = run(CATALOGUE(RES("r10k", "FX-10K", "Resistor_SMD:R_0603_1608Metric", ID_U, SRC_U)), true);
    assert(r.verdict == PARTS_CHECK_UNVERIFIED && r.fail_count == 0);
    assert(count(&r, "SOURCE_UNVERIFIED", "r10k") == 1);
    assert(!r.parts[0].identity_verified && r.parts[0].ratings_verified == 0 && r.parts[0].ratings == 2);
    const char *m = message(&r, "SOURCE_UNVERIFIED");
    assert(strstr(m, "identity") && strstr(m, "resistance") && strstr(m, "power"));
}

static void test_missing_required_rating(void)
{
    const char *text = CATALOGUE(
        "(part (id \"r_nopower\") (kind R) (manufacturer \"Fixture Mfr\") (mpn \"FX-NP\") " DS
        " (symbol \"Device:R\") (footprint \"Resistor_SMD:R_0603_1608Metric\") " ID_V
        " (ratings (rating resistance nominal 10000000000 uohm " SRC_V ")) " R_PINS ")");
    PartsCheckReport r = run(text, true);
    assert(r.verdict == PARTS_CHECK_UNVERIFIED);
    assert(count(&r, "REQUIRED_RATING_MISSING", "r_nopower") == 1 && strstr(message(&r, "REQUIRED_RATING_MISSING"), "power"));
}

static void test_missing_identity_and_identity_source(void)
{
    const char *text = CATALOGUE(
        "(part (id \"r_anon\") (kind R) (manufacturer \"Fixture Mfr\")"
        " (symbol \"Device:R\") (footprint \"Resistor_SMD:R_0603_1608Metric\")"
        " (ratings (rating resistance nominal 10000000000 uohm " SRC_V ") (rating power maximum 100000 uW " SRC_V ")) "
        R_PINS ")");
    PartsCheckReport r = run(text, true);
    assert(r.verdict == PARTS_CHECK_FAIL);
    const char *m = message(&r, "MISSING_IDENTITY");
    assert(count(&r, "MISSING_IDENTITY", "r_anon") == 1 && strstr(m, "mpn") && strstr(m, "datasheet") && !strstr(m, "manufacturer"));
    assert(count(&r, "NO_IDENTITY_SOURCE", "r_anon") == 1);
}

static void test_incomplete_citations(void)
{
    const char *text = CATALOGUE(
        "(part (id \"r_cite\") (kind R) (manufacturer \"Fixture Mfr\") (mpn \"FX-CITE\") " DS
        " (symbol \"Device:R\") (footprint \"Resistor_SMD:R_0603_1608Metric\")"
        " (identity_source (url \"https://example.invalid/fx.pdf\") (document \"FX datasheet\") (status \"unverified\"))"
        " (ratings (rating resistance nominal 10000000000 uohm " SRC_U ")"
        " (rating power maximum 100000 uW (source " WHERE " (status \"unverified\")))) " R_PINS ")");
    PartsCheckReport r = run(text, true);
    assert(r.verdict == PARTS_CHECK_FAIL);
    /* the identity source has no revision; the power rating's source has no page */
    assert(count(&r, "CITATION_INCOMPLETE", "r_cite") == 2);
}

static void test_every_kicad_problem_is_reported(void)
{
    const char *text = CATALOGUE(
        RES("r_nofp", "FX-A", "Resistor_SMD:R_9999_NoSuchFootprint", ID_V, SRC_V)
        "(part (id \"r_badpad\") (kind R) (manufacturer \"Fixture Mfr\") (mpn \"FX-B\") " DS
        " (symbol \"Device:R\") (footprint \"Resistor_SMD:R_0603_1608Metric\") " ID_V
        " (ratings (rating resistance nominal 10000000000 uohm " SRC_V ") (rating power maximum 100000 uW " SRC_V "))"
        " (pins (pin \"terminal_1\" \"1\" \"1\") (pin \"terminal_2\" \"3\" \"3\")))"
        RES("r_nosym", "FX-C", "Resistor_SMD:R_0603_1608Metric", ID_V, SRC_V));
    /* the third part asks for a symbol library that does not exist */
    char buf[8192];
    snprintf(buf, sizeof buf, "%s", text);
    char *p = strstr(buf, "(id \"r_nosym\")");
    assert(p);
    p = strstr(p, "Device:R");
    assert(p);
    memcpy(p, "NoLib:R_", 8);
    PartsCheckReport r = run(buf, true);
    if (r.verdict != PARTS_CHECK_FAIL) dump(&r);
    assert(r.verdict == PARTS_CHECK_FAIL);
    assert(count(&r, "KICAD_ASSET", "r_nofp") == 1 && count(&r, "KICAD_ASSET", "r_badpad") == 1 && count(&r, "KICAD_ASSET", "r_nosym") == 1);
    assert(!strcmp(r.parts[0].kicad, "FAIL") && !strcmp(r.parts[1].kicad, "FAIL") && !strcmp(r.parts[2].kicad, "FAIL"));
}

static void test_duplicate_mpn_and_unknown_field(void)
{
    const char *text = CATALOGUE(
        RES("r_one", "FX-SAME", "Resistor_SMD:R_0603_1608Metric", ID_V, SRC_V)
        RES("r_two", "FX-SAME", "Resistor_SMD:R_0603_1608Metric", ID_V, SRC_V)
        "(part (id \"r_typo\") (kind R) (manufacturer \"Fixture Mfr\") (mpn \"FX-T\") (mnp \"FX-T\") " DS
        " (symbol \"Device:R\") (footprint \"Resistor_SMD:R_0603_1608Metric\") " ID_V
        " (ratings (rating resistance nominal 10000000000 uohm " SRC_V ") (rating power maximum 100000 uW " SRC_V ")) "
        R_PINS ")");
    PartsCheckReport r = run(text, true);
    assert(r.verdict == PARTS_CHECK_FAIL);
    assert(count(&r, "DUPLICATE_MPN", "r_two") == 1 && count(&r, "DUPLICATE_MPN", "r_one") == 0);
    assert(count(&r, "UNKNOWN_FIELD", "r_typo") == 1 && strstr(message(&r, "UNKNOWN_FIELD"), "mnp"));
}

static void test_fail_outranks_unverified(void)
{
    PartsCheckReport r = run(CATALOGUE(RES("r_u", "FX-U", "Resistor_SMD:R_0603_1608Metric", ID_U, SRC_U)
                                       RES("r_f", "FX-F", "Resistor_SMD:R_0603_1608Metric_Missing", ID_V, SRC_V)), true);
    assert(r.verdict == PARTS_CHECK_FAIL && r.fail_count >= 1 && r.unverified_count >= 1);
}

static void test_invalid_input(void)
{
    /* a rating with a unit that does not match its name: the catalogue parser rejects the whole file */
    const char *text = CATALOGUE(
        "(part (id \"r_bad\") (kind R) (symbol \"Device:R\") (footprint \"Resistor_SMD:R_0603_1608Metric\")"
        " (ratings (rating resistance nominal 10 pF " SRC_V ")))");
    PartsCheckReport r = run(text, true);
    assert(r.verdict == PARTS_CHECK_INVALID_INPUT && r.invalid_message[0] && r.part_count == 0);
    PartsCheckReport f;
    assert(parts_check_file(&arena, COPPER_TEST_SOURCE_DIR "/tests/fixtures/parts_check/no_such_file.sx", SYMS, FPS, &f) ==
           PARTS_CHECK_INVALID_INPUT);
    /* the roots come in pairs */
    PartsCheckReport a;
    assert(parts_check_text(&arena, text, strlen(text), SYMS, NULL, &a) == PARTS_CHECK_ERR_ARGUMENT);
}

static void test_file_and_json(void)
{
    PartsCheckReport r;
    PartsCheckStatus st = parts_check_file(&arena, COPPER_TEST_SOURCE_DIR "/tests/fixtures/parts_check/sample_library.sx", SYMS, FPS, &r);
    if (st != PARTS_CHECK_UNVERIFIED) dump(&r);
    assert(st == PARTS_CHECK_UNVERIFIED && r.part_count == 2 && r.fail_count == 0);
    size_t n1 = 0, n2 = 0;
    const char *j1 = parts_check_report_json(&arena, &r, &n1);
    const char *j2 = parts_check_report_json(&arena, &r, &n2);
    assert(j1 && j2 && n1 == n2 && !memcmp(j1, j2, n1));
    assert(strstr(j1, "\"format\": \"copper.parts-check/1\"") && strstr(j1, "\"verdict\": \"UNVERIFIED\""));
    assert(strstr(j1, "\"SOURCE_UNVERIFIED\"") && strstr(j1, "\"ratings_verified\": 0"));
}

int main(void)
{
    arena_init(&arena, 1u << 22);
    test_verified_library_passes();
    test_without_kicad_roots_is_never_pass();
    test_unverified_sources_are_listed();
    test_missing_required_rating();
    test_missing_identity_and_identity_source();
    test_incomplete_citations();
    test_every_kicad_problem_is_reported();
    test_duplicate_mpn_and_unknown_field();
    test_fail_outranks_unverified();
    test_invalid_input();
    test_file_and_json();
    arena_destroy(&arena);
    puts("parts_check: all tests passed");
    return 0;
}

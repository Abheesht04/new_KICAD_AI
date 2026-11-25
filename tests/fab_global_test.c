/* The global fab profile (src/v2_src/fab_global.c, Step 3a) from the three real profiles in fab_profiles/:
 * jlcpcb_2layer (jlcpcb.com/capabilities/pcb-capabilities, read 2026-11-06 and 2026-11-25), oshpark_2layer
 * (docs.oshpark.com/services/two-layer, 2026-11-25) and pcbway_2layer (pcbway.com/capabilities.html, 2026-11-25).
 * These are Copper-written data files quoting the fabricators' pages, not KiCad exports; the expected values are
 * the table in docs/engineering/LAYOUT_RULES.md section 2. */
#include "fab_global.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

#define MM(x) ((G2Nm)(x))   /* nanometres */

static const char *const PATHS[3] = {
    FAB_ROOT "/fab_profiles/jlcpcb_2layer.json",
    FAB_ROOT "/fab_profiles/oshpark_2layer.json",
    FAB_ROOT "/fab_profiles/pcbway_2layer.json",
};

static void expect(const FabGlobal *g, DfmLimit l, G2Nm nm, int from, size_t missing)
{
    if (!g->limits[l].present || g->limits[l].nm != nm || g->from[l] != from || g->missing[l] != missing)
        fprintf(stderr, "  %s: present %d nm %lld from %d missing %zu\n", dfm_limit_key(l), g->limits[l].present,
                (long long)g->limits[l].nm, g->from[l], g->missing[l]);
    CHECK(g->limits[l].present && g->limits[l].nm == nm && g->from[l] == from && g->missing[l] == missing);
}

static void expect_ms(const FabGlobal *g, FabMaskSilkLimit l, G2Nm nm, int from, size_t missing)
{
    if (!g->mask_silk[l].present || g->mask_silk[l].nm != nm || g->mask_silk_from[l] != from || g->mask_silk_missing[l] != missing)
        fprintf(stderr, "  %s: present %d nm %lld from %d missing %zu\n", fab_mask_silk_key(l), g->mask_silk[l].present,
                (long long)g->mask_silk[l].nm, g->mask_silk_from[l], g->mask_silk_missing[l]);
    CHECK(g->mask_silk[l].present && g->mask_silk[l].nm == nm && g->mask_silk_from[l] == from && g->mask_silk_missing[l] == missing);
}

static void test_real_profiles(void)
{
    Arena a; arena_init(&a, 1 << 16);
    FabGlobalInput in[3];
    FabGlobalError e;
    CHECK(fab_global_load(&a, PATHS, 3, in, &e) == FAB_GLOBAL_OK);
    if (e.code) fprintf(stderr, "  load: profile %zu key %s: %s\n", e.profile, e.key ? e.key : "-", e.message ? e.message : "-");
    CHECK(in[0].mask_silk.present && in[1].mask_silk.present && in[2].mask_silk.present);

    FabGlobal g;
    CHECK(fab_global_build(&a, in, 3, &g, &e) == FAB_GLOBAL_OK);
    CHECK(g.count == 3);
    expect(&g, DFM_TRACK_WIDTH_MIN, MM(152400), 1, 0);              /* OSH Park 6 mil */
    expect(&g, DFM_TRACK_SPACING_MIN, MM(152400), 1, 0);
    expect(&g, DFM_CLEARANCE_DEFAULT_MIN, MM(152400), 1, 0);
    expect(&g, DFM_PAD_TO_TRACK_MIN, MM(100000), 0, 2);             /* JLCPCB only */
    expect(&g, DFM_SMD_PAD_TO_PAD_MIN, MM(150000), 0, 2);
    expect(&g, DFM_PTH_ANNULAR_RING_MIN, MM(180000), 0, 0);         /* JLCPCB 0.18 > PCBWay 0.15 > OSH 0.127 */
    expect(&g, DFM_VIA_DIAMETER_OVER_HOLE_MIN, MM(300000), 2, 0);   /* PCBWay 2 x 0.15 */
    expect(&g, DFM_VIA_HOLE_MIN, MM(254000), 1, 0);
    expect(&g, DFM_DRILL_MIN, MM(254000), 1, 0);
    expect(&g, DFM_COPPER_TO_EDGE_MIN, MM(381000), 1, 1);           /* PCBWay: not in the standard table */
    expect(&g, DFM_BOARD_MAX_X, MM(670000000), 0, 2);               /* a maximum: only JLCPCB publishes one */
    CHECK(!g.limits[DFM_HOLE_TO_EDGE_MIN].present && g.from[DFM_HOLE_TO_EDGE_MIN] == -1 && g.missing[DFM_HOLE_TO_EDGE_MIN] == 3);

    expect_ms(&g, FAB_SOLDER_MASK_WEB_MIN, MM(101600), 1, 1);
    expect_ms(&g, FAB_SILK_LINE_WIDTH_MIN, MM(150000), 0, 0);       /* JLCPCB and PCBWay tie at 0.15: the earlier */
    expect_ms(&g, FAB_SILK_TEXT_HEIGHT_MIN, MM(1000000), 0, 1);
    expect_ms(&g, FAB_PAD_TO_SILK_MIN, MM(150000), 0, 2);

    /* the deciding fab is named, and so are the ones that publish nothing */
    CHECK(g.limits[DFM_DRILL_MIN].row && strstr(g.limits[DFM_DRILL_MIN].row, "OSH Park"));
    CHECK(g.limits[DFM_COPPER_TO_EDGE_MIN].note && strstr(g.limits[DFM_COPPER_TO_EDGE_MIN].note, "PCBWay"));
    CHECK(g.limits[DFM_PAD_TO_TRACK_MIN].note && strstr(g.limits[DFM_PAD_TO_TRACK_MIN].note, "OSH Park") &&
          strstr(g.limits[DFM_PAD_TO_TRACK_MIN].note, "PCBWay"));
    CHECK(g.limits[DFM_TRACK_WIDTH_MIN].note == NULL || strstr(g.limits[DFM_TRACK_WIDTH_MIN].note, "not published") == NULL);

    /* the written profile is a copper.fab_profile/1 that dfm.c accepts, with the same values; deterministic */
    const char *t1, *t2; size_t n1, n2;
    CHECK(fab_global_write(&a, in, &g, "global_2layer", &t1, &n1) == FAB_GLOBAL_OK);
    CHECK(fab_global_write(&a, in, &g, "global_2layer", &t2, &n2) == FAB_GLOBAL_OK);
    CHECK(n1 == n2 && !memcmp(t1, t2, n1));
    DfmProfile p; DfmError de;
    CHECK(dfm_profile_parse(&a, t1, n1, &p, &de) == DFM_OK);
    if (de.code) fprintf(stderr, "  reparse: %s: %s\n", de.key ? de.key : "-", de.message ? de.message : "-");
    for (int k = 0; k < DFM_LIMIT_COUNT; k++)
        CHECK(p.limits[k].present == g.limits[k].present && (!p.limits[k].present || p.limits[k].nm == g.limits[k].nm));
    JsonValue *root; JsonError je;
    CHECK(json_parse(&a, t1, n1, &root, &je) == JSON_OK);
    FabMaskSilk ms;
    CHECK(fab_mask_silk_from(root, &ms, &e) == FAB_GLOBAL_OK && ms.present);
    for (int k = 0; k < FAB_MASK_SILK_COUNT; k++)
        CHECK(ms.limits[k].present == g.mask_silk[k].present && (!ms.limits[k].present || ms.limits[k].nm == g.mask_silk[k].nm));
    CHECK(strstr(t1, "\"derived_from\"") && strstr(t1, "https://docs.oshpark.com/services/two-layer/"));
    CHECK(strstr(t1, "\"drill_min_mm\"") && strstr(t1, "0.254"));
    CHECK(strstr(t1, "0.1524"));
    arena_destroy(&a);
}

/* Hand-made inputs: direction (minimum vs maximum), ties, no section, none published. */
static void set(DfmLimitValue *v, G2Nm nm) { v->present = true; v->nm = nm; v->row = "row"; v->quote = "quote"; }

static void test_directions_and_ties(void)
{
    Arena a; arena_init(&a, 1 << 14);
    FabGlobalInput in[2];
    memset(in, 0, sizeof in);
    in[0].profile.name = "a"; in[0].profile.fabricator = "FabA"; in[0].profile.source_url = "u"; in[0].profile.retrieved = "2026-01-01";
    in[1].profile.name = "b"; in[1].profile.fabricator = "FabB"; in[1].profile.source_url = "v"; in[1].profile.retrieved = "2026-01-02";
    for (int k = 0; k < DFM_LIMIT_COUNT; k++) { in[0].profile.limits[k].quote = "none"; in[1].profile.limits[k].quote = "none"; }
    set(&in[0].profile.limits[DFM_BOARD_MAX_X], 600000000); set(&in[1].profile.limits[DFM_BOARD_MAX_X], 500000000);
    set(&in[0].profile.limits[DFM_BOARD_MAX_THIN_Y], 400000000); set(&in[1].profile.limits[DFM_BOARD_MAX_THIN_Y], 450000000);
    set(&in[0].profile.limits[DFM_BOARD_MIN_X], 3000000); set(&in[1].profile.limits[DFM_BOARD_MIN_X], 5000000);
    set(&in[0].profile.limits[DFM_DRILL_MIN], 200000); set(&in[1].profile.limits[DFM_DRILL_MIN], 200000);
    /* in[1] has no mask_silk section: every mask/silk limit counts it as missing */
    in[0].mask_silk.present = true;
    for (int k = 0; k < FAB_MASK_SILK_COUNT; k++) in[0].mask_silk.limits[k].quote = "none";
    set(&in[0].mask_silk.limits[FAB_PAD_TO_SILK_MIN], 150000);

    FabGlobal g; FabGlobalError e;
    CHECK(fab_global_build(&a, in, 2, &g, &e) == FAB_GLOBAL_OK);
    expect(&g, DFM_BOARD_MAX_X, 500000000, 1, 0);       /* maximum: the smaller */
    expect(&g, DFM_BOARD_MAX_THIN_Y, 400000000, 0, 0);
    expect(&g, DFM_BOARD_MIN_X, 5000000, 1, 0);         /* minimum: the larger */
    expect(&g, DFM_DRILL_MIN, 200000, 0, 0);            /* tie: the earlier */
    CHECK(!g.limits[DFM_TRACK_WIDTH_MIN].present && g.from[DFM_TRACK_WIDTH_MIN] == -1 && g.missing[DFM_TRACK_WIDTH_MIN] == 2);
    expect_ms(&g, FAB_PAD_TO_SILK_MIN, 150000, 0, 1);
    CHECK(!g.mask_silk[FAB_SILK_TEXT_HEIGHT_MIN].present && g.mask_silk_missing[FAB_SILK_TEXT_HEIGHT_MIN] == 2);

    /* order matters only for ties; reversing gives the same values */
    FabGlobalInput rev[2] = {in[1], in[0]};
    FabGlobal g2;
    CHECK(fab_global_build(&a, rev, 2, &g2, &e) == FAB_GLOBAL_OK);
    for (int k = 0; k < DFM_LIMIT_COUNT; k++)
        CHECK(g2.limits[k].present == g.limits[k].present && g2.limits[k].nm == g.limits[k].nm);
    CHECK(g2.from[DFM_DRILL_MIN] == 0);

    CHECK(fab_global_build(&a, in, 0, &g, &e) == FAB_GLOBAL_INVALID_ARGUMENT);
    CHECK(fab_global_build(&a, NULL, 2, &g, &e) == FAB_GLOBAL_INVALID_ARGUMENT);
    arena_destroy(&a);
}

static FabGlobalStatus parse_ms(Arena *a, const char *text, FabGlobalError *e)
{
    JsonValue *root; JsonError je; FabMaskSilk ms;
    if (json_parse(a, text, strlen(text), &root, &je) != JSON_OK) return FAB_GLOBAL_INVALID_ARGUMENT;
    return fab_mask_silk_from(root, &ms, e);
}

#define MS_OK "\"solder_mask_web_min_mm\":{\"value\":0.1,\"row\":\"r\",\"quote\":\"q\"}," \
              "\"silk_line_width_min_mm\":{\"value\":null,\"row\":null,\"quote\":\"not on the page\"}," \
              "\"silk_text_height_min_mm\":{\"value\":1.0,\"row\":\"r\",\"quote\":\"q\"}," \
              "\"pad_to_silk_min_mm\":{\"value\":0.15,\"row\":\"r\",\"quote\":\"q\"}"

static void test_mask_silk_section_is_strict(void)
{
    Arena a; arena_init(&a, 1 << 14);
    FabGlobalError e;
    JsonValue *root; JsonError je; FabMaskSilk ms;
    const char *ok = "{\"mask_silk_limits\":{\"retrieved\":\"2026-11-25\"," MS_OK "}}";
    CHECK(json_parse(&a, ok, strlen(ok), &root, &je) == JSON_OK);
    CHECK(fab_mask_silk_from(root, &ms, &e) == FAB_GLOBAL_OK && ms.present);
    CHECK(ms.limits[FAB_SOLDER_MASK_WEB_MIN].nm == 100000 && !ms.limits[FAB_SILK_LINE_WIDTH_MIN].present);
    CHECK(ms.retrieved && !strcmp(ms.retrieved, "2026-11-25"));

    const char *none = "{\"name\":\"x\"}";   /* no section: fine, nothing present */
    CHECK(json_parse(&a, none, strlen(none), &root, &je) == JSON_OK);
    CHECK(fab_mask_silk_from(root, &ms, &e) == FAB_GLOBAL_OK && !ms.present);

    /* a misspelt key, a missing key, no quote, no row for a value, a bad number, no retrieved date */
    CHECK(parse_ms(&a, "{\"mask_silk_limits\":{\"retrieved\":\"d\"," MS_OK ",\"silk_line_widht_min_mm\":{}}}", &e) == FAB_GLOBAL_INVALID_PROFILE);
    CHECK(e.key && !strcmp(e.key, "silk_line_widht_min_mm"));
    CHECK(parse_ms(&a, "{\"mask_silk_limits\":{\"retrieved\":\"d\","
                       "\"solder_mask_web_min_mm\":{\"value\":0.1,\"row\":\"r\",\"quote\":\"q\"}}}", &e) == FAB_GLOBAL_INVALID_PROFILE);
    CHECK(e.key && !strcmp(e.key, "silk_line_width_min_mm"));
    CHECK(parse_ms(&a, "{\"mask_silk_limits\":{\"retrieved\":\"d\","
                       "\"solder_mask_web_min_mm\":{\"value\":0.1,\"row\":\"r\"},"
                       "\"silk_line_width_min_mm\":{\"value\":null,\"quote\":\"q\"},"
                       "\"silk_text_height_min_mm\":{\"value\":null,\"quote\":\"q\"},"
                       "\"pad_to_silk_min_mm\":{\"value\":null,\"quote\":\"q\"}}}", &e) == FAB_GLOBAL_INVALID_PROFILE);
    CHECK(e.key && !strcmp(e.key, "solder_mask_web_min_mm"));
    CHECK(parse_ms(&a, "{\"mask_silk_limits\":{\"retrieved\":\"d\","
                       "\"solder_mask_web_min_mm\":{\"value\":0.1,\"quote\":\"q\"},"
                       "\"silk_line_width_min_mm\":{\"value\":null,\"quote\":\"q\"},"
                       "\"silk_text_height_min_mm\":{\"value\":null,\"quote\":\"q\"},"
                       "\"pad_to_silk_min_mm\":{\"value\":null,\"quote\":\"q\"}}}", &e) == FAB_GLOBAL_INVALID_PROFILE);
    CHECK(parse_ms(&a, "{\"mask_silk_limits\":{\"retrieved\":\"d\","
                       "\"solder_mask_web_min_mm\":{\"value\":-0.1,\"row\":\"r\",\"quote\":\"q\"},"
                       "\"silk_line_width_min_mm\":{\"value\":null,\"quote\":\"q\"},"
                       "\"silk_text_height_min_mm\":{\"value\":null,\"quote\":\"q\"},"
                       "\"pad_to_silk_min_mm\":{\"value\":null,\"quote\":\"q\"}}}", &e) == FAB_GLOBAL_INVALID_PROFILE);
    CHECK(parse_ms(&a, "{\"mask_silk_limits\":{" MS_OK "}}", &e) == FAB_GLOBAL_INVALID_PROFILE);
    CHECK(e.key && !strcmp(e.key, "retrieved"));
    arena_destroy(&a);
}

static void test_load_errors(void)
{
    Arena a; arena_init(&a, 1 << 14);
    FabGlobalInput in[2]; FabGlobalError e;
    const char *bad[2] = {PATHS[0], FAB_ROOT "/fab_profiles/no_such_profile.json"};
    CHECK(fab_global_load(&a, bad, 2, in, &e) == FAB_GLOBAL_INVALID_PROFILE);
    CHECK(e.profile == 1);
    CHECK(fab_global_load(&a, bad, 0, in, &e) == FAB_GLOBAL_INVALID_ARGUMENT);
    arena_destroy(&a);
}

int main(void)
{
    test_real_profiles();
    test_directions_and_ties();
    test_mask_silk_section_is_strict();
    test_load_errors();
    if (failures) { fprintf(stderr, "fab_global: %d failure(s)\n", failures); return 1; }
    puts("fab_global: all tests passed");
    return 0;
}

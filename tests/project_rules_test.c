/* KiCad project rules from a fab profile (src/v2_src/project_rules.c, Step 3b).
 *
 * Golden files:
 *   - src/v2_src/tests/fixtures/sample_resistor_lol: a real KiCad 9.0 project (.kicad_pro, .kicad_pcb with
 *     generator_version "9.0"): the merge must keep every other member of the KiCad-written .kicad_pro.
 *   - The fine-pitch boards below are Copper-written. Their SOT-563 pads (0.675 x 0.35 mm at 0.5 mm pitch, a 0.15 mm
 *     gap) are copied from KiCad 10.0.5's Package_TO_SOT_SMD.pretty/SOT-563.kicad_mod; the pad angle in a board file
 *     is absolute (G-075).
 * Profiles: fab_profiles/{jlcpcb,oshpark,pcbway}_2layer.json; rules: routing_profiles/copper_2layer_default.json.
 * kicad-cli behaviour the mapping relies on (rules applied, memberOfFootprint) was checked by hand with
 * KiCad 10.0.5 (a development check, not KiCad 9 evidence); the project_rules_drc ctest repeats it when KiCad is set. */
#include "project_rules.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static Arena A;

static FabGlobalInput load_fab(const char *name)
{
    char path[512];
    snprintf(path, sizeof path, "%s/fab_profiles/%s_2layer.json", PR_ROOT, name);
    const char *paths[1] = {path};
    FabGlobalInput in; FabGlobalError e;
    if (fab_global_load(&A, paths, 1, &in, &e) != FAB_GLOBAL_OK) { fprintf(stderr, "cannot load %s: %s\n", path, e.message); exit(2); }
    return in;
}

static PrDesignRules load_rules(void)
{
    JsonValue *root; JsonError je; PrDesignRules r; PrError e;
    if (json_parse_file(&A, PR_ROOT "/routing_profiles/copper_2layer_default.json", &root, &je) != JSON_OK) exit(2);
    if (pr_design_rules_from(root, &r, &e) != PR_OK) { fprintf(stderr, "rules: %s %s\n", e.key, e.message); exit(2); }
    return r;
}

static const PrSetting *setting(const PrResult *r, const char *key)
{
    for (size_t i = 0; i < r->setting_count; i++) if (!strcmp(r->settings[i].kicad_key, key)) return &r->settings[i];
    return NULL;
}

static bool written(const PrResult *r, const char *key, G2Nm nm)
{
    const PrSetting *s = setting(r, key);
    if (!s || !s->written || s->nm != nm) fprintf(stderr, "  %s: %s %lld\n", key, s ? (s->written ? "written" : "not written") : "absent", s ? (long long)s->nm : 0LL);
    return s && s->written && s->nm == nm;
}

/* a number member of an object, in nm */
static G2Nm nm_at(const JsonValue *o, const char *key)
{
    G2Nm v = -1;
    const JsonValue *x = json_get(o, key);
    if (!x || json_to_nm(x, &v) != JSON_NUMBER_OK) return -1;
    return v;
}

static const JsonValue *default_class(const JsonValue *pro)
{
    const JsonValue *cls = json_get(json_get(pro, "net_settings"), "classes");
    if (!cls || cls->kind != JSON_ARRAY) return NULL;
    for (size_t i = 0; i < cls->count; i++) {
        const JsonValue *n = json_get(cls->items[i], "name");
        if (n && n->kind == JSON_STRING && !strcmp(n->text, "Default")) return cls->items[i];
    }
    return NULL;
}

static void test_real_kicad9_project(void)
{
    const char *dir = PR_ROOT "/src/v2_src/tests/fixtures/sample_resistor_lol/";
    Board b; BoardError be;
    CHECK(board_load(&A, PR_ROOT "/src/v2_src/tests/fixtures/sample_resistor_lol/sample_resistor_lol.kicad_pcb", &b, &be) == BOARD_OK);
    JsonValue *pro; JsonError je;
    char path[512]; snprintf(path, sizeof path, "%ssample_resistor_lol.kicad_pro", dir);
    CHECK(json_parse_file(&A, path, &pro, &je) == JSON_OK);
    FabGlobalInput jlc = load_fab("jlcpcb");
    PrDesignRules rules = load_rules();
    CHECK(rules.track == 250000 && rules.clearance == 300000 && rules.via_diameter == 600000 && rules.via_drill == 300000);

    PrResult r; PrError e;
    CHECK(pr_build(&A, &b, &jlc.profile, &jlc.mask_silk, &rules, pro, "sample_resistor_lol.kicad_pro", &r, &e) == PR_OK);
    /* board minimums: JLCPCB's limits */
    CHECK(written(&r, "min_clearance", 100000));
    CHECK(written(&r, "min_track_width", 100000));
    CHECK(written(&r, "min_through_hole_diameter", 150000));
    CHECK(written(&r, "min_hole_to_hole", 200000));
    CHECK(written(&r, "min_hole_clearance", 200000));
    CHECK(written(&r, "min_copper_edge_clearance", 200000));
    CHECK(written(&r, "min_via_diameter", 250000));
    CHECK(written(&r, "min_via_annular_width", 50000));
    CHECK(written(&r, "min_text_height", 1000000));
    CHECK(written(&r, "min_text_thickness", 150000));
    const PrSetting *silk = setting(&r, "min_silk_clearance");
    CHECK(silk && !silk->written && silk->note);   /* KiCad's is silk to silk; the fab's is pad to silk */
    CHECK(r.fine_count == 0 && r.verdict == DFM_PASS);

    /* the merged project: every other member kept, the rules and the Default class replaced */
    JsonValue *out;
    CHECK(json_parse(&A, r.pro_text, r.pro_len, &out, &je) == JSON_OK);
    CHECK(out->count == pro->count);
    for (size_t i = 0; i < pro->count; i++) CHECK(json_get(out, pro->keys[i]) != NULL);
    const JsonValue *rules_o = json_get(json_get(json_get(out, "board"), "design_settings"), "rules");
    CHECK(nm_at(rules_o, "min_through_hole_diameter") == 150000);
    CHECK(nm_at(rules_o, "min_microvia_drill") == 100000);            /* untouched KiCad value */
    CHECK(nm_at(rules_o, "max_error") == 5000);
    const JsonValue *uh = json_get(rules_o, "use_height_for_length_calcs");
    CHECK(uh && uh->kind == JSON_TRUE);
    CHECK(json_get(rules_o, "min_silk_clearance") != NULL && nm_at(rules_o, "min_silk_clearance") == 0);   /* kept as KiCad wrote it */
    const JsonValue *dc = default_class(out);
    CHECK(dc != NULL);
    CHECK(nm_at(dc, "clearance") == 300000 && nm_at(dc, "track_width") == 250000);
    CHECK(nm_at(dc, "via_diameter") == 600000 && nm_at(dc, "via_drill") == 300000);
    CHECK(nm_at(dc, "diff_pair_gap") == 250000);                      /* untouched */
    const JsonValue *bw = json_get(dc, "bus_width");
    CHECK(bw && bw->kind == JSON_NUMBER && !strcmp(bw->text, "12"));
    const JsonValue *meta = json_get(out, "meta");
    CHECK(meta && !strcmp(json_get(meta, "filename")->text, "sample_resistor_lol.kicad_pro"));

    CHECK(r.dru_text && !strncmp(r.dru_text, PR_DRU_HEADER "\n", strlen(PR_DRU_HEADER) + 1));
    CHECK(strstr(r.dru_text, "(version 1)") && !strstr(r.dru_text, "(rule"));

    /* deterministic */
    PrResult r2;
    CHECK(pr_build(&A, &b, &jlc.profile, &jlc.mask_silk, &rules, pro, "sample_resistor_lol.kicad_pro", &r2, &e) == PR_OK);
    CHECK(r.pro_len == r2.pro_len && !memcmp(r.pro_text, r2.pro_text, r.pro_len));
    CHECK(r.dru_len == r2.dru_len && !memcmp(r.dru_text, r2.dru_text, r.dru_len));
    CHECK(r.report_len == r2.report_len && !memcmp(r.report_text, r2.report_text, r.report_len));
}

/* SOT-563 (pads from KiCad 10.0.5) at `angle`, an 0603 resistor, and a footprint whose only close pads share a net. */
#define SOT563(ref, x, y, a) \
    "(footprint \"Package_TO_SOT_SMD:SOT-563\" (layer \"F.Cu\") (at " x " " y " " a ") (property \"Reference\" \"" ref "\")" \
    " (pad \"1\" smd roundrect (at -0.7125 -0.5 " a ") (size 0.675 0.35) (layers \"F.Cu\") (net 1 \"SCL\"))" \
    " (pad \"2\" smd roundrect (at -0.7125 0 " a ") (size 0.675 0.35) (layers \"F.Cu\") (net 2 \"GND\"))" \
    " (pad \"3\" smd roundrect (at -0.7125 0.5 " a ") (size 0.675 0.35) (layers \"F.Cu\"))" \
    " (pad \"4\" smd roundrect (at 0.7125 0.5 " a ") (size 0.675 0.35) (layers \"F.Cu\") (net 2 \"GND\"))" \
    " (pad \"5\" smd roundrect (at 0.7125 0 " a ") (size 0.675 0.35) (layers \"F.Cu\") (net 3 \"VCC\"))" \
    " (pad \"6\" smd roundrect (at 0.7125 -0.5 " a ") (size 0.675 0.35) (layers \"F.Cu\") (net 4 \"SDA\")))"

static const char *board_text(const char *ref)
{
    static char t[8192];
    snprintf(t, sizeof t,
        "(kicad_pcb (version 20241229) (generator \"test\") (general (thickness 1.6))\n"
        " (layers (0 \"F.Cu\" signal) (2 \"B.Cu\" signal) (25 \"Edge.Cuts\" user))\n"
        " (net 0 \"\") (net 1 \"SCL\") (net 2 \"GND\") (net 3 \"VCC\") (net 4 \"SDA\")\n"
        " (gr_rect (start 0 0) (end 40 30) (layer \"Edge.Cuts\"))\n"
        " %s\n"
        " (footprint \"Resistor_SMD:R_0603_1608Metric\" (layer \"F.Cu\") (at 20 10) (property \"Reference\" \"R1\")"
        "  (pad \"1\" smd roundrect (at -0.825 0) (size 0.8 0.95) (layers \"F.Cu\") (net 1 \"SCL\"))"
        "  (pad \"2\" smd roundrect (at 0.825 0) (size 0.8 0.95) (layers \"F.Cu\") (net 3 \"VCC\")))\n"
        " (footprint \"T:SameNet\" (layer \"F.Cu\") (at 30 10) (property \"Reference\" \"J1\")"
        "  (pad \"1\" smd rect (at 0 0) (size 1 1) (layers \"F.Cu\") (net 2 \"GND\"))"
        "  (pad \"2\" smd rect (at 1.1 0) (size 1 1) (layers \"F.Cu\") (net 2 \"GND\"))"
        "  (pad \"3\" smd rect (at 5 0) (size 1 1) (layers \"F.Cu\") (net 3 \"VCC\")))\n"
        ")\n", ref);
    return t;
}

static void load_board(const char *fps, Board *b)
{
    const char *t = board_text(fps);
    BoardError be;
    BoardStatus s = board_parse(&A, t, strlen(t), b, &be);
    if (s != BOARD_OK) fprintf(stderr, "board: %s\n", be.message ? be.message : "?");
    CHECK(s == BOARD_OK);
}

static void test_fine_pitch(void)
{
    PrDesignRules rules = load_rules();
    FabGlobalInput jlc = load_fab("jlcpcb"), osh = load_fab("oshpark"), pcbway = load_fab("pcbway");
    PrResult r; PrError e;
    Board b;

    /* JLCPCB: 0.15 mm pad to pad, the SOT-563 gap is exactly 0.15 mm: relaxed to 0.15, PASS */
    load_board(SOT563("U3", "10", "10", "0"), &b);
    CHECK(pr_build(&A, &b, &jlc.profile, &jlc.mask_silk, &rules, NULL, "t.kicad_pro", &r, &e) == PR_OK);
    CHECK(r.fine_count == 1);
    if (r.fine_count == 1) {
        const PrFinePitch *f = &r.fine[0];
        CHECK(!strcmp(f->reference, "U3") && f->min_gap == 150000 && !f->approximate);
        CHECK(f->floor_present && f->floor == 150000 && !strcmp(f->floor_from, "fab:smd_pad_to_pad_min_mm"));
        CHECK(f->rule_written && f->verdict == DFM_PASS);
    }
    CHECK(r.verdict == DFM_PASS);
    CHECK(strstr(r.dru_text, "(rule \"copper_fine_pitch_U3\""));
    CHECK(strstr(r.dru_text, "(constraint clearance (min 0.15mm))"));
    CHECK(strstr(r.dru_text, "(condition \"A.memberOfFootprint('U3') && B.memberOfFootprint('U3')\")"));
    /* a new project file: meta and the net_settings version of a KiCad 9.0 file */
    JsonValue *out; JsonError je;
    CHECK(json_parse(&A, r.pro_text, r.pro_len, &out, &je) == JSON_OK);
    CHECK(!strcmp(json_get(json_get(out, "meta"), "filename")->text, "t.kicad_pro"));
    const JsonValue *nsm = json_get(json_get(out, "net_settings"), "meta");
    CHECK(nsm && nm_at(nsm, "version") == 4000000);   /* json_to_nm reads 4 as 4 mm: the integer 4 */
    CHECK(default_class(out) && nm_at(default_class(out), "clearance") == 300000);

    /* the same part turned 90 degrees (pad angles absolute): the same gap */
    load_board(SOT563("U3", "10", "10", "90"), &b);
    CHECK(pr_build(&A, &b, &jlc.profile, &jlc.mask_silk, &rules, NULL, "t.kicad_pro", &r, &e) == PR_OK);
    CHECK(r.fine_count == 1 && r.fine[0].min_gap == 150000 && r.verdict == DFM_PASS);

    /* Step 3c: the router's escape stubs (routing rules fine_pitch_clearance_mm 0.15) get their own rule: a track
       that enters U3's courtyard keeps 0.15 mm to U3's pads (kicad-cli checks both A/B orders; checked by hand) */
    CHECK(rules.has_fine && rules.fine_clearance == 150000);
    CHECK(strstr(r.dru_text, "(rule \"copper_fine_pitch_U3_escape\""));
    CHECK(strstr(r.dru_text, "(condition \"A.memberOfFootprint('U3') && B.Type == 'Track' && B.intersectsCourtyard('U3')\")"));
    {
        const char *esc = strstr(r.dru_text, "copper_fine_pitch_U3_escape");
        CHECK(esc && strstr(esc, "(constraint clearance (min 0.15mm))"));
    }
    /* OSH Park's default copper clearance (0.1524 mm) is above the router's fine clearance: refused, as copper design
       route refuses it */
    load_board(SOT563("U3", "10", "10", "0"), &b);
    CHECK(pr_build(&A, &b, &osh.profile, &osh.mask_silk, &rules, NULL, "t.kicad_pro", &r, &e) == PR_INVALID_RULES);
    CHECK(e.key && !strcmp(e.key, "fine_pitch_clearance_mm"));

    /* OSH Park publishes no pad limit: its default copper clearance 0.1524 mm is the floor, and 0.15 < 0.1524
       (routing rules without the fine-pitch keys: nothing about escapes is written) */
    rules.has_fine = false;
    rules.fine_clearance = 0;
    CHECK(pr_build(&A, &b, &osh.profile, &osh.mask_silk, &rules, NULL, "t.kicad_pro", &r, &e) == PR_OK);
    CHECK(!strstr(r.dru_text, "_escape"));
    CHECK(r.fine_count == 1 && r.fine[0].floor == 152400 && !strcmp(r.fine[0].floor_from, "fab:clearance_default_min_mm"));
    CHECK(r.fine[0].verdict == DFM_FAIL && r.verdict == DFM_FAIL);
    CHECK(strstr(r.dru_text, "(constraint clearance (min 0.1524mm))"));   /* the rule still makes KiCad report it */
    CHECK(written(&r, "min_through_hole_diameter", 254000));
    CHECK(written(&r, "min_via_annular_width", 127000));
    CHECK(written(&r, "min_copper_edge_clearance", 381000));
    const PrSetting *th = setting(&r, "min_text_height");
    CHECK(th && !th->written && th->note && strstr(th->note, "KiCad default"));   /* OSH Park publishes none */

    /* PCBWay: no pad limit, default clearance 0.10 mm: PASS */
    CHECK(pr_build(&A, &b, &pcbway.profile, &pcbway.mask_silk, &rules, NULL, "t.kicad_pro", &r, &e) == PR_OK);
    CHECK(r.fine_count == 1 && r.fine[0].floor == 100000 && r.fine[0].verdict == DFM_PASS);
    const PrSetting *edge = setting(&r, "min_copper_edge_clearance");
    CHECK(edge && !edge->written && edge->note && strstr(edge->note, "KiCad default"));

    /* a reference that cannot be quoted in a rule: nothing relaxed, UNVERIFIED */
    load_board(SOT563("U'3", "10", "10", "0"), &b);
    CHECK(pr_build(&A, &b, &jlc.profile, &jlc.mask_silk, &rules, NULL, "t.kicad_pro", &r, &e) == PR_OK);
    CHECK(r.fine_count == 1 && !r.fine[0].rule_written && r.fine[0].verdict == DFM_UNVERIFIED && r.verdict == DFM_UNVERIFIED);
    CHECK(!strstr(r.dru_text, "(rule"));
}

static void test_derived_values_and_refusals(void)
{
    PrDesignRules rules = load_rules();
    FabGlobalInput jlc = load_fab("jlcpcb");
    PrResult r; PrError e;
    Board b;
    load_board("", &b);

    /* an odd via ring is rounded up (stricter) */
    DfmProfile p = jlc.profile;
    p.limits[DFM_VIA_DIAMETER_OVER_HOLE_MIN].nm = 100001;
    CHECK(pr_build(&A, &b, &p, &jlc.mask_silk, &rules, NULL, "t.kicad_pro", &r, &e) == PR_OK);
    CHECK(written(&r, "min_via_annular_width", 50001));

    /* Copper's rules below the fab's limit are refused, not written */
    PrDesignRules low = rules;
    low.clearance = 50000;
    CHECK(pr_build(&A, &b, &jlc.profile, &jlc.mask_silk, &low, NULL, "t.kicad_pro", &r, &e) == PR_INVALID_RULES);
    CHECK(e.key && !strcmp(e.key, "clearance_mm"));
    low = rules; low.via_drill = 100000;
    CHECK(pr_build(&A, &b, &jlc.profile, &jlc.mask_silk, &low, NULL, "t.kicad_pro", &r, &e) == PR_INVALID_RULES);
    CHECK(e.key && !strcmp(e.key, "via_drill_mm"));
    low = rules; low.track = 50000;
    CHECK(pr_build(&A, &b, &jlc.profile, &jlc.mask_silk, &low, NULL, "t.kicad_pro", &r, &e) == PR_INVALID_RULES);
    low = rules; low.via_diameter = 350000;   /* ring 0.025 < JLCPCB's 0.05 */
    CHECK(pr_build(&A, &b, &jlc.profile, &jlc.mask_silk, &low, NULL, "t.kicad_pro", &r, &e) == PR_INVALID_RULES);

    /* an existing project that is not a JSON object */
    JsonValue *arr; JsonError je;
    CHECK(json_parse(&A, "[1]", 3, &arr, &je) == JSON_OK);
    CHECK(pr_build(&A, &b, &jlc.profile, &jlc.mask_silk, &rules, arr, "t.kicad_pro", &r, &e) == PR_INVALID_PROJECT);

    /* routing rules with a value missing */
    JsonValue *root;
    const char *t = "{\"format\":\"copper.routing_rules/1\",\"name\":\"x\",\"rules\":{\"track_mm\":{\"value\":0.25},"
                    "\"clearance_mm\":{\"value\":0.3},\"via_size_mm\":{\"value\":0.6}}}";
    CHECK(json_parse(&A, t, strlen(t), &root, &je) == JSON_OK);
    PrDesignRules dr;
    CHECK(pr_design_rules_from(root, &dr, &e) == PR_INVALID_RULES && e.key && !strcmp(e.key, "via_drill_mm"));
    CHECK(pr_build(NULL, &b, &jlc.profile, &jlc.mask_silk, &rules, NULL, "t.kicad_pro", &r, &e) == PR_INVALID_ARGUMENT);
}

int main(void)
{
    arena_init(&A, 1 << 20);
    test_real_kicad9_project();
    test_fine_pitch();
    test_derived_values_and_refusals();
    arena_destroy(&A);
    if (failures) { fprintf(stderr, "project_rules: %d failure(s)\n", failures); return 1; }
    puts("project_rules: all tests passed");
    return 0;
}

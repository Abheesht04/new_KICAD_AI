/* Findings and the report (src/v2_src/finding.c, json.c writer; track E).
 *
 * The golden report tests/golden/report.json comes from fixed inputs:
 *   dfm       tests/golden/findings/board.kicad_pcb (test-authored, not a KiCad export) against
 *             fab_profiles/jlcpcb_2layer.json (JLCPCB's published limits, 2026-11-06);
 *   board     the same board's model issues;
 *   excellon  manufacturing_audit/drill_test/design.drl (real KiCad 9.0.6: no findings) and
 *             tests/golden/findings/broken.drl (test-authored problems);
 *   gerber    manufacturing_audit/gerber_test/design-Edge_Cuts.gm1 (real KiCad 9.0.6: no findings) and
 *             tests/golden/findings/arc.gbr (test-authored problems).
 * The report must equal the golden file byte for byte (line endings aside), and must not change when the inputs are
 * added in another order. `finding_test --write-golden FILE` writes the report for review. */
#include "finding.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

#define ROOT FINDING_ROOT "/"

typedef struct Inputs {
    Board board;
    DfmProfile profile;
    DfmReport dfm;
    ExcFile drill_ok, drill_bad;
    GerberFile edge, arc;
} Inputs;

static bool load_inputs(Arena *a, Inputs *in)
{
    BoardError be;
    DfmError de;
    bool ok = board_load(a, ROOT "tests/golden/findings/board.kicad_pcb", &in->board, &be) == BOARD_OK;
    ok = ok && dfm_profile_load(a, ROOT "fab_profiles/jlcpcb_2layer.json", &in->profile, &de) == DFM_OK;
    ok = ok && dfm_check(a, &in->board, &in->profile, &in->dfm, &de) == DFM_OK;
    ok = ok && exc_parse_path(a, ROOT "manufacturing_audit/drill_test/design.drl", &in->drill_ok) == EXC_OK;
    (void)exc_parse_path(a, ROOT "tests/golden/findings/broken.drl", &in->drill_bad);   /* content errors expected */
    ok = ok && gerber_parse_path(a, ROOT "manufacturing_audit/gerber_test/design-Edge_Cuts.gm1", &in->edge) == GERBER_OK;
    ok = ok && gerber_parse_path(a, ROOT "tests/golden/findings/arc.gbr", &in->arc) == GERBER_OK;
    return ok;
}

static const char *report(Arena *a, const Inputs *in, bool reverse, size_t *len)
{
    FindingList l;
    finding_list_init(&l, a);
    if (!reverse) {
        finding_from_dfm(&l, "board.kicad_pcb", &in->board, &in->profile, &in->dfm);
        finding_from_board(&l, "board.kicad_pcb", &in->board);
        finding_from_excellon(&l, "drill/design.drl", &in->drill_ok);
        finding_from_excellon(&l, "drill/broken.drl", &in->drill_bad);
        finding_from_gerber(&l, "gerber/design-Edge_Cuts.gm1", &in->edge);
        finding_from_gerber(&l, "gerber/arc.gbr", &in->arc);
    } else {
        finding_from_gerber(&l, "gerber/arc.gbr", &in->arc);
        finding_from_gerber(&l, "gerber/design-Edge_Cuts.gm1", &in->edge);
        finding_from_excellon(&l, "drill/broken.drl", &in->drill_bad);
        finding_from_excellon(&l, "drill/design.drl", &in->drill_ok);
        finding_from_board(&l, "board.kicad_pcb", &in->board);
        finding_from_dfm(&l, "board.kicad_pcb", &in->board, &in->profile, &in->dfm);
    }
    return finding_report_json(&l, a, len);
}

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    size_t got = fread(b, 1, (size_t)n, f), k = 0;
    fclose(f);
    for (size_t i = 0; i < got; i++) if (b[i] != '\r') b[k++] = b[i];   /* git may check the file out with CRLF */
    b[k] = 0;
    *len = k;
    return b;
}

static void test_writer(void)
{
    Arena a;
    JsonWriter w;
    size_t n;
    arena_init(&a, 1 << 12);
    json_writer_init(&w, &a);
    json_write_begin_object(&w);
    json_write_key(&w, "s"); json_write_string(&w, "q\"b\\n\n\t\x01\xc3\xa9");
    json_write_key(&w, "a");
    json_write_begin_array(&w); json_write_int(&w, -5); json_write_uint(&w, 18446744073709551615u); json_write_bool(&w, true); json_write_null(&w); json_write_end_array(&w);
    json_write_key(&w, "e"); json_write_begin_object(&w); json_write_end_object(&w);
    json_write_key(&w, "z"); json_write_begin_array(&w); json_write_end_array(&w);
    json_write_end_object(&w);
    const char *t = json_writer_finish(&w, &n);
    const char *want = "{\n  \"s\": \"q\\\"b\\\\n\\n\\t\\u0001\xc3\xa9\",\n  \"a\": [\n    -5,\n    18446744073709551615,\n    true,\n    null\n  ],\n"
                       "  \"e\": {},\n  \"z\": []\n}\n";
    CHECK(t && !strcmp(t, want) && n == strlen(want));
    /* The output reads back with the strict reader. */
    JsonValue *v;
    JsonError e;
    CHECK(t && json_parse(&a, t, n, &v, &e) == JSON_OK && !strcmp(json_get(v, "s")->text, "q\"b\\n\n\t\x01\xc3\xa9"));
    /* Misuse fails closed. */
    json_writer_init(&w, &a); json_write_begin_object(&w); json_write_int(&w, 1);
    CHECK(json_writer_finish(&w, &n) == NULL);
    json_writer_init(&w, &a); json_write_begin_array(&w);
    CHECK(json_writer_finish(&w, &n) == NULL);
    json_writer_init(&w, &a); json_write_begin_array(&w); json_write_end_object(&w);
    CHECK(json_writer_finish(&w, &n) == NULL);
    json_writer_init(&w, &a); json_write_int(&w, 1); json_write_int(&w, 2);
    CHECK(json_writer_finish(&w, &n) == NULL);
    arena_destroy(&a);
}

static void test_verdict(void)
{
    Arena a;
    FindingList l;
    Finding f;
    arena_init(&a, 1 << 12);
    memset(&f, 0, sizeof f);
    f.source = "x"; f.rule = "x/r"; f.message = "m";
    finding_list_init(&l, &a);
    CHECK(finding_verdict(&l) == FINDING_VERDICT_PASS);
    f.status = FINDING_INFO; finding_add(&l, &f);
    CHECK(finding_verdict(&l) == FINDING_VERDICT_PASS);
    f.status = FINDING_NOT_IMPLEMENTED; finding_add(&l, &f);
    CHECK(finding_verdict(&l) == FINDING_VERDICT_UNVERIFIED);
    f.status = FINDING_INVALID_INPUT; finding_add(&l, &f);
    CHECK(finding_verdict(&l) == FINDING_VERDICT_INVALID_INPUT);
    f.status = FINDING_FAIL; finding_add(&l, &f);
    CHECK(finding_verdict(&l) == FINDING_VERDICT_FAIL);
    f.message = NULL;
    CHECK(!finding_add(&l, &f) && l.failed && finding_verdict(&l) == FINDING_VERDICT_INVALID_INPUT);   /* incomplete list */
    arena_destroy(&a);
}

static void test_golden(void)
{
    Arena a;
    Inputs in;
    size_t n1, n2, ng;
    arena_init(&a, 1 << 20);
    CHECK(load_inputs(&a, &in));
    const char *forward = report(&a, &in, false, &n1), *backward = report(&a, &in, true, &n2);
    CHECK(forward && backward && n1 == n2 && !strcmp(forward, backward));
    char *golden = slurp(ROOT "tests/golden/report.json", &ng);
    CHECK(golden != NULL);
    if (golden && forward && strcmp(golden, forward)) {
        size_t i = 0;
        while (golden[i] && golden[i] == forward[i]) i++;
        fprintf(stderr, "report differs from tests/golden/report.json at byte %zu:\n  golden: %.60s\n  now:    %.60s\n", i, golden + i, forward + i);
        failures++;
    }
    /* What the golden report must say, independently of its bytes. */
    JsonValue *v;
    JsonError e;
    CHECK(forward && json_parse(&a, forward, n1, &v, &e) == JSON_OK);
    if (forward && v) {
        CHECK(!strcmp(json_get(v, "verdict")->text, "FAIL") && json_get(v, "complete")->kind == JSON_TRUE);
        const JsonValue *fs = json_get(v, "findings");
        size_t fail = 0, invalid = 0, notimpl = 0, quoted = 0;
        for (size_t i = 0; i < fs->count; i++) {
            const char *st = json_get(fs->items[i], "status")->text;
            fail += !strcmp(st, "FAIL");
            invalid += !strcmp(st, "INVALID_INPUT");
            notimpl += !strcmp(st, "NOT_IMPLEMENTED");
            if (!strcmp(st, "FAIL")) quoted += json_get(fs->items[i], "fabricator_says") != NULL;
        }
        /* track width, track spacing, via hole; the undefined drill tool; zone fills. */
        CHECK(fail == 3 && quoted == fail && invalid == 1 && notimpl == 1 && fs->count == 13);
        const JsonValue *src = json_get(v, "sources");
        CHECK(src && src->count == 6);
        for (size_t i = 0; i < src->count; i++) {
            const char *file = json_get(src->items[i], "file")->text, *verdict = json_get(src->items[i], "verdict")->text;
            if (!strcmp(file, "drill/design.drl") || !strcmp(file, "gerber/design-Edge_Cuts.gm1")) CHECK(!strcmp(verdict, "PASS"));
        }
    }
    free(golden);
    arena_destroy(&a);
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--write-golden")) {
        Arena a;
        Inputs in;
        size_t n;
        arena_init(&a, 1 << 20);
        if (!load_inputs(&a, &in)) { fputs("inputs did not load\n", stderr); return 1; }
        const char *t = report(&a, &in, false, &n);
        FILE *f = t ? fopen(argv[2], "wb") : NULL;
        if (!f) return 1;
        fwrite(t, 1, n, f);
        fclose(f);
        return 0;
    }
    test_writer();
    test_verdict();
    test_golden();
    if (failures) { fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
    puts("finding: all checks passed");
    return 0;
}

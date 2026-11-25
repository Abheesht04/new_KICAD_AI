/* place_intent (src/v2_src/place_intent.c): from a composed intent to the C placer's problem, and the placer's answer
 * back into the intent and routing.json.
 *
 * Inputs: the composed ESP32-S3 fixture (Copper-written compose file; the repository's blocks_library and
 * parts_library), KiCad footprints copied unchanged from KiCad 10.0.5 (tests/fixtures/compose/kicad) and
 * routing_profiles/copper_placement_default.json. The placer itself (placement.c) is in another library (G-044); its
 * answer here is a hand-written placer JSON in the documented format, so the transforms can be checked exactly. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "compose.h"
#include "place_intent.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROOT COPPER_TEST_SOURCE_DIR "/../../"
#define FIX COPPER_TEST_SOURCE_DIR "/tests/fixtures/compose/"

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

int main(void)
{
    arena_init(&arena, 1u << 25);
    /* compose the fixture */
    static PartCatalogue cats[5];
    static const PartCatalogue *refs[5];
    static const char *const files[5] = {"resistors", "capacitors", "semiconductors", "ics", "electromechanical"};
    for (size_t i = 0; i < 5; i++) {
        char path[512];
        snprintf(path, sizeof path, ROOT "parts_library/%s.parts", files[i]);
        PartCatalogueError e;
        assert(part_catalogue_parse_file(&arena, path, &cats[i], &e) == PART_CATALOGUE_OK);
        refs[i] = &cats[i];
    }
    ComposeInputs in = {ROOT "blocks_library", refs, 5, FIX "kicad/symbols", NULL};
    char *design = slurp(FIX "esp32s3_led_sensor.compose.json", NULL);
    ComposeReport cr;
    assert(compose_design(&arena, design, strlen(design), &in, &cr) == COMPOSE_COMPOSED);
    size_t rules_len = 0;
    char *rules = slurp(ROOT "routing_profiles/copper_placement_default.json", &rules_len);

    /* the placer's problem */
    const char *problem = NULL;
    size_t plen = 0;
    PlaceReport pr;
    PlaceStatus st = place_problem(&arena, cr.intent_json, cr.intent_len, cr.hints_json, cr.hints_len, rules, rules_len,
                                   FIX "kicad/footprints", &problem, &plen, &pr);
    if (st != PLACE_OK) fprintf(stderr, "place_problem: %s\n", pr.message);
    assert(st == PLACE_OK && pr.parts == 21 && pr.keepouts == 1 && pr.decoupling == 4);
    static const char head[] = "(placement_problem (version 1)\n  (board 70 60) (edge 1) (gap 0.5) (step 1)";
    assert(!strncmp(problem, head, sizeof head - 1));
    assert(strstr(problem, "(weights (placement_area_mm2 0.01) (decoupling_mm 1.0) (keepout_edge_mm 0.5))"));
    assert(occurrences(problem, "(part \"") == 21 && occurrences(problem, "(shape ") == 84);
    /* board edge strips: no bodies within 2.5 mm */
    assert(strstr(problem, "(keepout 0 0 70 2.5 8)") && strstr(problem, "(keepout 67.5 0 70 60 8)"));
    /* the module: courtyard -24..24 x -27.75..13.45 (KiCad), keep-out at each rotation; at 90 degrees (x, y) -> (y, -x) */
    assert(strstr(problem, "(shape 0 (body -24 -27.75 24 13.45)") && strstr(problem, "(shape 90 (body -27.75 -24 13.45 24)"));
    assert(strstr(problem, "(keepout -24 -27.75 24 -6.75 12)") && strstr(problem, "(keepout -27.75 -24 -6.75 24 12)"));
    assert(strstr(problem, "(decoupling \"C3\" ") && strstr(problem, "\"U2\")"));
    /* Step 3c: U3 (SOT-563, own pads 0.15 mm apart < fine_pitch_gap_mm 0.3) is the only fine-pitch part; its body is
       its courtyard (KiCad: -1.2..1.2 x -0.95..0.95) grown by fine_pitch_escape_mm (1.0) for its escape stubs */
    assert(pr.fine_pitch == 1);
    {
        const char *u3 = strstr(problem, "(part \"U3\"");
        assert(u3 && !strncmp(strstr(u3, "(shape 0 (body "), "(shape 0 (body -2.2 -1.95 2.2 1.95)", 35));
        assert(!strncmp(strstr(u3, "(shape 90 (body "), "(shape 90 (body -1.95 -2.2 1.95 2.2)", 36));
        const char *r6 = strstr(problem, "(part \"R6\"");   /* an 0603: not fine-pitch, its courtyard unchanged */
        assert(r6 && !strncmp(strstr(r6, "(shape 0 (body "), "(shape 0 (body -1.48 -0.73 1.48 0.73)", 37));
        /* without the two keys: no escape room; with only one of them: refused */
        char *plain = malloc(rules_len + 1), *half = malloc(rules_len + 1);
        assert(plain && half);
        memcpy(plain, rules, rules_len + 1);
        memcpy(half, rules, rules_len + 1);
        char *k1 = strstr(plain, "\"fine_pitch_gap_mm\""), *k2 = strstr(plain, "\"fine_pitch_escape_mm\"");
        char *h1 = strstr(half, "\"fine_pitch_escape_mm\"");
        assert(k1 && k2 && h1);
        memcpy(k1 + 1, "xxxx", 4); memcpy(k2 + 1, "xxxx", 4); memcpy(h1 + 1, "xxxx", 4);   /* rename the keys away */
        const char *p3 = NULL; size_t l3 = 0;
        assert(place_problem(&arena, cr.intent_json, cr.intent_len, cr.hints_json, cr.hints_len, plain, rules_len, FIX "kicad/footprints", &p3, &l3, &pr) == PLACE_OK);
        assert(pr.fine_pitch == 0);
        const char *u3p = strstr(p3, "(part \"U3\"");
        assert(u3p && !strncmp(strstr(u3p, "(shape 0 (body "), "(shape 0 (body -1.2 -0.95 1.2 0.95)", 35));
        assert(place_problem(&arena, cr.intent_json, cr.intent_len, cr.hints_json, cr.hints_len, half, rules_len, FIX "kicad/footprints", &p3, &l3, &pr) == PLACE_INVALID_INPUT);
        free(plain); free(half);
    }
    const char *problem2 = NULL;
    size_t plen2 = 0;
    assert(place_problem(&arena, cr.intent_json, cr.intent_len, cr.hints_json, cr.hints_len, rules, rules_len, FIX "kicad/footprints", &problem2, &plen2, &pr) == PLACE_OK);
    assert(plen == plen2 && !memcmp(problem, problem2, plen));   /* deterministic */

    /* the placer's answer back: every part placed; U2 rotated 90 degrees at (35, 30) */
    char answer[8192];
    size_t off = (size_t)snprintf(answer, sizeof answer, "{\"status\":\"OK\",\"constructive\":{\"placement\":{},\"failures\":[],\"cost_nm\":1},\"annealed\":["
                                  "{\"seed\":2,\"status\":\"OK\",\"cost_nm\":900,\"placement\":{");
    static const char *const prefixes[] = {"J1", "C1", "U1", "C2", "U2", "C3", "C4", "R1", "C5", "R2", "R3", "SW1", "C6", "SW2", "R4", "R5", "D1", "U3", "C7", "R6", "R7"};
    for (size_t i = 0; i < 21; i++)
        off += (size_t)snprintf(answer + off, sizeof answer - off, "%s\"%s\":[%s,%s,%d]", i ? "," : "", prefixes[i],
                                !strcmp(prefixes[i], "U2") ? "35" : "10.5", !strcmp(prefixes[i], "U2") ? "30" : "7.25", !strcmp(prefixes[i], "U2") ? 90 : 0);
    snprintf(answer + off, sizeof answer - off, "}},{\"seed\":1,\"status\":\"OK\",\"cost_nm\":1000,\"placement\":{}}]}");
    const char *intent2 = NULL, *routing = NULL;
    size_t ilen = 0, rlen = 0;
    st = place_apply(&arena, cr.intent_json, cr.intent_len, cr.hints_json, cr.hints_len, answer, strlen(answer), &intent2, &ilen, &routing, &rlen, &pr);
    if (st != PLACE_OK) fprintf(stderr, "place_apply: %s\n", pr.message);
    assert(st == PLACE_OK && pr.parts == 21 && !strcmp(pr.seed, "seed 2"));   /* the lowest cost wins */
    assert(occurrences(intent2, "\"position\"") == 21 && strstr(intent2, "\"x_mm\": 35,\n") && strstr(intent2, "\"rotation_deg\": 90"));
    assert(strstr(intent2, "\"x_mm\": 10.5") && strstr(intent2, "\"y_mm\": 7.25"));
    /* keep-out -24..24 x -27.75..-6.75 rotated 90 -> -27.75..-6.75 x -24..24, moved to (35, 30) -> 7.25..28.25 x 6..54 */
    assert(strstr(routing, "\"format\": \"copper.routing/1\"") && strstr(routing, "\"ref\": \"U2\""));
    assert(strstr(routing, "7.25,") && strstr(routing, "28.25,") && strstr(routing, "54\n") && strstr(routing, "\"B.Cu\""));

    /* a placer that could not place everything: NO_LEGAL_POSITION, naming the parts */
    const char *bad = "{\"status\":\"NO_LEGAL_POSITION\",\"constructive\":{\"placement\":{},\"failures\":[\"U2\",\"J1\"],\"cost_nm\":0},\"annealed\":[]}";
    assert(place_apply(&arena, cr.intent_json, cr.intent_len, cr.hints_json, cr.hints_len, bad, strlen(bad), &intent2, &ilen, &routing, &rlen, &pr) == PLACE_NO_LEGAL_POSITION);
    assert(strstr(pr.message, "U2, J1"));
    /* a missing footprint library */
    assert(place_problem(&arena, cr.intent_json, cr.intent_len, cr.hints_json, cr.hints_len, rules, rules_len, FIX "nowhere", &problem, &plen, &pr) == PLACE_INVALID_INPUT);

    free(design);
    free(rules);
    arena_destroy(&arena);
    puts("place_intent: all tests passed");
    return 0;
}

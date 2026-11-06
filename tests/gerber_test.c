/* The Gerber reader (src/v2_src/gerber.c, track A).
 *
 * Oracle: manufacturing_audit/gerber_test, real KiCad 9.0.6 Gerbers of a Copper divider variant (J1, R1, D1), and
 * manufacturing_audit/drill_test from the same export. Checks: every file parses with no error; Edge.Cuts closes
 * into one 40 x 30 mm loop; the RoundRect macro evaluates to the 0603 pad (0.8 x 0.95 mm); J1's copper flashes sit
 * exactly on KiCad's drill holes; and every flash of R1 and J1 (both also in examples/generated/divider) is where
 * the board model puts that pad, with that pad's size. Inline files cover every error and unsupported path. */
#include "board.h"
#include "excellon.h"
#include "gerber.h"
#include "kicad_cli.h"

#include <stdlib.h>

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)
#define DIR GERBER_ROOT "/manufacturing_audit/gerber_test/"

static const char *const FILES[] = {
    "design-B_Adhesive.gba", "design-B_Courtyard.gbr", "design-B_Cu.gbl", "design-B_Fab.gbr", "design-B_Mask.gbs",
    "design-B_Paste.gbp", "design-B_Silkscreen.gbo", "design-Edge_Cuts.gm1", "design-F_Adhesive.gta",
    "design-F_Courtyard.gbr", "design-F_Cu.gtl", "design-F_Fab.gbr", "design-F_Mask.gts", "design-F_Paste.gtp",
    "design-F_Silkscreen.gto", "design-Margin.gbr", "design-User_Drawings.gbr"};

static GerberStatus load(Arena *a, const char *name, GerberFile *g)
{
    char path[1024];
    snprintf(path, sizeof path, "%s%s", DIR, name);
    return gerber_parse_path(a, path, g);
}

static void test_all_real_files(void)
{
    Arena a;
    arena_init(&a, 1 << 20);
    size_t regions = 0;
    for (size_t i = 0; i < sizeof FILES / sizeof *FILES; i++) {
        GerberFile g;
        GerberStatus s = load(&a, FILES[i], &g);
        CHECK(s == GERBER_OK);
        CHECK(g.error_count == 0 && g.warning_count == 0 && g.saw_m02 && g.units == GERBER_UNITS_MM);
        CHECK(g.int_digits == 4 && g.dec_digits == 6 && g.arc_count == 0);
        if (s || g.issue_count) {
            fprintf(stderr, "%s: %s\n", FILES[i], gerber_status_name(s));
            for (size_t k = 0; k < g.issue_count && k < 5; k++)
                fprintf(stderr, "  line %zu %s %s\n", g.issues[k].line, gerber_issue_name(g.issues[k].code), g.issues[k].detail ? g.issues[k].detail : "");
        }
        const GerberAttribute *gen = gerber_file_attribute(&g, ".GenerationSoftware");
        CHECK(gen && gen->value_count == 3 && !strcmp(gen->values[0], "KiCad") && !strcmp(gen->values[2], "9.0.6"));
        for (size_t k = 0; k < g.object_count; k++) {
            const GerberObject *o = &g.objects[k];
            if (o->kind != GERBER_REGION) continue;
            regions++;
            CHECK(o->contour_count >= 4 && o->contour[0].x == o->contour[o->contour_count - 1].x &&
                  o->contour[0].y == o->contour[o->contour_count - 1].y);
        }
        for (size_t k = 0; k < g.aperture_count; k++) CHECK(g.apertures[k].exact && g.apertures[k].shape_count >= 1);
    }
    CHECK(regions == 10);
    arena_destroy(&a);
}

static void test_edge_cuts(void)
{
    Arena a;
    GerberFile g;
    GerberOutline o;
    arena_init(&a, 1 << 16);
    CHECK(load(&a, "design-Edge_Cuts.gm1", &g) == GERBER_OK);
    const GerberAttribute *fn = gerber_file_attribute(&g, ".FileFunction");
    CHECK(fn && fn->value_count == 2 && !strcmp(fn->values[0], "Profile") && !strcmp(fn->values[1], "NP"));
    CHECK(gerber_outline(&a, &g, &o) == GERBER_OK);
    CHECK(o.segments == 4 && o.loops == 1 && o.closed);
    CHECK(o.bounds.min.x == 0 && o.bounds.max.x == 40000000 && o.bounds.min.y == -30000000 && o.bounds.max.y == 0);
    CHECK(g.draw_count == 4 && g.objects[0].has_capsule && g.objects[0].capsule.radius == 25000);   /* 0.05 mm line */
    arena_destroy(&a);
}

static const GerberObject *flash(const GerberFile *g, const char *ref, const char *pin)
{
    for (size_t i = 0; i < g->object_count; i++) {
        const GerberObject *o = &g->objects[i];
        if (o->kind == GERBER_FLASH && o->pin_ref && !strcmp(o->pin_ref, ref) && !strcmp(o->pin_number, pin)) return o;
    }
    return NULL;
}

static void test_copper(void)
{
    Arena a;
    GerberFile g, back;
    ExcFile drl;
    Board b;
    BoardError be;
    arena_init(&a, 1 << 20);
    CHECK(load(&a, "design-F_Cu.gtl", &g) == GERBER_OK && load(&a, "design-B_Cu.gbl", &back) == GERBER_OK);
    CHECK(g.flash_count == 7 && back.flash_count == 3);
    /* The RoundRect macro (an outline, four circles, four vector lines) is the 0603 pad: 0.8 x 0.95 mm. */
    const GerberAperture *rr = gerber_find_aperture(&g, 10);
    CHECK(rr && rr->type == GERBER_AP_MACRO && !strcmp(rr->macro, "RoundRect") && rr->shape_count == 9 && rr->exact);
    CHECK(rr && rr->bounds.min.x == -400000 && rr->bounds.max.x == 400000 && rr->bounds.min.y == -475000 && rr->bounds.max.y == 475000);
    CHECK(rr && rr->attribute_count == 1 && !strcmp(rr->attributes[0].values[0], "SMDPad"));
    const GerberAperture *c13 = gerber_find_aperture(&g, 13);
    CHECK(c13 && c13->type == GERBER_AP_CIRCLE && c13->shapes[0].as.circle.radius == 850000);
    /* Object attributes. */
    const GerberObject *j1 = flash(&g, "J1", "1");
    CHECK(j1 && j1->net && !strcmp(j1->net, "/VIN") && j1->at.x == 8000000 && j1->at.y == -12000000);
    const GerberObject *r12 = flash(&g, "R1", "2");
    CHECK(r12 && r12->net && !strcmp(r12->net, "/OUT"));
    /* Tracks: 0.2 mm conductors as capsules, with their net. */
    size_t tracks = 0;
    for (size_t i = 0; i < g.object_count; i++)
        if (g.objects[i].kind == GERBER_DRAW) { tracks++; CHECK(g.objects[i].has_capsule && g.objects[i].capsule.radius == 100000 && g.objects[i].net); }
    CHECK(tracks > 0);
    /* J1's copper (front and back) is exactly where KiCad 9.0.6 drilled. */
    CHECK(exc_parse_path(&a, GERBER_ROOT "/manufacturing_audit/drill_test/design.drl", &drl) == EXC_OK && drl.hit_count == 3);
    for (size_t h = 0; h < drl.hit_count; h++) {
        bool front = false, rear = false;
        for (size_t i = 0; i < g.object_count; i++)
            front |= g.objects[i].kind == GERBER_FLASH && g.objects[i].pin_ref && !strcmp(g.objects[i].pin_ref, "J1") &&
                     g.objects[i].at.x == drl.hits[h].pos.x && g.objects[i].at.y == drl.hits[h].pos.y;
        for (size_t i = 0; i < back.object_count; i++)
            rear |= back.objects[i].kind == GERBER_FLASH && back.objects[i].at.x == drl.hits[h].pos.x && back.objects[i].at.y == drl.hits[h].pos.y;
        CHECK(front && rear);
    }
    /* R1 and J1 are also in the divider board: each flash is that pad, at (x, -y), with that pad's size. */
    CHECK(board_load(&a, GERBER_ROOT "/examples/generated/divider/design.kicad_pcb", &b, &be) == BOARD_OK);
    size_t compared = 0;
    for (size_t i = 0; i < g.object_count; i++) {
        const GerberObject *o = &g.objects[i];
        if (o->kind != GERBER_FLASH || !o->pin_ref || (strcmp(o->pin_ref, "R1") && strcmp(o->pin_ref, "J1"))) continue;
        const BoardPad *p = board_find_pad(&b, o->pin_ref, o->pin_number);
        const GerberAperture *ap = &g.apertures[o->aperture];
        CHECK(p && p->at.x == o->at.x && p->at.y == -o->at.y);
        CHECK(p && ap->bounds.max.x - ap->bounds.min.x == p->size_x && ap->bounds.max.y - ap->bounds.min.y == p->size_y);
        compared++;
    }
    CHECK(compared == 5);
    /* gerber_flash_shape moves a shape to the flash point. */
    G2Shape s;
    CHECK(j1 && gerber_flash_shape(&g, j1, 0, &s) == G2_OK && s.kind == G2_SHAPE_ROUNDED_RECT &&
          s.as.rounded_rect.core.min.x == 8000000 - 850000 && s.as.rounded_rect.core.max.y == -12000000 + 850000);
    arena_destroy(&a);
}

static GerberStatus parse(Arena *a, const char *t, GerberFile *g) { return gerber_parse(a, t, strlen(t), g); }

static bool has(const GerberFile *g, GerberIssueCode c, size_t line)
{
    for (size_t i = 0; i < g->issue_count; i++) if (g->issues[i].code == c && (!line || g->issues[i].line == line)) return true;
    return false;
}

#define HEAD "%FSLAX46Y46*%\n%MOMM*%\n%ADD10C,0.100000*%\n"

static void test_inline(void)
{
    Arena a;
    GerberFile g;
    arena_init(&a, 1 << 16);
    /* G02: recorded as an unsupported arc with its line, never drawn as a line. */
    CHECK(parse(&a, HEAD "D10*\nX0Y0D02*\nG02*\nX1000000Y1000000I1000000J0D01*\nM02*\n", &g) == GERBER_OK);
    CHECK(g.arc_count == 1 && g.draw_count == 0 && has(&g, GERBER_ISSUE_ARC_UNSUPPORTED, 7) && g.warning_count == 1);
    CHECK(g.objects[0].centre_offset.x == 1000000 && g.objects[0].end.y == 1000000);
    /* Polarity, modal coordinates, the combined G01 form and a bare D03. */
    CHECK(parse(&a, HEAD "D10*\n%LPC*%\nX1000000Y2000000D03*\nY3000000D03*\n%LPD*%\nG01X0Y0D02*\nX5000000D01*\nD03*\nM02*\n", &g) == GERBER_OK);
    CHECK(g.flash_count == 3 && !g.objects[0].dark && g.objects[1].at.x == 1000000 && g.objects[1].at.y == 3000000);
    CHECK(g.objects[2].kind == GERBER_DRAW && g.objects[2].dark && g.objects[2].end.x == 5000000 && g.objects[3].at.x == 5000000);
    /* Errors, each with its line. */
    CHECK(parse(&a, "%FSLIX46Y46*%\n%MOMM*%\nM02*\n", &g) == GERBER_CONTENT_ERROR && has(&g, GERBER_ISSUE_UNSUPPORTED_FORMAT, 1));
    CHECK(parse(&a, "%MOMM*%\nX0Y0D02*\nM02*\n", &g) == GERBER_CONTENT_ERROR && has(&g, GERBER_ISSUE_NO_FORMAT, 2));
    CHECK(parse(&a, HEAD "D11*\nX0Y0D03*\nM02*\n", &g) == GERBER_CONTENT_ERROR && has(&g, GERBER_ISSUE_UNDEFINED_APERTURE, 4));
    CHECK(parse(&a, HEAD "D10*\nG36*\nX0Y0D02*\nX1000000Y0D03*\nG37*\nM02*\n", &g) == GERBER_CONTENT_ERROR && has(&g, GERBER_ISSUE_D03_IN_REGION, 7));
    CHECK(parse(&a, HEAD "G36*\nX0Y0D02*\nX1000000Y0D01*\nX1000000Y1000000D01*\nG37*\nM02*\n", &g) == GERBER_CONTENT_ERROR &&
          has(&g, GERBER_ISSUE_REGION_NOT_CLOSED, 0));
    CHECK(parse(&a, HEAD "D10*\nX0Y0D03*\n", &g) == GERBER_OK && has(&g, GERBER_ISSUE_MISSING_M02, 0));
    CHECK(parse(&a, HEAD "%ADD12C,0.1*\nM02*\n", &g) == GERBER_CONTENT_ERROR && has(&g, GERBER_ISSUE_SYNTAX, 4));
    CHECK(parse(&a, HEAD "D10*\nX0Y0D03\n", &g) == GERBER_CONTENT_ERROR && has(&g, GERBER_ISSUE_SYNTAX, 0));
    CHECK(parse(&a, HEAD "%SRX2Y2I5.0J5.0*%\nM02*\n", &g) == GERBER_CONTENT_ERROR);
    CHECK(parse(&a, HEAD "%IPPOS*%\nM02*\n", &g) == GERBER_OK && has(&g, GERBER_ISSUE_UNSUPPORTED_COMMAND, 4));
    /* A closed region contour, as G2 polygon vertices. */
    CHECK(parse(&a, HEAD "G36*\nX0Y0D02*\nX1000000Y0D01*\nX1000000Y1000000D01*\nX0Y0D01*\nG37*\nM02*\n", &g) == GERBER_OK);
    CHECK(g.region_count == 1 && g.objects[0].contour_count == 4 && g.objects[0].aperture == SIZE_MAX);
    /* Inch: 0.001 in = 25400 nm exactly; 0.0000001 in is rounded (and said so). */
    CHECK(parse(&a, "%FSLAX26Y26*%\n%MOIN*%\n%ADD10C,0.010000*%\nD10*\nX1000Y0D03*\nM02*\n", &g) == GERBER_OK);
    CHECK(g.objects[0].at.x == 25400 && g.apertures[0].shapes[0].as.circle.radius == 127000 && g.warning_count == 0);
    CHECK(parse(&a, "%FSLAX27Y27*%\n%MOIN*%\n%ADD10C,0.010000*%\nD10*\nX1Y0D03*\nM02*\n", &g) == GERBER_OK && has(&g, GERBER_ISSUE_ROUNDED, 0));
    /* Macros: variables, $n=..., x and /, a centre line; a primitive 7 is reported. */
    CHECK(parse(&a, "%FSLAX46Y46*%\n%MOMM*%\n%AMBOX*\n$3=$1x2*\n21,1,$3,$2/2,0,0,0*%\n%ADD10BOX,0.5X1.0*%\nD10*\nX0Y0D03*\nM02*\n", &g) == GERBER_OK);
    CHECK(g.apertures[0].shape_count == 1 && g.apertures[0].exact && g.apertures[0].bounds.max.x == 500000 && g.apertures[0].bounds.max.y == 250000);
    CHECK(parse(&a, "%FSLAX46Y46*%\n%MOMM*%\n%AMT*\n7,0,0,1,0.8,0.1,0*%\n%ADD10T*%\nM02*\n", &g) == GERBER_OK && has(&g, GERBER_ISSUE_UNSUPPORTED_PRIMITIVE, 0));
    CHECK(!g.apertures[0].exact);
    CHECK(parse(&a, "%FSLAX46Y46*%\n%MOMM*%\n%AMBAD*\n1,1,$1+*%\n%ADD10BAD,1*%\nM02*\n", &g) == GERBER_CONTENT_ERROR && has(&g, GERBER_ISSUE_BAD_MACRO, 0));
    /* Polygon apertures: a square is exact, a hexagon is rounded and flagged. */
    CHECK(parse(&a, "%FSLAX46Y46*%\n%MOMM*%\n%ADD10P,2.0X4*%\n%ADD11P,2.0X6*%\nM02*\n", &g) == GERBER_OK);
    CHECK(g.apertures[0].exact && g.apertures[0].shapes[0].as.polygon.count == 4 && g.apertures[0].shapes[0].as.polygon.vertices[1].y == 1000000);
    CHECK(!g.apertures[1].exact && has(&g, GERBER_ISSUE_INEXACT_ROTATION, 0));
    /* KiCad's FreePoly macros: an outline wrapped across lines, with repeated vertices and a back-tracking spike;
     * a zero-size aperture draws nothing. */
    CHECK(parse(&a, "%FSLAX46Y46*%\n%MOMM*%\n%AMFP*\n4,1,6,0,0,1.0,0,\n1.0,0,1.0,1.0,1.0,0.5,\n1.0,1.0,0,0,$1*%\n"
                    "%ADD10FP,0.000000*%\n%ADD11C,0.000000*%\nD10*\nX0Y0D03*\nD11*\nX0Y0D03*\nM02*\n", &g) == GERBER_OK);
    /* Left: the triangle (0,0) (1,0) (1,1) with (1,0.5) kept as a point on its edge. */
    CHECK(g.apertures[0].shape_count == 1 && g.apertures[0].exact && g.apertures[0].shapes[0].as.polygon.count == 4);
    CHECK(g.apertures[0].bounds.max.x == 1000000 && g.apertures[0].bounds.max.y == 1000000);
    CHECK(g.apertures[1].shape_count == 0 && g.flash_count == 2 && g.warning_count == 0);
    /* Attributes: TO applies to later objects until TD removes it. */
    CHECK(parse(&a, HEAD "D10*\n%TO.N,GND*%\nX0Y0D03*\n%TD.N*%\nX1Y0D03*\nM02*\n", &g) == GERBER_OK);
    CHECK(g.objects[0].net && !strcmp(g.objects[0].net, "GND") && !g.objects[1].net);
    arena_destroy(&a);
}

/* Scratch cleanup for --real (ASCII build paths). */
#ifdef _WIN32
#include <windows.h>
static void rm_tree(const char *p)
{
    char pat[4096];
    snprintf(pat, sizeof pat, "%s\\*", p);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            char c[4096];
            snprintf(c, sizeof c, "%s\\%s", p, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) rm_tree(c); else DeleteFileA(c);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(p);
}
static void make_dir(const char *p) { CreateDirectoryA(p, NULL); }
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
static void rm_tree(const char *p)
{
    DIR *d = opendir(p);
    if (!d) { unlink(p); return; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char c[4096];
        struct stat st;
        snprintf(c, sizeof c, "%s/%s", p, e->d_name);
        if (!lstat(c, &st) && S_ISDIR(st.st_mode)) rm_tree(c); else unlink(c);
    }
    closedir(d);
    rmdir(p);
}
static void make_dir(const char *p) { mkdir(p, 0700); }
#endif

/* --real: KiCad's own Gerbers of the KiCad 9.0-saved demo boards (KICAD_DEMOS_DIR), exported through the track D
 * runner (KICAD_CLI) into a scratch folder, must all parse without an error. A KiCad 10 CLI makes this a development
 * check: the boards are KiCad 9 files, the Gerber writer is the CLI's. Exit 77 (UNVERIFIED) without both. */
static int test_real(void)
{
    static const char *const BOARDS[] = {"video/video.kicad_pcb", "interf_u/interf_u.kicad_pcb", "pic_programmer/pic_programmer.kicad_pcb",
        "tiny_tapeout/tinytapeout-demo.kicad_pcb", "kit-dev-coldfire-xilinx_5213/kit-dev-coldfire-xilinx_5213.kicad_pcb",
        "royalblue54L_feather/RoyalBlue54L-Feather.kicad_pcb", "ecc83/ecc83-pp.kicad_pcb", "sonde xilinx/sonde xilinx.kicad_pcb",
        "complex_hierarchy/complex_hierarchy.kicad_pcb", "multichannel/multichannel_mixer.kicad_pcb"};
    const char *cli_path = getenv("KICAD_CLI"), *demos = getenv("KICAD_DEMOS_DIR");
    if (!cli_path || !*cli_path || !demos || !*demos) { puts("gerber --real: UNVERIFIED (set KICAD_CLI and KICAD_DEMOS_DIR)"); return 77; }
    Arena a;
    KicadCli cli;
    KicadCliResult r;
    rm_tree(GERBER_SCRATCH);   /* the runner never overwrites: start from an empty scratch folder */
    make_dir(GERBER_SCRATCH);
    arena_init(&a, 1 << 20);
    if (kicad_cli_open(&a, cli_path, 600000, false, &cli, &r) != KCLI_OK) { fprintf(stderr, "kicad-cli: %s\n", r.message); return 1; }
    printf("gerber --real: Gerbers written by kicad-cli %s%s\n", cli.version, cli.major == 9 ? "" : " (development check)");
    size_t boards = 0, files = 0, objects = 0, arcs = 0;
    for (size_t i = 0; i < sizeof BOARDS / sizeof *BOARDS; i++) {
        char src[2048], copy[2048], out[2048];
        snprintf(src, sizeof src, "%s/%s", demos, BOARDS[i]);
        const char *base = strrchr(BOARDS[i], '/') + 1;
        snprintf(copy, sizeof copy, "%s/%s", GERBER_SCRATCH, base);
        snprintf(out, sizeof out, "%s/%s-gerbers", GERBER_SCRATCH, base);
        FILE *in = fopen(src, "rb");
        if (!in) continue;                          /* not in this KiCad install */
        FILE *o = fopen(copy, "wb");                /* a copy: kicad-cli writes .kicad_prl next to the board */
        CHECK(o != NULL);
        if (!o) { fclose(in); continue; }
        char buf[1 << 16];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, in))) fwrite(buf, 1, n, o);
        fclose(in);
        fclose(o);
        KicadCliStatus s = kicad_cli_gerbers(&a, &cli, copy, out, &r);
        if (s == KCLI_EXISTS) { fprintf(stderr, "%s: delete %s from an earlier run first\n", base, out); }
        CHECK(s == KCLI_OK);
        if (s) { fprintf(stderr, "%s: %s\n", base, kicad_cli_status_name(s)); continue; }
        boards++;
        for (size_t k = 0; k < r.file_count; k++) {
            if (strstr(r.files[k], ".gbrjob")) continue;
            char path[4096];
            snprintf(path, sizeof path, "%s/%s", out, r.files[k]);
            GerberFile g;
            GerberStatus gs = gerber_parse_path(&a, path, &g);
            CHECK(gs == GERBER_OK && g.error_count == 0);
            if (gs != GERBER_OK)
                for (size_t q = 0; q < g.issue_count; q++)
                    if (g.issues[q].severity == GERBER_ERROR) { fprintf(stderr, "%s line %zu %s\n", r.files[k], g.issues[q].line, gerber_issue_name(g.issues[q].code)); break; }
            for (size_t q = 0; q < g.issue_count; q++) CHECK(g.issues[q].code == GERBER_ISSUE_ARC_UNSUPPORTED);
            files++;
            objects += g.object_count;
            arcs += g.arc_count;
        }
    }
    printf("  %zu boards, %zu Gerber files, %zu objects read with no error (%zu G02/G03 arcs reported as unsupported)\n", boards, files, objects, arcs);
    CHECK(boards > 0);
    arena_destroy(&a);
    return failures ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--real")) {
        int rc = test_real();
        if (rc == 1) fprintf(stderr, "%d check(s) failed\n", failures);
        return rc;
    }
    test_all_real_files();
    test_edge_cuts();
    test_copper();
    test_inline();
    if (failures) { fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
    puts("gerber: all checks passed");
    return 0;
}

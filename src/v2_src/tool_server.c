/* tool_server.c - JSON-RPC 2.0 tool server (see tool_server.h). */
#include "tool_server.h"

#include "dfm.h"
#include "excellon.h"
#include "filesystem.h"
#include "finding.h"
#include "gerber.h"
#include "json.h"
#include "kicad_cli.h"

#include <stdio.h>
#include <string.h>

enum {
    RPC_PARSE_ERROR = -32700, RPC_INVALID_REQUEST = -32600, RPC_METHOD_NOT_FOUND = -32601, RPC_INVALID_PARAMS = -32602,
    TOOL_INPUT_NOT_FOUND = -32001, TOOL_INPUT_INVALID = -32002, TOOL_UNKNOWN_BOARD = -32003,
    TOOL_KICAD_UNAVAILABLE = -32010, TOOL_KICAD_FAILED = -32011, TOOL_OUTPUT_EXISTS = -32012
};

typedef struct Call {
    ToolServer *s;
    Arena *a;                    /* scratch */
    const JsonValue *args;
    int code;                    /* 0: no error */
    const char *kind, *message, *hint, *file, *key;
    size_t line, item;
    char detail[1024];
} Call;

static bool fail(Call *c, int code, const char *kind, const char *message, const char *hint)
{
    c->code = code; c->kind = kind; c->message = message; c->hint = hint;
    return false;
}

static char *dup(Arena *a, const char *s)
{
    size_t n = strlen(s);
    char *d = arena_alloc(a, n + 1);
    if (d) memcpy(d, s, n + 1);
    return d;
}

static const char *str_arg(const Call *c, const char *key)
{
    const JsonValue *v = json_get(c->args, key);
    return v && v->kind == JSON_STRING && v->text_len ? v->text : NULL;
}

static const char *resolve(Call *c, const char *p)
{
    if (!c->s->config.root || !*c->s->config.root || fs_path_is_absolute(p)) return p;
    size_t nr = strlen(c->s->config.root), np = strlen(p);
    char *d = arena_alloc(c->a, nr + np + 2);
    if (!d) return p;
    memcpy(d, c->s->config.root, nr);
    d[nr] = '/';
    memcpy(d + nr + 1, p, np + 1);
    return d;
}

/* ------------------------------------------------------------------------------------------------ inputs */

static bool load_board(Call *c, Arena *into, const char *path, Board *out)
{
    BoardError e = {0};
    BoardStatus s = board_load(into, resolve(c, path), out, &e);
    if (s == BOARD_OK) return true;
    c->file = path; c->line = e.line; c->item = e.item;
    if (e.context) snprintf(c->detail, sizeof c->detail, "%s", e.context);
    if (s == BOARD_IO_ERROR) return fail(c, TOOL_INPUT_NOT_FOUND, "BOARD_NOT_FOUND", "the board file cannot be read",
                                         "check the path; relative paths resolve against the server root");
    return fail(c, TOOL_INPUT_INVALID, board_status_name(s), e.message ? e.message : "the board is not valid",
                "the board must be a KiCad .kicad_pcb (format 20241229 or later); see line/item");
}

/* The board named by board_id, or loaded from `path_key`. */
static bool get_board(Call *c, const char *path_key, const Board **b, const char **label)
{
    const char *id = str_arg(c, "board_id"), *path = str_arg(c, path_key);
    if (id) {
        for (size_t i = 0; i < c->s->board_count; i++)
            if (!strcmp(c->s->boards[i].id, id)) { *b = &c->s->boards[i].board; *label = c->s->boards[i].path; return true; }
        snprintf(c->detail, sizeof c->detail, "%s", id);
        return fail(c, TOOL_UNKNOWN_BOARD, "UNKNOWN_BOARD_ID", "no board was loaded with this board_id", "call board.load first and use the board_id it returns");
    }
    if (!path) {
        snprintf(c->detail, sizeof c->detail, "%s", path_key);
        return fail(c, RPC_INVALID_PARAMS, "MISSING_BOARD", "give board_id (from board.load) or a board path", "e.g. {\"path\": \"board.kicad_pcb\"}");
    }
    Board *nb = arena_alloc(c->a, sizeof *nb);
    if (!nb || !load_board(c, c->a, path, nb)) return nb ? false : fail(c, TOOL_INPUT_INVALID, "OUT_OF_MEMORY", "out of memory", "");
    *b = nb;
    *label = path;
    return true;
}

/* ------------------------------------------------------------------------------------------------ CAM compare */

typedef struct Hole { G2Point at; G2Nm d; const char *what; bool used; } Hole;

static const char *pad_name(Arena *a, const Board *b, size_t i)
{
    char buf[256];
    const BoardPad *p = &b->pads[i];
    const char *ref = b->footprints[p->footprint].reference;
    snprintf(buf, sizeof buf, "pad %s.%s", ref && *ref ? ref : "?", p->number && *p->number ? p->number : "-");
    return dup(a, buf);
}

static bool add_cam(FindingList *l, const char *label, const char *rule, FindingStatus st, const char *msg,
                    bool has_at, G2Point at, const char *item, bool has_measured, G2Nm measured, G2Nm limit)
{
    Finding f;
    memset(&f, 0, sizeof f);
    f.source = "cam"; f.rule = rule; f.status = st; f.file = label; f.message = msg;
    f.severity = st == FINDING_FAIL ? FINDING_SEV_ERROR : FINDING_SEV_WARNING;
    f.has_point = has_at; f.at = at; f.items[0] = item;
    f.has_measured = has_measured; f.measured_nm = measured;
    f.has_limit = has_measured; f.limit_nm = limit;
    return finding_add(l, &f);
}

/* Every drill hit must be a board hole (Excellon y = -board y under KiCad's absolute origin): position within
 * `tol`, diameter within `tol`; every round board hole must be drilled. Slots and blind/micro vias are counted, not
 * compared. */
static void cam_compare(Arena *a, FindingList *l, const char *label, const Board *b, Hole *hits, size_t nh, G2Nm tol)
{
    size_t nb = 0, slots = 0, blind = 0;
    Hole *holes = arena_alloc_array(a, sizeof *holes, b->pad_count + b->via_count + 1);
    if (!holes) { l->failed = true; return; }
    for (size_t i = 0; i < b->pad_count; i++) {
        const BoardPad *p = &b->pads[i];
        if (!p->has_drill) continue;
        if (p->drill_x != p->drill_y) { slots++; continue; }
        holes[nb++] = (Hole){{p->at.x, -p->at.y}, p->drill_x, pad_name(a, b, i), false};
    }
    for (size_t i = 0; i < b->via_count; i++) {
        const BoardVia *v = &b->vias[i];
        if (v->type != BOARD_VIA_THROUGH) { blind++; continue; }
        char buf[64];
        snprintf(buf, sizeof buf, "via %zu", i);
        holes[nb++] = (Hole){{v->at.x, -v->at.y}, v->drill, dup(a, buf), false};
    }
    for (size_t h = 0; h < nh; h++) {
        size_t at = SIZE_MAX, near = SIZE_MAX;
        for (size_t k = 0; k < nb && at == SIZE_MAX; k++) {
            if (holes[k].used) continue;
            G2Nm dx = hits[h].at.x - holes[k].at.x, dy = hits[h].at.y - holes[k].at.y;
            if (dx < -tol || dx > tol || dy < -tol || dy > tol) continue;
            G2Nm dd = hits[h].d - holes[k].d;
            if (dd >= -tol && dd <= tol) at = k;
            else if (near == SIZE_MAX) near = k;
        }
        if (at != SIZE_MAX) { holes[at].used = true; continue; }
        if (near != SIZE_MAX) {
            holes[near].used = true;
            add_cam(l, label, "cam/diameter_mismatch", FINDING_FAIL, "drilled with a different diameter than the board hole",
                    true, hits[h].at, holes[near].what, true, hits[h].d, holes[near].d);
        } else {
            add_cam(l, label, "cam/hole_extra", FINDING_FAIL, "a drilled hole that is not on the board", true, hits[h].at, NULL, true, hits[h].d, 0);
        }
    }
    for (size_t k = 0; k < nb; k++)
        if (!holes[k].used)
            add_cam(l, label, "cam/hole_missing", FINDING_FAIL, "a board hole that no drill file drills", true, holes[k].at, holes[k].what, false, 0, 0);
    char msg[160];
    if (slots) {
        snprintf(msg, sizeof msg, "%zu slotted pad holes are not compared (routed slots are not read yet)", slots);
        add_cam(l, label, "cam/slots_not_compared", FINDING_UNVERIFIED, msg, false, (G2Point){0, 0}, NULL, false, 0, 0);
    }
    if (blind) {
        snprintf(msg, sizeof msg, "%zu blind or micro vias are not compared (layer-pair drill files are not read)", blind);
        add_cam(l, label, "cam/blind_vias_not_compared", FINDING_UNVERIFIED, msg, false, (G2Point){0, 0}, NULL, false, 0, 0);
    }
}

static size_t collect_hits(Arena *a, const ExcFile *x, Hole **out, size_t n)
{
    Hole *more = arena_alloc_array(a, sizeof *more, n + x->hit_count + 1);
    if (!more) return n;
    if (n) memcpy(more, *out, n * sizeof *more);
    for (size_t i = 0; i < x->hit_count; i++)
        more[n++] = (Hole){x->hits[i].pos, x->tools[x->hits[i].tool].diameter, NULL, false};
    *out = more;
    return n;
}

/* ------------------------------------------------------------------------------------------------ tools */

static void write_outline(JsonWriter *w, const Board *b)
{
    json_write_begin_object(w);
    json_write_key(w, "closed"); json_write_bool(w, b->outline.closed);
    json_write_key(w, "loops"); json_write_uint(w, b->outline.loop_count);
    json_write_key(w, "max_gap_nm"); json_write_int(w, b->outline.max_gap);
    if (b->outline.has_bounds) {
        json_write_key(w, "bounds_nm");
        json_write_begin_array(w);
        json_write_int(w, b->outline.bounds.min.x); json_write_int(w, b->outline.bounds.min.y);
        json_write_int(w, b->outline.bounds.max.x); json_write_int(w, b->outline.bounds.max.y);
        json_write_end_array(w);
    }
    json_write_end_object(w);
}

static bool tool_board_load(Call *c, JsonWriter *w)
{
    const char *path = str_arg(c, "path");
    if (!path) return fail(c, RPC_INVALID_PARAMS, "MISSING_PATH", "board.load needs {\"path\": ...}", "a .kicad_pcb path, relative to the server root");
    ToolServer *s = c->s;
    if (s->board_count == s->board_cap) {
        size_t cap = s->board_cap ? s->board_cap * 2 : 8;
        ToolBoard *nb = arena_alloc_array(&s->session, sizeof *nb, cap);
        if (!nb) return fail(c, TOOL_INPUT_INVALID, "OUT_OF_MEMORY", "out of memory", "");
        if (s->board_count) memcpy(nb, s->boards, s->board_count * sizeof *nb);
        s->boards = nb;
        s->board_cap = cap;
    }
    ToolBoard *tb = &s->boards[s->board_count];
    if (!load_board(c, &s->session, path, &tb->board)) return false;
    char id[32];
    snprintf(id, sizeof id, "board-%zu", s->board_count + 1);
    tb->id = dup(&s->session, id);
    tb->path = dup(&s->session, path);
    s->board_count++;
    const Board *b = &tb->board;
    json_write_begin_object(w);
    json_write_key(w, "board_id"); json_write_string(w, tb->id);
    json_write_key(w, "path"); json_write_string(w, path);
    json_write_key(w, "version"); json_write_int(w, b->version);
    json_write_key(w, "generator"); json_write_string(w, b->generator);
    json_write_key(w, "copper_layers"); json_write_uint(w, b->copper_layers);
    json_write_key(w, "nets"); json_write_uint(w, b->net_count);
    json_write_key(w, "footprints"); json_write_uint(w, b->footprint_count);
    json_write_key(w, "pads"); json_write_uint(w, b->pad_count);
    json_write_key(w, "tracks"); json_write_uint(w, b->track_count);
    json_write_key(w, "vias"); json_write_uint(w, b->via_count);
    json_write_key(w, "zones"); json_write_uint(w, b->zone_count);
    json_write_key(w, "outline"); write_outline(w, b);
    FindingList l;
    finding_list_init(&l, c->a);
    finding_from_board(&l, path, b);
    json_write_key(w, "report"); finding_report_write(&l, w);
    json_write_end_object(w);
    return true;
}

static bool load_profile(Call *c, DfmProfile *p)
{
    const char *path = str_arg(c, "profile");
    if (!path) return fail(c, RPC_INVALID_PARAMS, "MISSING_PROFILE", "dfm.check needs {\"profile\": ...}", "a fab profile such as fab_profiles/jlcpcb_2layer.json");
    DfmError e = {0};
    DfmStatus s = dfm_profile_load(c->a, resolve(c, path), p, &e);
    if (s == DFM_OK) return true;
    c->file = path; c->line = e.line; c->key = e.key;
    if (s == DFM_IO_ERROR) return fail(c, TOOL_INPUT_NOT_FOUND, "PROFILE_NOT_FOUND", "the profile cannot be read", "check the path; relative paths resolve against the server root");
    return fail(c, TOOL_INPUT_INVALID, s == DFM_PARSE_ERROR ? "PROFILE_NOT_JSON" : "PROFILE_INVALID", e.message ? e.message : "invalid profile",
                "every limit must be present as {value, row, quote}, with a cited source and date; see fab_profiles/jlcpcb_2layer.json");
}

static bool tool_dfm_check(Call *c, JsonWriter *w)
{
    const Board *b;
    const char *label;
    DfmProfile p;
    DfmReport r;
    DfmError e = {0};
    if (!get_board(c, "path", &b, &label) || !load_profile(c, &p)) return false;
    if (dfm_check(c->a, b, &p, &r, &e) != DFM_OK) return fail(c, TOOL_INPUT_INVALID, "DFM_FAILED", e.message ? e.message : "DFM could not run", "");
    FindingList l;
    finding_list_init(&l, c->a);
    finding_from_dfm(&l, label, b, &p, &r);
    json_write_begin_object(w);
    json_write_key(w, "board"); json_write_string(w, label);
    json_write_key(w, "profile");
    json_write_begin_object(w);
    json_write_key(w, "name"); json_write_string(w, p.name);
    json_write_key(w, "fabricator"); json_write_string(w, p.fabricator);
    json_write_key(w, "source"); json_write_string(w, p.source_url);
    json_write_key(w, "retrieved"); json_write_string(w, p.retrieved);
    json_write_end_object(w);
    json_write_key(w, "report"); finding_report_write(&l, w);
    json_write_end_object(w);
    return true;
}

static G2Nm tolerance(Call *c, bool *ok)
{
    const JsonValue *v = json_get(c->args, "tolerance_nm");
    int64_t t = 500;   /* half of KiCad's 1 um Excellon resolution */
    *ok = true;
    if (v && (json_to_i64(v, &t) != JSON_NUMBER_OK || t < 0 || t > 1000000)) *ok = false;
    return t;
}

static bool tool_excellon_check(Call *c, JsonWriter *w)
{
    const char *path = str_arg(c, "path");
    if (!path) return fail(c, RPC_INVALID_PARAMS, "MISSING_PATH", "excellon.check needs {\"path\": ...}", "an Excellon .drl file");
    bool ok;
    G2Nm tol = tolerance(c, &ok);
    if (!ok) return fail(c, RPC_INVALID_PARAMS, "BAD_TOLERANCE", "tolerance_nm is an integer from 0 to 1000000", "");
    ExcFile *x = arena_alloc(c->a, sizeof *x);
    if (!x) return fail(c, TOOL_INPUT_INVALID, "OUT_OF_MEMORY", "out of memory", "");
    ExcStatus es = exc_parse_path(c->a, resolve(c, path), x);
    c->file = path;
    if (es == EXC_ERR_IO) return fail(c, TOOL_INPUT_NOT_FOUND, "DRILL_NOT_FOUND", "the drill file cannot be read", "check the path; relative paths resolve against the server root");
    if (es != EXC_OK && es != EXC_ERR_CONTENT) return fail(c, TOOL_INPUT_INVALID, exc_status_name(es), "the drill file could not be read", "");
    c->file = NULL;
    const Board *b = NULL;
    const char *blabel = NULL;
    bool with_board = str_arg(c, "board_id") || str_arg(c, "board");
    if (with_board && !get_board(c, "board", &b, &blabel)) return false;
    FindingList l;
    finding_list_init(&l, c->a);
    finding_from_excellon(&l, path, x);
    if (b) {
        Hole *hits = NULL;
        size_t n = collect_hits(c->a, x, &hits, 0);
        cam_compare(c->a, &l, path, b, hits, n, tol);
    }
    json_write_begin_object(w);
    json_write_key(w, "path"); json_write_string(w, path);
    json_write_key(w, "units"); json_write_string(w, x->units == EXC_UNITS_METRIC ? "mm" : x->units == EXC_UNITS_INCH ? "inch" : "unknown");
    json_write_key(w, "tools"); json_write_uint(w, x->tool_count);
    json_write_key(w, "holes"); json_write_uint(w, x->hit_count);
    if (b) { json_write_key(w, "board"); json_write_string(w, blabel); json_write_key(w, "tolerance_nm"); json_write_int(w, tol); }
    json_write_key(w, "report"); finding_report_write(&l, w);
    json_write_end_object(w);
    return true;
}

static bool kicad_failed(Call *c, const KicadCliResult *r, const char *job)
{
    size_t n = r->stderr_text ? strlen(r->stderr_text) : 0;
    snprintf(c->detail, sizeof c->detail, "%s: %s, exit %d%s%.600s", job, kicad_cli_status_name(r->status), r->exit_code,
             n ? ": " : "", n ? r->stderr_text + (n > 600 ? n - 600 : 0) : "");
    if (r->status == KCLI_EXISTS) return fail(c, TOOL_OUTPUT_EXISTS, "OUTPUT_EXISTS", "the output already exists", "choose a new folder: nothing is ever overwritten");
    return fail(c, TOOL_KICAD_FAILED, kicad_cli_status_name(r->status), r->message ? r->message : "kicad-cli failed", "see detail for kicad-cli's stderr");
}

static bool tool_export_fab(Call *c, JsonWriter *w)
{
    const char *out = str_arg(c, "out");
    if (!out) return fail(c, RPC_INVALID_PARAMS, "MISSING_OUT", "export.fab needs {\"out\": ...}, a folder that does not exist", "");
    const Board *b;
    const char *label;
    if (!get_board(c, "path", &b, &label)) return false;
    const char *board_path = resolve(c, label);
    const char *dir = resolve(c, out);
    if (fs_file_exists(dir) || fs_directory_exists(dir)) {
        c->file = out;
        return fail(c, TOOL_OUTPUT_EXISTS, "OUTPUT_EXISTS", "the output folder already exists", "choose a new folder: nothing is ever overwritten");
    }
    KicadCli cli;
    KicadCliResult r;
    uint32_t timeout = c->s->config.kicad_timeout_ms ? c->s->config.kicad_timeout_ms : 600000u;
    KicadCliStatus ks = kicad_cli_open(c->a, c->s->config.kicad_cli, timeout, false, &cli, &r);
    if (ks != KCLI_OK) {
        snprintf(c->detail, sizeof c->detail, "%s", kicad_cli_status_name(ks));
        return fail(c, TOOL_KICAD_UNAVAILABLE, ks == KCLI_NOT_FOUND ? "KICAD_CLI_NOT_FOUND" : "KICAD_CLI_UNUSABLE",
                    r.message ? r.message : "kicad-cli is not usable", "install KiCad 9.x or set KICAD_CLI_PATH (or start the server with --kicad-cli)");
    }
    if (fs_create_directory(dir) != FS_OK) { c->file = out; return fail(c, TOOL_OUTPUT_EXISTS, "OUTPUT_NOT_CREATED", "the output folder cannot be created", "its parent must exist and be writable"); }
    size_t nd = strlen(dir);
    char *gdir = arena_alloc(c->a, nd + 10), *ddir = arena_alloc(c->a, nd + 10);
    if (!gdir || !ddir) return fail(c, TOOL_INPUT_INVALID, "OUT_OF_MEMORY", "out of memory", "");
    snprintf(gdir, nd + 10, "%s/gerbers", dir);
    snprintf(ddir, nd + 10, "%s/drill", dir);
    KicadCliResult gr, dr;
    if (kicad_cli_gerbers(c->a, &cli, board_path, gdir, &gr) != KCLI_OK) return kicad_failed(c, &gr, "gerbers");
    if (kicad_cli_drill(c->a, &cli, board_path, ddir, &dr) != KCLI_OK) return kicad_failed(c, &dr, "drill");

    FindingList l;
    finding_list_init(&l, c->a);
    Hole *hits = NULL;
    size_t nh = 0;
    for (size_t i = 0; i < gr.file_count; i++) {
        const char *ext = strrchr(gr.files[i], '.');   /* KiCad's Gerber extensions all start with .g; skip the job file */
        if (!ext || ext[1] != 'g' || !strcmp(ext, ".gbrjob")) continue;
        char path[4096], lab[512];
        snprintf(path, sizeof path, "%s/%s", gdir, gr.files[i]);
        snprintf(lab, sizeof lab, "gerbers/%s", gr.files[i]);
        GerberFile g;
        GerberStatus gs = gerber_parse_path(c->a, path, &g);
        if (gs == GERBER_IO_ERROR || gs == GERBER_OUT_OF_MEMORY) {
            Finding f;
            memset(&f, 0, sizeof f);
            f.source = "gerber"; f.rule = "gerber/unreadable"; f.status = FINDING_INVALID_INPUT; f.severity = FINDING_SEV_ERROR;
            f.file = lab; f.message = "the exported Gerber file cannot be read back";
            finding_add(&l, &f);
            continue;
        }
        finding_from_gerber(&l, lab, &g);
    }
    for (size_t i = 0; i < dr.file_count; i++) {
        if (!strstr(dr.files[i], ".drl")) continue;
        char path[4096], lab[512];
        snprintf(path, sizeof path, "%s/%s", ddir, dr.files[i]);
        snprintf(lab, sizeof lab, "drill/%s", dr.files[i]);
        ExcFile *x = arena_alloc(c->a, sizeof *x);
        if (!x) return fail(c, TOOL_INPUT_INVALID, "OUT_OF_MEMORY", "out of memory", "");
        ExcStatus es = exc_parse_path(c->a, path, x);
        if (es != EXC_OK && es != EXC_ERR_CONTENT) continue;
        finding_from_excellon(&l, lab, x);
        nh = collect_hits(c->a, x, &hits, nh);
    }
    cam_compare(c->a, &l, "drill", b, hits, nh, 500);

    json_write_begin_object(w);
    json_write_key(w, "out"); json_write_string(w, out);
    json_write_key(w, "board"); json_write_string(w, label);
    json_write_key(w, "kicad_cli");
    json_write_begin_object(w);
    json_write_key(w, "version"); json_write_string(w, cli.version);
    json_write_key(w, "kicad9"); json_write_bool(w, cli.major == 9);
    json_write_end_object(w);
    json_write_key(w, "gerbers"); json_write_begin_array(w);
    for (size_t i = 0; i < gr.file_count; i++) json_write_string(w, gr.files[i]);
    json_write_end_array(w);
    json_write_key(w, "drill"); json_write_begin_array(w);
    for (size_t i = 0; i < dr.file_count; i++) json_write_string(w, dr.files[i]);
    json_write_end_array(w);
    json_write_key(w, "report"); finding_report_write(&l, w);
    json_write_end_object(w);
    return true;
}

/* ------------------------------------------------------------------------------------------------ catalogue */

typedef bool (*ToolFn)(Call *, JsonWriter *);
typedef struct ToolDef { const char *name, *description; ToolFn fn; const char *schema; } ToolDef;

static const ToolDef TOOLS[] = {
    {"board.load", "Load a KiCad .kicad_pcb into the exact board model; returns a board_id, counts, the outline and model issues.",
     tool_board_load, "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"],\"additionalProperties\":false}"},
    {"dfm.check", "DFM of a board against a fab profile (fab_profiles/*.json): a sorted findings report with the fabricator's wording.",
     tool_dfm_check, "{\"type\":\"object\",\"properties\":{\"board_id\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"profile\":{\"type\":\"string\"}},\"required\":[\"profile\"],\"additionalProperties\":false}"},
    {"export.fab", "Export Gerbers and drill files with kicad-cli into a new folder (never over anything), then read them back and compare the holes with the board.",
     tool_export_fab, "{\"type\":\"object\",\"properties\":{\"board_id\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"out\":{\"type\":\"string\"}},\"required\":[\"out\"],\"additionalProperties\":false}"},
    {"excellon.check", "Read an Excellon drill file; with a board, every hole must match a board hole (position within tolerance_nm, default 500, and diameter).",
     tool_excellon_check, "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"board_id\":{\"type\":\"string\"},\"board\":{\"type\":\"string\"},\"tolerance_nm\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":1000000}},\"required\":[\"path\"],\"additionalProperties\":false}"},
};
#define TOOL_COUNT (sizeof TOOLS / sizeof *TOOLS)

static const ToolDef *find_tool(const char *name)
{
    for (size_t i = 0; i < TOOL_COUNT; i++) if (!strcmp(TOOLS[i].name, name)) return &TOOLS[i];
    return NULL;
}

/* Arguments are checked against the schema's property names: an unknown argument is an error, never ignored. */
static bool known_args(Call *c, const ToolDef *t)
{
    if (!c->args) return true;
    for (size_t i = 0; i < c->args->count; i++) {
        char quoted[96];
        snprintf(quoted, sizeof quoted, "\"%s\":", c->args->keys[i]);
        if (!strstr(t->schema, quoted)) {
            snprintf(c->detail, sizeof c->detail, "%s", c->args->keys[i]);
            return fail(c, RPC_INVALID_PARAMS, "UNKNOWN_ARGUMENT", "an argument this tool does not take", "see tools/list for each tool's inputSchema");
        }
    }
    return true;
}

static void write_tools(JsonWriter *w)
{
    json_write_begin_object(w);
    json_write_key(w, "tools");
    json_write_begin_array(w);
    for (size_t i = 0; i < TOOL_COUNT; i++) {
        json_write_begin_object(w);
        json_write_key(w, "name"); json_write_string(w, TOOLS[i].name);
        json_write_key(w, "description"); json_write_string(w, TOOLS[i].description);
        json_write_key(w, "inputSchema"); json_write_raw(w, TOOLS[i].schema, strlen(TOOLS[i].schema));
        json_write_end_object(w);
    }
    json_write_end_array(w);
    json_write_end_object(w);
}

/* ------------------------------------------------------------------------------------------------ dispatch */

void tool_server_init(ToolServer *s, const ToolServerConfig *config)
{
    memset(s, 0, sizeof *s);
    if (config) s->config = *config;
    arena_init(&s->session, 1 << 20);
    arena_init(&s->scratch, 1 << 20);
}

void tool_server_destroy(ToolServer *s)
{
    arena_destroy(&s->session);
    arena_destroy(&s->scratch);
    memset(s, 0, sizeof *s);
}

static void log_line(ToolServer *s, const char *dir, const char *json, size_t len, bool raw)
{
    if (!s->config.log) return;
    JsonWriter w;
    json_writer_init_compact(&w, &s->scratch);
    json_write_begin_object(&w);
    json_write_key(&w, "seq"); json_write_uint(&w, s->seq);
    json_write_key(&w, dir);
    if (raw) json_write_raw(&w, json, len);
    else {
        char *t = arena_alloc(&s->scratch, len + 1);
        if (t) { memcpy(t, json, len); t[len] = 0; }
        json_write_string(&w, t ? t : "");
    }
    json_write_end_object(&w);
    size_t n;
    const char *text = json_writer_finish(&w, &n);
    if (text) s->config.log(s->config.log_ctx, text, n);
}

static void write_id(JsonWriter *w, const JsonValue *id)
{
    if (!id || id->kind == JSON_NULL) json_write_null(w);
    else if (id->kind == JSON_NUMBER) json_write_raw(w, id->text, id->text_len);
    else json_write_string(w, id->text);
}

static const char *error_response(Call *c, const JsonValue *id, size_t *len)
{
    JsonWriter w;
    json_writer_init_compact(&w, c->a);
    json_write_begin_object(&w);
    json_write_key(&w, "jsonrpc"); json_write_string(&w, "2.0");
    json_write_key(&w, "id"); write_id(&w, id);
    json_write_key(&w, "error");
    json_write_begin_object(&w);
    json_write_key(&w, "code"); json_write_int(&w, c->code);
    json_write_key(&w, "message"); json_write_string(&w, c->message);
    json_write_key(&w, "data");
    json_write_begin_object(&w);
    json_write_key(&w, "kind"); json_write_string(&w, c->kind);
    if (c->hint && *c->hint) { json_write_key(&w, "hint"); json_write_string(&w, c->hint); }
    if (c->file) { json_write_key(&w, "file"); json_write_string(&w, c->file); }
    if (c->line) { json_write_key(&w, "line"); json_write_uint(&w, c->line); }
    if (c->item) { json_write_key(&w, "item"); json_write_uint(&w, c->item); }
    if (c->key) { json_write_key(&w, "key"); json_write_string(&w, c->key); }
    if (c->detail[0]) { json_write_key(&w, "detail"); json_write_string(&w, c->detail); }
    json_write_end_object(&w);
    json_write_end_object(&w);
    json_write_end_object(&w);
    return json_writer_finish(&w, len);
}

static const char *result_response(Call *c, const JsonValue *id, const char *result, size_t rlen, size_t *len)
{
    JsonWriter w;
    json_writer_init_compact(&w, c->a);
    json_write_begin_object(&w);
    json_write_key(&w, "jsonrpc"); json_write_string(&w, "2.0");
    json_write_key(&w, "id"); write_id(&w, id);
    json_write_key(&w, "result"); json_write_raw(&w, result, rlen);
    json_write_end_object(&w);
    return json_writer_finish(&w, len);
}

/* Run a method; on success writes the result value into `w`. */
static bool run(Call *c, const char *method, JsonWriter *w)
{
    if (!strcmp(method, "initialize")) {
        json_write_begin_object(w);
        json_write_key(w, "protocolVersion"); json_write_string(w, "copper.tools/1");
        json_write_key(w, "serverInfo");
        json_write_begin_object(w);
        json_write_key(w, "name"); json_write_string(w, "copper-core");
        json_write_key(w, "version"); json_write_string(w, c->s->config.version ? c->s->config.version : "dev");
        json_write_end_object(w);
        json_write_key(w, "capabilities"); json_write_raw(w, "{\"tools\":{}}", 12);
        json_write_end_object(w);
        return true;
    }
    if (!strcmp(method, "tools/list")) { write_tools(w); return true; }
    if (!strcmp(method, "shutdown")) { c->s->shutdown = true; json_write_null(w); return true; }
    const ToolDef *t = NULL;
    if (!strcmp(method, "tools/call")) {
        const JsonValue *params = c->args;
        const JsonValue *name = json_get(params, "name"), *args = json_get(params, "arguments");
        if (!name || name->kind != JSON_STRING) return fail(c, RPC_INVALID_PARAMS, "MISSING_TOOL_NAME", "tools/call needs {\"name\": ..., \"arguments\": {...}}", "see tools/list");
        if (args && args->kind != JSON_OBJECT) return fail(c, RPC_INVALID_PARAMS, "BAD_ARGUMENTS", "arguments must be an object", "");
        t = find_tool(name->text);
        if (!t) { snprintf(c->detail, sizeof c->detail, "%s", name->text); return fail(c, RPC_METHOD_NOT_FOUND, "UNKNOWN_TOOL", "no tool with this name", "see tools/list"); }
        c->args = args;
    } else {
        t = find_tool(method);
        if (!t) { snprintf(c->detail, sizeof c->detail, "%s", method); return fail(c, RPC_METHOD_NOT_FOUND, "UNKNOWN_METHOD", "no method with this name", "methods: initialize, tools/list, tools/call, shutdown, or a tool name"); }
    }
    return known_args(c, t) && t->fn(c, w);
}

const char *tool_server_handle(ToolServer *s, const char *line, size_t len, size_t *out_len)
{
    size_t dummy;
    if (!out_len) out_len = &dummy;
    *out_len = 0;
    arena_reset(&s->scratch);
    s->seq++;
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) len--;
    Call c;
    memset(&c, 0, sizeof c);
    c.s = s;
    c.a = &s->scratch;
    JsonValue *req = NULL;
    JsonError je = {0};
    const JsonValue *id = NULL;
    bool parsed = json_parse(&s->scratch, line, len, &req, &je) == JSON_OK;
    log_line(s, "in", line, len, parsed);
    const char *resp = NULL;
    if (!parsed) {
        c.line = je.line;
        snprintf(c.detail, sizeof c.detail, "column %zu: %s", je.column, je.message ? je.message : "");
        fail(&c, RPC_PARSE_ERROR, "PARSE_ERROR", "the request is not JSON", "one JSON-RPC 2.0 object per line");
        resp = error_response(&c, NULL, out_len);
    } else if (req->kind == JSON_ARRAY) {
        fail(&c, RPC_INVALID_REQUEST, "BATCH_UNSUPPORTED", "batch requests are not supported", "send one request per line");
        resp = error_response(&c, NULL, out_len);
    } else {
        const JsonValue *ver = json_get(req, "jsonrpc"), *method = json_get(req, "method"), *params = json_get(req, "params");
        id = json_get(req, "id");
        bool id_ok = !id || id->kind == JSON_NULL || id->kind == JSON_NUMBER || id->kind == JSON_STRING;
        if (req->kind != JSON_OBJECT || !ver || ver->kind != JSON_STRING || strcmp(ver->text, "2.0") || !method ||
            method->kind != JSON_STRING || !id_ok || (params && params->kind != JSON_OBJECT)) {
            fail(&c, RPC_INVALID_REQUEST, "INVALID_REQUEST", "not a JSON-RPC 2.0 request",
                 "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}; params, when given, are an object");
            resp = error_response(&c, id_ok ? id : NULL, out_len);
        } else {
            bool notification = !id;
            if (!strcmp(method->text, "exit")) { s->shutdown = s->exited = true; notification = true; }
            JsonWriter w;
            json_writer_init_compact(&w, &s->scratch);
            c.args = params;
            bool ok = s->shutdown && strcmp(method->text, "shutdown") && strcmp(method->text, "exit")
                ? fail(&c, RPC_INVALID_REQUEST, "SHUT_DOWN", "the server is shutting down", "")
                : !strcmp(method->text, "exit") ? true : run(&c, method->text, &w);
            if (notification) { *out_len = 0; return NULL; }
            if (ok) {
                size_t rl;
                const char *result = json_writer_finish(&w, &rl);
                if (!result) { fail(&c, TOOL_INPUT_INVALID, "OUT_OF_MEMORY", "the result could not be written", ""); resp = error_response(&c, id, out_len); }
                else resp = result_response(&c, id, result, rl, out_len);
            } else {
                resp = error_response(&c, id, out_len);
            }
        }
    }
    if (resp) log_line(s, "out", resp, *out_len, true);
    return resp;
}

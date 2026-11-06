/* The REST front end (src/v2_src/rest_server.c): rest_handle for every route, status mapping and error bodies.
 * tests/test_rest_server.py drives the real process over HTTP. */
#include "rest_server.h"

#include "json.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static RestResponse call(ToolServer *s, Arena *a, const char *m, const char *p, const char *body)
{
    return rest_handle(s, a, m, p, body, body ? strlen(body) : 0);
}

static const JsonValue *parse(Arena *a, const RestResponse *r)
{
    JsonValue *v;
    JsonError e;
    return r->body_len && json_parse(a, r->body, r->body_len, &v, &e) == JSON_OK ? v : NULL;
}

static const char *kind(Arena *a, const RestResponse *r)
{
    const JsonValue *v = parse(a, r), *k = json_get(json_get(json_get(v, "error"), "data"), "kind");
    return k ? k->text : "";
}

int main(void)
{
    Arena a;
    ToolServer s;
    ToolServerConfig c;
    memset(&c, 0, sizeof c);
    c.root = REST_ROOT;
    c.version = "test";
    tool_server_init(&s, &c);
    arena_init(&a, 1 << 20);
    RestResponse r = call(&s, &a, "GET", "/v1/health", NULL);
    CHECK(r.status == 200 && strstr(r.body, "\"ok\""));
    r = call(&s, &a, "GET", "/v1/tools", NULL);
    const JsonValue *v = parse(&a, &r);
    CHECK(r.status == 200 && v && json_get(json_get(v, "result"), "tools")->count == 4);
    r = call(&s, &a, "POST", "/v1/tools/board.load", "{\"path\":\"tests/golden/board/r0603x5.kicad_pcb\"}");
    v = parse(&a, &r);
    CHECK(r.status == 200 && v && !strcmp(json_get(json_get(v, "result"), "board_id")->text, "board-1"));
    r = call(&s, &a, "POST", "/v1/tools/dfm.check", "{\"board_id\":\"board-1\",\"profile\":\"fab_profiles/jlcpcb_2layer.json\"}");
    v = parse(&a, &r);
    CHECK(r.status == 200 && v && !strcmp(json_get(json_get(json_get(v, "result"), "report"), "verdict")->text, "UNVERIFIED"));
    /* Errors: status from the tool error's kind, body {"error": {...}}. */
    r = call(&s, &a, "POST", "/v1/tools/board.load", "{\"path\":\"missing.kicad_pcb\"}");
    CHECK(r.status == 404 && !strcmp(kind(&a, &r), "BOARD_NOT_FOUND"));
    r = call(&s, &a, "POST", "/v1/tools/board.load", "{\"path\":\"tests/golden/findings/broken.drl\"}");
    CHECK(r.status == 422);
    r = call(&s, &a, "POST", "/v1/tools/route.all", "{}");
    CHECK(r.status == 404 && !strcmp(kind(&a, &r), "UNKNOWN_TOOL"));
    r = call(&s, &a, "POST", "/v1/tools/board.load", "{\"pth\":\"x\"}");
    CHECK(r.status == 400 && !strcmp(kind(&a, &r), "UNKNOWN_ARGUMENT"));
    r = call(&s, &a, "POST", "/v1/tools/board.load", "[1]");
    CHECK(r.status == 400 && !strcmp(kind(&a, &r), "BAD_ARGUMENTS"));
    r = call(&s, &a, "POST", "/v1/tools/dfm.check", "{\"board_id\":\"board-9\",\"profile\":\"fab_profiles/jlcpcb_2layer.json\"}");
    CHECK(r.status == 404 && !strcmp(kind(&a, &r), "UNKNOWN_BOARD_ID"));
    r = call(&s, &a, "GET", "/v1/tools/board.load", NULL);
    CHECK(r.status == 405);
    r = call(&s, &a, "GET", "/etc/passwd", NULL);
    CHECK(r.status == 404 && !strcmp(kind(&a, &r), "NOT_FOUND"));
    /* JSON-RPC passthrough and notifications. */
    r = call(&s, &a, "POST", "/v1/rpc", "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"initialize\"}");
    v = parse(&a, &r);
    CHECK(r.status == 200 && v && json_get(v, "result") && !strcmp(json_get(v, "id")->text, "5"));
    r = call(&s, &a, "POST", "/v1/rpc", "{\"jsonrpc\":\"2.0\",\"method\":\"tools/list\"}");
    CHECK(r.status == 204 && r.body_len == 0);
    r = call(&s, &a, "POST", "/v1/shutdown", NULL);
    CHECK(r.status == 200 && r.stop);
    tool_server_destroy(&s);
    arena_destroy(&a);
    if (failures) { fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
    puts("rest_server: all checks passed");
    return 0;
}

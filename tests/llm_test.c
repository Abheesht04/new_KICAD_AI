/* The LLM bridge (track G): schema_gbnf.c, llm_mock.c and agent_loop.c. Everything here runs without a model (the
 * Ollama backend and its tests were removed on 2026-11-20; Manus, the only model, runs outside copper-core).
 * `llm_test --write-golden` rewrites tests/golden/llm/agent_reply.gbnf for review. */
#include "agent_loop.h"
#include "json.h"
#include "llm.h"
#include "tool_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    size_t got = fread(b, 1, (size_t)n, f), k = 0;
    fclose(f);
    for (size_t i = 0; i < got; i++) if (b[i] != '\r') b[k++] = b[i];
    b[k] = 0;
    return b;
}

static void test_gbnf(bool write)
{
    Arena a;
    const char *g, *err;
    size_t n;
    arena_init(&a, 1 << 16);
    const char *s = agent_reply_schema();
    CHECK(llm_schema_to_gbnf(&a, s, strlen(s), &g, &n, &err) == LLM_OK);
    if (write) {
        FILE *f = fopen(LLM_ROOT "/tests/golden/llm/agent_reply.gbnf", "wb");
        if (f) { fwrite(g, 1, n, f); fclose(f); }
    } else {
        char *golden = slurp(LLM_ROOT "/tests/golden/llm/agent_reply.gbnf");
        CHECK(golden && !strcmp(golden, g));
        if (golden && strcmp(golden, g)) fprintf(stderr, "gbnf differs:\n%s\n", g);
        free(golden);
    }
    CHECK(strstr(g, "root ::= \"{\" ws \"\\\"action\\\"\" ws \":\" ws (\"\\\"tool\\\"\" | \"\\\"final\\\"\")"));
    const char *arr = "{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"integer\"}},\"required\":[\"x\"]}}";
    CHECK(llm_schema_to_gbnf(&a, arr, strlen(arr), &g, &n, &err) == LLM_OK && strstr(g, "root ::= \"[\" ws ( root-item") && strstr(g, "root-item ::= \"{\""));
    const char *opt = "{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"string\"}}}";
    CHECK(llm_schema_to_gbnf(&a, opt, strlen(opt), &g, &n, &err) == LLM_INVALID_ARGUMENT && strstr(err, "optional"));
    const char *bad = "{\"type\":\"date\"}";
    CHECK(llm_schema_to_gbnf(&a, bad, strlen(bad), &g, &n, &err) == LLM_INVALID_ARGUMENT && strstr(err, "unsupported"));
    arena_destroy(&a);
}

/* P01/F1 regressions (schema_gbnf.c at 7199ef6): (a) an array rule longer than 8 KiB was cut off with status OK;
 * (b) a rule name that filled its 256-byte buffer made rule_name write one byte past it. */
static char *object_schema(const char *key, const char *inner)
{
    size_t need = 2 * strlen(key) + strlen(inner) + 96;   /* the key appears twice */
    char *s = malloc(need);
    if (s && snprintf(s, need, "{\"type\":\"object\",\"properties\":{\"%s\":%s},\"required\":[\"%s\"]}", key, inner, key) >= (int)need) {
        free(s);   /* never hand a cut schema to the converter */
        s = NULL;
    }
    return s;
}

static void test_gbnf_bounds(void)
{
    Arena a;
    const char *g, *err;
    size_t n;
    arena_init(&a, 1 << 20);

    /* (a) the item expression is ("\"<4070 v>\""): 4078 bytes, twice in one rule of 8196 bytes. */
    enum { V = 4070 };
    char *value = malloc(V + 1), *schema = malloc(V + 64), *item = malloc(V + 16), *want = malloc(2 * V + 128);
    if (value && schema && item && want) {
        memset(value, 'v', V);
        value[V] = 0;
        snprintf(schema, V + 64, "{\"type\":\"array\",\"items\":{\"enum\":[\"%s\"]}}", value);
        snprintf(item, V + 16, "(\"\\\"%s\\\"\")", value);
        snprintf(want, 2 * V + 128, "root ::= \"[\" ws ( %s ( ws \",\" ws %s )* )? ws \"]\"\n", item, item);
        CHECK(strlen(want) > 8192);
        CHECK(llm_schema_to_gbnf(&a, schema, strlen(schema), &g, &n, &err) == LLM_OK);
        CHECK(g && !strncmp(g, want, strlen(want)));
        CHECK(g && strstr(g, "\nws ::= "));            /* the shared rules follow the complete array rule */
    } else CHECK(!"out of memory");
    free(value); free(schema); free(item); free(want);

    /* (b) Every property gets a name, so the deepest is "root-<key>-y"; with its NUL it must fit in 256 bytes.
     * key 248: 255 bytes, accepted. key 249: 256 bytes, refused (7199ef6 silently cut it to "root-<key>-").
     * key 250: refused (7199ef6 wrote its NUL one byte past the buffer). */
    const char *inner = "{\"type\":\"object\",\"properties\":{\"y\":{\"type\":\"integer\"}},\"required\":[\"y\"]}";
    char key[260], rule[300];
    for (size_t len = 248; len <= 250; len++) {
        memset(key, 'k', len);
        key[len] = 0;
        char *s = object_schema(key, inner);
        err = NULL;
        LlmStatus st = s ? llm_schema_to_gbnf(&a, s, strlen(s), &g, &n, &err) : LLM_OUT_OF_MEMORY;
        if (len == 248) {
            snprintf(rule, sizeof rule, "root-%s ::= \"{\"", key);   /* children are emitted first: may start g */
            CHECK(st == LLM_OK && strstr(g, rule));
        } else {
            CHECK(st == LLM_INVALID_ARGUMENT && err && strstr(err, "rule name"));
        }
        free(s);
    }
    arena_destroy(&a);
}

typedef struct Lines { size_t n; } Lines;
static void count_line(void *ctx, const char *line, size_t len) { (void)line; (void)len; ((Lines *)ctx)->n++; }

static void server(ToolServer *ts)
{
    ToolServerConfig c;
    memset(&c, 0, sizeof c);
    c.root = LLM_ROOT;
    c.version = "test";
    tool_server_init(ts, &c);
}

#define LOAD "{\"action\":\"tool\",\"tool\":\"board.load\",\"arguments\":{\"path\":\"examples/generated/divider/design.kicad_pcb\"},\"answer\":\"\"}"
#define FINAL "{\"action\":\"final\",\"tool\":\"\",\"arguments\":{},\"answer\":\"3 footprints.\"}"

static void test_agent_script(void)
{
    Arena a;
    ToolServer ts;
    LlmBackend b;
    LlmMockState s;
    AgentResult r;
    Lines lines = {0};
    AgentConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.log = count_line;
    cfg.log_ctx = &lines;
    arena_init(&a, 1 << 20);

    /* A tool call, then a final answer in a ```json fence (accepted). */
    static const char *const ok[] = {LOAD, "```json\n" FINAL "\n```"};
    server(&ts);
    llm_mock_init_script(&b, &s, ok, 2);
    CHECK(agent_run(&a, &b, &ts, &cfg, "how many footprints?", &r) == AGENT_DONE);
    CHECK(r.turns == 2 && r.tool_count == 1 && !strcmp(r.tools[0], "board.load") && r.tool_errors == 0 && !strcmp(r.answer, "3 footprints."));
    CHECK(s.last_schema == agent_reply_schema() && s.last_message_count == 4 && lines.n == 2);   /* system, user, assistant, tool */
    tool_server_destroy(&ts);

    /* The 12-turn limit holds whatever the config asks. */
    static const char *loop[13];
    for (int i = 0; i < 13; i++) loop[i] = LOAD;
    server(&ts);
    llm_mock_init_script(&b, &s, loop, 13);
    cfg.max_turns = 50;
    CHECK(agent_run(&a, &b, &ts, &cfg, "loop", &r) == AGENT_TURN_LIMIT && r.turns == 12 && r.tool_count == 12 && s.calls == 12);
    cfg.max_turns = 0;
    tool_server_destroy(&ts);

    /* Three invalid replies in a row end the run; one invalid reply is corrected. */
    static const char *const junk[] = {"hello", "{\"action\":\"dance\"}", "{\"action\":\"final\",\"answer\":\"\"}"};
    server(&ts);
    llm_mock_init_script(&b, &s, junk, 3);
    CHECK(agent_run(&a, &b, &ts, &cfg, "x", &r) == AGENT_INVALID_REPLIES && r.invalid_replies == 3);
    tool_server_destroy(&ts);
    static const char *const fixed[] = {"not json", FINAL};
    server(&ts);
    llm_mock_init_script(&b, &s, fixed, 2);
    CHECK(agent_run(&a, &b, &ts, &cfg, "x", &r) == AGENT_DONE && r.invalid_replies == 1 && r.turns == 2);
    tool_server_destroy(&ts);

    /* A tool error goes back to the model as a tool message; the run goes on. */
    static const char *const err[] = {"{\"action\":\"tool\",\"tool\":\"route.all\",\"arguments\":{},\"answer\":\"\"}", FINAL};
    server(&ts);
    llm_mock_init_script(&b, &s, err, 2);
    CHECK(agent_run(&a, &b, &ts, &cfg, "x", &r) == AGENT_DONE && r.tool_errors == 1);
    tool_server_destroy(&ts);

    /* A backend failure is BACKEND_ERROR, and scoring calls it UNVERIFIED, not FAIL. */
    server(&ts);
    llm_mock_init_script(&b, &s, NULL, 0);
    CHECK(agent_run(&a, &b, &ts, &cfg, "x", &r) == AGENT_BACKEND_ERROR && r.backend_status == LLM_SCRIPT_EXHAUSTED);
    JsonValue *exp;
    JsonError je;
    const char *none = "{\"tools\":[],\"answer_contains\":[]}";
    CHECK(json_parse(&a, none, strlen(none), &exp, &je) == JSON_OK);
    char why[256];
    CHECK(agent_score(&r, exp, why, sizeof why) == AGENT_UNVERIFIED);
    tool_server_destroy(&ts);
    arena_destroy(&a);
}

static void test_eval_with_rules(void)
{
    char *text = slurp(LLM_ROOT "/eval/prompts.jsonl");
    CHECK(text != NULL);
    if (!text) return;
    size_t prompts = 0, passed = 0;
    for (char *line = text, *next; *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0; else next = line + strlen(line);
        if (!*line) continue;
        Arena a;
        ToolServer ts;
        LlmBackend b;
        LlmMockState s;
        AgentResult r;
        JsonValue *v;
        JsonError je;
        arena_init(&a, 1 << 20);
        CHECK(json_parse(&a, line, strlen(line), &v, &je) == JSON_OK);
        server(&ts);
        llm_mock_init_rules(&b, &s);
        agent_run(&a, &b, &ts, NULL, json_get(v, "prompt")->text, &r);
        char why[2048];
        AgentVerdict verdict = agent_score(&r, json_get(v, "expect"), why, sizeof why);
        prompts++;
        if (verdict == AGENT_PASS) passed++;
        else fprintf(stderr, "eval %s: %s: %s (answer: %s)\n", json_get(v, "id")->text, agent_verdict_name(verdict), why, r.answer ? r.answer : "-");
        tool_server_destroy(&ts);
        arena_destroy(&a);
    }
    CHECK(prompts == 10 && passed == 10);
    free(text);
}

int main(int argc, char **argv)
{
    bool write = argc > 1 && !strcmp(argv[1], "--write-golden");
    test_gbnf(write);
    test_gbnf_bounds();
    test_agent_script();
    test_eval_with_rules();
    if (failures) { fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
    puts(write ? "llm: golden written; review it" : "llm: all checks passed");
    return 0;
}

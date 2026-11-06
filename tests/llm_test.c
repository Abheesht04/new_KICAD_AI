/* The LLM bridge (track G): llm_ollama.c pure parts, schema_gbnf.c, llm_mock.c and agent_loop.c. Everything here
 * runs without a model; `llm_test --live` talks to a real Ollama server (OLLAMA_URL, OLLAMA_MODEL) and exits 77
 * (UNVERIFIED) without one. `llm_test --write-golden` rewrites tests/golden/llm/agent_reply.gbnf for review. */
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

static void test_http_and_url(void)
{
    Arena a;
    LlmUrl u;
    int st;
    const char *body;
    size_t n;
    arena_init(&a, 1 << 16);
    CHECK(llm_parse_url(&a, "http://localhost:11434/api/chat", &u) == LLM_OK && !strcmp(u.host, "localhost") && u.port == 11434 && !strcmp(u.path, "/api/chat"));
    CHECK(llm_parse_url(&a, "http://gpu-box/x", &u) == LLM_OK && u.port == 80 && !strcmp(u.path, "/x"));
    CHECK(llm_parse_url(&a, "http://10.0.0.5:8080", &u) == LLM_OK && !strcmp(u.path, "/"));
    CHECK(llm_parse_url(&a, "https://example.com/", &u) == LLM_INVALID_ARGUMENT);   /* no TLS */
    CHECK(llm_parse_url(&a, "http://host:99999/", &u) == LLM_INVALID_ARGUMENT);
    const char *r1 = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 7\r\n\r\n{\"a\":1}";
    CHECK(llm_http_parse(&a, r1, strlen(r1), &st, &body, &n) == LLM_OK && st == 200 && n == 7 && !strcmp(body, "{\"a\":1}"));
    const char *r2 = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4\r\n{\"a\"\r\n3\r\n:1}\r\n0\r\n\r\n";
    CHECK(llm_http_parse(&a, r2, strlen(r2), &st, &body, &n) == LLM_OK && n == 7 && !strcmp(body, "{\"a\":1}"));
    const char *r3 = "HTTP/1.1 404 Not Found\r\nContent-Length: 30\r\n\r\n{\"error\":\"short";
    CHECK(llm_http_parse(&a, r3, strlen(r3), &st, &body, &n) == LLM_BAD_RESPONSE);
    CHECK(llm_http_parse(&a, "garbage", 7, &st, &body, &n) == LLM_BAD_RESPONSE);
    arena_destroy(&a);
}

static void test_ollama_body_and_reply(void)
{
    Arena a;
    const char *body;
    size_t n;
    arena_init(&a, 1 << 16);
    LlmOllamaConfig c = {NULL, "qwen3:8b", 0};
    LlmMessage m[2] = {{LLM_SYSTEM, "be brief"}, {LLM_USER, "say \"hi\"\n"}};
    CHECK(llm_ollama_request_body(&a, &c, m, 2, "{\"type\":\"object\"}", &body, &n) == LLM_OK);
    const char *want = "{\"model\":\"qwen3:8b\",\"messages\":[{\"role\":\"system\",\"content\":\"be brief\"},{\"role\":\"user\","
                       "\"content\":\"say \\\"hi\\\"\\n\"}],\"stream\":false,\"think\":false,\"format\":{\"type\":\"object\"},"
                       "\"options\":{\"temperature\":0}}";
    CHECK(!strcmp(body, want));
    CHECK(llm_ollama_request_body(&a, &c, m, 2, NULL, &body, &n) == LLM_OK && !strstr(body, "format"));
    CHECK(llm_ollama_request_body(&a, &c, m, 2, "not json", &body, &n) == LLM_INVALID_ARGUMENT);
    LlmReply r;
    memset(&r, 0, sizeof r);
    const char *ok = "{\"model\":\"qwen3:8b\",\"created_at\":\"x\",\"message\":{\"role\":\"assistant\",\"content\":\"{\\\"a\\\":1}\"},"
                     "\"done\":true,\"prompt_eval_count\":120,\"eval_count\":9}";
    CHECK(llm_ollama_parse_reply(&a, ok, strlen(ok), &r) == LLM_OK && !strcmp(r.content, "{\"a\":1}") && r.prompt_tokens == 120 && r.completion_tokens == 9);
    memset(&r, 0, sizeof r);
    CHECK(llm_ollama_parse_reply(&a, "{\"error\":\"model 'x' not found\"}", 31, &r) == LLM_HTTP_ERROR && strstr(r.error, "not found"));
    CHECK(llm_ollama_parse_reply(&a, "<html>", 6, &r) == LLM_BAD_RESPONSE);
    /* No server on this port: UNAVAILABLE, not a hang and not a crash. */
    LlmBackend b;
    LlmOllamaState s;
    LlmOllamaConfig dead = {"http://127.0.0.1:9", "qwen3:8b", 2000};
    llm_ollama_init(&b, &s, &dead);
    memset(&r, 0, sizeof r);
    LlmStatus st = b.chat(&b, m, 2, NULL, &a, &r);
    CHECK(st == LLM_UNAVAILABLE || st == LLM_TIMEOUT);
    arena_destroy(&a);
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

static int test_live(void)
{
    const char *url = getenv("OLLAMA_URL"), *model = getenv("OLLAMA_MODEL");
    if (!url || !*url || !model || !*model) { puts("llm --live: UNVERIFIED (set OLLAMA_URL and OLLAMA_MODEL)"); return 77; }
    Arena a;
    LlmBackend b;
    LlmOllamaState s;
    LlmOllamaConfig c = {url, model, 300000};
    LlmReply r;
    arena_init(&a, 1 << 20);
    llm_ollama_init(&b, &s, &c);
    LlmMessage m[2] = {{LLM_SYSTEM, "Reply with one JSON object only."},
                       {LLM_USER, "Give a final answer whose text is exactly: ok"}};
    LlmStatus st = b.chat(&b, m, 2, agent_reply_schema(), &a, &r);
    printf("llm --live: %s %s, %s, %llu ms, %llu+%llu tokens: %.200s\n", url, model, llm_status_name(st),
           (unsigned long long)r.elapsed_ms, (unsigned long long)r.prompt_tokens, (unsigned long long)r.completion_tokens,
           st == LLM_OK ? r.content : (r.error ? r.error : ""));
    JsonValue *v;
    JsonError je;
    CHECK(st == LLM_OK && json_parse(&a, r.content, r.content_len, &v, &je) == JSON_OK && json_get(v, "action"));
    arena_destroy(&a);
    return failures ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--live")) return test_live();
    bool write = argc > 1 && !strcmp(argv[1], "--write-golden");
    test_http_and_url();
    test_ollama_body_and_reply();
    test_gbnf(write);
    test_agent_script();
    test_eval_with_rules();
    if (failures) { fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
    puts(write ? "llm: golden written; review it" : "llm: all checks passed");
    return 0;
}

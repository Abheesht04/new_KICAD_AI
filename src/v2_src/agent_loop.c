/* agent_loop.c - a model drives the tool server (see agent_loop.h). */
#include "agent_loop.h"

#include "json.h"

#include <stdio.h>
#include <string.h>

static const char *const STATUS[] = {"DONE", "TURN_LIMIT", "INVALID_REPLIES", "BACKEND_ERROR", "INVALID_ARGUMENT", "OUT_OF_MEMORY"};

const char *agent_status_name(AgentStatus s) { return (unsigned)s < sizeof STATUS / sizeof *STATUS ? STATUS[s] : "UNKNOWN"; }

static const char REPLY_SCHEMA[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"action\":{\"type\":\"string\",\"enum\":[\"tool\",\"final\"]},"
    "\"tool\":{\"type\":\"string\"},"
    "\"arguments\":{\"type\":\"object\"},"
    "\"answer\":{\"type\":\"string\"}},"
    "\"required\":[\"action\",\"tool\",\"arguments\",\"answer\"]}";

const char *agent_reply_schema(void) { return REPLY_SCHEMA; }

static const char DEFAULT_PROMPT[] =
    "You are Copper's PCB assistant. You work on the user's KiCad files only through the tools below.\n"
    "Reply with exactly one JSON object and nothing else:\n"
    "  {\"action\":\"tool\",\"tool\":\"<tool name>\",\"arguments\":{...},\"answer\":\"\"} to call a tool, or\n"
    "  {\"action\":\"final\",\"tool\":\"\",\"arguments\":{},\"answer\":\"<your answer>\"} when you are done.\n"
    "Each tool result comes back as a message. Base your answer only on tool results: never invent part numbers,\n"
    "ratings, fabricator capabilities or measurements. Say plainly when a check is FAIL, UNVERIFIED or not run.\n";

/* Re-serialize a parsed value (numbers keep their source text). */
static void write_value(JsonWriter *w, const JsonValue *v)
{
    switch (v->kind) {
    case JSON_NULL: json_write_null(w); break;
    case JSON_TRUE: json_write_bool(w, true); break;
    case JSON_FALSE: json_write_bool(w, false); break;
    case JSON_NUMBER: json_write_raw(w, v->text, v->text_len); break;
    case JSON_STRING: json_write_string(w, v->text); break;
    case JSON_ARRAY:
        json_write_begin_array(w);
        for (size_t i = 0; i < v->count; i++) write_value(w, v->items[i]);
        json_write_end_array(w);
        break;
    case JSON_OBJECT:
        json_write_begin_object(w);
        for (size_t i = 0; i < v->count; i++) { json_write_key(w, v->keys[i]); write_value(w, v->items[i]); }
        json_write_end_object(w);
        break;
    }
}

static char *copy(Arena *a, const char *s, size_t n)
{
    char *d = arena_alloc(a, n + 1);
    if (d) { memcpy(d, s, n); d[n] = 0; }
    return d;
}

typedef struct Conv { Arena *a; LlmMessage *m; size_t n, cap; bool oom; } Conv;

static void say(Conv *c, LlmRole role, const char *text)
{
    if (c->oom) return;
    if (c->n == c->cap) {
        size_t cap = c->cap ? c->cap * 2 : 16;
        LlmMessage *m = arena_alloc_array(c->a, sizeof *m, cap);
        if (!m) { c->oom = true; return; }
        if (c->n) memcpy(m, c->m, c->n * sizeof *m);
        c->m = m;
        c->cap = cap;
    }
    c->m[c->n].role = role;
    c->m[c->n].content = text;
    c->n++;
}

/* The system prompt with the tool list from tools/list. */
static const char *system_prompt(Arena *a, ToolServer *ts, const char *base)
{
    size_t rl;
    const char *req = "{\"jsonrpc\":\"2.0\",\"id\":\"agent-tools\",\"method\":\"tools/list\"}";
    const char *resp = tool_server_handle(ts, req, strlen(req), &rl);
    JsonValue *v;
    JsonError e;
    char *text = NULL;
    size_t cap = strlen(base) + 64, n;
    if (!resp || json_parse(a, resp, rl, &v, &e) != JSON_OK) return base;
    const JsonValue *tools = json_get(json_get(v, "result"), "tools");
    for (size_t i = 0; tools && i < tools->count; i++) cap += 512 + (json_get(tools->items[i], "description") ? json_get(tools->items[i], "description")->text_len : 0) + 1024;
    text = arena_alloc(a, cap);
    if (!text) return base;
    n = (size_t)snprintf(text, cap, "%sTools:", base);
    for (size_t i = 0; tools && i < tools->count && n < cap; i++) {
        const JsonValue *t = tools->items[i];
        JsonWriter w;
        json_writer_init_compact(&w, a);
        write_value(&w, json_get(t, "inputSchema"));
        size_t sl;
        const char *schema = json_writer_finish(&w, &sl);
        n += (size_t)snprintf(text + n, cap - n, "\n- %s: %s Arguments: %s", json_get(t, "name")->text,
                              json_get(t, "description")->text, schema ? schema : "{}");
    }
    return text;
}

/* Strip whitespace and a ```json fence some models add. */
static const char *unfence(const char *s, size_t *n)
{
    while (*n && (*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t')) { s++; (*n)--; }
    if (*n >= 3 && !strncmp(s, "```", 3)) {
        const char *nl = memchr(s, '\n', *n);
        if (nl) { *n -= (size_t)(nl + 1 - s); s = nl + 1; }
        while (*n && (s[*n - 1] == ' ' || s[*n - 1] == '\n' || s[*n - 1] == '\r')) (*n)--;
        if (*n >= 3 && !strncmp(s + *n - 3, "```", 3)) *n -= 3;
    }
    while (*n && (s[*n - 1] == ' ' || s[*n - 1] == '\n' || s[*n - 1] == '\r' || s[*n - 1] == '\t')) (*n)--;
    return s;
}

static void log_turn(const AgentConfig *cfg, Arena *a, size_t turn, const char *reply, const char *llm_status,
                     const char *action, const char *tool, const char *response, size_t rlen, const char *note)
{
    if (!cfg || !cfg->log) return;
    JsonWriter w;
    json_writer_init_compact(&w, a);
    json_write_begin_object(&w);
    json_write_key(&w, "turn"); json_write_uint(&w, turn);
    json_write_key(&w, "backend_status"); json_write_string(&w, llm_status);
    json_write_key(&w, "reply"); json_write_string(&w, reply);
    if (action) { json_write_key(&w, "action"); json_write_string(&w, action); }
    if (tool) { json_write_key(&w, "tool"); json_write_string(&w, tool); }
    if (response) { json_write_key(&w, "tool_response"); json_write_raw(&w, response, rlen); }
    if (note) { json_write_key(&w, "note"); json_write_string(&w, note); }
    json_write_end_object(&w);
    size_t n;
    const char *text = json_writer_finish(&w, &n);
    if (text) cfg->log(cfg->log_ctx, text, n);
}

AgentStatus agent_run(Arena *a, LlmBackend *b, ToolServer *ts, const AgentConfig *cfg, const char *prompt, AgentResult *out)
{
    if (!out) return AGENT_INVALID_ARGUMENT;
    memset(out, 0, sizeof *out);
    if (!a || !b || !b->chat || !ts || !prompt) return out->status = AGENT_INVALID_ARGUMENT;
    size_t max_turns = cfg && cfg->max_turns && cfg->max_turns < AGENT_MAX_TURNS ? cfg->max_turns : AGENT_MAX_TURNS;
    size_t max_bytes = cfg && cfg->max_tool_result_bytes ? cfg->max_tool_result_bytes : 16384;
    Conv c = {a, NULL, 0, 0, false};
    say(&c, LLM_SYSTEM, system_prompt(a, ts, cfg && cfg->system_prompt ? cfg->system_prompt : DEFAULT_PROMPT));
    say(&c, LLM_USER, copy(a, prompt, strlen(prompt)));
    size_t tools_cap = 0, consecutive_invalid = 0;
    for (size_t turn = 1; turn <= max_turns; turn++) {
        if (c.oom) return out->status = AGENT_OUT_OF_MEMORY;
        LlmReply r;
        LlmStatus ls = b->chat(b, c.m, c.n, REPLY_SCHEMA, a, &r);
        out->turns = turn;
        out->prompt_tokens += r.prompt_tokens;
        out->completion_tokens += r.completion_tokens;
        out->model_ms += r.elapsed_ms;
        if (ls != LLM_OK) {
            out->backend_status = ls;
            out->backend_error = r.error;
            log_turn(cfg, a, turn, "", llm_status_name(ls), NULL, NULL, NULL, 0, r.error);
            return out->status = AGENT_BACKEND_ERROR;
        }
        const char *raw = copy(a, r.content, r.content_len);
        size_t n = r.content_len;
        const char *body = unfence(raw, &n);
        JsonValue *v = NULL;
        JsonError je;
        const char *why = NULL;
        const JsonValue *action = NULL, *tool = NULL, *args = NULL, *answer = NULL;
        if (json_parse(a, body, n, &v, &je) != JSON_OK || v->kind != JSON_OBJECT) why = "the reply is not one JSON object";
        else {
            action = json_get(v, "action"); tool = json_get(v, "tool"); args = json_get(v, "arguments"); answer = json_get(v, "answer");
            if (!action || action->kind != JSON_STRING || (strcmp(action->text, "tool") && strcmp(action->text, "final"))) why = "\"action\" must be \"tool\" or \"final\"";
            else if (!strcmp(action->text, "tool") && (!tool || tool->kind != JSON_STRING || !tool->text_len)) why = "a tool call needs \"tool\": the tool's name";
            else if (!strcmp(action->text, "tool") && (!args || args->kind != JSON_OBJECT)) why = "\"arguments\" must be an object";
            else if (!strcmp(action->text, "final") && (!answer || answer->kind != JSON_STRING || !answer->text_len)) why = "a final reply needs a non-empty \"answer\"";
        }
        say(&c, LLM_ASSISTANT, raw);
        if (why) {
            out->invalid_replies++;
            log_turn(cfg, a, turn, raw, "OK", "invalid", NULL, NULL, 0, why);
            if (++consecutive_invalid >= 3) return out->status = AGENT_INVALID_REPLIES;
            char fix[512];
            snprintf(fix, sizeof fix, "Your reply was not valid: %s. Reply with exactly one JSON object as specified.", why);
            say(&c, LLM_USER, copy(a, fix, strlen(fix)));
            continue;
        }
        consecutive_invalid = 0;
        if (!strcmp(action->text, "final")) {
            out->answer = answer->text;
            log_turn(cfg, a, turn, raw, "OK", "final", NULL, NULL, 0, NULL);
            return out->status = AGENT_DONE;
        }
        /* Run the tool through the server, exactly as a JSON-RPC client would. */
        JsonWriter w;
        json_writer_init_compact(&w, a);
        json_write_begin_object(&w);
        json_write_key(&w, "jsonrpc"); json_write_string(&w, "2.0");
        json_write_key(&w, "id"); json_write_uint(&w, turn);
        json_write_key(&w, "method"); json_write_string(&w, "tools/call");
        json_write_key(&w, "params");
        json_write_begin_object(&w);
        json_write_key(&w, "name"); json_write_string(&w, tool->text);
        json_write_key(&w, "arguments"); write_value(&w, args);
        json_write_end_object(&w);
        json_write_end_object(&w);
        size_t ql, rl;
        const char *req = json_writer_finish(&w, &ql);
        const char *resp_tmp = req ? tool_server_handle(ts, req, ql, &rl) : NULL;
        const char *resp = resp_tmp ? copy(a, resp_tmp, rl) : NULL;   /* the server's text lives until its next call */
        if (out->tool_count == tools_cap) {
            size_t cap2 = tools_cap ? tools_cap * 2 : 8;
            const char **t2 = arena_alloc_array(a, sizeof *t2, cap2);
            if (!t2) return out->status = AGENT_OUT_OF_MEMORY;
            if (out->tool_count) memcpy(t2, out->tools, out->tool_count * sizeof *t2);
            out->tools = t2;
            tools_cap = cap2;
        }
        out->tools[out->tool_count++] = tool->text;
        log_turn(cfg, a, turn, raw, "OK", "tool", tool->text, resp, resp ? rl : 0, NULL);
        /* The tool message: {"tool": name, "result": ...} or {"tool": name, "error": ...}, cut to max_bytes. */
        JsonValue *rv = NULL;
        JsonError re;
        json_writer_init_compact(&w, a);
        json_write_begin_object(&w);
        json_write_key(&w, "tool"); json_write_string(&w, tool->text);
        if (resp && json_parse(a, resp, rl, &rv, &re) == JSON_OK && json_get(rv, "result")) {
            json_write_key(&w, "result"); write_value(&w, json_get(rv, "result"));
        } else {
            out->tool_errors++;
            json_write_key(&w, "error");
            if (rv && json_get(rv, "error")) write_value(&w, json_get(rv, "error"));
            else json_write_string(&w, "the tool server gave no answer");
        }
        json_write_end_object(&w);
        size_t ml;
        const char *msg = json_writer_finish(&w, &ml);
        if (msg && ml > max_bytes) {
            char *cut = copy(a, msg, max_bytes);
            JsonWriter w2;
            json_writer_init_compact(&w2, a);
            json_write_begin_object(&w2);
            json_write_key(&w2, "tool"); json_write_string(&w2, tool->text);
            json_write_key(&w2, "truncated"); json_write_bool(&w2, true);
            json_write_key(&w2, "head"); json_write_string(&w2, cut ? cut : "");
            json_write_end_object(&w2);
            msg = json_writer_finish(&w2, &ml);
        }
        say(&c, LLM_TOOL, msg ? msg : "{\"error\":\"out of memory\"}");
    }
    return out->status = AGENT_TURN_LIMIT;
}

/* ------------------------------------------------------------------------------------------------ eval scoring */

static const char *const VERDICTS[] = {"PASS", "FAIL", "UNVERIFIED"};

const char *agent_verdict_name(AgentVerdict v) { return (unsigned)v < 3 ? VERDICTS[v] : "UNKNOWN"; }

static bool contains_ci(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    if (!n) return true;
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < n && p[i] && (p[i] | 32) == (needle[i] | 32)) i++;
        if (i == n) return true;
    }
    return false;
}

static void reason(char *buf, size_t cap, const char *a, const char *b)
{
    size_t n = strlen(buf);
    if (n + 2 < cap) snprintf(buf + n, cap - n, "%s%s%s", n ? "; " : "", a, b ? b : "");
}

AgentVerdict agent_score(const AgentResult *r, const JsonValue *expect, char *reasons, size_t cap)
{
    if (reasons && cap) reasons[0] = 0;
    if (r->status == AGENT_BACKEND_ERROR) { reason(reasons, cap, "the backend did not answer: ", r->backend_error); return AGENT_UNVERIFIED; }
    bool ok = true;
    if (r->status != AGENT_DONE) { reason(reasons, cap, "the run ended without a final answer: ", agent_status_name(r->status)); ok = false; }
    const JsonValue *tools = json_get(expect, "tools"), *phrases = json_get(expect, "answer_contains");
    size_t k = 0;
    for (size_t i = 0; tools && i < tools->count; i++) {
        while (k < r->tool_count && strcmp(r->tools[k], tools->items[i]->text)) k++;
        if (k == r->tool_count) { reason(reasons, cap, "expected tool not called (in order): ", tools->items[i]->text); ok = false; break; }
        k++;
    }
    for (size_t i = 0; phrases && i < phrases->count; i++)
        if (!r->answer || !contains_ci(r->answer, phrases->items[i]->text)) { reason(reasons, cap, "the answer does not mention: ", phrases->items[i]->text); ok = false; }
    return ok ? AGENT_PASS : AGENT_FAIL;
}

/* llm_mock.c - the default, model-free backend (see llm.h): scripted replies or a deterministic rule planner. */
#include "llm.h"

#include "json.h"

#include <stdio.h>
#include <string.h>

static LlmStatus done(LlmReply *out, const char *text, const char *model)
{
    out->content = text;
    out->content_len = text ? strlen(text) : 0;
    out->model = model;
    return text ? LLM_OK : LLM_OUT_OF_MEMORY;
}

static LlmStatus script_chat(LlmBackend *b, const LlmMessage *msgs, size_t n, const char *schema, Arena *a, LlmReply *out)
{
    LlmMockState *s = b->state;
    (void)msgs; (void)a;
    memset(out, 0, sizeof *out);
    s->calls++;
    s->last_schema = schema;
    s->last_message_count = n;
    if (s->next >= s->script_count) { out->error = "the mock script has no reply left"; return LLM_SCRIPT_EXHAUSTED; }
    return done(out, s->script[s->next++], "mock-script");
}

void llm_mock_init_script(LlmBackend *b, LlmMockState *s, const char *const *script, size_t n)
{
    memset(s, 0, sizeof *s);
    s->script = script;
    s->script_count = n;
    b->name = "mock";
    b->chat = script_chat;
    b->state = s;
}

/* ------------------------------------------------------------------------------------------------ rules */

typedef struct Plan {
    const char *board, *drill, *profile, *out;
    const char *tool[4];
    size_t count;
    bool asks_tools;
} Plan;

static char *copy(Arena *a, const char *s, size_t n)
{
    char *d = arena_alloc(a, n + 1);
    if (d) { memcpy(d, s, n); d[n] = 0; }
    return d;
}

static bool ends_with(const char *s, size_t n, const char *suffix)
{
    size_t k = strlen(suffix);
    return n >= k && !memcmp(s + n - k, suffix, k);
}

static bool contains_ci(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < n && p[i] && (p[i] | 32) == (needle[i] | 32)) i++;
        if (i == n) return true;
    }
    return false;
}

static void plan_from(Arena *a, const char *prompt, Plan *p)
{
    memset(p, 0, sizeof *p);
    const char *s = prompt, *prev = NULL;
    size_t prev_n = 0;
    while (*s) {
        while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '(' || *s == '"' || *s == '\'') s++;
        const char *w = s;
        while (*s && *s != ' ' && *s != '\t' && *s != '\n') s++;
        size_t n = (size_t)(s - w);
        while (n && strchr(".,;:?!)\"'", w[n - 1])) n--;              /* sentence punctuation, not the extension */
        if (!n) continue;
        if (ends_with(w, n, ".kicad_pcb") && !p->board) p->board = copy(a, w, n);
        else if ((ends_with(w, n, ".drl") || ends_with(w, n, ".DRL")) && !p->drill) p->drill = copy(a, w, n);
        else if (ends_with(w, n, ".json") && !p->profile) p->profile = copy(a, w, n);
        else if (prev && prev_n == 4 && !memcmp(prev, "into", 4) && !p->out) p->out = copy(a, w, n);
        prev = w;
        prev_n = n;
    }
    bool dfm = contains_ci(prompt, "dfm") || contains_ci(prompt, "manufactur") || contains_ci(prompt, "jlc") || p->profile;
    bool exp = contains_ci(prompt, "export");
    bool load = contains_ci(prompt, "load") || contains_ci(prompt, "how many") || contains_ci(prompt, "outline") ||
                contains_ci(prompt, "summar");
    p->asks_tools = contains_ci(prompt, "tool");
    if (p->board && !dfm && !p->drill && !exp) load = true;
    if (p->board && load) p->tool[p->count++] = "board.load";
    if (p->board && dfm) p->tool[p->count++] = "dfm.check";
    if (p->drill) p->tool[p->count++] = "excellon.check";
    if (p->board && exp && p->out) p->tool[p->count++] = "export.fab";
    if (!p->profile) p->profile = "fab_profiles/jlcpcb_2layer.json";
}

static void write_tool_reply(JsonWriter *w, const Plan *p, const char *tool)
{
    json_write_begin_object(w);
    json_write_key(w, "action"); json_write_string(w, "tool");
    json_write_key(w, "tool"); json_write_string(w, tool);
    json_write_key(w, "arguments");
    json_write_begin_object(w);
    if (!strcmp(tool, "board.load")) { json_write_key(w, "path"); json_write_string(w, p->board); }
    else if (!strcmp(tool, "dfm.check")) {
        json_write_key(w, "path"); json_write_string(w, p->board);
        json_write_key(w, "profile"); json_write_string(w, p->profile);
    } else if (!strcmp(tool, "excellon.check")) {
        json_write_key(w, "path"); json_write_string(w, p->drill);
        if (p->board) { json_write_key(w, "board"); json_write_string(w, p->board); }
    } else {
        json_write_key(w, "path"); json_write_string(w, p->board);
        json_write_key(w, "out"); json_write_string(w, p->out);
    }
    json_write_end_object(w);
    json_write_key(w, "answer"); json_write_string(w, "");
    json_write_end_object(w);
}

static void append(char *buf, size_t cap, const char *text)
{
    size_t n = strlen(buf), k = strlen(text);
    if (n + k + 1 > cap) k = cap - n - 1;
    memcpy(buf + n, text, k);
    buf[n + k] = 0;
}

/* One sentence per tool result. */
static void describe(Arena *a, const char *content, char *answer, size_t cap)
{
    JsonValue *v;
    JsonError e;
    char line[1024];
    if (json_parse(a, content, strlen(content), &v, &e) != JSON_OK) { append(answer, cap, "A tool returned something I could not read. "); return; }
    const JsonValue *tool = json_get(v, "tool"), *err = json_get(v, "error"), *res = json_get(v, "result");
    const char *name = tool && tool->kind == JSON_STRING ? tool->text : "a tool";
    if (err) {
        const JsonValue *msg = json_get(err, "message"), *data = json_get(err, "data"), *kind = json_get(data, "kind");
        char spaced[128] = "";
        if (kind && kind->kind == JSON_STRING) {
            size_t i = 0;
            for (; kind->text[i] && i + 1 < sizeof spaced; i++) spaced[i] = kind->text[i] == '_' ? ' ' : (char)(kind->text[i] | 32);
            spaced[i] = 0;
        }
        snprintf(line, sizeof line, "%s failed: %s (%s). ", name, msg && msg->kind == JSON_STRING ? msg->text : "error", spaced);
        append(answer, cap, line);
        return;
    }
    if (!strcmp(name, "board.load")) {
        const JsonValue *o = json_get(res, "outline"), *closed = json_get(o, "closed");
        snprintf(line, sizeof line, "%s: %s footprints, %s pads, %s tracks; the outline is %s. ",
                 json_get(res, "path") ? json_get(res, "path")->text : "the board",
                 json_get(res, "footprints") ? json_get(res, "footprints")->text : "?",
                 json_get(res, "pads") ? json_get(res, "pads")->text : "?",
                 json_get(res, "tracks") ? json_get(res, "tracks")->text : "?",
                 closed && closed->kind == JSON_TRUE ? "closed" : "open");
        append(answer, cap, line);
        return;
    }
    const JsonValue *rep = json_get(res, "report"), *verdict = json_get(rep, "verdict"), *fs = json_get(rep, "findings");
    snprintf(line, sizeof line, "%s verdict: %s", name, verdict && verdict->kind == JSON_STRING ? verdict->text : "unknown");
    append(answer, cap, line);
    size_t listed = 0;
    for (size_t i = 0; fs && i < fs->count && listed < 6; i++) {
        const JsonValue *st = json_get(fs->items[i], "status"), *rule = json_get(fs->items[i], "rule");
        if (!st || strcmp(st->text, "FAIL") || !rule) continue;
        bool seen = false;
        for (size_t k = 0; k < i && !seen; k++) {
            const JsonValue *r2 = json_get(fs->items[k], "rule"), *s2 = json_get(fs->items[k], "status");
            seen = r2 && s2 && !strcmp(s2->text, "FAIL") && !strcmp(r2->text, rule->text);
        }
        if (seen) continue;
        append(answer, cap, listed ? ", " : " (failing: ");
        append(answer, cap, rule->text);
        listed++;
    }
    append(answer, cap, listed ? "). " : ". ");
}

static LlmStatus rules_chat(LlmBackend *b, const LlmMessage *msgs, size_t n, const char *schema, Arena *a, LlmReply *out)
{
    LlmMockState *s = b->state;
    memset(out, 0, sizeof *out);
    s->calls++;
    s->last_schema = schema;
    s->last_message_count = n;
    const char *user = NULL, *system = NULL;
    size_t tools_seen = 0;
    for (size_t i = 0; i < n; i++) {
        if (msgs[i].role == LLM_USER && !user) user = msgs[i].content;
        if (msgs[i].role == LLM_SYSTEM && !system) system = msgs[i].content;
        if (msgs[i].role == LLM_TOOL) tools_seen++;
    }
    if (!user) { out->error = "no user message"; return LLM_INVALID_ARGUMENT; }
    Plan p;
    plan_from(a, user, &p);
    JsonWriter w;
    json_writer_init_compact(&w, a);
    if (tools_seen < p.count) {
        write_tool_reply(&w, &p, p.tool[tools_seen]);
    } else {
        char answer[4096] = "";
        for (size_t i = 0; i < n; i++) if (msgs[i].role == LLM_TOOL) describe(a, msgs[i].content, answer, sizeof answer);
        if (!p.count) {
            if (p.asks_tools && system) {
                append(answer, sizeof answer, "I can use these tools:");
                for (const char *q = strstr(system, "\n- "); q; q = strstr(q + 3, "\n- ")) {
                    const char *colon = strchr(q + 3, ':');
                    if (!colon) break;
                    char name[64];
                    snprintf(name, sizeof name, " %.*s", (int)(colon - q - 3), q + 3);
                    append(answer, sizeof answer, name);
                }
                append(answer, sizeof answer, ".");
            } else {
                append(answer, sizeof answer, "I need a board file (.kicad_pcb) to work on.");
            }
        }
        size_t len = strlen(answer);
        while (len && answer[len - 1] == ' ') answer[--len] = 0;
        json_write_begin_object(&w);
        json_write_key(&w, "action"); json_write_string(&w, "final");
        json_write_key(&w, "tool"); json_write_string(&w, "");
        json_write_key(&w, "arguments"); json_write_begin_object(&w); json_write_end_object(&w);
        json_write_key(&w, "answer"); json_write_string(&w, answer);
        json_write_end_object(&w);
    }
    size_t len;
    return done(out, json_writer_finish(&w, &len), "mock-rules");
}

void llm_mock_init_rules(LlmBackend *b, LlmMockState *s)
{
    memset(s, 0, sizeof *s);
    b->name = "mock";
    b->chat = rules_chat;
    b->state = s;
}

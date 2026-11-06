#ifndef COPPER_V2_AGENT_LOOP_H
#define COPPER_V2_AGENT_LOOP_H

/*
 * agent_loop.h - a model drives the tool server (Copper V2 track G).
 *
 * The model is told the tools (tools/list) and must answer every turn with one JSON object (agent_reply_schema):
 *   {"action": "tool",  "tool": "<name>", "arguments": {...}, "answer": ""}
 *   {"action": "final", "tool": "",       "arguments": {},    "answer": "<text for the user>"}
 * A tool request is run through tool_server_handle (tools/call) and its result or error goes back as a tool
 * message {"tool": name, "result": ...} / {"tool": name, "error": ...}. A reply that is not valid against the schema
 * is answered with a correction; three in a row end the run. At most 12 model turns (AGENT_MAX_TURNS), whatever the
 * config asks. Every turn is logged as one JSON line: the turn, the raw reply, the action, the tool and its response.
 */

#include "arena.h"
#include "llm.h"
#include "tool_server.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AGENT_MAX_TURNS 12

typedef enum AgentStatus {
    AGENT_DONE = 0,          /* the model gave a final answer */
    AGENT_TURN_LIMIT,        /* 12 turns without a final answer */
    AGENT_INVALID_REPLIES,   /* three replies in a row that were not the reply format */
    AGENT_BACKEND_ERROR,     /* the backend failed (backend_status says how) */
    AGENT_INVALID_ARGUMENT,
    AGENT_OUT_OF_MEMORY
} AgentStatus;

const char *agent_status_name(AgentStatus status);

typedef struct AgentConfig {
    size_t max_turns;               /* 0 or > 12 means 12 */
    size_t max_tool_result_bytes;   /* tool results longer than this are cut for the model (0 = 16384) */
    const char *system_prompt;      /* NULL = the built-in prompt (tools appended either way) */
    ToolLogFn log;                  /* JSONL turn log, may be NULL */
    void *log_ctx;
} AgentConfig;

typedef struct AgentResult {
    AgentStatus status;
    size_t turns;
    const char *answer;             /* the final answer, or NULL */
    const char **tools;             /* tools called, in order */
    size_t tool_count;
    size_t tool_errors;             /* tool calls that returned a JSON-RPC error */
    size_t invalid_replies;
    LlmStatus backend_status;
    const char *backend_error;
    uint64_t prompt_tokens, completion_tokens, model_ms;
} AgentResult;

/* The reply schema (JSON Schema, all properties required), as passed to the backend. */
const char *agent_reply_schema(void);

AgentStatus agent_run(Arena *arena, LlmBackend *backend, ToolServer *tools, const AgentConfig *config,
                      const char *user_prompt, AgentResult *out);

/* Eval scoring (eval/prompts.jsonl "expect"): {"tools": [names in order, as a subsequence of the calls],
 * "answer_contains": [phrases, case-insensitive]}. PASS when the run is DONE and both hold; FAIL otherwise;
 * UNVERIFIED when the backend could not answer (no model, no server). `reasons` gets one line per failed check. */
typedef enum AgentVerdict { AGENT_PASS = 0, AGENT_FAIL, AGENT_UNVERIFIED } AgentVerdict;
const char *agent_verdict_name(AgentVerdict v);
struct JsonValue;
AgentVerdict agent_score(const AgentResult *result, const struct JsonValue *expect, char *reasons, size_t cap);

#ifdef __cplusplus
}
#endif

#endif

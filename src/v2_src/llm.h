#ifndef COPPER_V2_LLM_H
#define COPPER_V2_LLM_H

/*
 * llm.h - the LLM bridge client (Copper V2 track G). C side of the language split (CHARTER 3.1): the model runs
 * elsewhere (Ollama, possibly on another machine); this is only the client, the agent loop and the eval.
 *
 * One backend interface:
 *   LlmStatus (*chat)(LlmBackend*, const LlmMessage* msgs, size_t n, const char* json_schema, Arena*, LlmReply* out);
 * Backends:
 *   mock    (llm_mock.c, the default): scripted replies, or a deterministic rule planner that drives the tools.
 *           Every test and the default eval use it; nothing needs a model.
 *   ollama  (llm_ollama.c): POST {base}/api/chat with "stream": false and "think": false, the reply schema as
 *           "format" (Ollama's structured output) and temperature 0, over plain HTTP/1.1 sockets.
 * schema_gbnf.c turns the reply schema into a GBNF grammar for llama.cpp-style servers.
 * agent_loop.c runs the conversation against the tool server (track F) with at most 12 turns, logging JSONL.
 * Memory: the caller's Arena. No printf, no exit.
 */

#include "arena.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum LlmStatus {
    LLM_OK = 0,
    LLM_INVALID_ARGUMENT,
    LLM_UNAVAILABLE,          /* cannot connect (no server at the URL) */
    LLM_TIMEOUT,
    LLM_HTTP_ERROR,           /* a non-200 status; reply->http_status and reply->error say which */
    LLM_BAD_RESPONSE,         /* the server's answer is not the expected JSON */
    LLM_SCRIPT_EXHAUSTED,     /* mock: no scripted reply left */
    LLM_OUT_OF_MEMORY
} LlmStatus;

const char *llm_status_name(LlmStatus status);

typedef enum LlmRole { LLM_SYSTEM = 0, LLM_USER, LLM_ASSISTANT, LLM_TOOL } LlmRole;

const char *llm_role_name(LlmRole role);

typedef struct LlmMessage {
    LlmRole role;
    const char *content;
} LlmMessage;

typedef struct LlmReply {
    const char *content;      /* the assistant's text (NUL-terminated) */
    size_t content_len;
    const char *model;        /* as reported by the backend */
    uint64_t prompt_tokens, completion_tokens;
    uint64_t elapsed_ms;      /* wall time of the call */
    int http_status;          /* ollama: the HTTP status; 0 otherwise */
    const char *error;        /* a static or arena message when status != LLM_OK */
} LlmReply;

typedef struct LlmBackend LlmBackend;
struct LlmBackend {
    const char *name;         /* "mock", "ollama" */
    LlmStatus (*chat)(LlmBackend *backend, const LlmMessage *msgs, size_t n, const char *json_schema, Arena *arena, LlmReply *out);
    void *state;
};

/* ---------------------------------------------------------------- mock (llm_mock.c) */

typedef struct LlmMockState {
    const char *const *script;    /* scripted replies, used in order; NULL for the rule planner */
    size_t script_count, next;
    size_t calls;
    const char *last_schema;      /* the schema of the latest call (tests check it is passed) */
    size_t last_message_count;
} LlmMockState;

/* Replies `script[0..n)` in order, then LLM_SCRIPT_EXHAUSTED. */
void llm_mock_init_script(LlmBackend *backend, LlmMockState *state, const char *const *script, size_t n);
/* A deterministic planner: reads file paths and keywords in the first user message, asks for board.load,
 * dfm.check, excellon.check or export.fab in turn, then answers from the tool results. It answers in the agent's
 * reply format (agent_reply_schema). */
void llm_mock_init_rules(LlmBackend *backend, LlmMockState *state);

/* ---------------------------------------------------------------- ollama (llm_ollama.c) */

typedef struct LlmOllamaConfig {
    const char *base_url;     /* default "http://localhost:11434" */
    const char *model;        /* e.g. "qwen3:8b" (required) */
    uint32_t timeout_ms;      /* default 300000 */
} LlmOllamaConfig;

typedef struct LlmOllamaState { LlmOllamaConfig config; } LlmOllamaState;

void llm_ollama_init(LlmBackend *backend, LlmOllamaState *state, const LlmOllamaConfig *config);

/* The request body for /api/chat (exposed for tests): model, messages, "stream": false, "think": false, "format":
 * the schema (as a JSON value) when given, "options": {"temperature": 0}. */
LlmStatus llm_ollama_request_body(Arena *arena, const LlmOllamaConfig *config, const LlmMessage *msgs, size_t n,
                                  const char *json_schema, const char **body, size_t *len);
/* Parse an /api/chat response body into `out` (exposed for tests). {"error": ...} is LLM_HTTP_ERROR. */
LlmStatus llm_ollama_parse_reply(Arena *arena, const char *body, size_t len, LlmReply *out);

/* HTTP pieces (exposed for tests). */
typedef struct LlmUrl { const char *host; int port; const char *path; } LlmUrl;
LlmStatus llm_parse_url(Arena *arena, const char *url, LlmUrl *out);              /* http:// only */
/* Split a raw HTTP/1.1 response into status and body (Content-Length, chunked or until close). */
LlmStatus llm_http_parse(Arena *arena, const char *raw, size_t len, int *status, const char **body, size_t *body_len);

/* ---------------------------------------------------------------- GBNF (schema_gbnf.c) */

/* A GBNF grammar (llama.cpp syntax, root rule "root") for a JSON schema subset: object (properties, all
 * required), {"type":"object"} without properties (any JSON object), string (with enum), integer, number,
 * boolean, null, array (items). Anything else is refused with a message in `error`. */
LlmStatus llm_schema_to_gbnf(Arena *arena, const char *schema, size_t len, const char **gbnf, size_t *gbnf_len,
                             const char **error);

#ifdef __cplusplus
}
#endif

#endif

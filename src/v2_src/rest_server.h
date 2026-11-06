#ifndef COPPER_V2_REST_SERVER_H
#define COPPER_V2_REST_SERVER_H

/*
 * rest_server.h - an HTTP/1.1 REST front end for the tool server (Copper V2; CHARTER 3.1: REST serving is C).
 *
 * The same tools as the JSON-RPC stdio server (track F), over HTTP:
 *   GET  /v1/health              {"status":"ok","server":"copper-core"}
 *   GET  /v1/tools               the tool list (names, descriptions, input schemas)
 *   POST /v1/tools/{name}        body: the tool's arguments (a JSON object) -> 200 {"result": ...}
 *   POST /v1/rpc                 body: one JSON-RPC 2.0 message -> 200 with the JSON-RPC response (204 for a
 *                                notification)
 *   POST /v1/shutdown            stops rest_serve after this response
 * A tool error is {"error": {code, message, data: {kind, hint, ...}}} with an HTTP status from its kind:
 * 400 bad request / params, 404 unknown tool / input not found / unknown board_id, 409 output exists,
 * 422 invalid input, 502 kicad-cli failed, 503 kicad-cli unavailable. Other paths 404, other methods 405.
 *
 * rest_handle is pure (one request in, one response out) and is what the tests drive; rest_serve runs a
 * single-threaded accept loop on host:port (127.0.0.1 by default: there is no authentication, so do not expose it
 * beyond a trusted network). Requests are read up to 16 MiB with Content-Length; every response closes the
 * connection. Memory: the caller's Arena per request. No printf, no exit.
 */

#include "arena.h"
#include "tool_server.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RestResponse {
    int status;               /* HTTP status */
    const char *body;         /* JSON (or empty for 204) */
    size_t body_len;
    bool stop;                /* /v1/shutdown */
} RestResponse;

RestResponse rest_handle(ToolServer *server, Arena *arena, const char *method, const char *path, const char *body, size_t len);

typedef enum RestStatus { REST_OK = 0, REST_BIND_FAILED, REST_SOCKET_ERROR } RestStatus;

/* Serve until /v1/shutdown. `ready` (may be NULL) is called once the socket listens, with the bound port. */
RestStatus rest_serve(ToolServer *server, const char *host, int port, void (*ready)(void *ctx, int port), void *ctx);

#ifdef __cplusplus
}
#endif

#endif

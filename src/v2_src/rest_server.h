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
 *   GET  /  (or /index.html)     the browser page given to rest_handle_ui / rest_serve_ui (text/html); 404 when
 *                                none was given
 * Over sockets, a POST must carry Content-Type: application/json (415 otherwise): a page on another origin can
 * only send that header after a CORS preflight, which this server refuses, so other sites cannot drive it.
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
    const char *content_type; /* NULL: application/json */
} RestResponse;

/* A browser page served at GET / (owned by the caller, unchanged while serving). */
typedef struct RestUi {
    const char *html;
    size_t len;
} RestUi;

RestResponse rest_handle(ToolServer *server, Arena *arena, const char *method, const char *path, const char *body, size_t len);
/* rest_handle plus GET / for `ui` (NULL: no page). */
RestResponse rest_handle_ui(ToolServer *server, const RestUi *ui, Arena *arena, const char *method, const char *path,
                            const char *body, size_t len);

typedef enum RestStatus { REST_OK = 0, REST_BIND_FAILED, REST_SOCKET_ERROR } RestStatus;

/* Serve until /v1/shutdown. `ready` (may be NULL) is called once the socket listens, with the bound port. */
RestStatus rest_serve(ToolServer *server, const char *host, int port, void (*ready)(void *ctx, int port), void *ctx);
/* rest_serve that also serves `ui` (NULL: no page) at GET /. */
RestStatus rest_serve_ui(ToolServer *server, const RestUi *ui, const char *host, int port,
                         void (*ready)(void *ctx, int port), void *ctx);

#ifdef __cplusplus
}
#endif

#endif

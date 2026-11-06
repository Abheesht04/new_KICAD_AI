#ifndef COPPER_V2_TOOL_SERVER_H
#define COPPER_V2_TOOL_SERVER_H

/*
 * tool_server.h - JSON-RPC 2.0 tool server over line-delimited stdio (Copper V2 track F).
 *
 * One request per line in, one compact response per line out (notifications get none). The library does no I/O:
 * tool_server_handle takes one line and returns the response; `copper-core serve` runs the stdio loop.
 *
 * Methods: initialize, tools/list, tools/call {name, arguments}, shutdown; each tool can also be called directly
 * as a method with its arguments as params. Tools:
 *   board.load      {path}                          load a .kicad_pcb (track B); returns a board_id and a summary
 *   dfm.check       {board_id | path, profile}      DFM against a fab profile (track C); a findings report (track E)
 *   excellon.check  {path, [board_id | board], [tolerance_nm]}
 *                                                    read an Excellon file; with a board, every hole must match a
 *                                                    board hole (position within tolerance, exact diameter)
 *   export.fab      {board_id | path, out}           Gerbers and drill files through kicad-cli (track D) into a new
 *                                                    folder `out`, then re-read (tracks A, E): a findings report
 * Errors are JSON-RPC errors with a machine-readable kind and a hint in data:
 *   -32700 parse error, -32600 invalid request, -32601 unknown method or tool, -32602 invalid params,
 *   -32001 input not found, -32002 input invalid (with line / item where known), -32003 unknown board_id,
 *   -32010 kicad-cli unavailable, -32011 kicad-cli failed, -32012 output exists.
 * Every message in and out goes to the session log callback as one JSON line: {"seq":n,"in":...} / {"seq":n,"out":...}
 * (an unparseable request is logged as a string). Relative paths resolve against config.root; replies echo paths
 * as given. Memory: a session Arena for loaded boards, a scratch Arena reset per request. No printf, no exit.
 */

#include "arena.h"
#include "board.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ToolLogFn)(void *ctx, const char *line, size_t len);

typedef struct ToolServerConfig {
    const char *root;            /* base for relative paths; NULL = as given */
    const char *kicad_cli;       /* explicit kicad-cli path; NULL = discovery (KICAD_CLI_PATH, PATH, KiCad 9 dirs) */
    uint32_t kicad_timeout_ms;   /* per kicad-cli job; 0 = 600000 */
    const char *version;         /* reported by initialize */
    ToolLogFn log;               /* session log sink (JSONL), may be NULL */
    void *log_ctx;
} ToolServerConfig;

typedef struct ToolBoard { const char *id, *path; Board board; } ToolBoard;

typedef struct ToolServer {
    ToolServerConfig config;
    Arena session, scratch;
    ToolBoard *boards;
    size_t board_count, board_cap;
    uint64_t seq;
    bool shutdown;               /* set by "shutdown" (or "exit"): later requests other than exit are refused */
    bool exited;                 /* set by "exit": the stdio loop stops */
} ToolServer;

void tool_server_init(ToolServer *server, const ToolServerConfig *config);
void tool_server_destroy(ToolServer *server);

/* Handle one request line. Returns the response line (no trailing newline), or NULL for a notification. The text
 * stays valid until the next call. */
const char *tool_server_handle(ToolServer *server, const char *line, size_t len, size_t *response_len);

#ifdef __cplusplus
}
#endif

#endif
